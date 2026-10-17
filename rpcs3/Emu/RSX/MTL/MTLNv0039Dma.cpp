#include "MTLNv0039Dma.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/RSXThread.h"

namespace mtl
{
	buffer_transfer capture_buffer_transfer()
	{
		buffer_transfer transfer;

		transfer.input_pitch = rsx::method_registers.nv0039_input_pitch();
		transfer.output_pitch = rsx::method_registers.nv0039_output_pitch();
		transfer.line_length = rsx::method_registers.nv0039_line_length();
		transfer.line_count = rsx::method_registers.nv0039_line_count();
		transfer.input_stride = rsx::method_registers.nv0039_input_format();
		transfer.output_stride = rsx::method_registers.nv0039_output_format();

		const u32 input_offset = rsx::method_registers.nv0039_input_offset();
		const u32 input_dma = rsx::method_registers.nv0039_input_location();
		const u32 output_offset = rsx::method_registers.nv0039_output_offset();
		const u32 output_dma = rsx::method_registers.nv0039_output_location();

		transfer.read_address = rsx::get_address(input_offset, input_dma);
		transfer.write_address = rsx::get_address(output_offset, output_dma);

		// The read/write extents are the pitched extents, not the texture pitches
		// (nv0039.cpp:101-102).
		const u32 rows = transfer.line_count ? transfer.line_count - 1 : 0;
		transfer.read_length = transfer.input_pitch * rows + transfer.line_length * transfer.input_stride;
		transfer.write_length = transfer.output_pitch * rows + transfer.line_length * transfer.output_stride;

		return transfer;
	}

	bool is_valid_transfer(const buffer_transfer& transfer)
	{
		if (!transfer.line_length || !transfer.line_count)
		{
			return false;
		}

		return transfer.input_stride >= 1 && transfer.input_stride <= 4
			&& transfer.output_stride >= 1 && transfer.output_stride <= 4;
	}

	bool is_block_transfer(const buffer_transfer& transfer)
	{
		return transfer.input_stride == 1 && transfer.output_stride == 1
			&& transfer.input_pitch == transfer.line_length * transfer.input_stride
			&& transfer.output_pitch == transfer.line_length * transfer.output_stride;
	}

	bool is_strided_transfer(const buffer_transfer& transfer)
	{
		return transfer.input_stride > 1 || transfer.output_stride > 1;
	}

	MTL4::VisibilityOptions get_transfer_visibility()
	{
		return MTL4::VisibilityOptionDevice;
	}
}

#endif // RSX_ENABLE_METAL4
