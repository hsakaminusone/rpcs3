#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

namespace mtl
{
	// The latched NV3089 source and geometry state (LLD-nv3089-image §2).  The
	// destination descriptor belongs to nv2d-surface, so it is absent here; the
	// destination select is latched because the trigger reads it (nv3089.cpp:97).
	struct image_in_state
	{
		u32 source_dma = 0;
		u32 source_offset = 0;
		u32 source_pitch = 0;
		u16 source_width = 0;
		u16 source_height = 0;

		rsx::blit_engine::transfer_source_format source_format = rsx::blit_engine::transfer_source_format::a8r8g8b8;
		bool source_format_valid = false;
		rsx::blit_engine::transfer_operation operation = rsx::blit_engine::transfer_operation::srccopy;
		rsx::blit_engine::transfer_origin origin = rsx::blit_engine::transfer_origin::corner;
		rsx::blit_engine::transfer_interpolator interpolator = rsx::blit_engine::transfer_interpolator::foh;
		rsx::blit_engine::context_surface surface = rsx::blit_engine::context_surface::surface2d;

		u16 clip_x = 0;
		u16 clip_y = 0;
		u16 clip_width = 0;
		u16 clip_height = 0;
		u16 out_x = 0;
		u16 out_y = 0;
		u16 out_width = 0;
		u16 out_height = 0;
		f32 scale_x = 1.0f;
		f32 scale_y = 1.0f;
	};

	image_in_state capture_image_in_state();

	// srccopy is the only operation RPCS3 accepts (nv3089.cpp:73-78).
	bool is_supported_operation(rsx::blit_engine::transfer_operation operation);

	// transfer_source_format -> MTL::PixelFormat.  Only r5g6b5/a8r8g8b8 reach the
	// pixel loop; x8r8g8b8 is promoted to a8r8g8b8 (nv3089.cpp:263-276).
	MTL::PixelFormat get_source_pixel_format(rsx::blit_engine::transfer_source_format format);

	// The source bytes per pixel (nv3089.cpp:143).
	u32 get_source_bpp(rsx::blit_engine::transfer_source_format format);

	// foh interpolates, otherwise point (nv3089.cpp:631).
	MTL::SamplerMinMagFilter get_interpolation_filter(rsx::blit_engine::transfer_interpolator interpolator);

	// The clip is clamped to the output extent; a zero dimension NOPs the
	// operation (nv3089.cpp:39-47).
	bool resolve_clip(const image_in_state& state, u16& clip_width, u16& clip_height);

	// swizzle2d selects the NV309E swizzled destination path (nv3089.cpp:97-141).
	bool uses_swizzle_destination(const image_in_state& state);

	// A negative scale mirrors that axis (nv3089.cpp:612-614).
	bool is_mirrored_x(const image_in_state& state);
	bool is_mirrored_y(const image_in_state& state);
}

#endif // RSX_ENABLE_METAL4
