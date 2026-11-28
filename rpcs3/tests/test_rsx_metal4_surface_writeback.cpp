#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLSurface.h"

// The surface write-back is fed a byte range, not a row rectangle
// (surface_store.h:995-1014).  The clip below is what keeps a range that starts
// or ends inside a row from being dropped, and what keeps the destination column
// from being lost.
#ifdef RSX_ENABLE_METAL4

namespace
{
	constexpr u32 pitch = 256;
	constexpr u32 height = 4;
}

TEST(MTLSurfaceWriteback, AFullRowRangeKeepsEveryByte)
{
	const auto range = mtl::clip_surface_write_range(0, 2 * pitch, pitch, height);
	EXPECT_EQ(range.first_row, 0u);
	EXPECT_EQ(range.rows, 2u);
	EXPECT_EQ(range.row_offset, 0u);
	EXPECT_EQ(range.bytes, 2 * pitch);
}

TEST(MTLSurfaceWriteback, ARangeShorterThanARowIsOneRow)
{
	const auto range = mtl::clip_surface_write_range(0, 100, pitch, height);
	EXPECT_EQ(range.first_row, 0u);
	EXPECT_EQ(range.rows, 1u);
	EXPECT_EQ(range.row_offset, 0u);
	EXPECT_EQ(range.bytes, 100u);
}

TEST(MTLSurfaceWriteback, ARangeInsideARowKeepsItsColumn)
{
	const auto range = mtl::clip_surface_write_range(100, 100, pitch, height);
	EXPECT_EQ(range.first_row, 0u);
	EXPECT_EQ(range.rows, 1u);
	EXPECT_EQ(range.row_offset, 100u);
	EXPECT_EQ(range.bytes, 100u);
}

TEST(MTLSurfaceWriteback, ARangeAcrossARowBoundaryCarriesBothRows)
{
	const auto range = mtl::clip_surface_write_range(200, 200, pitch, height);
	EXPECT_EQ(range.first_row, 0u);
	EXPECT_EQ(range.rows, 2u);
	EXPECT_EQ(range.row_offset, 200u);
	EXPECT_EQ(range.bytes, 200u);
}

TEST(MTLSurfaceWriteback, ALengthThatEndsMidRowKeepsTheLastPartialRow)
{
	const auto range = mtl::clip_surface_write_range(0, pitch + 1, pitch, height);
	EXPECT_EQ(range.rows, 2u);
	EXPECT_EQ(range.bytes, pitch + 1);
}

TEST(MTLSurfaceWriteback, ARangePastTheLastRowIsClippedNotDropped)
{
	const auto range = mtl::clip_surface_write_range(0, 3 * pitch, pitch, 2);
	EXPECT_EQ(range.rows, 2u);
	EXPECT_EQ(range.bytes, 2 * pitch);

	const auto outside = mtl::clip_surface_write_range(2 * pitch, pitch, pitch, 2);
	EXPECT_EQ(outside.rows, 0u);
	EXPECT_EQ(outside.bytes, 0u);
}

TEST(MTLSurfaceWriteback, AZeroLengthOrPitchCopiesNothing)
{
	EXPECT_EQ(mtl::clip_surface_write_range(0, 0, pitch, height).rows, 0u);
	EXPECT_EQ(mtl::clip_surface_write_range(0, 16, 0, height).rows, 0u);
	EXPECT_EQ(mtl::clip_surface_write_range(0, 16, pitch, 0).rows, 0u);
}

#endif // RSX_ENABLE_METAL4
