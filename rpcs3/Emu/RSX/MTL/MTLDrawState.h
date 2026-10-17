#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

namespace mtl
{
	// The draw cluster value state (LLD-draw §2.2).  The ranges, the inline array and
	// the immediate indices are clause-transient and live in draw_clause and in the
	// shared draw_command_processor, so they are absent here.  The primitive enters
	// the system only through SET_BEGIN_END (LLD-draw §2.3), so it is a call
	// argument, not a latch field.  SET_RENDER_ENABLE has no register value either:
	// it arms cond_render_ctrl on the thread (nv4097.cpp:692-726).
	struct draw_state
	{
		u32 base_offset = 0;
		u32 base_index = 0;
		u32 index_address = 0;
		u8 index_location = 0;
		rsx::index_array_type index_type = rsx::index_array_type::u16;
		bool restart_enabled = false;
		u32 restart_index = 0;
	};

	draw_state capture_draw_state();

	// SET_BEGIN_END -> MTL::PrimitiveType (LLD-draw §3.5).  A non-native type expands
	// into a native topology in the shared write_index_array_data_to_buffer
	// (Common/BufferUtils.cpp:942-957), so the result is the expansion target.
	MTL::PrimitiveType get_primitive_type(rsx::primitive_type mode);

	// SET_INDEX_ARRAY_DMA -> MTL::IndexType (LLD-draw §3.3).
	MTL::IndexType get_index_type(rsx::index_array_type type);
}

#endif // RSX_ENABLE_METAL4
