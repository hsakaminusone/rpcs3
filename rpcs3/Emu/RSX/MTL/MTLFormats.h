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

	// The Metal write mask of the RSX_GCM_CLEAR_* colour bits.
	MTL::ColorWriteMask to_color_write_mask(u32 clear_mask);

	// z24s8 becomes Depth32Float_Stencil8.  Apple silicon has no
	// Depth24Unorm_Stencil8 (HLD-02 §2.7; 04 §2.3).
	MTL::PixelFormat get_surface_depth_format(rsx::surface_depth_format2 format);

	// The blit option that names one plane of a combined depth-stencil texture
	// (REVIEW-FINDINGS.md, B15).  A texture that is not combined takes no option.
	MTL::BlitOption get_depth_plane_option(MTL::PixelFormat format, bool stencil_plane);

	// The sRGB view format of an 8-bit colour format, or PixelFormatInvalid when
	// the format has no sRGB counterpart (T8).
	MTL::PixelFormat to_srgb(MTL::PixelFormat format);

	// The byte size of one guest texel of a texture in this format.  A
	// Depth32Float_Stencil8 image is a host 8-byte texel but a guest z24s8
	// 4-byte texel, and transfer arithmetic works in the guest grid (T6).  Zero
	// for a compressed or unknown format, so a bit-preserving copy can reject a
	// layout it cannot name.
	u32 get_texel_size(MTL::PixelFormat format);

	// The size of one texel as the host stores it.  It differs from
	// get_texel_size only where the host substitution is wider than the guest
	// format (T6).
	u32 host_texel_size(MTL::PixelFormat format);

	// The bytes one depth plane texel occupies in a blit staging buffer: the
	// depth aspect of Depth32Float_Stencil8 is four bytes, not the eight of the
	// combined texel.
	u32 depth_plane_texel_size(MTL::PixelFormat format);

	// The size of one guest word in a surface of `bpp` bytes per texel, for the
	// GPU byte swap (T2).  The guest is big-endian, so a 2-byte surface swaps
	// 16-bit words, an 8-byte surface (four 16-bit channels) also swaps 16-bit
	// words, and a 4- or 16-byte surface swaps 32-bit words.  One byte needs no
	// swap, so zero tells the caller to copy straight through.
	u32 guest_endian_element_size(u32 bpp);

	// True for the depth formats this backend creates.  Metal has no colour view of
	// a depth image (EXPERIMENT-blit-and-attachments.mm, probe G), so a render pass
	// cannot use one as a colour attachment.
	bool is_depth_format(MTL::PixelFormat format);

	// True when the guest depth pack differs from the Metal depth plane, so the
	// guest words cannot be copied as-is (REVIEW-FINDINGS.md, B8).
	bool depth_format_needs_decode(rsx::surface_depth_format2 format);

	// True when the guest texture format maps to a Metal colour format with an
	// sRGB view.  The packed 16-bit formats keep a Metal packed format, which has
	// no sRGB counterpart, so their gamma fetch stays a shader operation (T8).
	bool guest_format_has_srgb_host(u32 gcm_format);

	// Map a sampled depth texture format to the surface depth format whose guest
	// pack the upload plane decode understands.  False for a colour or compressed
	// format (REVIEW-FINDINGS.md, B8).
	bool get_depth_texture_decode_format(u32 gcm_format, rsx::surface_depth_format2& format);

	// Decode guest depth words into the Metal depth plane.  dst rows are width * 4
	// bytes; src rows are src_pitch bytes apart with src_bpp bytes per texel.
	void decode_depth_plane(void* dst, const void* src, u32 width, u32 height,
		u32 src_pitch, u32 src_bpp, rsx::surface_depth_format2 format);

	// Extract the stencil plane of a guest z24s8 pack into the Metal stencil plane.
	// A z24s8 pack carries the stencil byte in bits 7:0 of each 32-bit texel, so dst
	// is one byte per texel at dst_pitch bytes per row and src rows are src_pitch
	// bytes apart.  The inverse of the stencil arm of encode_depth_plane.
	void decode_stencil_plane(void* dst, const void* src, u32 width, u32 height,
		u32 src_pitch, u32 src_bpp, u32 dst_pitch);

	// Encode the Metal depth plane back into the guest depth pack: the inverse of
	// decode_depth_plane.  dst rows are dst_pitch bytes apart with dst_bpp bytes per
	// texel; src rows are width * 4 bytes, one f32 per texel.  A z24s8 pack carries
	// the stencil byte in bits 7:0, which the depth plane does not hold, so a stencil
	// plane may be named: stencil_src is one byte per texel at stencil_pitch bytes per
	// row, and a null pointer writes zero there (REVIEW-FINDINGS.md, B16).
	void encode_depth_plane(void* dst, const void* src, u32 width, u32 height,
		u32 dst_pitch, u32 dst_bpp, rsx::surface_depth_format2 format,
		const void* stencil_src = nullptr, u32 stencil_pitch = 0);

	// True when the guest packed 16-bit colour pack is not in the Metal packed
	// format's field order, so a raw row copy exchanges channels (REVIEW-RSX-NATIVE.md,
	// R2).  The colour counterpart of depth_format_needs_decode.
	bool packed_format_needs_convert(rsx::surface_color_format format);

	// Decode guest packed 16-bit colour words into the Metal packed plane: the inverse
	// of encode_packed16_plane.  dst rows are width * 2 bytes; src rows are src_pitch
	// bytes apart with 2 bytes per texel.  r5g6b5 (B5G6R5Unorm) trades the five-bit red
	// and blue fields; x1r5g5b5_* (BGR5A1Unorm) repacks the whole word and carries the
	// guest x1/o1 bit into the host alpha field.
	void decode_packed16_plane(void* dst, const void* src, u32 width, u32 height,
		u32 src_pitch, rsx::surface_color_format format);

	// Encode the Metal packed plane back into the guest pack: the inverse of
	// decode_packed16_plane.  dst rows are dst_pitch bytes apart with 2 bytes per texel.
	// The x1/o1 bit keeps its stored value; the guest read of the alpha channel is
	// governed by the surface's component layout either way.
	void encode_packed16_plane(void* dst, const void* src, u32 width, u32 height,
		u32 dst_pitch, rsx::surface_color_format format);

	// Encode a host BGRA8 texture plane back into the guest r6g5b5 pack: the inverse
	// of the Apple upload expand (TextureUtils.cpp convert_rgb655_to_bgra8, TX20).
	// dst rows are dst_pitch bytes apart with one big-endian 16-bit word per texel,
	// the same layout the packed-plane helpers write for the guest; src rows are
	// width * 4 bytes, one BGRA8 texel per texel.  The expand's multiplies have no
	// exact inverse, so the encode truncates each host channel to the guest width
	// instead of inverting them; it is not the algebraic inverse of the expand
	// (the VK-accepted lossy round trip).
	void encode_r6g5b5_plane(void* dst, const void* src, u32 width, u32 height, u32 dst_pitch);

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
