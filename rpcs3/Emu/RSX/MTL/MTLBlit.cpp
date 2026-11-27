#include "MTLDevice.h"
#include "MTLInstrumentation.h"

#include "MTLBlit.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLConstants.h"
#include "MTLDepthStencil.h"
#include "MTLFormats.h"

#include "util/logs.hpp"

#include <algorithm>
#include <memory>
#include <string>

namespace mtl
{
	namespace
	{
		// The blit's own argument slots.  It owns an argument table of its own
		// (frame_slot::blit_table), so the numbers only have to agree with the
		// library below and stay inside the table's declared counts
		// (MTLArgumentBinds.h, argument_buffer_count / argument_texture_count /
		// argument_sampler_count).  BLIT_VALUES_SLOT is replaced with the constant so
		// the shader and this file cannot drift apart.
		constexpr u32 blit_values_slot = 0;
		constexpr u32 blit_source_slot = 0;
		constexpr u32 blit_sampler_slot = 0;

		// One triangle over the clip square, so no vertex buffer is bound.  `corner`
		// spans [0, 2]: the visible clip square is the [0, 1] part of it, which makes
		// the varying a plain fraction of the destination rectangle.  Texture v grows
		// downward while clip y grows upward, so the v term is (1 - corner.y).
		constexpr const char* blit_msl = R"(#include <metal_stdlib>
using namespace metal;

struct blit_values
{
    float4 src_uv;
};

struct blit_vertex_output
{
    float4 position [[position]];
    float2 uv;
};

vertex blit_vertex_output blit_vertex(uint vertex_id [[vertex_id]],
    constant blit_values& values [[buffer(BLIT_VALUES_SLOT)]])
{
    const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    blit_vertex_output out;
    out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    out.uv = values.src_uv.xy + float2(corner.x, 1.0 - corner.y) * values.src_uv.zw;
    return out;
}

fragment float4 blit_fragment(blit_vertex_output in [[stage_in]],
    texture2d<float> source [[texture(BLIT_SOURCE_SLOT)]],
    sampler filter [[sampler(BLIT_SAMPLER_SLOT)]])
{
    return source.sample(filter, in.uv);
}
)";

		std::string build_blit_source()
		{
			std::string source = blit_msl;

			const auto replace = [&source](const char* token, u32 value)
			{
				const std::string name = token;
				const std::string text = std::to_string(value);

				for (usz pos = source.find(name); pos != std::string::npos; pos = source.find(name, pos + text.size()))
				{
					source.replace(pos, name.size(), text);
				}
			};

			replace("BLIT_VALUES_SLOT", blit_values_slot);
			replace("BLIT_SOURCE_SLOT", blit_source_slot);
			replace("BLIT_SAMPLER_SLOT", blit_sampler_slot);
			return source;
		}
	}

	void blit_pass::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_compiler = dev.compiler();
		m_resources = &resources;

		if (!m_device || !m_compiler)
		{
			return;
		}

		const std::string source = build_blit_source();
		MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
		descriptor->setSource(NS::String::string(source.c_str(), NS::UTF8StringEncoding));

		NS::Error* error = nullptr;
		m_library = m_compiler->newLibrary(descriptor, &error);
		descriptor->release();

		if (!m_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: blit library creation failed: %s", message);
			return;
		}

		// The copy writes no depth and the pass has no depth attachment; the state
		// is set anyway so nothing it inherits can reject a fragment.
		m_depth_off = create_depth_stencil_state(m_device, depth_stencil_state{});

		// A scaled copy must not blend in texels outside the source rectangle, so
		// both filters clamp.  The mip filter is off: the source of a transfer is a
		// slice of guest memory and level 0 is the whole image.
		const auto make_sampler = [this](MTL::SamplerMinMagFilter filter)
		{
			MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
			descriptor->setMinFilter(filter);
			descriptor->setMagFilter(filter);
			descriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
			descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
			descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
			descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);

			MTL::SamplerState* state = m_device->newSamplerState(descriptor);
			descriptor->release();
			return state;
		};

		m_nearest = make_sampler(MTL::SamplerMinMagFilterNearest);
		m_linear = make_sampler(MTL::SamplerMinMagFilterLinear);

		if (!m_nearest || !m_linear)
		{
			rsx_log.error("Metal: blit sampler creation failed");
		}
	}

	void blit_pass::shutdown()
	{
		for (const auto& [key, state] : m_pipelines)
		{
			state->release();
		}

		m_pipelines.clear();

		if (m_nearest)
		{
			m_nearest->release();
			m_nearest = nullptr;
		}

		if (m_linear)
		{
			m_linear->release();
			m_linear = nullptr;
		}

		if (m_depth_off)
		{
			m_depth_off->release();
			m_depth_off = nullptr;
		}

		if (m_library)
		{
			m_library->release();
			m_library = nullptr;
		}

		m_resources = nullptr;
		m_compiler = nullptr;
		m_device = nullptr;
	}

	MTL::SamplerState* blit_pass::sampler(bool interpolate)
	{
		return interpolate ? m_linear : m_nearest;
	}

	MTL::RenderPipelineState* blit_pass::pipeline(MTL::PixelFormat color_format)
	{
		// The pass is single-sampled by construction (the caller declines anything
		// else), so the format is the whole key.
		const u64 key = static_cast<u64>(color_format);

		if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
		{
			return it->second;
		}

		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(m_library);
		vertex_function->setName(NS::String::string("blit_vertex", NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(m_library);
		fragment_function->setName(NS::String::string("blit_fragment", NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);
		descriptor->setRasterSampleCount(1);

		MTL4::RenderPipelineColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
		attachment->setPixelFormat(color_format);
		attachment->setWriteMask(MTL::ColorWriteMaskAll);

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = m_compiler->newRenderPipelineState(descriptor, nullptr, &error);

		vertex_function->release();
		fragment_function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: blit pipeline creation failed for format %u: %s", static_cast<u32>(color_format), message);
			return nullptr;
		}

		m_pipelines.emplace(key, state);
		return state;
	}

	void blit_pass::scale_image(command_recorder& cmd, MTL::GPUAddress values,
		MTL::Texture* src, MTL::Texture* dst, const areai& dst_area, bool interpolate,
		u32 level, u32 slice, u32 depth_plane)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "blit/scale image");
		if (!m_library || !src || !dst || !values)
		{
			return;
		}

		MTL4::ArgumentTable* table = cmd.blit_table();
		if (!table)
		{
			return;
		}

		// The pipeline is a single-sample colour one, and a texture copy cannot read
		// a 3D slice through a 2D sampler without a view, so anything else stays with
		// the caller (the gate declined what it could see).
		if (dst->sampleCount() != 1 || src->sampleCount() != 1)
		{
			rsx_log.error("Metal 4: blit declined a multisample transfer");
			return;
		}

		if (src->textureType() != MTL::TextureType2D || src->arrayLength() != 1)
		{
			rsx_log.error("Metal 4: blit declined a source that is not a single 2D image");
			return;
		}

		// A depth image has no colour view (probe G), so it cannot be the attachment
		// of a colour pass.  The upload gate already declined depth on both sides;
		// this covers the texture-cache route, which calls the pass directly.
		if (is_depth_format(dst->pixelFormat()))
		{
			rsx_log.error("Metal 4: blit declined a depth destination (format %u)",
				static_cast<u32>(dst->pixelFormat()));
			return;
		}

		const s32 dst_x1 = std::max(dst_area.x1, dst_area.x2);
		const s32 dst_y1 = std::max(dst_area.y1, dst_area.y2);

		if (dst_x1 <= 0 || dst_y1 <= 0)
		{
			rsx_log.error("Metal 4: blit request consists of an empty region descriptor");
			return;
		}

		// The render target is anchored at the attachment's top-left corner, and
		// renderTargetWidth/Height only "constrain the render target to" a size
		// (MTLRenderPass.h): they do not move its origin.  The viewport and scissor
		// below stay at (x0, y0), so the constrained area has to span the far corner
		// of the written rectangle; a w x h area would place that rectangle outside
		// the render target whenever the destination offset is non-zero.
		const u32 width = static_cast<u32>(dst_x1);
		const u32 height = static_cast<u32>(dst_y1);

		const s32 x0 = std::min(dst_area.x1, dst_area.x2);
		const s32 y0 = std::min(dst_area.y1, dst_area.y2);
		const s32 w = dst_x1 - x0;
		const s32 h = dst_y1 - y0;

		MTL::RenderPipelineState* state = pipeline(dst->pixelFormat());
		MTL::SamplerState* filter = sampler(interpolate);
		if (!state || !filter || !m_depth_off)
		{
			return;
		}

		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
		// The render area only has to cover the written rectangle: the attachment
		// loads and stores exactly the area of the pass, so a small transfer into a
		// large image does not touch the rest of it.
		descriptor->setRenderTargetWidth(width);
		descriptor->setRenderTargetHeight(height);
		descriptor->setDefaultRasterSampleCount(1);

		MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
		attachment->setTexture(dst);
		attachment->setLevel(level);
		attachment->setSlice(slice);
		attachment->setDepthPlane(depth_plane);
		attachment->setLoadAction(MTL::LoadActionLoad);
		attachment->setStoreAction(MTL::StoreActionStore);

		MTL4::RenderCommandEncoder* encoder = cmd.open_pass(descriptor);
		descriptor->release();

		if (!encoder)
		{
			return;
		}

		MTL::Viewport viewport{};
		viewport.originX = static_cast<double>(x0);
		viewport.originY = static_cast<double>(y0);
		viewport.width = static_cast<double>(w);
		viewport.height = static_cast<double>(h);
		viewport.znear = 0.0;
		viewport.zfar = 1.0;
		encoder->setViewport(viewport);

		MTL::ScissorRect scissor{};
		scissor.x = static_cast<NS::UInteger>(x0);
		scissor.y = static_cast<NS::UInteger>(y0);
		scissor.width = static_cast<NS::UInteger>(w);
		scissor.height = static_cast<NS::UInteger>(h);
		encoder->setScissorRect(scissor);

		encoder->setRenderPipelineState(state);
		encoder->setDepthStencilState(m_depth_off);
		encoder->setCullMode(MTL::CullModeNone);
		encoder->setTriangleFillMode(MTL::TriangleFillModeFill);

		// The vertex stage reads the values and the fragment stage reads the image
		// and its sampler, and both entries live in the one blit table.
		table->setAddress(values, blit_values_slot);
		table->setTexture(src->gpuResourceID(), blit_source_slot);
		table->setSamplerState(filter->gpuResourceID(), blit_sampler_slot);

		encoder->setArgumentTable(table, MTL::RenderStageVertex);
		encoder->setArgumentTable(table, MTL::RenderStageFragment);

		encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, 0, 3);
		cmd.close_encoder();
	}

	bool blitter::supports_transfer(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst,
		u8 src_surface_bpp, u8 dst_surface_bpp, bool src_is_depth, bool dst_is_depth,
		bool is_copy_op, bool is_format_convert, bool is_graphics2d,
		bool src_is_tiled, bool dst_is_tiled, bool use_null_region) const
	{
		// Without a pass there is no GPU route at all (init failed, or the backend
		// is shutting down).
		if (!m_pass)
		{
			return false;
		}

		// A depth aspect has no colour view on Metal: a float depth image can be
		// sampled, but a depth surface cannot also be a colour attachment, and no
		// view maps a depth format onto a colour one (probe G).
		if (src_is_depth || dst_is_depth)
		{
			return false;
		}

		// The surface's storage granularity differs from the transfer's texel grid.
		// The shared code resolves that by reinterpreting the surface as the transfer
		// format (configure_typeless_format); a sampling pass would decode the wrong
		// channels, and there is no host view that changes a texel's size.
		if (src_surface_bpp && src_surface_bpp != src.bpp)
		{
			return false;
		}

		if (dst_surface_bpp && dst_surface_bpp != dst.bpp)
		{
			return false;
		}

		// A tiled range has no surface-store allocation to sample or to render into.
		if (src_is_tiled || dst_is_tiled)
		{
			return false;
		}

		// A guest-memory mirror (a main-memory copy, or a write overlapping a
		// display buffer) needs a DMA section: it holds guest memory rather than a
		// host image, and this backend has none because Metal 4 has no blit encoder
		// to fill one.  The shared code owns that decision, so it is declined here
		// before the cache is touched; the CPU fallback is byte-exact.
		if (use_null_region)
		{
			return false;
		}

		(void)is_copy_op;
		(void)is_format_convert;
		(void)is_graphics2d;
		return true;
	}

	void blitter::scale_image(command_recorder& cmd, MTL::Texture* src, MTL::Texture* dst,
		areai src_area, areai dst_area, bool interpolate, u32 level, u32 slice, u32 depth_plane) const
	{
		if (!m_pass || !m_constants || !src || !dst)
		{
			return;
		}

		// Validate before any normalisation, so a flipped rectangle is checked
		// against its real extent rather than its signed one.
		const s32 src_x0 = std::min(src_area.x1, src_area.x2);
		const s32 src_x1 = std::max(src_area.x1, src_area.x2);
		const s32 src_y0 = std::min(src_area.y1, src_area.y2);
		const s32 src_y1 = std::max(src_area.y1, src_area.y2);
		const s32 dst_x0 = std::min(dst_area.x1, dst_area.x2);
		const s32 dst_x1 = std::max(dst_area.x1, dst_area.x2);
		const s32 dst_y0 = std::min(dst_area.y1, dst_area.y2);
		const s32 dst_y1 = std::max(dst_area.y1, dst_area.y2);

		const s32 src_width = static_cast<s32>(src->width());
		const s32 src_height = static_cast<s32>(src->height());

		// The pass renders level 0 of the texture object it is handed, so the
		// destination rectangle and the subresource are checked against *that* level:
		// a mip is smaller than the base level and a volume slice is one plane of it.
		// The guest level comes from a u8, so the shift is clamped.
		if (level >= dst->mipmapLevelCount())
		{
			rsx_log.error("Metal 4: blit request names mip level %u of a %llu-level image",
				level, static_cast<unsigned long long>(dst->mipmapLevelCount()));
			return;
		}

		const u32 level_shift = std::min<u32>(level, 31u);
		const s32 dst_width = static_cast<s32>(std::max<u32>(1u, static_cast<u32>(dst->width()) >> level_shift));
		const s32 dst_height = static_cast<s32>(std::max<u32>(1u, static_cast<u32>(dst->height()) >> level_shift));

		const bool dst_3d = dst->textureType() == MTL::TextureType3D;
		const u32 dst_planes = dst_3d
			? std::max<u32>(static_cast<u32>(dst->depth()) >> level_shift, 1u)
			: static_cast<u32>(dst->arrayLength());

		if ((dst_3d ? depth_plane : slice) >= dst_planes)
		{
			rsx_log.error("Metal 4: blit request names %s %u of an image with %u",
				dst_3d ? "depth plane" : "array slice", dst_3d ? depth_plane : slice, dst_planes);
			return;
		}

		if (src_x0 < 0 || src_x1 > src_width || src_y0 < 0 || src_y1 > src_height)
		{
			rsx_log.error("Metal 4: blit request denied because the source region does not fit");
			return;
		}

		if (dst_x0 < 0 || dst_x1 > dst_width || dst_y0 < 0 || dst_y1 > dst_height)
		{
			rsx_log.error("Metal 4: blit request denied because the destination region does not fit");
			return;
		}

		if (src_x1 <= src_x0 || src_y1 <= src_y0 || dst_x1 <= dst_x0 || dst_y1 <= dst_y0)
		{
			rsx_log.error("Metal 4: blit request consists of an empty region descriptor");
			return;
		}

		// Only the source rectangle carries a direction (the shared code folds the
		// guest flip into it); a reversed destination would mirror the image twice.
		if (dst_area.x2 < dst_area.x1 || dst_area.y2 < dst_area.y1)
		{
			rsx_log.error("Metal 4: blit request has a reversed destination rectangle");
			return;
		}

		const f32 inv_src_w = 1.0f / static_cast<f32>(std::max(src_width, 1));
		const f32 inv_src_h = 1.0f / static_cast<f32>(std::max(src_height, 1));

		const f32 origin_u = static_cast<f32>(src_area.x1) * inv_src_w;
		const f32 origin_v = static_cast<f32>(src_area.y1) * inv_src_h;
		const f32 extent_u = static_cast<f32>(src_area.x2 - src_area.x1) * inv_src_w;
		const f32 extent_v = static_cast<f32>(src_area.y2 - src_area.y1) * inv_src_h;

		const MTL::GPUAddress values = m_constants->alloc_blit_values(origin_u, origin_v, extent_u, extent_v);
		if (!values)
		{
			rsx_log.error("Metal 4: blit value ring exhausted");
			return;
		}

		m_pass->scale_image(cmd, values, src, dst, dst_area, interpolate, level, slice, depth_plane);
	}

	void blitter::scale_image(command_recorder& cmd, texture* src, texture* dst,
		areai src_area, areai dst_area, bool interpolate, const rsx::typeless_xfer& xfer) const
	{
		if (!src || !dst)
		{
			return;
		}

		// The shared code reports a flip out of band because the guest descriptor
		// stores a direction, not a rectangle.  Folding it into the rectangle here
		// makes the extent signed, which is what the shader wants.
		if (xfer.flip_horizontal)
		{
			src_area.flip_horizontal();
		}

		if (xfer.flip_vertical)
		{
			src_area.flip_vertical();
		}

		// The copy moves guest channels.  A source texture stores the guest bits
		// unchanged behind a Metal format whose field order differs for the packed
		// formats (MTLFormats.cpp, get_texture_component_layout), so only the
		// guest-channel view holds the channels the copy moves: the probe reads
		// guest red 0x001f as blue from the raw image and as red through the view
		// (EXPERIMENT-blit-channel-order.mm B/C).  The destination stays raw
		// because Metal applies a swizzle on reads only (MTLTexture.h:250), and a
		// surface's raw fields already hold the guest channels.
		texture* source = src->get_view(rsx::default_remap_vector);

		if (!source || !source->value())
		{
			rsx_log.error("Metal 4: blit could not create the source channel view");
			return;
		}

		// A pass cannot sample the image it renders into: Metal treats the texture
		// as one resource, so a view of it is not a separate attachment.  In-place
		// scaling and blur blits are common, so stage the source region in a
		// private copy and sample that instead (T5; VKTexture.cpp:631).  The copy
		// keeps the raw guest bits, and the view above the copy applies the same
		// channel map as the original source.
		std::unique_ptr<texture> staging;

		if (src->image() == dst->image())
		{
			resource_manager* resources = m_pass ? m_pass->resources() : nullptr;

			if (!resources)
			{
				rsx_log.error("Metal 4: in-place blit has no resource manager");
				return;
			}

			MTL::Texture* src_image = src->value();

			// The pass samples one single-sample 2D image.  The checks in the pass
			// would reject anything else after the copy had already run, so reject
			// it here before allocating.
			if (src_image->sampleCount() != 1 ||
				src_image->textureType() != MTL::TextureType2D ||
				src_image->arrayLength() != 1)
			{
				rsx_log.error("Metal 4: in-place blit declined a source that is not a single 2D image");
				return;
			}

			const s32 sx0 = std::min(src_area.x1, src_area.x2);
			const s32 sx1 = std::max(src_area.x1, src_area.x2);
			const s32 sy0 = std::min(src_area.y1, src_area.y2);
			const s32 sy1 = std::max(src_area.y1, src_area.y2);

			if (sx0 < 0 || sy0 < 0 ||
				sx1 > static_cast<s32>(src_image->width()) ||
				sy1 > static_cast<s32>(src_image->height()))
			{
				rsx_log.error("Metal 4: in-place blit request does not fit the source image");
				return;
			}

			const u32 copy_width = static_cast<u32>(sx1 - sx0);
			const u32 copy_height = static_cast<u32>(sy1 - sy0);

			if (!copy_width || !copy_height)
			{
				rsx_log.error("Metal 4: in-place blit request consists of an empty region descriptor");
				return;
			}

			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::texture2DDescriptor(
				source->pixel_format(), copy_width, copy_height, false);
			descriptor->setUsage(MTL::TextureUsageShaderRead);

			MTL::Texture* image = resources->new_texture(descriptor, MTL::StorageModePrivate);
			descriptor->release();

			if (!image)
			{
				rsx_log.error("Metal 4: in-place blit could not allocate a staging texture");
				return;
			}

			resources->make_persistent(image);

			staging = std::make_unique<texture>(resources);
			staging->assign(image);
			staging->set_native_component_layout(src->image()->get_native_component_layout());

			MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();

			if (!encoder)
			{
				rsx_log.error("Metal 4: in-place blit could not open a copy encoder");
				return;
			}

			// Copy the normalised region, so the rebased rectangle below can keep
			// the direction of the transfer in its signed extent.
			encoder->copyFromTexture(src_image, 0, 0,
				MTL::Origin::Make(sx0, sy0, 0),
				MTL::Size::Make(copy_width, copy_height, 1),
				image, 0, 0, MTL::Origin::Make(0, 0, 0));

			texture* staged = staging->get_view(rsx::default_remap_vector);

			if (!staged || !staged->value())
			{
				rsx_log.error("Metal 4: in-place blit could not create the staging view");
				return;
			}

			const s32 width = static_cast<s32>(copy_width);
			const s32 height = static_cast<s32>(copy_height);
			const areai staged_area =
			{
				src_area.x1 <= src_area.x2 ? 0 : width,
				src_area.y1 <= src_area.y2 ? 0 : height,
				src_area.x1 <= src_area.x2 ? width : 0,
				src_area.y1 <= src_area.y2 ? height : 0
			};

			scale_image(cmd, staged->value(), dst->value(), staged_area, dst_area, interpolate);
			return;
		}

		scale_image(cmd, source->value(), dst->value(), src_area, dst_area, interpolate);
	}
}

#endif // RSX_ENABLE_METAL4
