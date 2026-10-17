#include "MTLNv308aImage.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "util/logs.hpp"

namespace mtl
{
	image_upload capture_image_upload()
	{
		image_upload upload;

		upload.x = rsx::method_registers.nv308a_x();
		upload.y = rsx::method_registers.nv308a_y();
		upload.size_out_x = rsx::method_registers.nv308a_size_out_x();

		// SET_COLOR_FORMAT and SET_CONTEXT_SURFACE are undecoded (LLD-nv308a-image
		// §2), so latch them raw for the format-of-record question (§6 Q1/Q2).
		upload.color_format = rsx::method_registers.registers[NV308A_SET_COLOR_FORMAT];
		upload.context_surface = rsx::method_registers.registers[NV308A_SET_CONTEXT_SURFACE];

		// The reference path hard-codes the NV3062 destination (nv308a.cpp:45-47).
		upload.destination = capture_surface_destination(rsx::blit_engine::context_surface::surface2d);

		return upload;
	}

	upload_kind get_upload_kind(const image_upload& upload)
	{
		if (!upload.destination.format_valid)
		{
			// No NV3062 format pins the destination layout (LLD-nv2d-surface §2).
			return upload_kind::unsupported;
		}

		switch (upload.destination.format)
		{
		case rsx::blit_engine::transfer_destination_format::a8r8g8b8:
		case rsx::blit_engine::transfer_destination_format::y32:
			return upload_kind::bit_cast;
		case rsx::blit_engine::transfer_destination_format::r5g6b5:
			return upload_kind::convert_r5g6b5;
		default:
			// Any other format aborts in the reference (nv308a.cpp:158-161).
			rsx_log.error("Metal 4: unsupported NV308A destination format 0x%x", static_cast<u32>(upload.destination.format));
			return upload_kind::unsupported;
		}
	}

	u32 get_upload_destination_x(const image_upload& upload, u32 index)
	{
		return upload.x + index;
	}

	u32 get_upload_byte_offset(const image_upload& upload, u32 index)
	{
		const u32 bpp = get_destination_bpp(upload.destination.format);
		return get_upload_destination_x(upload, index) * bpp + upload.destination.pitch * upload.y;
	}

	bool is_index_in_bounds(const image_upload& upload, u32 index)
	{
		return index < upload.size_out_x;
	}

	u16 convert_argb8_to_r5g6b5(u32 argb)
	{
		u32 r = (argb >> 16) & 0xFF;
		u32 g = (argb >> 8) & 0xFF;
		u32 b = argb & 0xFF;

		r = (r * 32) / 255;
		g = (g * 64) / 255;
		b = (b * 32) / 255;

		return static_cast<u16>((r << 11) | (g << 5) | b);
	}

	MTL4::VisibilityOptions get_upload_visibility()
	{
		return MTL4::VisibilityOptionDevice;
	}
}

#endif // RSX_ENABLE_METAL4
