#include "MTLSurfaceState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Common/TextureUtils.h"

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

	bool surface_state::clear_covers_attachment() const
	{
		return clear_x == 0 && clear_y == 0 && clear_width >= clip_width && clear_height >= clip_height;
	}

	surface_state capture_surface_state()
	{
		surface_state state;

		state.color_format = rsx::method_registers.surface_color();
		state.depth_format = rsx::method_registers.surface_depth_fmt();
		state.antialias = rsx::method_registers.surface_antialias();
		state.raster_type = rsx::method_registers.surface_type();

		state.color_target = rsx::method_registers.surface_color_target();
		state.color_target_mask = rsx::method_registers.surface_color_target_mask();

		state.clip_x = rsx::method_registers.surface_clip_origin_x();
		state.clip_y = rsx::method_registers.surface_clip_origin_y();
		state.clip_width = rsx::method_registers.surface_clip_width();
		state.clip_height = rsx::method_registers.surface_clip_height();

		state.clear_x = rsx::method_registers.scissor_origin_x();
		state.clear_y = rsx::method_registers.scissor_origin_y();
		state.clear_width = rsx::method_registers.scissor_width();
		state.clear_height = rsx::method_registers.scissor_height();

		const u32 window_offset_x = rsx::method_registers.window_offset_x();
		const u32 window_offset_y = rsx::method_registers.window_offset_y();
		const bool shifted = window_offset_x || window_offset_y;

		const u32 color_texel_size = rsx::get_format_block_size_in_bytes(state.color_format);
		const u32 depth_texel_size = rsx::get_format_block_size_in_bytes(state.depth_format);

		for (u32 index = 0; index < 4; ++index)
		{
			auto& surface = state.color[index];
			surface.address = rsx::get_address(rsx::method_registers.surface_offset(index), rsx::method_registers.surface_dma(index));
			surface.pitch = rsx::method_registers.surface_pitch(index);

			// SET_WINDOW_OFFSET shifts the base address before the identity lookup
			// (RSXThread.cpp:1844; LLD-surface §3.4).
			if (shifted && surface.address)
			{
				surface.address += (surface.pitch * window_offset_y) + (color_texel_size * window_offset_x);
			}
		}

		state.zeta.address = rsx::get_address(rsx::method_registers.surface_z_offset(), rsx::method_registers.surface_z_dma());
		state.zeta.pitch = rsx::method_registers.surface_z_pitch();

		if (shifted && state.zeta.address)
		{
			state.zeta.address += (state.zeta.pitch * window_offset_y) + (depth_texel_size * window_offset_x);
		}

		state.clear_color = MTL::ClearColor::Make(
			rsx::method_registers.clear_color_r() / 255.0,
			rsx::method_registers.clear_color_g() / 255.0,
			rsx::method_registers.clear_color_b() / 255.0,
			rsx::method_registers.clear_color_a() / 255.0);

		state.clear_depth = rsx::get_depth_clear_value(state.depth_format,
			rsx::method_registers.z_clear_value(rsx::is_depth_stencil_format(state.depth_format)));
		state.clear_stencil = rsx::method_registers.stencil_clear_value();

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
