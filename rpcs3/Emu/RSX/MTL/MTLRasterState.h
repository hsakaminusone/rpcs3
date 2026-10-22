#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

#include <array>

namespace mtl
{
	// The latched `raster` cluster state (LLD-raster §2).  The latch is read once per
	// draw; the derivations below consume it instead of re-reading the register file.
	//
	// The scissor and the Metal viewport rectangle are deliberately absent.
	// rsx::thread::get_scissor owns the scissor (the viewport-rect intersection, the
	// window clamp and the invalid case; RSXThread.cpp:2115-2146), and the Metal
	// viewport is the surface clip, not the RSX viewport rect (LLD-raster §3.1,
	// invariant 1).  The RSX viewport rect only clips the scissor.
	struct raster_state
	{
		rsx::cull_face cull_face = rsx::cull_face::back;
		bool cull_enabled = false;
		rsx::front_face front_face = rsx::front_face::ccw;
		rsx::polygon_mode front_polygon_mode = rsx::polygon_mode::fill;
		rsx::polygon_mode back_polygon_mode = rsx::polygon_mode::fill;

		bool poly_offset_fill = false;
		f32 poly_offset_scale = 0.0f;
		f32 poly_offset_bias = 0.0f;

		// Vertex env (LLD-raster §2.2, §2.5, §2.7).
		f32 clip_min = 0.0f;
		f32 clip_max = 1.0f;

		// Captured for the dial harness only (REVIEW-FINDINGS.md E13): no path in the
		// backend consumes these, because the real consumer reads the register file at
		// the point of use — point size reaches the shader through
		// `vertex_context::point_size` (MTLGSRender.cpp:1007, MTLConstants.h:23).  They
		// stay so the harness can assert the decode; pruning them is a harness
		// decision, recorded in the E13 entry.
		f32 point_size = 1.0f;
		f32 line_width = 1.0f;
		bool line_smooth = false;
		u32 user_clip_control = 0;

		// Fragment program state (LLD-raster §2.6, §2.7).
		bool stipple_enabled = false;

		// The RSX polygon stipple pattern is a 32-word register array
		// (rsx_methods.h:1349, stipple_block_count = 8 blocks of four words).  The
		// latch copies it rather than holding a guest pointer into register space, so a
		// consumer that holds the latch across a register write cannot read retired
		// data (E13); every other field of this struct copies its value.  The stipple
		// heap copies from register space at the point of use, not from here
		// (MTLGSRender.cpp:1013).
		static constexpr u32 stipple_word_count = 32;
		std::array<u32, stipple_word_count> stipple_pattern{};

		// Captured for the dial harness only (E13), as with the vertex-env fields
		// above.
		bool point_sprite_enabled = false;
		u16 point_sprite_mask = 0;
	};

	raster_state capture_raster_state();

	// SET_CULL_FACE / SET_CULL_FACE_ENABLE -> MTL::CullMode (LLD-raster §3.3).
	// Metal has no cull-both mode, so front_and_back culls nothing here and
	// culls_both_faces() tells the pipeline key to drop the raster (Q6).
	MTL::CullMode capture_cull_mode(const raster_state& state);
	bool culls_both_faces(const raster_state& state);

	// The clip-space Y flip of the RSX draw path.  The RSX and the translated
	// shaders emit clip space whose NDC Y points down and Metal's points up, so
	// the viewport, the scissor and the winding all carry that one flip, as the
	// present passes already do (MTLOverlays.cpp:381, kk_cmd_draw.c:785-793).
	// SET_FRONT_FACE -> MTL::Winding (LLD-raster §3.3).  The viewport negates its
	// height, so the winding is reversed (invariant 2).
	MTL::Winding capture_front_face(const raster_state& state);

	// SET_FRONT_POLYGON_MODE / SET_BACK_POLYGON_MODE -> MTL::TriangleFillMode
	// (LLD-raster §3.3).  Metal has one fill mode per encoder, so the pair collapses
	// to the mode of the face that survives culling.
	MTL::TriangleFillMode capture_fill_mode(const raster_state& state);

	// SET_POLY_OFFSET_FILL_ENABLE + SET_POLYGON_OFFSET_SCALE_FACTOR / _BIAS ->
	// setDepthBias(depthBias:slopeScale:clamp:) (LLD-raster §3.4).  False when the
	// offset is off; the point and line enables have no Metal path (§4).
	bool capture_depth_bias(const raster_state& state, f32& bias, f32& slope_scale);

	// SET_CLIP_MIN / SET_CLIP_MAX -> MTLViewport.znear/zfar (LLD-raster §3.2).
	// False when the range leaves [0,1]: the viewport depth range is clamped, so the
	// vertex shader owns those values (invariant 4).
	bool capture_clip_range(const raster_state& state, f64& znear, f64& zfar);

	// The MTLViewport rectangle: the surface clip, never the RSX viewport rect, and
	// the one clip-space Y flip the backend carries (LLD-raster §3.1, invariants 1
	// and 2).
	MTL::Viewport build_viewport(u16 clip_width, u16 clip_height, f64 znear, f64 zfar);

	// The MTLScissorRect from the resolved region (LLD-raster §3.1).  The region
	// comes from rsx::thread::get_scissor, which owns the viewport-rect intersection,
	// the window clamp and the invalid case.  It is in guest window coordinates, so
	// the viewport flip moves it to the mirrored target row.
	MTL::ScissorRect build_scissor_rect(u32 x1, u32 y1, u32 x2, u32 y2, u16 clip_height);
}

#endif // RSX_ENABLE_METAL4
