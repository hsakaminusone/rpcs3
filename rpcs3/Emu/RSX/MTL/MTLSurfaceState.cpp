#include "MTLSurfaceState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Core/RSXContext.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Common/surface_store.h"

#include <bit>

namespace mtl
{
	u8 surface_state::sample_count() const
	{
		return rsx::get_format_sample_count(antialias);
	}

	u32 surface_state::color_attachment_count() const
	{
		// MRT1/2/3 select contiguous A..D prefixes, so the count is the mask
		// population (rsx_methods.h:768-784, LLD-surface invariant 3).
		return std::popcount(color_target_mask);
	}

	bool surface_state::has_stencil() const
	{
		return rsx::is_depth_stencil_format(depth_format);
	}

	u32 surface_state::minimum_color_pitch() const
	{
		return rsx::get_format_block_size_in_bytes(color_format) * clip_width;
	}

	u32 surface_state::minimum_zeta_pitch() const
	{
		return rsx::get_format_block_size_in_bytes(depth_format) * clip_width;
	}

	surface_state capture_surface_state(const rsx::context& ctx)
	{
		const rsx::framebuffer_layout& layout = ctx.fb.layout;
		surface_state state;

		state.color_format = layout.color_format;
		state.depth_format = layout.depth_format;
		state.antialias = layout.aa_mode;
		state.raster_type = layout.raster_type;

		state.color_target = layout.target;
		state.color_target_mask = (1u << rsx::utility::get_mrt_buffers_count(layout.target)) - 1;

		state.clip_width = layout.width;
		state.clip_height = layout.height;

		for (u32 index = 0; index < 4; ++index)
		{
			auto& surface = state.color[index];

			// The window offset is already applied to the layout address, and the
			// actual pitch is what the surface store allocated.
			surface.address = layout.color_addresses[index];
			surface.pitch = layout.actual_color_pitch[index];
		}

		state.zeta.address = layout.zeta_address;
		state.zeta.pitch = layout.actual_zeta_pitch;

		return state;
	}

	void fill_texture_descriptor(MTL::TextureDescriptor* descriptor, const surface_state& state, bool depth)
	{
		descriptor->setTextureType(MTL::TextureType::TextureType2D);
		descriptor->setPixelFormat(depth ? get_surface_depth_format(state.depth_format) : get_surface_color_format(state.color_format));
		descriptor->setWidth(state.width());
		descriptor->setHeight(state.height());
		descriptor->setSampleCount(state.sample_count());
		descriptor->setMipmapLevelCount(1);
		descriptor->setArrayLength(1);
	}
}

#endif // RSX_ENABLE_METAL4
