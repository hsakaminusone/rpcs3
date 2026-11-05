#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

#include <array>

namespace mtl
{
	// The surface pixel format (HLD-02 §2.7).  The RSX enum is the source.  Metal
	// has no 16-bit packed colour format and no 24-bit depth format, so the table
	// records the substitution the design doc selects.
	//
	// The channel-remap question is not answered here.  The live state answers it
	// through surface_is_swizzle_remapped() (rsx_methods.h:967, rsx_decode.h:3373).
	MTL::PixelFormat get_surface_color_format(rsx::surface_color_format format);

	// The guest-to-host channel map of a surface colour format, in A-R-G-B
	// order so it composes with the RSX remap (MTLHandles.cpp, get_view).  A
	// guest channel the format does not store reads as one or zero (VKGSRender.cpp
	// get_compatible_surface_format; color_utils.hpp:300).
	std::array<MTL::TextureSwizzle, 4> get_surface_component_layout(rsx::surface_color_format format);

	// The colour a clear loads for this surface format, and the channels it writes.
	// The clear registers hold an A8R8G8B8 word that the hardware copies into the
	// surface, so a packed or reordered format needs a transform before the value
	// reaches Metal (VKGSRender.cpp:1526-1578; GLGSRender.cpp:742-791).
	struct resolved_clear
	{
		MTL::ClearColor color{};
		// The effective RSX_GCM_CLEAR_* colour bits, or zero for a NOP.
		u32 mask = 0;
		// True when a single MTLLoadActionClear expresses every channel the format
		// stores.  A false value needs the masked fill.
		bool fast = false;
	};

	resolved_clear resolve_surface_clear(rsx::surface_color_format format, u8 r, u8 g, u8 b, u8 a, u32 mask);

	// The Metal write mask of the RSX_GCM_CLEAR_* colour bits.
	MTL::ColorWriteMask to_color_write_mask(u32 clear_mask);

	// z24s8 becomes Depth32Float_Stencil8.  Apple silicon has no
	// Depth24Unorm_Stencil8 (HLD-02 §2.7; 04 §2.3).
	MTL::PixelFormat get_surface_depth_format(rsx::surface_depth_format2 format);

	// The blit option that names one plane of a combined depth-stencil texture
	// (REVIEW-FINDINGS.md, B15).  A texture that is not combined takes no option.
	MTL::BlitOption get_depth_plane_option(MTL::PixelFormat format, bool stencil_plane);

	// The byte size of one uncompressed host texel.  Zero for a compressed or
	// unknown format, so a bit-preserving copy can reject a layout it cannot name.
	u32 get_texel_size(MTL::PixelFormat format);

	// True when the guest depth pack differs from the Metal depth plane, so the
	// guest words cannot be copied as-is (REVIEW-FINDINGS.md, B8).
	bool depth_format_needs_decode(rsx::surface_depth_format2 format);

	// Map a sampled depth texture format to the surface depth format whose guest
	// pack the upload plane decode understands.  False for a colour or compressed
	// format (REVIEW-FINDINGS.md, B8).
	bool get_depth_texture_decode_format(u32 gcm_format, rsx::surface_depth_format2& format);

	// Decode guest depth words into the Metal depth plane.  dst rows are width * 4
	// bytes; src rows are src_pitch bytes apart with src_bpp bytes per texel.
	void decode_depth_plane(void* dst, const void* src, u32 width, u32 height,
		u32 src_pitch, u32 src_bpp, rsx::surface_depth_format2 format);

	// Encode the Metal depth plane back into the guest depth pack: the inverse of
	// decode_depth_plane.  dst rows are dst_pitch bytes apart with dst_bpp bytes per
	// texel; src rows are width * 4 bytes, one f32 per texel.  A z24s8 pack carries
	// the stencil byte in bits 7:0, which the depth plane does not hold, so a stencil
	// plane may be named: stencil_src is one byte per texel at stencil_pitch bytes per
	// row, and a null pointer writes zero there (REVIEW-FINDINGS.md, B16).
	void encode_depth_plane(void* dst, const void* src, u32 width, u32 height,
		u32 dst_pitch, u32 dst_bpp, rsx::surface_depth_format2 format,
		const void* stencil_src = nullptr, u32 stencil_pitch = 0);

	// The guest texture format (HLD-02 §2.7; LLD-texture §4).  A sampled texture is
	// read through the unit's remap, so the packed colour formats expand to their
	// natural channel count instead of a surface layout.  A device without BC
	// texture compression reads a DXT texture as expanded BGRA8, which is the same
	// fallback Vulkan uses when the feature is absent (VKFormats.cpp:218-220).
	MTL::PixelFormat get_texture_format(u32 gcm_format, bool supports_bc = true);

	// The native channel order of a guest format, in A-R-G-B order so it composes
	// with the RSX remap vector (GLTexture.cpp:214-269).
	std::array<MTL::TextureSwizzle, 4> get_texture_component_layout(u32 gcm_format);

	// The byte size of one pixel of a Metal colour format the output path carries.
	// Zero means the format is not presentable.
	u32 get_pixel_size(MTL::PixelFormat format);

	// True when the memory order of a format is blue, green, red, alpha.
	bool is_bgra_format(MTL::PixelFormat format);
}

#endif // RSX_ENABLE_METAL4
