#include "MTLNv2dSurface.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "util/logs.hpp"

namespace mtl
{
	surface_destination capture_surface_destination(rsx::blit_engine::context_surface surface)
	{
		surface_destination destination;

		switch (surface)
		{
		case rsx::blit_engine::context_surface::swizzle2d:
		{
			destination.swizzled = true;
			destination.dma = rsx::method_registers.blit_engine_nv309E_location();
			destination.offset = rsx::method_registers.blit_engine_nv309E_offset();

			const auto format = rsx::method_registers.blit_engine_output_format_nv309E();
			if (format)
			{
				destination.format = format;
				destination.format_valid = true;
			}

			break;
		}
		case rsx::blit_engine::context_surface::surface2d:
		{
			destination.dma = rsx::method_registers.blit_engine_output_location_nv3062();
			destination.offset = rsx::method_registers.blit_engine_output_offset_nv3062();
			destination.pitch = rsx::method_registers.blit_engine_output_pitch_nv3062();
			destination.alignment = rsx::method_registers.blit_engine_output_alignment_nv3062();

			const auto format = rsx::method_registers.blit_engine_nv3062_color_format();
			if (format)
			{
				destination.format = format;
				destination.format_valid = true;
			}

			break;
		}
		}

		return destination;
	}

	MTL::PixelFormat get_destination_pixel_format(rsx::blit_engine::transfer_destination_format format)
	{
		switch (format)
		{
		case rsx::blit_engine::transfer_destination_format::r5g6b5:
			return MTL::PixelFormat::PixelFormatB5G6R5Unorm;
		case rsx::blit_engine::transfer_destination_format::a8r8g8b8:
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		case rsx::blit_engine::transfer_destination_format::y32:
			// The CPU path never reaches y32 (nv3089.cpp:257-261) and its Metal
			// enum is not pinned (LLD-nv2d-surface §6 Q4).
			rsx_log.error("Metal 4: y32 blit destination has no pinned MTLPixelFormat");
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		default:
			rsx_log.error("Metal 4: unknown blit destination format 0x%x", static_cast<u32>(format));
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		}
	}

	u32 get_destination_bpp(rsx::blit_engine::transfer_destination_format format)
	{
		return format == rsx::blit_engine::transfer_destination_format::r5g6b5 ? 2 : 4;
	}

	u32 resolve_destination_pitch(const surface_destination& destination, u16 width)
	{
		return destination.pitch ? destination.pitch : get_destination_bpp(destination.format) * width;
	}
}

#endif // RSX_ENABLE_METAL4
