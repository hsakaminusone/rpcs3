#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	// The vertex-stage MTL4ArgumentTable index map (LLD-vertex §3.0).  The numbers are
	// the RSX shader ABI (inventory/out/rsx_binding_slots.csv).  The Metal backend
	// repeats them because the ABI is shared, not because it reuses the Vulkan slot
	// header (VK/vkutils/pipeline_binding_table.h).
	//
	// Slot 6 is the volatile half of the two-stream split (LLD-vertex §3.0); the
	// shared slot list stops at 5.
	enum vertex_slot : u32
	{
		vertex_context_slot = 0,        // vertex context params: branch bits, user clip, scale/offset
		vertex_constants_slot = 1,      // the flat vec4[468] transform-constant file
		vertex_buffers_slot = 5,        // persistent interleaved attribute streams
		vertex_volatile_slot = 6,       // register-value attributes and inline draws
		instancing_lut_slot = 10,       // indexed-constant indirection table
		instancing_constants_slot = 11, // indexed-constant vec4 array
		vertex_textures_slot = 12,      // VTF units 0..3
	};

	// The VTF units sit at 12+id (LLD-vertex §3.0); there are four
	// (rsx::limits::vertex_textures_count).
	constexpr u32 vertex_texture_slot(u32 index)
	{
		return vertex_textures_slot + index;
	}

	// The vertex cluster state that the shader and PSO key need and that no shared
	// helper holds (LLD-vertex §2.3, §3.2, §3.7, §3.8).  The streams, the transform
	// constants and the VTF registers stay in rsx::method_registers and in
	// rsx::draw_command_processor; this latch holds only the key inputs.
	struct vertex_state
	{
		u16 input_mask = 0;
		u32 output_mask = 0;
		u32 program_start = 0;
		u32 branch_bits = 0;
		bool srgb_output = false;
	};

	vertex_state capture_vertex_state();

	// Invariant 1 (LLD-vertex §5): a stream or a register value supplies each live
	// attribute.  An attribute with neither is unwritten (RSXDrawCommands.cpp:106-116).
	// Check it only for the attributes the input mask selects.
	bool attribute_sourced(u32 index);
}

#endif // RSX_ENABLE_METAL4
