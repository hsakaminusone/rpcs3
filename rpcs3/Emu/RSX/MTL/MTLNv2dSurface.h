#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

namespace mtl
{
	// The destination surface descriptor of a blit (LLD-nv2d-surface §2).  NV3062
	// describes a linear destination, NV309E a swizzled one; the trigger selects
	// between them (nv3089.cpp:97-141).  The source descriptor belongs to
	// nv3089-image, not here.
	struct surface_destination
	{
		u32 dma = 0;
		u32 offset = 0;
		u32 pitch = 0;
		u16 alignment = 0;
		rsx::blit_engine::transfer_destination_format format = rsx::blit_engine::transfer_destination_format::a8r8g8b8;
		bool swizzled = false;
		bool format_valid = false;
	};

	// Read the destination the trigger selected.  The surface kind is a sibling
	// register (NV3089_SET_CONTEXT_SURFACE), so it is a call argument.
	surface_destination capture_surface_destination(rsx::blit_engine::context_surface surface);

	// transfer_destination_format -> MTL::PixelFormat (HLD-02 §2.7).  y32 has no
	// pinned enum (LLD-nv2d-surface §6 Q4), so it is rejected.
	MTL::PixelFormat get_destination_pixel_format(rsx::blit_engine::transfer_destination_format format);

	// The destination bytes per pixel (nv3089.cpp:144).
	u32 get_destination_bpp(rsx::blit_engine::transfer_destination_format format);

	// out_pitch == 0 means bpp * width (nv3089.cpp:146-149).
	u32 resolve_destination_pitch(const surface_destination& destination, u16 width);
}

#endif // RSX_ENABLE_METAL4
