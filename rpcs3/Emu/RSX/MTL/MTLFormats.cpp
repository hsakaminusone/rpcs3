#include "MTLFormats.h"

#ifdef RSX_ENABLE_METAL4

#include "util/logs.hpp"

namespace mtl
{
	MTL::PixelFormat get_surface_color_format(rsx::surface_color_format format)
	{
		switch (format)
		{
		case rsx::surface_color_format::b8:
			return MTL::PixelFormat::PixelFormatR8Unorm;
		case rsx::surface_color_format::g8b8:
			return MTL::PixelFormat::PixelFormatRG8Unorm;
		case rsx::surface_color_format::r5g6b5:
			return MTL::PixelFormat::PixelFormatB5G6R5Unorm;
		case rsx::surface_color_format::x1r5g5b5_z1r5g5b5:
		case rsx::surface_color_format::x1r5g5b5_o1r5g5b5:
			// The A1B5G5R5 border-colour hardware bug forces the RGB swap
			// (HLD-02 §2.7; 04 §2.3).
			return MTL::PixelFormat::PixelFormatBGR5A1Unorm;
		case rsx::surface_color_format::x8r8g8b8_z8r8g8b8:
		case rsx::surface_color_format::x8r8g8b8_o8r8g8b8:
		case rsx::surface_color_format::a8r8g8b8:
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		case rsx::surface_color_format::x8b8g8r8_z8b8g8r8:
		case rsx::surface_color_format::x8b8g8r8_o8b8g8r8:
		case rsx::surface_color_format::a8b8g8r8:
			return MTL::PixelFormat::PixelFormatRGBA8Unorm;
		case rsx::surface_color_format::w16z16y16x16:
			return MTL::PixelFormat::PixelFormatRGBA16Float;
		case rsx::surface_color_format::w32z32y32x32:
			return MTL::PixelFormat::PixelFormatRGBA32Float;
		case rsx::surface_color_format::x32:
			return MTL::PixelFormat::PixelFormatR32Float;
		default:
			rsx_log.error("Metal 4: unsupported surface colour format 0x%x", static_cast<u32>(format));
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		}
	}

	MTL::PixelFormat get_surface_depth_format(rsx::surface_depth_format2 format)
	{
		switch (format)
		{
		case rsx::surface_depth_format2::z16_uint:
		case rsx::surface_depth_format2::z16_float:
			return MTL::PixelFormat::PixelFormatDepth16Unorm;
		case rsx::surface_depth_format2::z24s8_uint:
		case rsx::surface_depth_format2::z24s8_float:
			return MTL::PixelFormat::PixelFormatDepth32Float_Stencil8;
		default:
			rsx_log.error("Metal 4: unsupported surface depth format 0x%x", static_cast<u32>(format));
			return MTL::PixelFormat::PixelFormatDepth32Float;
		}
	}
}

#endif // RSX_ENABLE_METAL4
