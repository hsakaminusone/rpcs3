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

	// z24s8 becomes Depth32Float_Stencil8.  Apple silicon has no
	// Depth24Unorm_Stencil8 (HLD-02 §2.7; 04 §2.3).
	MTL::PixelFormat get_surface_depth_format(rsx::surface_depth_format2 format);

	// The guest texture format (HLD-02 §2.7; LLD-texture §4).  A sampled texture is
	// read through the unit's remap, so the packed colour formats expand to their
	// natural channel count instead of a surface layout.
	MTL::PixelFormat get_texture_format(u32 gcm_format);

	// The native channel order of a guest format, in A-R-G-B order so it composes
	// with the RSX remap vector (GLTexture.cpp:214-269).
	std::array<MTL::TextureSwizzle, 4> get_texture_component_layout(u32 gcm_format);
}

#endif // RSX_ENABLE_METAL4
