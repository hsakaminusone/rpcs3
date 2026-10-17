#include "MTLFormats.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"

#include "util/logs.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

namespace mtl
{
	// The byte size of one pixel of a Metal colour format the output path carries.
	// Returns 0 for a format the display path does not present.
	u32 get_pixel_size(MTL::PixelFormat format)
	{
		switch (format)
		{
		case MTL::PixelFormat::PixelFormatBGRA8Unorm:
		case MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB:
		case MTL::PixelFormat::PixelFormatRGBA8Unorm:
		case MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB:
			return 4;
		default:
			return 0;
		}
	}

	// True when the memory order of a format is blue, green, red, alpha.  The video
	// sink and the screenshot writer take the byte order as a flag.
	bool is_bgra_format(MTL::PixelFormat format)
	{
		return format == MTL::PixelFormat::PixelFormatBGRA8Unorm || format == MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB;
	}

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
			return MTL::PixelFormat::PixelFormatDepth16Unorm;
		case rsx::surface_depth_format2::z16_float:
			// A unorm target cannot hold the float range, so the float variant uses
			// a float target (VKFormats.cpp:15-16; GLRenderTargets.cpp:78-79).
			return MTL::PixelFormat::PixelFormatDepth32Float;
		case rsx::surface_depth_format2::z24s8_uint:
		case rsx::surface_depth_format2::z24s8_float:
			return MTL::PixelFormat::PixelFormatDepth32Float_Stencil8;
		default:
			rsx_log.error("Metal 4: unsupported surface depth format 0x%x", static_cast<u32>(format));
			return MTL::PixelFormat::PixelFormatDepth32Float;
		}
	}

	MTL::BlitOption get_depth_plane_option(MTL::PixelFormat format, bool stencil_plane)
	{
		if (format != MTL::PixelFormat::PixelFormatDepth32Float_Stencil8)
		{
			return MTL::BlitOptionNone;
		}

		return stencil_plane ? MTL::BlitOptionStencilFromDepthStencil : MTL::BlitOptionDepthFromDepthStencil;
	}

	bool depth_format_needs_decode(rsx::surface_depth_format2 format)
	{
		switch (format)
		{
		case rsx::surface_depth_format2::z16_uint:
			return false;
		case rsx::surface_depth_format2::z16_float:
		case rsx::surface_depth_format2::z24s8_uint:
		case rsx::surface_depth_format2::z24s8_float:
			return true;
		default:
			return false;
		}
	}

	bool get_depth_texture_decode_format(u32 gcm_format, rsx::surface_depth_format2& format)
	{
		switch (gcm_format)
		{
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
			format = rsx::get_compatible_surface_depth_format(rsx::classify_format(gcm_format));
			return true;
		default:
			return false;
		}
	}

	void decode_depth_plane(void* dst, const void* src, u32 width, u32 height, u32 src_pitch, u32 src_bpp, rsx::surface_depth_format2 format)
	{
		ensure(depth_format_needs_decode(format) || format == rsx::surface_depth_format2::z16_uint);

		const u8* src_row = static_cast<const u8*>(src);
		u8* dst_row = static_cast<u8*>(dst);

		for (u32 y = 0; y < height; ++y)
		{
			for (u32 x = 0; x < width; ++x)
			{
				const u32 word = src_bpp == 4 ? read_from_ptr_unsafe<u32>(src_row, x * 4) : read_from_ptr_unsafe<u16>(src_row, x * 2);
				const f32 value = rsx::get_depth_clear_value(format, src_bpp == 4 ? (word >> 8) : (word & 0xFFFF));
				std::memcpy(dst_row + x * 4, &value, sizeof(value));
			}

			src_row += src_pitch;
			dst_row += width * 4;
		}
	}

	namespace
	{
		// The scalar form of pack_e4m12_pack16
		// (Program/GLSLSnippets/E4M12Conversion.glsl:5-12): clamp to the format's
		// maximum, rebias by 2^-111 so a plain float add is not needed, then round the
		// 11 dropped mantissa bits.  The inverse of decode_e4m12.
		u32 encode_e4m12(f32 value)
		{
			constexpr f32 e4m12_max = std::bit_cast<f32>(0x3F7FF800u); // decode_e4m12(0xFFFF)
			constexpr f32 e4m12_rebias = std::bit_cast<f32>(0x08000000u); // 2^-111
			const f32 scaled = std::clamp(value, 0.0f, e4m12_max) * e4m12_rebias;
			return (std::bit_cast<u32>(scaled) + 0x400u) >> 11;
		}

		// The RSX depth pack holds an unsigned depth.  The upload's decode divides by
		// the full range, so the encode rounds to nearest.  The arithmetic is in f64,
		// because a full-range value overflows f32: 0xFFFF + 0.5 rounds to 2^16 and
		// 0xFFFFFF + 0.5 rounds to 2^24, each one over the range, and the z24s8 pack
		// shifts that bit away entirely.  The clamp holds the rounded maximum.
		u32 encode_unorm(f32 value, u32 range)
		{
			const f64 scaled = std::clamp(static_cast<f64>(value), 0.0, 1.0) * static_cast<f64>(range) + 0.5;
			return std::min(static_cast<u32>(scaled), range);
		}
	}

	void encode_depth_plane(void* dst, const void* src, u32 width, u32 height, u32 dst_pitch, u32 dst_bpp,
		rsx::surface_depth_format2 format, const void* stencil_src, u32 stencil_pitch)
	{
		ensure(depth_format_needs_decode(format) || format == rsx::surface_depth_format2::z16_uint);

		// The pack's own word size: a z16 pack is one 16-bit word per texel and a z24s8
		// pack is one 32-bit word.  It must match the caller's stride, or the encode
		// would write past every texel.
		const u32 format_bpp = (format == rsx::surface_depth_format2::z16_uint || format == rsx::surface_depth_format2::z16_float) ? 2u : 4u;
		ensure(dst_bpp == format_bpp);

		const u8* src_row = static_cast<const u8*>(src);
		u8* dst_row = static_cast<u8*>(dst);
		const u8* stencil_row = static_cast<const u8*>(stencil_src);

		for (u32 y = 0; y < height; ++y)
		{
			for (u32 x = 0; x < width; ++x)
			{
				f32 value = 0.0f;
				std::memcpy(&value, src_row + x * 4, sizeof(value));

				u32 depth = 0;

				switch (format)
				{
				case rsx::surface_depth_format2::z16_uint:
					depth = encode_unorm(value, 0xFFFF);
					break;
				case rsx::surface_depth_format2::z24s8_uint:
					depth = encode_unorm(value, 0xFFFFFF);
					break;
				case rsx::surface_depth_format2::z16_float:
					depth = encode_e4m12(value);
					break;
				case rsx::surface_depth_format2::z24s8_float:
					// The guest value is a float whose low eight mantissa bits are the
					// stencil byte.
					depth = (std::bit_cast<u32>(value) >> 7) & 0xFFFFFF;
					break;
				default:
					fmt::throw_exception("Unreachable");
				}

				if (dst_bpp == 4)
				{
					const u32 stencil = stencil_row ? stencil_row[x] : 0;
					write_to_ptr_unsafe<u32>(dst_row, x * 4, (depth << 8) | stencil);
				}
				else
				{
					write_to_ptr_unsafe<u16>(dst_row, x * 2, static_cast<u16>(depth));
				}
			}

			src_row += width * 4;
			dst_row += dst_pitch;
			stencil_row = stencil_row ? stencil_row + stencil_pitch : nullptr;
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
