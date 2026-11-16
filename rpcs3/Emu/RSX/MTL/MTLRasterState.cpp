#include "MTLRasterState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Core/RSXContext.h"
#include "Emu/RSX/Core/RSXFrameBuffer.h"
#include "Emu/RSX/rsx_methods.h"

#include <cstring>

namespace mtl
{
	raster_state capture_raster_state(const rsx::context& ctx)
	{
		const rsx::raster_state& raster = ctx.draw_state.raster;
		raster_state state;

		state.cull_face = raster.cull_face_mode;
		state.cull_enabled = raster.cull_face_enabled;
		state.front_face = raster.front_face_mode;
		state.front_polygon_mode = raster.polygon_mode_front;
		state.back_polygon_mode = raster.polygon_mode_back;

		state.poly_offset_fill = raster.poly_offset_fill_enabled;
		state.poly_offset_scale = raster.poly_offset_scale;
		state.poly_offset_bias = raster.poly_offset_bias;

		// The clip range is decoded as part of the viewport group, not the raster
		// group; it is the only decoded source of the clip min/max.
		state.clip_min = ctx.draw_state.viewport.z_clip_min;
		state.clip_max = ctx.draw_state.viewport.z_clip_max;
		state.point_size = ctx.register_state->point_size();
		state.line_width = raster.line_width;
		state.line_smooth = raster.line_smooth_enabled;
		state.user_clip_control = ctx.register_state->clip_planes_mask();

		// The pattern is only decoded while stipple is enabled; copy it only then.
		state.stipple_enabled = ctx.draw_state.stipple.enabled;
		if (state.stipple_enabled)
		{
			std::memcpy(state.stipple_pattern.data(), ctx.draw_state.stipple.pattern.data(), sizeof(state.stipple_pattern));
		}

		state.point_sprite_enabled = ctx.register_state->point_sprite_enabled();
		state.point_sprite_mask = ctx.register_state->point_sprite_control_mask();

		return state;
	}

	MTL::CullMode capture_cull_mode(const raster_state& state)
	{
		if (!state.cull_enabled)
		{
			return MTL::CullModeNone;
		}

		switch (state.cull_face)
		{
		case rsx::cull_face::front:
			return MTL::CullModeFront;
		case rsx::cull_face::back:
			return MTL::CullModeBack;
		case rsx::cull_face::front_and_back:
			break;
		}

		return MTL::CullModeNone;
	}

	bool culls_both_faces(const raster_state& state)
	{
		return state.cull_enabled && state.cull_face == rsx::cull_face::front_and_back;
	}

	MTL::Winding capture_front_face(const raster_state& state)
	{
		// The guest rule is stated in screen space, where y grows downward.  The
		// negated viewport height already lands that space in Metal's framebuffer
		// coordinates, which also grow downward, so the mirror is accounted for by
		// the viewport alone and the winding maps straight across.  This is the
		// Vulkan backend's arrangement too: a positive rectangle and a direct
		// mapping (VKGSRender.cpp:205).  Inverting here as well flips the test a
		// second time and culls the quads the app draws with culling enabled.
		const bool clockwise = state.front_face == rsx::front_face::cw;

		return clockwise ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
	}

	MTL::TriangleFillMode capture_fill_mode(const raster_state& state)
	{
		const bool front_survives = !state.cull_enabled || state.cull_face != rsx::cull_face::front;
		const rsx::polygon_mode mode = front_survives ? state.front_polygon_mode : state.back_polygon_mode;

		// Metal has no point fill mode; draw the triangles.
		return mode == rsx::polygon_mode::line ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
	}

	bool capture_depth_bias(const raster_state& state, f32& bias, f32& slope_scale)
	{
		bias = state.poly_offset_bias;
		slope_scale = state.poly_offset_scale;
		return state.poly_offset_fill;
	}

	bool capture_clip_range(const raster_state& state, f64& znear, f64& zfar)
	{
		if (state.clip_min < 0.0f || state.clip_max > 1.0f || state.clip_max <= state.clip_min)
		{
			return false;
		}

		znear = state.clip_min;
		zfar = state.clip_max;
		return true;
	}

	areau scaled_scissor(const rsx::scissor_state& region, const rsx::framebuffer_layout& layout,
		const rsx::surface_scaling_config_t& scaling)
	{
		areau scissor;
		std::tie(scissor.x1, scissor.y1) = rsx::apply_resolution_scale<false>(scaling, region.x1, region.y1, layout.width, layout.height);
		std::tie(scissor.x2, scissor.y2) = rsx::apply_resolution_scale<true>(scaling, region.x2, region.y2, layout.width, layout.height);
		return scissor;
	}

	MTL::Viewport build_viewport(u16 clip_width, u16 clip_height, f64 znear, f64 zfar)
	{
		// The rectangle keeps its top edge and its height is negated, because the
		// translated clip space is y-down and Metal's NDC is y-up
		// (kk_cmd_draw.c:785-793).  The present and overlay passes carry the same
		// flip for the same reason (MTLOverlays.cpp:381).
		const f64 height = static_cast<f64>(clip_height);

		return MTL::Viewport{ 0.0, height, static_cast<f64>(clip_width), -height, znear, zfar };
	}

	MTL::ScissorRect build_scissor_rect(u32 x1, u32 y1, u32 x2, u32 y2, u16 clip_height)
	{
		// The geometry lands mirrored about the surface, so a region given in guest
		// window coordinates lands at the mirrored target row.  It keeps its width
		// and its height.
		const u32 y = static_cast<u32>(clip_height) - y2;

		return MTL::ScissorRect{ x1, y, x2 - x1, y2 - y1 };
	}
}

#endif // RSX_ENABLE_METAL4
