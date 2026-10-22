#include "MTLEncoderState.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	encoder_state capture_encoder_state(bool emulate_depth_range)
	{
		// One latch per cluster; the helpers consume it instead of re-reading the
		// register file.
		const raster_state raster = capture_raster_state();
		const surface_state surface = capture_surface_state();

		encoder_state state;
		state.cull_mode = capture_cull_mode(raster);
		state.winding = capture_front_face(raster);
		state.fill_mode = capture_fill_mode(raster);

		state.depth_clip_mode = capture_depth_clip_mode();

		// The helper writes both out-params; keep the defaults when the offset is
		// off (LLD-raster §3.4, invariant 6).
		f32 depth_bias = 0.0f;
		f32 depth_slope_scale = 0.0f;
		if (capture_depth_bias(raster, depth_bias, depth_slope_scale))
		{
			state.depth_bias = depth_bias;
			state.depth_slope_scale = depth_slope_scale;
		}

		const stencil_references references = capture_stencil_references();
		state.stencil_front = references.front;
		state.stencil_back = references.back;

		const depth_bounds bounds = capture_depth_bounds(emulate_depth_range);
		state.depth_bounds_min = bounds.min;
		state.depth_bounds_max = bounds.max;

		// The helper writes znear/zfar only when the range is inside [0,1], so the
		// defaults (0.0, 1.0) survive the false case.
		state.has_clip_range = capture_clip_range(raster, state.znear, state.zfar);

		state.blend_color = capture_blend_color();

		state.clip_width = surface.clip_width;
		state.clip_height = surface.clip_height;

		state.depth_stencil = capture_depth_stencil_state();

		return state;
	}

	void apply_encoder_state(MTL4::RenderCommandEncoder* encoder, const encoder_state& state, const MTL::ScissorRect& scissor, depth_stencil_cache& depth_cache)
	{
		if (encoder == nullptr)
		{
			return;
		}

		apply_depth_stencil_state(encoder, depth_cache, state.depth_stencil);

		encoder->setCullMode(state.cull_mode);
		encoder->setFrontFacingWinding(state.winding);
		encoder->setTriangleFillMode(state.fill_mode);
		encoder->setDepthClipMode(state.depth_clip_mode);

		// The RSX depth-bounds test has no usable native path on this driver.  The
		// encoder setter reaches -[AGXG17FamilyRenderContext_mtlnext
		// setDepthTestMinBound:maxBound:] and traps there (EXC_BREAKPOINT, SIGTRAP;
		// crash report 2026-10-18 01:07, frame 0), which is the design's position:
		// LLD-depth-rop §3.4 maps the test to a fragment-shader discard fed by
		// fragment constants and §4 records the drop as the bring-up fallback, the
		// same fallback the VK path takes on a device without support
		// (VKPipelineCompiler.cpp:118-125).  The captured bounds stay in the state,
		// unread until that epilogue lands.

		// The RSX has no depth-bias clamp term (LLD-raster §3.4).
		encoder->setDepthBias(state.depth_bias, state.depth_slope_scale, 0.0f);

		encoder->setStencilReferenceValues(state.stencil_front, state.stencil_back);

		encoder->setBlendColor(state.blend_color[0], state.blend_color[1], state.blend_color[2], state.blend_color[3]);

		// The viewport rectangle is the surface clip; the RSX viewport transform
		// lives in the vertex env (LLD-raster §3.1, invariant 1), and the rectangle
		// carries the one clip-space Y flip (invariant 2).
		const f64 znear = state.has_clip_range ? state.znear : 0.0;
		const f64 zfar = state.has_clip_range ? state.zfar : 1.0;
		encoder->setViewport(build_viewport(state.clip_width, state.clip_height, znear, zfar));

		encoder->setScissorRect(scissor);
	}
}

#endif // RSX_ENABLE_METAL4
