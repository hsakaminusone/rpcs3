#include "MTLRasterState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"

namespace mtl
{
	raster_state capture_raster_state()
	{
		raster_state state;

		state.cull_face = rsx::method_registers.cull_face_mode();
		state.cull_enabled = rsx::method_registers.cull_face_enabled();
		state.front_face = rsx::method_registers.front_face_mode();
		state.front_polygon_mode = rsx::method_registers.polygon_mode_front();
		state.back_polygon_mode = rsx::method_registers.polygon_mode_back();

		state.poly_offset_fill = rsx::method_registers.poly_offset_fill_enabled();
		state.poly_offset_scale = rsx::method_registers.poly_offset_scale();
		state.poly_offset_bias = rsx::method_registers.poly_offset_bias();

		state.clip_min = rsx::method_registers.clip_min();
		state.clip_max = rsx::method_registers.clip_max();
		state.point_size = rsx::method_registers.point_size();
		state.line_width = rsx::method_registers.line_width();
		state.line_smooth = rsx::method_registers.line_smooth_enabled();
		state.user_clip_control = rsx::method_registers.clip_planes_mask();

		state.stipple_enabled = rsx::method_registers.polygon_stipple_enabled();
		state.stipple_pattern = rsx::method_registers.polygon_stipple_pattern();

		state.point_sprite_enabled = rsx::method_registers.point_sprite_enabled();
		state.point_sprite_mask = rsx::method_registers.point_sprite_control_mask();

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
		return state.front_face == rsx::front_face::cw ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
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

	MTL::Viewport build_viewport(u16 clip_width, u16 clip_height, f64 znear, f64 zfar)
	{
		return MTL::Viewport{ 0.0, 0.0, static_cast<f64>(clip_width), static_cast<f64>(clip_height), znear, zfar };
	}

	MTL::ScissorRect build_scissor_rect(u32 x1, u32 y1, u32 x2, u32 y2)
	{
		return MTL::ScissorRect{ x1, y1, x2 - x1, y2 - y1 };
	}
}

#endif // RSX_ENABLE_METAL4
