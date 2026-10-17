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

	MTL::PixelFormat get_texture_format(u32 gcm_format)
	{
		switch (gcm_format)
		{
		case CELL_GCM_TEXTURE_B8:
			return MTL::PixelFormat::PixelFormatR8Unorm;
		case CELL_GCM_TEXTURE_A8R8G8B8:
		case CELL_GCM_TEXTURE_D8R8G8B8:
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		case CELL_GCM_TEXTURE_R5G6B5:
		case CELL_GCM_TEXTURE_A1R5G5B5:
		case CELL_GCM_TEXTURE_A4R4G4B4:
		case CELL_GCM_TEXTURE_R5G5B5A1:
		case CELL_GCM_TEXTURE_D1R5G5B5:
		case CELL_GCM_TEXTURE_R6G5B5:
			// The Apple arm64 uploader expands every packed colour format to BGRA8
			// (TextureUtils.cpp:950-1015).
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		case CELL_GCM_TEXTURE_G8B8:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
			return MTL::PixelFormat::PixelFormatRG8Unorm;
		case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
			return MTL::PixelFormat::PixelFormatRG8Snorm;
		case CELL_GCM_TEXTURE_Y16_X16:
			return MTL::PixelFormat::PixelFormatRG16Unorm;
		case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
			return MTL::PixelFormat::PixelFormatRG16Float;
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
			return MTL::PixelFormat::PixelFormatRGBA16Float;
		case CELL_GCM_TEXTURE_X16:
			return MTL::PixelFormat::PixelFormatR16Unorm;
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
			return MTL::PixelFormat::PixelFormatRGBA32Float;
		case CELL_GCM_TEXTURE_X32_FLOAT:
			return MTL::PixelFormat::PixelFormatR32Float;
		case CELL_GCM_TEXTURE_DEPTH16:
			return MTL::PixelFormat::PixelFormatDepth16Unorm;
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
			return MTL::PixelFormat::PixelFormatDepth32Float;
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
			return MTL::PixelFormat::PixelFormatDepth32Float_Stencil8;
		case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
			return MTL::PixelFormat::PixelFormatBC1_RGBA;
		case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
			return MTL::PixelFormat::PixelFormatBC2_RGBA;
		case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
			return MTL::PixelFormat::PixelFormatBC3_RGBA;
		case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
		case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		default:
			rsx_log.error("Metal 4: unsupported texture format 0x%x", gcm_format);
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		}
	}

	std::array<MTL::TextureSwizzle, 4> get_texture_component_layout(u32 gcm_format)
	{
		// A-R-G-B order, so it composes with the RSX remap vector.
		switch (gcm_format)
		{
		case CELL_GCM_TEXTURE_A1R5G5B5:
		case CELL_GCM_TEXTURE_R5G5B5A1:
		case CELL_GCM_TEXTURE_R6G5B5:
		case CELL_GCM_TEXTURE_R5G6B5:
		case CELL_GCM_TEXTURE_A4R4G4B4:
		case CELL_GCM_TEXTURE_A8R8G8B8:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT1:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT23:
		case CELL_GCM_TEXTURE_COMPRESSED_DXT45:
		case CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8:
		case CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8:
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
			return { MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
		case CELL_GCM_TEXTURE_X32_FLOAT:
			return { MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed };
		case CELL_GCM_TEXTURE_B8:
			return { MTL::TextureSwizzleOne, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed };
		case CELL_GCM_TEXTURE_X16:
			return { MTL::TextureSwizzleRed, MTL::TextureSwizzleOne, MTL::TextureSwizzleRed, MTL::TextureSwizzleOne };
		case CELL_GCM_TEXTURE_G8B8:
		case CELL_GCM_TEXTURE_Y16_X16:
			return { MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed };
		case CELL_GCM_TEXTURE_Y16_X16_FLOAT:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO8:
		case CELL_GCM_TEXTURE_COMPRESSED_HILO_S8:
			return { MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen };
		case CELL_GCM_TEXTURE_D1R5G5B5:
		case CELL_GCM_TEXTURE_D8R8G8B8:
			return { MTL::TextureSwizzleOne, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
		default:
			rsx_log.error("Metal 4: unsupported texture format 0x%x", gcm_format);
			return { MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
		}
	}
}

#endif // RSX_ENABLE_METAL4
