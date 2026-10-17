#pragma once

#include "MTLBlendState.h"
#include "MTLDepthStencil.h"
#include "MTLRasterState.h"
#include "MTLSurfaceState.h"

#ifdef RSX_ENABLE_METAL4

#include <array>

namespace mtl
{
	// The per-draw encoder state (HLD-05 §3.5, corrected for the Metal 4 object
	// model).  Metal 4 keeps every field here on MTL4::RenderCommandEncoder, so none
	// of it enters a cached object (CONFLICTS.md C7).
	//
	// HLD-05 §3.5 lists cull mode and front face as pipeline fields.  That is wrong
	// for Metal 4: MTL4::RenderPipelineDescriptor has no setCullMode, no
	// setFrontFacingWinding, no setTriangleFillMode and no depth/stencil attachment
	// format setters (unlike the older MTL::RenderPipelineDescriptor).  All of those
	// are encoder state in Metal 4, and this unit owns exactly that dynamic set:
	// setCullMode, setFrontFacingWinding, setTriangleFillMode, setDepthClipMode,
	// setDepthBias, setViewport, setScissorRect, setStencilReferenceValues and
	// setBlendColor.
	struct encoder_state
	{
		MTL::CullMode cull_mode = MTL::CullModeNone;
		MTL::Winding winding = MTL::WindingClockwise;
		MTL::TriangleFillMode fill_mode = MTL::TriangleFillModeFill;

		MTL::DepthClipMode depth_clip_mode = MTL::DepthClipModeClip;
		f32 depth_bias = 0.0f;
		f32 depth_slope_scale = 0.0f;

		// NV4097_SET_DEPTH_BOUNDS (MTLDepthStencil.h).  Dynamic encoder state, like
		// the clip mode.
		f32 depth_bounds_min = 0.0f;
		f32 depth_bounds_max = 1.0f;

		u8 stencil_front = 0;
		u8 stencil_back = 0;

		// The viewport depth range.  False when the range leaves [0,1]: the viewport
		// depth range is clamped, so the vertex shader owns those values
		// (LLD-raster invariant 4).
		bool has_clip_range = false;
		f64 znear = 0.0;
		f64 zfar = 1.0;

		std::array<f32, 4> blend_color{};

		// The attachment extent, which sets the viewport rectangle
		// (LLD-raster §3.1, invariant 1).
		u16 clip_width = 0;
		u16 clip_height = 0;

		// The depth-stencil latch.  Metal 4 reaches depth state only through
		// setDepthStencilState, so the latch is the key of a cached object and the
		// cache, not the encoder, owns the compare function and the write flag
		// (CONFLICTS.md C7).
		depth_stencil_state depth_stencil{};
	};

	// Capture the per-draw encoder state from the live registers.  It composes the
	// latches in MTLRasterState, MTLDepthStencil and MTLBlendState; it does not
	// re-read the register file.  `emulate_depth_range` is the draw's
	// RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE, which only the caller holds.
	encoder_state capture_encoder_state(bool emulate_depth_range);

	// Apply the state to the open encoder.  The scissor is the caller's: it comes
	// from rsx::thread::get_scissor, which owns the viewport-rect intersection, the
	// window clamp and the invalid case (MTLRasterState.h).
	void apply_encoder_state(MTL4::RenderCommandEncoder* encoder, const encoder_state& state, const MTL::ScissorRect& scissor, depth_stencil_cache& depth_cache);
}

#endif // RSX_ENABLE_METAL4
