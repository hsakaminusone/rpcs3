#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

namespace mtl
{
	// One NV0039 buffer-to-buffer transfer (LLD-nv0039-dma §2, §3).  The copy is a
	// host byte copy over the shared guest window, so the shared handler
	// rsx::nv0039::buffer_notify owns it (nv0039.cpp:69-177); this struct only
	// describes the transfer and the ordering it needs.
	struct buffer_transfer
	{
		u32 input_pitch = 0;
		u32 output_pitch = 0;
		u32 line_length = 0;
		u32 line_count = 0;
		u8 input_stride = 1;
		u8 output_stride = 1;

		u32 read_address = 0;
		u32 write_address = 0;
		u32 read_length = 0;
		u32 write_length = 0;
	};

	// Read the live NV0039 registers and resolve the two guest ranges.  The
	// addresses come from the shared get_address (RSXThread.cpp:139).
	buffer_transfer capture_buffer_transfer();

	// validate_buffer_notify rejects a zero area and a stride outside 1..4
	// (nv0039.cpp:46-67).
	bool is_valid_transfer(const buffer_transfer& transfer);

	// A transfer is a block transfer iff both strides are 1 and both pitches equal
	// line_length * stride (nv0039.cpp:95-98).
	bool is_block_transfer(const buffer_transfer& transfer);

	// A strided gather runs when either stride exceeds 1 (nv0039.cpp:144).
	bool is_strided_transfer(const buffer_transfer& transfer);

	// The transfer needs the .device visibility point: the source read is a
	// GPU->CPU edge and the destination write is a CPU->GPU edge (LLD-nv0039-dma
	// §3, §4).
	MTL4::VisibilityOptions get_transfer_visibility();
}

#endif // RSX_ENABLE_METAL4
