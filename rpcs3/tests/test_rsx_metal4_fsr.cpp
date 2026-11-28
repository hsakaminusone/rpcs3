#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLUpscaleFSR.h"

// The pure half of the Metal FSR 1 pass (MTLUpscaleFSR.h).
//
// FSR is defined by its taps, and the taps are indexed by the ubershader's lane
// geometry: 64 lanes write a 16x16 tile as four points each, and the MSL kernels
// index tables that are generated from the functions checked here.  A wrong
// mapping is not a cosmetic bug, it is a silently shuffled image, so the bijection
// and the exact coverage are checked rather than assumed.  The shader text is
// checked for the same reason: it is assembled from those tables, and a stale
// token or a moved binding is a compile error at runtime, long after startup.
#ifdef RSX_ENABLE_METAL4

#include <array>
#include <set>
#include <string>
#include <utility>

namespace
{
	std::string table_body(const std::string& source, const std::string& header)
	{
		const auto start = source.find(header);

		if (start == std::string::npos)
		{
			return {};
		}

		const auto open = source.find('{', start);
		const auto close = source.find('}', open);

		if (open == std::string::npos || close == std::string::npos || close <= open)
		{
			return {};
		}

		return source.substr(open + 1, close - open - 1);
	}

	usz count_text(const std::string& text, const std::string& token)
	{
		usz hits = 0;

		for (usz pos = text.find(token); pos != std::string::npos; pos = text.find(token, pos + token.size()))
		{
			++hits;
		}

		return hits;
	}

	std::string point_text(const mtl::fsr::point& point)
	{
		return "uint2(" + std::to_string(point.x) + "u, " + std::to_string(point.y) + "u)";
	}
}

TEST(MTLFsr, LaneToTileIsABijectionOntoTheEightByEightGrid)
{
	std::set<std::pair<u32, u32>> points;

	for (u32 lane = 0; lane < mtl::fsr::lane_count; ++lane)
	{
		const mtl::fsr::point tile = mtl::fsr::lane_to_tile(lane);

		ASSERT_LT(tile.x, 8u) << "lane " << lane;
		ASSERT_LT(tile.y, 8u) << "lane " << lane;
		points.emplace(tile.x, tile.y);
	}

	// One lane per point: no duplicate, no hole.
	EXPECT_EQ(points.size(), mtl::fsr::lane_count);
	EXPECT_EQ(mtl::fsr::lane_count, 8u * 8u);

	for (u32 y = 0; y < 8; ++y)
	{
		for (u32 x = 0; x < 8; ++x)
		{
			EXPECT_TRUE(points.contains({ x, y })) << "missing " << x << "," << y;
		}
	}
}

TEST(MTLFsr, FourTilePointsCoverEveryTexelOfA16x16TileExactlyOnce)
{
	ASSERT_EQ(mtl::fsr::tile_point_count, 4u);

	std::array<u32, 16u * 16u> hits{};

	for (u32 lane = 0; lane < mtl::fsr::lane_count; ++lane)
	{
		const mtl::fsr::point base = mtl::fsr::lane_to_tile(lane);

		for (u32 index = 0; index < mtl::fsr::tile_point_count; ++index)
		{
			const u32 x = base.x + mtl::fsr::tile_offsets[index].x;
			const u32 y = base.y + mtl::fsr::tile_offsets[index].y;

			ASSERT_LT(x, 16u) << "lane " << lane << " point " << index;
			ASSERT_LT(y, 16u) << "lane " << lane << " point " << index;
			++hits[y * 16u + x];
		}
	}

	for (u32 y = 0; y < 16; ++y)
	{
		for (u32 x = 0; x < 16; ++x)
		{
			EXPECT_EQ(hits[y * 16u + x], 1u) << "texel " << x << "," << y;
		}
	}
}

TEST(MTLFsr, GroupCountRoundsUpAndHandlesZero)
{
	EXPECT_EQ(mtl::fsr::group_count(0), 0u);
	EXPECT_EQ(mtl::fsr::group_count(1), 1u);
	EXPECT_EQ(mtl::fsr::group_count(mtl::fsr::tile_extent), 1u);
	EXPECT_EQ(mtl::fsr::group_count(mtl::fsr::tile_extent + 1u), 2u);
	EXPECT_EQ(mtl::fsr::group_count(1280), 80u);
	EXPECT_EQ(mtl::fsr::group_count(720), 45u);
	EXPECT_EQ(mtl::fsr::group_count(1920), 120u);
	EXPECT_EQ(mtl::fsr::group_count(1080), 68u);
}

TEST(MTLFsr, IsEnlargementRequiresBothAxesToGrow)
{
	// 1280x720 guest frame into a 1920x1080 output rectangle.
	EXPECT_TRUE(mtl::fsr::is_enlargement(1280, 720, 1920, 1080));

	// Already at the output size: the compose's own filter is the fallback
	// (fsr_pass.cpp:304).
	EXPECT_FALSE(mtl::fsr::is_enlargement(1920, 1080, 1920, 1080));

	// One axis is already large enough: FSR does not run.
	EXPECT_FALSE(mtl::fsr::is_enlargement(1920, 720, 1920, 1080));
	EXPECT_FALSE(mtl::fsr::is_enlargement(1280, 1080, 1920, 1080));

	// Shrinking is never an enlargement.
	EXPECT_FALSE(mtl::fsr::is_enlargement(1920, 1080, 1280, 720));
	EXPECT_FALSE(mtl::fsr::is_enlargement(1281, 721, 1280, 720));

	// A zero extent cannot enlarge anything.
	EXPECT_FALSE(mtl::fsr::is_enlargement(0, 0, 0, 0));
}

TEST(MTLFsr, CanUpscaleNeedsAPlainSourceAndSixNonZeroExtents)
{
	// The live case: a single-sample 2D source being enlarged.
	EXPECT_TRUE(mtl::fsr::can_upscale(true, true, 1280, 720, 1920, 1080));

	// A multisample or array source has no plain 2D gather.
	EXPECT_FALSE(mtl::fsr::can_upscale(false, true, 1280, 720, 1920, 1080));

	// No source image at all.
	EXPECT_FALSE(mtl::fsr::can_upscale(true, false, 1280, 720, 1920, 1080));

	// Any missing extent declines rather than dispatching a degenerate grid.
	EXPECT_FALSE(mtl::fsr::can_upscale(true, true, 0, 720, 1920, 1080));
	EXPECT_FALSE(mtl::fsr::can_upscale(true, true, 1280, 0, 1920, 1080));
	EXPECT_FALSE(mtl::fsr::can_upscale(true, true, 1280, 720, 0, 1080));
	EXPECT_FALSE(mtl::fsr::can_upscale(true, true, 1280, 720, 1920, 0));

	// Equal sizes are still encodable; the enlargement gate is the caller's
	// (fsr::is_enlargement), not this predicate's.
	EXPECT_TRUE(mtl::fsr::can_upscale(true, true, 1920, 1080, 1920, 1080));
}

TEST(MTLFsr, RcasAttenuationHalvesPerUnitAndSpansTheFullRange)
{
	EXPECT_FLOAT_EQ(mtl::fsr::rcas_attenuation(0), 2.0f);
	EXPECT_FLOAT_EQ(mtl::fsr::rcas_attenuation(25), 1.5f);
	EXPECT_FLOAT_EQ(mtl::fsr::rcas_attenuation(50), 1.0f);
	EXPECT_FLOAT_EQ(mtl::fsr::rcas_attenuation(75), 0.5f);
	EXPECT_FLOAT_EQ(mtl::fsr::rcas_attenuation(100), 0.0f);

	// Monotone: more sharpening never means more attenuation.
	for (u32 intensity = 1; intensity <= 100; ++intensity)
	{
		EXPECT_LE(mtl::fsr::rcas_attenuation(intensity), mtl::fsr::rcas_attenuation(intensity - 1));
	}
}

TEST(MTLFsr, LibrarySourceIsFullySubstituted)
{
	const std::string source = mtl::fsr::library_source();

	ASSERT_FALSE(source.empty());

	// Every generated table and every slot token has been replaced.
	EXPECT_EQ(source.find('@'), std::string::npos);

	EXPECT_NE(source.find("kernel void fsr_easu_kernel"), std::string::npos);
	EXPECT_NE(source.find("kernel void fsr_rcas_kernel"), std::string::npos);

	// The RCAS negative-lobe cap (FSR_RCAS_LIMIT) survives into the shader.
	EXPECT_NE(source.find("-0.1875f"), std::string::npos);

	// Both kernels read the generated tables.
	EXPECT_NE(source.find("fsr_lane_tile[lane.x]"), std::string::npos);
	EXPECT_NE(source.find("fsr_tile_offsets[index]"), std::string::npos);
	EXPECT_NE(source.find("fsr_tile_point_count"), std::string::npos);

	// The bindings are the slots this header and the pass agree on.
	EXPECT_NE(source.find("[[buffer(" + std::to_string(mtl::fsr::constants_slot) + ")]]"), std::string::npos);
	EXPECT_NE(source.find("[[texture(" + std::to_string(mtl::fsr::source_slot) + ")]]"), std::string::npos);
	EXPECT_NE(source.find("[[texture(" + std::to_string(mtl::fsr::destination_slot) + ")]]"), std::string::npos);
	EXPECT_NE(source.find("[[sampler(" + std::to_string(mtl::fsr::sampler_slot) + ")]]"), std::string::npos);

	// Both kernels carry the constants buffer and both textures.
	EXPECT_EQ(count_text(source, "[[buffer("), 2u);
	EXPECT_EQ(count_text(source, "[[texture("), 4u);
	EXPECT_EQ(count_text(source, "[[sampler("), 1u);
}

TEST(MTLFsr, LibrarySourceCarriesTheGeneratedLaneTable)
{
	const std::string source = mtl::fsr::library_source();
	const std::string body = table_body(source,
		"constant uint2 fsr_lane_tile[" + std::to_string(mtl::fsr::lane_count) + "]");

	ASSERT_FALSE(body.empty());
	EXPECT_EQ(count_text(body, "uint2("), mtl::fsr::lane_count);

	// One entry per lane, and it is the entry this header computes.
	for (u32 lane = 0; lane < mtl::fsr::lane_count; ++lane)
	{
		EXPECT_NE(body.find(point_text(mtl::fsr::lane_to_tile(lane))), std::string::npos)
			<< "lane " << lane;
	}

	// The table is 8x8 points, so every entry is a pair of small unsigned values.
	EXPECT_EQ(count_text(body, "u, "), mtl::fsr::lane_count);
}

TEST(MTLFsr, LibrarySourceCarriesTheGeneratedTileOffsets)
{
	const std::string source = mtl::fsr::library_source();
	const std::string body = table_body(source,
		"constant uint2 fsr_tile_offsets[" + std::to_string(mtl::fsr::tile_point_count) + "]");

	ASSERT_FALSE(body.empty());
	EXPECT_EQ(count_text(body, "uint2("), mtl::fsr::tile_point_count);

	for (u32 index = 0; index < mtl::fsr::tile_point_count; ++index)
	{
		EXPECT_NE(body.find(point_text(mtl::fsr::tile_offsets[index])), std::string::npos)
			<< "offset " << index;
	}

	// The kernels' loop bound is generated from the same count.
	EXPECT_NE(source.find("constant uint fsr_tile_point_count = " +
		std::to_string(mtl::fsr::tile_point_count) + ";"), std::string::npos);
}

#endif // RSX_ENABLE_METAL4
