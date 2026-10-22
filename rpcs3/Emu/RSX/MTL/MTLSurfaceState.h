#pragma once

#include "MTLFormats.h"

#ifdef RSX_ENABLE_METAL4

#include <array>

namespace mtl
{
	// One RSX surface identity (LLD-surface §3.1/§3.2): a guest address and a guest
	// pitch.  The address is the app-address identity, not a Metal field (HLD-02
	// §2.1).  The pitch is the guest bytes-per-row, distinct from the texture row
	// stride (native_pitch, HLD-02 §2.3).
	struct surface_attachment
	{
		u32 address = 0;
		u32 pitch = 0;
	};

	// The latched `surface` cluster state (LLD-surface §2).  These registers are the
	// operands that build the attachment set.  The MTL::Texture, the aliasing and
	// the residency belong to the surface store (HLD-02 §2.5), not here.
	struct surface_state
	{
		rsx::surface_color_format color_format{};
		rsx::surface_depth_format2 depth_format{};
		rsx::surface_antialiasing antialias{};
		rsx::surface_raster_type raster_type{};

		rsx::surface_target color_target{};
		u32 color_target_mask = 0;
		std::array<surface_attachment, 4> color{};
		surface_attachment zeta{};
		bool depth_enabled = false;

		u16 clip_x = 0;
		u16 clip_y = 0;
		u16 clip_width = 0;
		u16 clip_height = 0;

		// The clear region is the scissor, not SET_CLEAR_RECT_* (CONFLICTS.md C1;
		// VKGSRender.cpp:1441, GLGSRender.cpp:668).
		u16 clear_x = 0;
		u16 clear_y = 0;
		u16 clear_width = 0;
		u16 clear_height = 0;

		MTL::ClearColor clear_color{};
		// The colour channels the clear actually writes for this format, and whether
		// one MTLLoadActionClear can express them (MTLFormats.h resolve_surface_clear).
		u32 clear_colormask = 0;
		bool clear_color_fast = false;
		f32 clear_depth = 1.0f;
		u8 clear_stencil = 0;
		// A stencil write mask below 0xff makes the stencil clear a partial write, so
		// the pass cannot express it as MTLLoadActionClear and the fill runs instead
		// (S5; VKGSRender.cpp:1624-1638).
		bool clear_stencil_fast = true;

		// Extent.  The surface clip is authoritative; the SET_SURFACE_FORMAT log2
		// fields are not (LLD-surface §2.4, invariant 1).
		u16 width() const { return clip_width; }
		u16 height() const { return clip_height; }

		u8 sample_count() const;
		u32 color_attachment_count() const;
		bool has_stencil() const;

		// Least guest pitch that holds the surface (invariant 2).  The layout step
		// uses it to correct or to drop a depth surface (RSXThread.cpp:1644).
		u32 minimum_color_pitch() const;
		u32 minimum_zeta_pitch() const;

		// True when the clear covers the whole attachment.  Only then may the clear
		// become MTLLoadActionClear (LLD-surface §3.5, invariant 4).
		bool clear_covers_attachment() const;
	};

	// Read the live cluster registers (rsx::method_registers).  The caller sets
	// depth_enabled afterwards, because the depth-surface survival decision belongs
	// to the layout step, not to this latch.
	surface_state capture_surface_state();

	// Set the format, the extent and the sample count on a texture descriptor
	// (LLD-surface §3.1).  The caller sets the storage mode and the usage; HLD-02
	// decides those.
	void fill_texture_descriptor(MTL::TextureDescriptor* descriptor, const surface_state& state, bool depth);
}

#endif // RSX_ENABLE_METAL4
