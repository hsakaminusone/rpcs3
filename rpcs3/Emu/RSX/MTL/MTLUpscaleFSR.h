#pragma once

#include "MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

#include <string>

// The pure half of the Metal FSR 1 pass: the lane geometry of the shared
// ubershader, the pass's own binding slots, and the two predicates that decide
// whether the pass runs.  Nothing here touches Metal, so the tables the MSL
// kernels index are the tables the unit tests check, and the shader text is
// generated from them rather than restating them.
namespace mtl::fsr
{
	// The reference workgroup: 64 lanes (the ubershader's `local_size_x`), each
	// writing the four texels of one 16x16 tile.
	inline constexpr u32 lane_count = 64;
	inline constexpr u32 tile_extent = 16;

	// The four texels a lane writes, in the ubershader's CurrFilter order.
	inline constexpr u32 tile_point_count = 4;

	// The pass owns both argument tables, so these numbers only have to agree
	// with the generated MSL below.
	inline constexpr u32 constants_slot = 0;
	inline constexpr u32 source_slot = 0;
	inline constexpr u32 destination_slot = 1;
	inline constexpr u32 sampler_slot = 0;

	struct point
	{
		u32 x;
		u32 y;

		friend constexpr bool operator==(const point&, const point&) = default;
	};

	// FFX's ARmp8x8: bits 1..3 of the lane are x, and the low lane bit replaces
	// the low bit of bits 3..5 to make y (fsr_ubershader.glsl, `sp`/`ep`).
	constexpr point lane_to_tile(u32 lane) noexcept
	{
		return point{ (lane >> 1u) & 7u, (((lane >> 3u) & 7u) & ~1u) | (lane & 1u) };
	}

	// The ubershader's four CurrFilter calls, in its order: this point, +8 in x,
	// +8 in both, then -8 in x.  Together with the 8x8 lane grid this covers one
	// 16x16 tile with every texel exactly once.
	inline constexpr point tile_offsets[tile_point_count] = {
		{ 0u, 0u }, { 8u, 0u }, { 8u, 8u }, { 0u, 8u }
	};

	// Threadgroups needed to cover an image of `extent` texels.
	constexpr u32 group_count(u32 extent) noexcept
	{
		return (extent + tile_extent - 1u) / tile_extent;
	}

	// FSR 1 is an enlargement filter: both axes must grow, or the caller keeps
	// its own filter (fsr_pass.cpp:304).
	constexpr bool is_enlargement(u32 input_width, u32 input_height, u32 width, u32 height) noexcept
	{
		return input_width < width && input_height < height;
	}

	// Everything `fsr_upscale_pass::run` needs that is not a Metal object: the
	// source must be a plain single-sample 2D image and all six extents must be
	// non-zero.
	constexpr bool can_upscale(bool single_sample_2d, bool has_source, u32 input_width,
		u32 input_height, u32 width, u32 height) noexcept
	{
		return single_sample_2d && has_source && input_width && input_height && width && height;
	}

	// 0 is the sharpest with 2 being the chosen limit; each progressive unit
	// halves the sharpening intensity (fsr_pass.cpp:193-199).
	constexpr f32 rcas_attenuation(u32 intensity) noexcept
	{
		return 2.0f - (static_cast<f32>(intensity) / 50.0f);
	}

	// The two kernels as MSL, with the lane table and tile offsets generated from
	// the functions above and the binding slots substituted.
	std::string library_source();
}

#endif // RSX_ENABLE_METAL4
