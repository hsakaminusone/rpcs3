#include "MTLNv3089Image.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "util/logs.hpp"

#include <algorithm>

namespace mtl
{
	image_in_state capture_image_in_state()
	{
		image_in_state state;

		state.source_dma = rsx::method_registers.blit_engine_input_location();
		state.source_offset = rsx::method_registers.blit_engine_input_offset();
		state.source_pitch = rsx::method_registers.blit_engine_input_pitch();
		state.source_width = rsx::method_registers.blit_engine_input_width();
		state.source_height = rsx::method_registers.blit_engine_input_height();

		const auto format = rsx::method_registers.blit_engine_src_color_format();
		if (format)
		{
			state.source_format = format;
			state.source_format_valid = true;
		}

		state.operation = rsx::method_registers.blit_engine_operation();
		state.origin = rsx::method_registers.blit_engine_input_origin();
		state.interpolator = rsx::method_registers.blit_engine_input_inter();
		state.surface = rsx::method_registers.blit_engine_context_surface();

		state.clip_x = rsx::method_registers.blit_engine_clip_x();
		state.clip_y = rsx::method_registers.blit_engine_clip_y();
		state.clip_width = rsx::method_registers.blit_engine_clip_width();
		state.clip_height = rsx::method_registers.blit_engine_clip_height();

		state.out_x = rsx::method_registers.blit_engine_output_x();
		state.out_y = rsx::method_registers.blit_engine_output_y();
		state.out_width = rsx::method_registers.blit_engine_output_width();
		state.out_height = rsx::method_registers.blit_engine_output_height();

		state.scale_x = rsx::method_registers.blit_engine_ds_dx();
		state.scale_y = rsx::method_registers.blit_engine_dt_dy();

		return state;
	}

	bool is_supported_operation(rsx::blit_engine::transfer_operation operation)
	{
		return operation == rsx::blit_engine::transfer_operation::srccopy;
	}

	MTL::PixelFormat get_source_pixel_format(rsx::blit_engine::transfer_source_format format)
	{
		switch (format)
		{
		case rsx::blit_engine::transfer_source_format::r5g6b5:
			return MTL::PixelFormat::PixelFormatB5G6R5Unorm;
		case rsx::blit_engine::transfer_source_format::a8r8g8b8:
		case rsx::blit_engine::transfer_source_format::x8r8g8b8:
			// Alpha has no meaning in either format, so x8r8g8b8 shares the enum
			// (nv3089.cpp:263-270).
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		default:
			rsx_log.error("Metal 4: unsupported blit source format 0x%x", static_cast<u32>(format));
			return MTL::PixelFormat::PixelFormatBGRA8Unorm;
		}
	}

	u32 get_source_bpp(rsx::blit_engine::transfer_source_format format)
	{
		return format == rsx::blit_engine::transfer_source_format::r5g6b5 ? 2 : 4;
	}

	MTL::SamplerMinMagFilter get_interpolation_filter(rsx::blit_engine::transfer_interpolator interpolator)
	{
		return interpolator == rsx::blit_engine::transfer_interpolator::foh
			? MTL::SamplerMinMagFilterLinear
			: MTL::SamplerMinMagFilterNearest;
	}

	bool resolve_clip(const image_in_state& state, u16& clip_width, u16& clip_height)
	{
		clip_width = std::min(state.clip_width, state.out_width);
		clip_height = std::min(state.clip_height, state.out_height);
		return clip_width != 0 && clip_height != 0;
	}

	bool uses_swizzle_destination(const image_in_state& state)
	{
		return state.surface == rsx::blit_engine::context_surface::swizzle2d;
	}

	bool is_mirrored_x(const image_in_state& state)
	{
		return state.scale_x < 0.0f;
	}

	bool is_mirrored_y(const image_in_state& state)
	{
		return state.scale_y < 0.0f;
	}
}

#endif // RSX_ENABLE_METAL4
