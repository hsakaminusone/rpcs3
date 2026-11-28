#pragma once

#include "MTLCommandRecorder.h"
#include "MTLHandles.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Utils/rsx_utils.h"

#include <unordered_map>

namespace mtl
{
	class constants_heap;

	// The built-in scaled copy.  Metal has no 2D engine and Metal 4 has no blit
	// encoder, so a guest transfer that scales, flips or changes width runs as one
	// full-screen draw: the source is sampled with a nearest or linear filter and
	// the destination is the pass's colour attachment.  It is the Metal form of
	// vkCmdBlitImage, and it follows fill_pass for construction, library creation
	// and argument-table use (MTLFill.cpp).
	//
	// The source rectangle reaches the shader as one `blit_values` block, not as
	// setBytes (Metal 4 has none).  The block holds the normalised origin and the
	// *signed* extent, so a flip is a negative extent and the shader has no branch.
	class blit_pass
	{
	public:
		void init(const device& dev, resource_manager& resources);
		void shutdown();

		// Draw `src` onto `dst_area` of `dst`, which becomes the colour attachment
		// of a fresh pass.  The source rectangle arrives in `values` (the normalised
		// origin and signed extent the shader samples), so the pass itself only needs
		// the destination rectangle.  `level`/`slice`/`depth_plane` select the
		// destination subresource: a pass may render a mip level, an array slice or a
		// volume slice directly, so no texture view is needed.  Texels outside
		// `dst_area` are loaded and stored unchanged.
		void scale_image(command_recorder& cmd, MTL::GPUAddress values,
			MTL::Texture* src, MTL::Texture* dst, const areai& dst_area, bool interpolate,
			u32 level = 0, u32 slice = 0, u32 depth_plane = 0);

		// The frame's resource manager, so an in-place blit can allocate the
		// staging image that breaks the read-write hazard (T5).
		resource_manager* resources() const { return m_resources; }

	private:
		MTL::RenderPipelineState* pipeline(MTL::PixelFormat color_format);
		MTL::SamplerState* sampler(bool interpolate);

		MTL::Device* m_device = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		MTL4::CompilerTaskOptions* m_options = nullptr;
		resource_manager* m_resources = nullptr;
		MTL::Library* m_library = nullptr;
		MTL::DepthStencilState* m_depth_off = nullptr;
		MTL::SamplerState* m_nearest = nullptr;
		MTL::SamplerState* m_linear = nullptr;
		std::unordered_map<u64, MTL::RenderPipelineState*> m_pipelines;
	};

	// The value type the shared texture cache holds.  It carries the pass and the
	// frame ring the pass's values come from, so the shared template can call it
	// with nothing but the two images and the rectangles.
	class blitter
	{
	public:
		void bind(blit_pass* pass, constants_heap* constants)
		{
			m_pass = pass;
			m_constants = constants;
		}

		// The opt-in predicate of the shared upload_scaled_image.  It must not
		// mutate anything, because the caller is deciding whether to run its own
		// CPU fallback.  False means "decline": the transfer stays on the CPU path,
		// which is byte-exact.  `src_surface_bpp`/`dst_surface_bpp` are the guest
		// byte width of the matched surface, or zero when the endpoint is not one.
		// `use_null_region` is the shared code's guest-memory route (a main-memory
		// copy, or a write that overlaps a display buffer): it needs a DMA section,
		// and this backend has none (MTLBlit.cpp).
		bool supports_transfer(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst,
			u8 src_surface_bpp, u8 dst_surface_bpp, bool src_is_depth, bool dst_is_depth,
			bool is_copy_op, bool is_format_convert, bool is_graphics2d,
			bool src_is_tiled, bool dst_is_tiled, bool use_null_region) const;

		// The shared template's blit hook.  `xfer` carries the flips the shared
		// code folded out of the guest descriptor.
		void scale_image(command_recorder& cmd, texture* src, texture* dst,
			areai src_area, areai dst_area, bool interpolate, const rsx::typeless_xfer& xfer) const;

		// The same operation with the images as the cache holds them.  The texture
		// cache uses it for a transfer into a pooled temporary at a mip level, an
		// array slice or a volume slice.
		void scale_image(command_recorder& cmd, MTL::Texture* src, MTL::Texture* dst,
			areai src_area, areai dst_area, bool interpolate,
			u32 level = 0, u32 slice = 0, u32 depth_plane = 0) const;

	private:
		blit_pass* m_pass = nullptr;
		constants_heap* m_constants = nullptr;
	};
}

#endif // RSX_ENABLE_METAL4
