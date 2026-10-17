#include "MTLDrawState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "util/logs.hpp"

namespace mtl
{
	draw_state capture_draw_state()
	{
		draw_state state;

		state.base_offset = rsx::method_registers.vertex_data_base_offset();
		state.base_index = rsx::method_registers.vertex_data_base_index();
		state.index_address = rsx::method_registers.index_array_address();
		state.index_location = rsx::method_registers.index_array_location();
		state.index_type = rsx::method_registers.index_type();
		state.restart_enabled = rsx::method_registers.restart_index_enabled();
		state.restart_index = rsx::method_registers.restart_index();

		return state;
	}

	MTL::PrimitiveType get_primitive_type(rsx::primitive_type mode)
	{
		switch (mode)
		{
		case rsx::primitive_type::points:
			return MTL::PrimitiveTypePoint;
		case rsx::primitive_type::lines:
			return MTL::PrimitiveTypeLine;
		case rsx::primitive_type::line_strip:
		case rsx::primitive_type::line_loop:
			// line_loop expands to a strip with one closing segment
			// (Common/BufferUtils.cpp:942-948).
			return MTL::PrimitiveTypeLineStrip;
		case rsx::primitive_type::triangles:
		case rsx::primitive_type::triangle_fan:
		case rsx::primitive_type::quads:
		case rsx::primitive_type::polygon:
			// A fan, a quad list and a polygon expand to a triangle list
			// (Common/BufferUtils.cpp:949-957).
			return MTL::PrimitiveTypeTriangle;
		case rsx::primitive_type::triangle_strip:
		case rsx::primitive_type::quad_strip:
			// A quad strip is native and uploads untouched
			// (Common/BufferUtils.cpp:823); CONFLICTS.md C5.
			return MTL::PrimitiveTypeTriangleStrip;
		default:
			rsx_log.error("Metal 4: unknown primitive type 0x%x", static_cast<u8>(mode));
			return MTL::PrimitiveTypeTriangle;
		}
	}

	MTL::IndexType get_index_type(rsx::index_array_type type)
	{
		switch (type)
		{
		case rsx::index_array_type::u16:
			return MTL::IndexTypeUInt16;
		case rsx::index_array_type::u32:
			return MTL::IndexTypeUInt32;
		}

		return MTL::IndexTypeUInt16;
	}
}

#endif // RSX_ENABLE_METAL4
