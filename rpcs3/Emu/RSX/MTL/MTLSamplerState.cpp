#include "MTLSamplerState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Utils/algorithm.hpp"
#include "Emu/RSX/rsx_methods.h"

#include "util/fnv_hash.hpp"

#include <algorithm>
#include <bit>

namespace mtl
{
	MTL::SamplerAddressMode get_address_mode(rsx::texture_wrap_mode mode)
	{
		switch (mode)
		{
		case rsx::texture_wrap_mode::wrap:
			return MTL::SamplerAddressModeRepeat;
		case rsx::texture_wrap_mode::mirror:
			return MTL::SamplerAddressModeMirrorRepeat;
		case rsx::texture_wrap_mode::clamp_to_edge:
		case rsx::texture_wrap_mode::clamp:
			return MTL::SamplerAddressModeClampToEdge;
		case rsx::texture_wrap_mode::border:
			return MTL::SamplerAddressModeClampToBorderColor;
		case rsx::texture_wrap_mode::mirror_once_clamp_to_edge:
		case rsx::texture_wrap_mode::mirror_once_border:
		case rsx::texture_wrap_mode::mirror_once_clamp:
			// Metal has no mirror-once variant.  MirrorClampToEdge keeps the mirror
			// half and drops the border (RSX has no per-unit border fetch).
			return MTL::SamplerAddressModeMirrorClampToEdge;
		}

		return MTL::SamplerAddressModeClampToEdge;
	}

	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_minify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_minify_filter::nearest:
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::nearest_linear:
			return MTL::SamplerMinMagFilterNearest;
		case rsx::texture_minify_filter::linear:
		case rsx::texture_minify_filter::linear_nearest:
		case rsx::texture_minify_filter::linear_linear:
		case rsx::texture_minify_filter::convolution_min:
			return MTL::SamplerMinMagFilterLinear;
		}

		return MTL::SamplerMinMagFilterNearest;
	}

	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_magnify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_magnify_filter::nearest:
			return MTL::SamplerMinMagFilterNearest;
		case rsx::texture_magnify_filter::linear:
		case rsx::texture_magnify_filter::convolution_mag:
			return MTL::SamplerMinMagFilterLinear;
		}

		return MTL::SamplerMinMagFilterNearest;
	}

	MTL::SamplerMipFilter get_mip_filter(rsx::texture_minify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_minify_filter::nearest:
		case rsx::texture_minify_filter::linear:
			// The minify filter names the mipmap base level (gcm_enums.h:1526-1527).
			return MTL::SamplerMipFilterNotMipmapped;
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::linear_nearest:
			return MTL::SamplerMipFilterNearest;
		case rsx::texture_minify_filter::nearest_linear:
		case rsx::texture_minify_filter::linear_linear:
			return MTL::SamplerMipFilterLinear;
		case rsx::texture_minify_filter::convolution_min:
			// Vulkan keeps the linear min filter but sets sample_mipmaps = false
			// (VKFormats.cpp:44), so the chain is not sampled.
			return MTL::SamplerMipFilterNotMipmapped;
		}

		return MTL::SamplerMipFilterNotMipmapped;
	}

	u32 get_max_anisotropy(rsx::texture_max_anisotropy aniso)
	{
		u32 samples;

		switch (aniso)
		{
		case rsx::texture_max_anisotropy::x1: samples = 1; break;
		case rsx::texture_max_anisotropy::x2: samples = 2; break;
		case rsx::texture_max_anisotropy::x4: samples = 4; break;
		case rsx::texture_max_anisotropy::x6: samples = 6; break;
		case rsx::texture_max_anisotropy::x8: samples = 8; break;
		case rsx::texture_max_anisotropy::x10: samples = 10; break;
		case rsx::texture_max_anisotropy::x12: samples = 12; break;
		case rsx::texture_max_anisotropy::x16: samples = 16; break;
		default: samples = 1; break;
		}

		// Metal accepts only the powers of two 1, 2, 4, 8 and 16 (K1).  Round up,
		// because the guest asked for at least this many samples, and clamp to the
		// Metal ceiling.
		return std::min(rsx::next_pow2(samples), 16u);
	}

	f32 get_sampler_lod_bias(rsx::texture_minify_filter filter, f32 bias)
	{
		if (get_mip_filter(filter) == MTL::SamplerMipFilterNearest)
		{
			// Round to nearest 0.5 to match the Vulkan path (VKDraw.cpp:506-511).
			bias = std::floor(bias * 2.0f + 0.5f) * 0.5f;
		}

		return std::clamp(bias, -16.0f, 15.999f);
	}

	MTL::SamplerBorderColor get_closest_border_color(const color4f& color)
	{
		// Metal has three border colours (04 R18).  A bright colour becomes white, then
		// an opaque colour becomes black, then transparent black.
		if ((color.r + color.g + color.b) > 1.35f)
		{
			return MTL::SamplerBorderColorOpaqueWhite;
		}

		if (color.a > 0.5f)
		{
			return MTL::SamplerBorderColorOpaqueBlack;
		}

		return MTL::SamplerBorderColorTransparentBlack;
	}

	namespace
	{
		// The fields shared by the two texture classes.  The comparison sampler is
		// selected by the sampled format: the RSX compares a fetched depth against
		// the reference instead of filtering it (VKDraw.cpp:404-410).
		template <typename Texture>
		sampler_state capture_sampler(const Texture& texture)
		{
			sampler_state state;

			state.wrap_s = texture.wrap_s();
			state.wrap_t = texture.wrap_t();
			state.wrap_r = texture.wrap_r();
			state.min_filter = texture.min_filter();
			state.mag_filter = texture.mag_filter();

			// The LOD range is clamped to the resident mip chain, because the guest
			// value can exceed it (VKDraw.cpp:467-518).  The bias is only sampled
			// when the chain has mips, and is quantized for Metal in the helper.
			const u32 mipmaps = texture.get_exact_mipmap_count();
			if (get_mip_filter(state.min_filter) != MTL::SamplerMipFilterNotMipmapped && mipmaps > 1)
			{
				state.min_lod = std::min(texture.min_lod(), static_cast<f32>(mipmaps - 1));
				state.max_lod = std::min(texture.max_lod(), static_cast<f32>(mipmaps - 1));
				state.lod_bias = get_sampler_lod_bias(state.min_filter, texture.bias());
			}

			const u32 format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
			state.depth_compare = (format >= CELL_GCM_TEXTURE_DEPTH24_D8 && format <= CELL_GCM_TEXTURE_DEPTH16_FLOAT);

			return state;
		}
	}

	sampler_state capture_fragment_sampler(u8 unit)
	{
		const auto& texture = rsx::method_registers.fragment_textures[unit];
		sampler_state state = capture_sampler(texture);

		// The SNORM expansion is part of the RSX border colour
		// (RSXTexture.cpp:376-378; VKDraw.cpp:412-413).
		state.border_color = get_closest_border_color(texture.remapped_border_color(texture.argb_signed() != 0));

		// The comparison reference is SET_TEXTURE_ADDRESS.zfunc.
		state.compare = texture.zfunc();
		state.max_aniso = get_max_anisotropy(texture.max_aniso());

		// The shader re-expands signed channels and applies gamma, and the depth
		// pack is decoded after the fetch.  Linear filtering runs before that
		// work, so it would filter the undecoded value (VKDraw.cpp:419-437).
		const rsx::texture_format_ex format_ex = texture.format_ex();
		state.force_nearest = state.depth_compare ||
			(format_ex.texel_remap_control & rsx::texture_control_bits::SEXT_MASK) != 0 ||
			format_ex.hw_SRGB_possible();

		return state;
	}

	sampler_state capture_vertex_sampler(u8 unit)
	{
		// A vertex texture has no zfunc or signed-channel register, so it is never
		// a comparison sampler and its border colour carries no expansion.
		const auto& texture = rsx::method_registers.vertex_textures[unit];
		sampler_state state = capture_sampler(texture);

		// A vertex fetch has no derivatives, so Vulkan and OpenGL force nearest and
		// do not sample the mip chain (VKDraw.cpp:606-619).
		state.min_filter = rsx::texture_minify_filter::nearest;
		state.mag_filter = rsx::texture_magnify_filter::nearest;
		state.min_lod = 0.0f;
		state.max_lod = 0.0f;
		state.lod_bias = 0.0f;

		// CELL_GCM_TEXTURE_UN selects unnormalized coordinates.  Metal requires
		// clamp-to-edge on R and no mip filter for that mode (VKDraw.cpp:594,608,617;
		// KK kk_sampler.c:52-60, :108-110).  A vertex texture is 2D, so R is unused
		// otherwise; Vulkan binds repeat there.
		state.unnormalized_coords = (texture.format() & CELL_GCM_TEXTURE_UN) != 0;
		state.wrap_r = state.unnormalized_coords
			? rsx::texture_wrap_mode::clamp_to_edge
			: rsx::texture_wrap_mode::wrap;

		state.border_color = get_closest_border_color(texture.remapped_border_color());
		state.depth_compare = false;

		return state;
	}

	sampler_state stencil_mirror_sampler()
	{
		// Clamp-to-border with an opaque-black border, nearest, no mip
		// (VKDraw.cpp:707-717).
		sampler_state state;
		state.wrap_s = state.wrap_t = state.wrap_r = rsx::texture_wrap_mode::border;
		state.border_color = MTL::SamplerBorderColorOpaqueBlack;

		return state;
	}

	MTL::SamplerState* create_sampler_state(MTL::Device* device, const sampler_state& state)
	{
		MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();

		descriptor->setSAddressMode(get_address_mode(state.wrap_s));
		descriptor->setTAddressMode(get_address_mode(state.wrap_t));
		descriptor->setRAddressMode(get_address_mode(state.wrap_r));
		if (state.force_nearest)
		{
			// The filter must not run on the stored value; the shader decodes it.
			// The mip chain stays available, with nearest selection, as in Vulkan.
			descriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
			descriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
			descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
		}
		else
		{
			descriptor->setMinFilter(get_min_mag_filter(state.min_filter));
			descriptor->setMagFilter(get_min_mag_filter(state.mag_filter));
			descriptor->setMipFilter(get_mip_filter(state.min_filter));
			descriptor->setMaxAnisotropy(state.max_aniso);
		}

		descriptor->setLodMinClamp(state.min_lod);
		descriptor->setLodMaxClamp(state.max_lod);
		descriptor->setLodBias(state.lod_bias);
		descriptor->setBorderColor(state.border_color);
		descriptor->setNormalizedCoordinates(!state.unnormalized_coords);

		if (state.depth_compare)
		{
			// The sampler sees the reference first, the RSX compares the texel first,
			// so a directed function is swapped (VKDraw.cpp:32-45, REVIEW-FINDINGS.md A7).
			descriptor->setCompareFunction(get_compare_function(state.compare, true));
		}

		MTL::SamplerState* result = device->newSamplerState(descriptor);

		descriptor->release();
		return result;
	}

	bool operator==(const sampler_state& a, const sampler_state& b)
	{
		return a.wrap_s == b.wrap_s
			&& a.wrap_t == b.wrap_t
			&& a.wrap_r == b.wrap_r
			&& a.min_filter == b.min_filter
			&& a.mag_filter == b.mag_filter
			&& a.min_lod == b.min_lod
			&& a.max_lod == b.max_lod
			&& a.lod_bias == b.lod_bias
			&& a.max_aniso == b.max_aniso
			&& a.unnormalized_coords == b.unnormalized_coords
			&& a.border_color == b.border_color
			&& a.depth_compare == b.depth_compare
			&& a.compare == b.compare
			&& a.force_nearest == b.force_nearest;
	}

	u64 hash_sampler_state(const sampler_state& state)
	{
		// The comparison function is a 16-bit GL constant, so the fields are hashed
		// at their own widths.  A packed word cannot hold them: a four-bit slot for
		// `compare` overlapped `depth_compare` and survived only because the current
		// comparison values happen to be small.
		usz hash = rpcs3::hash64(rpcs3::fnv_seed, static_cast<u16>(state.wrap_s));
		hash = rpcs3::hash64(hash, static_cast<u16>(state.wrap_t));
		hash = rpcs3::hash64(hash, static_cast<u16>(state.wrap_r));
		hash = rpcs3::hash64(hash, static_cast<u16>(state.min_filter));
		hash = rpcs3::hash64(hash, static_cast<u16>(state.mag_filter));
		hash = rpcs3::hash64(hash, static_cast<u32>(state.border_color));
		hash = rpcs3::hash64(hash, static_cast<u16>(state.compare));
		hash = rpcs3::hash64(hash, static_cast<u8>(state.depth_compare));
		hash = rpcs3::hash64(hash, static_cast<u8>(state.force_nearest));
		hash = rpcs3::hash64(hash, static_cast<u8>(state.unnormalized_coords));
		hash = rpcs3::hash64(hash, state.max_aniso);
		hash = rpcs3::hash64(hash, std::bit_cast<u32>(state.min_lod));
		hash = rpcs3::hash64(hash, std::bit_cast<u32>(state.max_lod));
		return rpcs3::hash64(hash, std::bit_cast<u32>(state.lod_bias));
	}

	void sampler_cache::shutdown()
	{
		for (const auto& [key, object] : m_cache)
		{
			object->release();
		}

		m_cache.clear();
		m_device = nullptr;
	}

	MTL::SamplerState* sampler_cache::get(const sampler_state& state)
	{
		const auto found = m_cache.find(state);
		if (found != m_cache.end())
		{
			return found->second;
		}

		MTL::SamplerState* object = create_sampler_state(m_device, state);
		if (object)
		{
			m_cache.emplace(state, object);
		}

		return object;
	}
}

#endif // RSX_ENABLE_METAL4
