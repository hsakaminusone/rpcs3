#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"
#include "MTLNv2dSurface.h"

namespace mtl
{
	// The store/convert branch of one COLOR batch (LLD-nv308a-image §3.2).
	enum class upload_kind
	{
		bit_cast,       // a8r8g8b8 / y32: a 4-byte store
		convert_r5g6b5, // ARGB8 -> RGB565
		unsupported,    // aborts (nv308a.cpp:158-161)
	};

	// The latched NV308A upload state (LLD-nv308a-image §2).  The destination
	// descriptor is borrowed from NV3062, so it is captured through nv2d-surface.
	// SET_COLOR_FORMAT and SET_CONTEXT_SURFACE are undecoded (LLD-nv308a-image §2),
	// so they are latched raw.
	struct image_upload
	{
		u16 x = 0;
		u16 y = 0;
		u16 size_out_x = 0;
		u32 color_format = 0;
		u32 context_surface = 0;
		surface_destination destination;
	};

	image_upload capture_image_upload();

	// The destination format of record is NV3062_SET_COLOR_FORMAT, not
	// NV308A_SET_COLOR_FORMAT (nv308a.cpp:69).
	upload_kind get_upload_kind(const image_upload& upload);

	// Destination X is nv308a_x() + index (nv308a.cpp:49).
	u32 get_upload_destination_x(const image_upload& upload, u32 index);

	// The destination byte origin (nv308a.cpp:78,117).  The pitch is the raw NV3062
	// pitch, not the resolved one.
	u32 get_upload_byte_offset(const image_upload& upload, u32 index);

	// The per-batch write bound is nv308a_size_out_x() (nv308a.cpp:18).
	bool is_index_in_bounds(const image_upload& upload, u32 index);

	// The ARGB8 -> RGB565 reference rounding (nv308a.cpp:129-140).  The host
	// converter and the MSL kernel must match it exactly (invariant 4).
	u16 convert_argb8_to_r5g6b5(u32 argb);

	// The CPU store is a CPU->GPU edge; the destination needs the .device
	// visibility point (LLD-nv308a-image §3.3).
	MTL4::VisibilityOptions get_upload_visibility();
}

#endif // RSX_ENABLE_METAL4
