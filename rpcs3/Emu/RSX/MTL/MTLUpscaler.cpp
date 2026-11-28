#include "MTLDevice.h"
#include "MTLInstrumentation.h"

#include "MTLUpscaler.h"
#include "MTLUpscaleFSR.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"
#include "MTLConstants.h"
#include "MTLResourceManager.h"

#include "Emu/system_config.h"

#include "util/logs.hpp"

#include <string>

// The CPU half of FSR 1: the constant builders are the reference ones, so the
// numbers that reach the kernels are the numbers the Vulkan and OpenGL passes
// receive.  Only this translation unit includes it.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wunused-function"
#endif

#define A_CPU 1
#include "3rdparty/GPUOpen/include/ffx_a.h"
#include "3rdparty/GPUOpen/include/ffx_fsr1.h"
#undef A_CPU

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace mtl
{
	namespace
	{
		// One aligned block per dispatch: EASU takes five vec4s and RCAS one.  The
		// ring hands out 16-byte blocks, which is what a `constant` reference needs.
		constexpr u32 fsr_easu_constants_size = 80;
		constexpr u32 fsr_rcas_constants_size = 16;
		constexpr u32 fsr_alignment = 16;

		// FSR 1, float path, as MSL.  This is a restatement of AMD's ffx_fsr1.h
		// (MIT), which the Vulkan and OpenGL backends compile from
		// Program/Upscalers/FSR1/fsr_ubershader.glsl.  The structure, the order of
		// every arithmetic operation and the two exponent approximations are kept
		// line for line: FSR is defined by its exact taps, so a "cleaner" formulation
		// is a different upscaler.
		//
		// Only the float path is carried.  The shared ubershader compiles with
		// SAMPLE_SLOW_FALLBACK and without A_HALF, so the half path is dead on the
		// other backends too, and Metal needs it even less.
		//
		// The one deliberate difference is the RCAS guard: the reference fetches with
		// texelFetch, which Vulkan defines as zero outside the image, while MSL leaves
		// an out-of-range read undefined.  The guard supplies the zero.
		constexpr const char* fsr_msl = R"(#include <metal_stdlib>
using namespace metal;

struct fsr_easu_constants
{
    uint4 con0;
    uint4 con1;
    uint4 con2;
    uint4 con3;
};

struct fsr_rcas_constants
{
    uint4 con0;
};

// [FsrEasuF]'s reciprocal and inverse root are exponent estimates, not library
// calls: the reference builds them by subtracting the input's bits from a magic
// constant.  `fast::` versions are different functions and would move the image.
static inline float fsr_prx_lo_rcp(float value)
{
    return as_type<float>(0x7ef07ebbu - as_type<uint>(value));
}

static inline float fsr_prx_lo_rsq(float value)
{
    return as_type<float>(0x5f347d74u - (as_type<uint>(value) >> 1u));
}

static inline float fsr_prx_med_rcp(float value)
{
    const float guess = as_type<float>(0x7ef19fffu - as_type<uint>(value));
    return guess * (-guess * value + 2.0f);
}

static inline float fsr_min3(float a, float b, float c)
{
    return min(a, min(b, c));
}

static inline float fsr_max3(float a, float b, float c)
{
    return max(a, max(b, c));
}

static inline float3 fsr_min3(float3 a, float3 b, float3 c)
{
    return min(a, min(b, c));
}

static inline float3 fsr_max3(float3 a, float3 b, float3 c)
{
    return max(a, max(b, c));
}

// FFX's ARmp8x8 lane remap.  The table is generated from mtl::fsr::lane_to_tile
// so the geometry the unit tests check is the geometry the kernels use.
@FSR_LANE_TABLE@

// The ubershader's four CurrFilter calls, generated from mtl::fsr::tile_offsets.
@FSR_TILE_OFFSETS@

static inline void fsr_easu_tap(thread float3& colour, thread float& weight,
    float2 offset, float2 direction, float2 length, float lobe, float clip_point, float3 tap)
{
    float2 v;
    v.x = (offset.x * direction.x) + (offset.y * direction.y);
    v.y = (offset.x * (-direction.y)) + (offset.y * direction.x);
    v *= length;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clip_point);
    float wb = (2.0f / 5.0f) * d2 + (-1.0f);
    float wa = lobe * d2 + (-1.0f);
    wb *= wb;
    wa *= wa;
    wb = (25.0f / 16.0f) * wb + (-(25.0f / 16.0f - 1.0f));
    const float w = wb * wa;
    colour += tap * w;
    weight += w;
}

static inline void fsr_easu_set(thread float2& direction, thread float& length, float2 pp,
    bool bi_s, bool bi_t, bool bi_u, bool bi_v,
    float l_a, float l_b, float l_c, float l_d, float l_e)
{
    float w = 0.0f;

    if (bi_s)
    {
        w = (1.0f - pp.x) * (1.0f - pp.y);
    }

    if (bi_t)
    {
        w = pp.x * (1.0f - pp.y);
    }

    if (bi_u)
    {
        w = (1.0f - pp.x) * pp.y;
    }

    if (bi_v)
    {
        w = pp.x * pp.y;
    }

    const float dc = l_d - l_c;
    const float cb = l_c - l_b;
    float len_x = max(abs(dc), abs(cb));
    len_x = fsr_prx_lo_rcp(len_x);
    const float dir_x = l_d - l_b;
    direction.x += dir_x * w;
    len_x = saturate(abs(dir_x) * len_x);
    len_x *= len_x;
    length += len_x * w;

    const float ec = l_e - l_c;
    const float ca = l_c - l_a;
    float len_y = max(abs(ec), abs(ca));
    len_y = fsr_prx_lo_rcp(len_y);
    const float dir_y = l_e - l_a;
    direction.y += dir_y * w;
    len_y = saturate(abs(dir_y) * len_y);
    len_y *= len_y;
    length += len_y * w;
}

static inline float3 fsr_easu_texel(texture2d<float> source, sampler filter, uint2 ip,
    constant fsr_easu_constants& constants)
{
    float2 pp = float2(ip) * as_type<float2>(constants.con0.xy) + as_type<float2>(constants.con0.zw);
    const float2 fp = floor(pp);
    pp -= fp;

    const float2 p0 = fp * as_type<float2>(constants.con1.xy) + as_type<float2>(constants.con1.zw);
    const float2 p1 = p0 + as_type<float2>(constants.con2.xy);
    const float2 p2 = p0 + as_type<float2>(constants.con2.zw);
    const float2 p3 = p0 + as_type<float2>(constants.con3.xy);

    // MSL's gather takes (sampler, coord, int2 offset, component) and the offset is
    // defaulted *before* the component, so the component cannot be passed third:
    // the zero offset has to be written out even though it is never used.
    const float4 bczz_r = source.gather(filter, p0, int2(0), component::x);
    const float4 bczz_g = source.gather(filter, p0, int2(0), component::y);
    const float4 bczz_b = source.gather(filter, p0, int2(0), component::z);
    const float4 ijfe_r = source.gather(filter, p1, int2(0), component::x);
    const float4 ijfe_g = source.gather(filter, p1, int2(0), component::y);
    const float4 ijfe_b = source.gather(filter, p1, int2(0), component::z);
    const float4 klhg_r = source.gather(filter, p2, int2(0), component::x);
    const float4 klhg_g = source.gather(filter, p2, int2(0), component::y);
    const float4 klhg_b = source.gather(filter, p2, int2(0), component::z);
    const float4 zzon_r = source.gather(filter, p3, int2(0), component::x);
    const float4 zzon_g = source.gather(filter, p3, int2(0), component::y);
    const float4 zzon_b = source.gather(filter, p3, int2(0), component::z);

    const float4 bczz_l = bczz_b * 0.5f + (bczz_r * 0.5f + bczz_g);
    const float4 ijfe_l = ijfe_b * 0.5f + (ijfe_r * 0.5f + ijfe_g);
    const float4 klhg_l = klhg_b * 0.5f + (klhg_r * 0.5f + klhg_g);
    const float4 zzon_l = zzon_b * 0.5f + (zzon_r * 0.5f + zzon_g);

    const float bl = bczz_l.x;
    const float cl = bczz_l.y;
    const float il = ijfe_l.x;
    const float jl = ijfe_l.y;
    const float fl = ijfe_l.z;
    const float el = ijfe_l.w;
    const float kl = klhg_l.x;
    const float ll = klhg_l.y;
    const float hl = klhg_l.z;
    const float gl = klhg_l.w;
    const float ol = zzon_l.z;
    const float nl = zzon_l.w;

    float2 direction = float2(0.0f);
    float length = 0.0f;
    fsr_easu_set(direction, length, pp, true, false, false, false, bl, el, fl, gl, jl);
    fsr_easu_set(direction, length, pp, false, true, false, false, cl, fl, gl, hl, kl);
    fsr_easu_set(direction, length, pp, false, false, true, false, fl, il, jl, kl, nl);
    fsr_easu_set(direction, length, pp, false, false, false, true, gl, jl, kl, ll, ol);

    const float2 direction2 = direction * direction;
    float dir_r = direction2.x + direction2.y;
    const bool zero = dir_r < (1.0f / 32768.0f);
    dir_r = fsr_prx_lo_rsq(dir_r);
    dir_r = zero ? 1.0f : dir_r;
    direction.x = zero ? 1.0f : direction.x;
    direction *= float2(dir_r);
    length = length * 0.5f;
    length *= length;

    const float stretch = (direction.x * direction.x + direction.y * direction.y) *
        fsr_prx_lo_rcp(max(abs(direction.x), abs(direction.y)));
    const float2 len2 = float2(1.0f + (stretch - 1.0f) * length, 1.0f + (-0.5f) * length);
    // The reference folds ((1.0/4.0 - 0.04) - 0.5) in double and casts once, and
    // the two-step float form rounds to the next float up. -0.29f is that value.
    const float lobe = 0.5f + (-0.29f) * length;
    const float clip_point = fsr_prx_lo_rcp(lobe);

    const float3 min4 = min(fsr_min3(float3(ijfe_r.z, ijfe_g.z, ijfe_b.z),
        float3(klhg_r.w, klhg_g.w, klhg_b.w), float3(ijfe_r.y, ijfe_g.y, ijfe_b.y)),
        float3(klhg_r.x, klhg_g.x, klhg_b.x));
    const float3 max4 = max(fsr_max3(float3(ijfe_r.z, ijfe_g.z, ijfe_b.z),
        float3(klhg_r.w, klhg_g.w, klhg_b.w), float3(ijfe_r.y, ijfe_g.y, ijfe_b.y)),
        float3(klhg_r.x, klhg_g.x, klhg_b.x));

    float3 colour = float3(0.0f);
    float weight = 0.0f;
    fsr_easu_tap(colour, weight, float2( 0.0f, -1.0f) - pp, direction, len2, lobe, clip_point,
        float3(bczz_r.x, bczz_g.x, bczz_b.x)); // b
    fsr_easu_tap(colour, weight, float2( 1.0f, -1.0f) - pp, direction, len2, lobe, clip_point,
        float3(bczz_r.y, bczz_g.y, bczz_b.y)); // c
    fsr_easu_tap(colour, weight, float2(-1.0f,  1.0f) - pp, direction, len2, lobe, clip_point,
        float3(ijfe_r.x, ijfe_g.x, ijfe_b.x)); // i
    fsr_easu_tap(colour, weight, float2( 0.0f,  1.0f) - pp, direction, len2, lobe, clip_point,
        float3(ijfe_r.y, ijfe_g.y, ijfe_b.y)); // j
    fsr_easu_tap(colour, weight, float2( 0.0f,  0.0f) - pp, direction, len2, lobe, clip_point,
        float3(ijfe_r.z, ijfe_g.z, ijfe_b.z)); // f
    fsr_easu_tap(colour, weight, float2(-1.0f,  0.0f) - pp, direction, len2, lobe, clip_point,
        float3(ijfe_r.w, ijfe_g.w, ijfe_b.w)); // e
    fsr_easu_tap(colour, weight, float2( 1.0f,  1.0f) - pp, direction, len2, lobe, clip_point,
        float3(klhg_r.x, klhg_g.x, klhg_b.x)); // k
    fsr_easu_tap(colour, weight, float2( 2.0f,  1.0f) - pp, direction, len2, lobe, clip_point,
        float3(klhg_r.y, klhg_g.y, klhg_b.y)); // l
    fsr_easu_tap(colour, weight, float2( 2.0f,  0.0f) - pp, direction, len2, lobe, clip_point,
        float3(klhg_r.z, klhg_g.z, klhg_b.z)); // h
    fsr_easu_tap(colour, weight, float2( 1.0f,  0.0f) - pp, direction, len2, lobe, clip_point,
        float3(klhg_r.w, klhg_g.w, klhg_b.w)); // g
    fsr_easu_tap(colour, weight, float2( 1.0f,  2.0f) - pp, direction, len2, lobe, clip_point,
        float3(zzon_r.z, zzon_g.z, zzon_b.z)); // o
    fsr_easu_tap(colour, weight, float2( 0.0f,  2.0f) - pp, direction, len2, lobe, clip_point,
        float3(zzon_r.w, zzon_g.w, zzon_b.w)); // n

    return min(max4, max(min4, colour * (1.0f / weight)));
}

static inline float4 fsr_rcas_load(texture2d<float, access::read> source, int2 p, uint2 size)
{
    if (p.x < 0 || p.y < 0 || uint(p.x) >= size.x || uint(p.y) >= size.y)
    {
        return float4(0.0f);
    }

    return source.read(uint2(p));
}

static inline float3 fsr_rcas_texel(texture2d<float, access::read> source, uint2 ip, uint2 size,
    constant fsr_rcas_constants& constants)
{
    const float3 b = fsr_rcas_load(source, int2(ip) + int2( 0, -1), size).rgb;
    const float3 d = fsr_rcas_load(source, int2(ip) + int2(-1,  0), size).rgb;
    const float3 e = fsr_rcas_load(source, int2(ip), size).rgb;
    const float3 f = fsr_rcas_load(source, int2(ip) + int2( 1,  0), size).rgb;
    const float3 h = fsr_rcas_load(source, int2(ip) + int2( 0,  1), size).rgb;

    const float min4_r = min(fsr_min3(b.r, d.r, f.r), h.r);
    const float min4_g = min(fsr_min3(b.g, d.g, f.g), h.g);
    const float min4_b = min(fsr_min3(b.b, d.b, f.b), h.b);
    const float max4_r = max(fsr_max3(b.r, d.r, f.r), h.r);
    const float max4_g = max(fsr_max3(b.g, d.g, f.g), h.g);
    const float max4_b = max(fsr_max3(b.b, d.b, f.b), h.b);

    const float hit_min_r = min4_r * (1.0f / (4.0f * max4_r));
    const float hit_min_g = min4_g * (1.0f / (4.0f * max4_g));
    const float hit_min_b = min4_b * (1.0f / (4.0f * max4_b));
    const float hit_max_r = (1.0f - max4_r) * (1.0f / (4.0f * min4_r - 4.0f));
    const float hit_max_g = (1.0f - max4_g) * (1.0f / (4.0f * min4_g - 4.0f));
    const float hit_max_b = (1.0f - max4_b) * (1.0f / (4.0f * min4_b - 4.0f));

    const float lobe_r = max(-hit_min_r, hit_max_r);
    const float lobe_g = max(-hit_min_g, hit_max_g);
    const float lobe_b = max(-hit_min_b, hit_max_b);

    // FSR_RCAS_LIMIT, the reference's cap on the negative lobe.
    const float lobe = max(-0.1875f, min(fsr_max3(lobe_r, lobe_g, lobe_b), 0.0f)) *
        as_type<float>(constants.con0.x);
    const float rcp_l = fsr_prx_med_rcp(4.0f * lobe + 1.0f);

    return (lobe * b + lobe * d + lobe * h + lobe * f + e) * rcp_l;
}

kernel void fsr_easu_kernel(texture2d<float> source [[texture(@FSR_SOURCE_SLOT@)]],
    sampler filter [[sampler(@FSR_SAMPLER_SLOT@)]],
    texture2d<float, access::write> destination [[texture(@FSR_DESTINATION_SLOT@)]],
    constant fsr_easu_constants& constants [[buffer(@FSR_CONSTANTS_SLOT@)]],
    // MSL requires every stage input to be scalar, or every one to be a vector of
    // the same width: a uint3 position cannot be mixed with a scalar lane index.
    uint2 group [[threadgroup_position_in_grid]],
    uint2 lane [[thread_position_in_threadgroup]])
{
    const uint2 size = uint2(destination.get_width(), destination.get_height());
    const uint2 base = uint2(group.x << 4u, group.y << 4u) + fsr_lane_tile[lane.x];

    // The four texels of the 16x16 tile the ubershader writes, in its order:
    // (0,0), (8,0), (8,8), (0,8) from the lane's point.
    for (uint index = 0; index < fsr_tile_point_count; ++index)
    {
        const uint2 xy = base + fsr_tile_offsets[index];

        if (xy.x < size.x && xy.y < size.y)
        {
            destination.write(float4(fsr_easu_texel(source, filter, xy, constants), 1.0f), xy);
        }
    }
}

kernel void fsr_rcas_kernel(texture2d<float, access::read> source [[texture(@FSR_SOURCE_SLOT@)]],
    texture2d<float, access::write> destination [[texture(@FSR_DESTINATION_SLOT@)]],
    constant fsr_rcas_constants& constants [[buffer(@FSR_CONSTANTS_SLOT@)]],
    // MSL requires every stage input to be scalar, or every one to be a vector of
    // the same width: a uint3 position cannot be mixed with a scalar lane index.
    uint2 group [[threadgroup_position_in_grid]],
    uint2 lane [[thread_position_in_threadgroup]])
{
    const uint2 size = uint2(destination.get_width(), destination.get_height());
    const uint2 base = uint2(group.x << 4u, group.y << 4u) + fsr_lane_tile[lane.x];

    for (uint index = 0; index < fsr_tile_point_count; ++index)
    {
        const uint2 xy = base + fsr_tile_offsets[index];

        if (xy.x < size.x && xy.y < size.y)
        {
            destination.write(float4(fsr_rcas_texel(source, xy, size, constants), 1.0f), xy);
        }
    }
}
)";

		void replace_token(std::string& text, const char* token, const std::string& value)
		{
			const std::string name = token;

			for (usz pos = text.find(name); pos != std::string::npos; pos = text.find(name, pos + value.size()))
			{
				text.replace(pos, name.size(), value);
			}
		}

		std::string build_lane_table()
		{
			std::string text = "constant uint2 fsr_lane_tile[" + std::to_string(fsr::lane_count) + "] =\n{\n";

			for (u32 lane = 0; lane < fsr::lane_count; ++lane)
			{
				const fsr::point tile = fsr::lane_to_tile(lane);
				text += "    uint2(" + std::to_string(tile.x) + "u, " + std::to_string(tile.y) + "u)";
				text += (lane + 1u == fsr::lane_count) ? "\n" : ",\n";
			}

			text += "};\n";
			return text;
		}

		std::string build_tile_offsets()
		{
			std::string text = "constant uint2 fsr_tile_offsets[" + std::to_string(fsr::tile_point_count) + "] =\n{\n";

			for (u32 index = 0; index < fsr::tile_point_count; ++index)
			{
				const fsr::point offset = fsr::tile_offsets[index];
				text += "    uint2(" + std::to_string(offset.x) + "u, " + std::to_string(offset.y) + "u)";
				text += (index + 1u == fsr::tile_point_count) ? "\n" : ",\n";
			}

			// The kernels' loop bound is the same number, so a change to the
			// geometry cannot leave a stale `4u` in the shader.
			text += "};\n\nconstant uint fsr_tile_point_count = " + std::to_string(fsr::tile_point_count) + ";\n";
			return text;
		}
	}

	namespace fsr
	{
		std::string library_source()
		{
			std::string source = fsr_msl;

			const auto slot = [](u32 value) { return std::to_string(value); };

			replace_token(source, "@FSR_LANE_TABLE@", build_lane_table());
			replace_token(source, "@FSR_TILE_OFFSETS@", build_tile_offsets());
			replace_token(source, "@FSR_CONSTANTS_SLOT@", slot(constants_slot));
			replace_token(source, "@FSR_SOURCE_SLOT@", slot(source_slot));
			replace_token(source, "@FSR_DESTINATION_SLOT@", slot(destination_slot));
			replace_token(source, "@FSR_SAMPLER_SLOT@", slot(sampler_slot));
			return source;
		}
	}

	bool fsr_upscale_pass::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_compiler = dev.compiler();
		m_resources = &resources;

		if (!m_device || !m_compiler)
		{
			return false;
		}

		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		const std::string source = fsr::library_source();
		MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
		descriptor->setSource(NS::String::string(source.c_str(), NS::UTF8StringEncoding));

		// The upscaler is small and bandwidth-bound, so it takes the precise mode
		// unconditionally instead of inheriting the pipeline maths setting (U1).
		MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
		options->setMathMode(MTL::MathModeSafe);
		descriptor->setOptions(options);

		NS::Error* error = nullptr;
		m_library = m_compiler->newLibrary(descriptor, &error);
		options->release();
		descriptor->release();

		if (!m_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: FSR library creation failed: %s", message);
			pool->release();
			return false;
		}

		m_easu = build_pipeline("fsr_easu_kernel");
		m_rcas = build_pipeline("fsr_rcas_kernel");

		// EASU gathers with the reference sampler's clamp-to-edge modes; the filter
		// is unused by `gather` and is set to linear to match the Vulkan pass.
		MTL::SamplerDescriptor* sampler_descriptor = MTL::SamplerDescriptor::alloc()->init();
		sampler_descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		m_sampler = m_device->newSamplerState(sampler_descriptor);
		sampler_descriptor->release();

		MTL4::ArgumentTableDescriptor* table_descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
		table_descriptor->setMaxBufferBindCount(fsr::constants_slot + 1);
		table_descriptor->setMaxTextureBindCount(fsr::destination_slot + 1);
		table_descriptor->setMaxSamplerStateBindCount(fsr::sampler_slot + 1);
		table_descriptor->setInitializeBindings(true);
		m_easu_table = m_device->newArgumentTable(table_descriptor, nullptr);
		m_rcas_table = m_device->newArgumentTable(table_descriptor, nullptr);
		table_descriptor->release();

		pool->release();

		if (!m_easu || !m_rcas || !m_sampler || !m_easu_table || !m_rcas_table)
		{
			rsx_log.error("Metal: FSR pass initialization failed");
			return false;
		}

		return true;
	}

	void fsr_upscale_pass::shutdown()
	{
		const auto release_image = [this](MTL::Texture*& image)
		{
			if (image)
			{
				m_resources->defer_release(image);
				image = nullptr;
			}
		};

		release_image(m_intermediate);
		release_image(m_output);

		if (m_easu_table)
		{
			m_easu_table->release();
			m_easu_table = nullptr;
		}

		if (m_rcas_table)
		{
			m_rcas_table->release();
			m_rcas_table = nullptr;
		}

		if (m_easu)
		{
			m_easu->release();
			m_easu = nullptr;
		}

		if (m_rcas)
		{
			m_rcas->release();
			m_rcas = nullptr;
		}

		if (m_sampler)
		{
			m_sampler->release();
			m_sampler = nullptr;
		}

		if (m_library)
		{
			m_library->release();
			m_library = nullptr;
		}

		m_compiler = nullptr;
		m_device = nullptr;
		m_resources = nullptr;
	}

	MTL::ComputePipelineState* fsr_upscale_pass::build_pipeline(const char* name)
	{
		MTL4::LibraryFunctionDescriptor* function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		function->setLibrary(m_library);
		function->setName(NS::String::string(name, NS::UTF8StringEncoding));

		MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
		descriptor->setComputeFunctionDescriptor(function);

		NS::Error* error = nullptr;
		MTL::ComputePipelineState* state = m_compiler->newComputePipelineState(descriptor, nullptr, &error);

		function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: FSR pipeline creation failed for %s: %s", name, message);
			return nullptr;
		}

		return state;
	}

	MTL::Texture* fsr_upscale_pass::ensure_image(MTL::Texture*& image, u32 width, u32 height)
	{
		// The size is exact: the compose pass samples this image one-to-one over
		// the aspect rectangle, so a larger reused image would shift every texel.
		if (image && image->width() == width && image->height() == height)
		{
			return image;
		}

		if (image)
		{
			m_resources->defer_release(image);
			image = nullptr;
		}

		// The kernels write the image and the next stage samples it, so the usage is
		// the pair.  BGRA8 is the drawable's own format, so the output pass samples
		// it without a repack.
		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(MTL::TextureType2D);
		descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
		descriptor->setWidth(width);
		descriptor->setHeight(height);
		descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
		image = m_resources->new_texture(descriptor, MTL::StorageModePrivate);
		descriptor->release();

		if (image)
		{
			m_resources->make_persistent(image);
		}

		return image;
	}

	void fsr_upscale_pass::dispatch(MTL4::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* state,
		MTL4::ArgumentTable* table, MTL::Texture* source, MTL::Texture* destination,
		MTL::GPUAddress constants, u32 width, u32 height)
	{
		table->setTexture(source->gpuResourceID(), fsr::source_slot);
		table->setTexture(destination->gpuResourceID(), fsr::destination_slot);
		table->setAddress(constants, fsr::constants_slot);

		encoder->setComputePipelineState(state);
		encoder->setArgumentTable(table);

		const MTL::Size groups(fsr::group_count(width), fsr::group_count(height), 1);
		encoder->dispatchThreadgroups(groups, MTL::Size::Make(fsr::lane_count, 1, 1));
	}

	MTL::Texture* fsr_upscale_pass::run(command_recorder& recorder, constants_heap& constants,
		MTL::Texture* source, u32 input_width, u32 input_height, u32 width, u32 height)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/fsr");

		if (!m_library || !m_easu || !m_rcas || !m_sampler || !m_easu_table || !m_rcas_table)
		{
			return nullptr;
		}

		// A multisample or array source has no plain 2D gather, and the present
		// image never is one.
		const bool single_sample_2d = source &&
			source->sampleCount() == 1 && source->textureType() == MTL::TextureType2D;

		if (!fsr::can_upscale(single_sample_2d, source != nullptr, input_width, input_height, width, height))
		{
			return nullptr;
		}

		MTL::Texture* intermediate = ensure_image(m_intermediate, width, height);
		MTL::Texture* output = ensure_image(m_output, width, height);

		if (!intermediate || !output)
		{
			return nullptr;
		}

		// The constants are the reference CPU helpers, exactly as the Vulkan pass
		// builds them (fsr_pass.cpp:150-160, :193-199).
		u32 easu_offset = 0;
		auto* easu = static_cast<u32*>(constants.overlay_uniform_ring().alloc(fsr_easu_constants_size, easu_offset, fsr_alignment));
		u32 rcas_offset = 0;
		auto* rcas = static_cast<u32*>(constants.overlay_uniform_ring().alloc(fsr_rcas_constants_size, rcas_offset, fsr_alignment));

		if (!easu || !rcas)
		{
			return nullptr;
		}

		FsrEasuCon(&easu[0], &easu[4], &easu[8], &easu[12],
			static_cast<f32>(input_width), static_cast<f32>(input_height),
			static_cast<f32>(source->width()), static_cast<f32>(source->height()),
			static_cast<f32>(width), static_cast<f32>(height));

		// 0 is the sharpest with 2 being the chosen limit, and each progressive unit
		// halves the sharpening intensity (fsr_pass.cpp:193-199).
		FsrRcasCon(&rcas[0], fsr::rcas_attenuation(g_cfg.video.rcas_sharpening_intensity));

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			return nullptr;
		}

		// The source was written by the frame's own encoders, so the read needs the
		// queue form of the edge (HLD-03 §2.1 item 6).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageDispatch, MTL4::VisibilityOptionDevice, true });

		// Only EASU gathers, so only its table carries the sampler.
		m_easu_table->setSamplerState(m_sampler->gpuResourceID(), fsr::sampler_slot);

		dispatch(encoder, m_easu, m_easu_table, source, intermediate,
			constants.overlay_uniform_ring().address() + easu_offset, width, height);

		// EASU and RCAS share one encoder, so their edge is the encoder-scope form.
		emit_barrier(encoder, barrier_spec{ MTL::StageDispatch, MTL::StageDispatch, MTL4::VisibilityOptionDevice, false });

		dispatch(encoder, m_rcas, m_rcas_table, intermediate, output,
			constants.overlay_uniform_ring().address() + rcas_offset, width, height);

		// TODO(excise): level 2; the FSR pass ran (area A).
		MTL_PROBE_LATCH(2, "present/fsr ran");

		return output;
	}
}

#endif // RSX_ENABLE_METAL4
