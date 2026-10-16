#include "MTLVertexState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"

namespace mtl
{
	vertex_state capture_vertex_state()
	{
		vertex_state state;

		state.input_mask = rsx::method_registers.vertex_attrib_input_mask();
		state.output_mask = rsx::method_registers.vertex_attrib_output_mask();
		state.program_start = rsx::method_registers.transform_program_start();
		state.branch_bits = rsx::method_registers.transform_branch_bits();
		state.srgb_output = rsx::method_registers.framebuffer_srgb_enabled();

		return state;
	}

	bool attribute_sourced(u32 index)
	{
		return rsx::method_registers.vertex_arrays_info[index].size() > 0 ||
			rsx::method_registers.register_vertex_info[index].size > 0;
	}
}

#endif // RSX_ENABLE_METAL4
