#include <gtest/gtest.h>

#include "Utilities/ppu_region_scan.h"

// CPU-REGION-0xx: loop-first region formation (§8.2.1). The rules under test are
// the ones that decide how much guest code shares one host implementation, so a
// mistake here shows up as duplicated code or a missing entry.
namespace
{
	using ppu_region::block;

	block straight(u32 addr, u32 size = 4)
	{
		block b;
		b.addr = addr;
		b.size = size;
		return b;
	}

	block branch(u32 addr, u32 target, u32 size = 4)
	{
		block b = straight(addr, size);
		b.has_target = true;
		b.target = target;
		return b;
	}
}

TEST(PpuRegionScan, StraightLineChainFormsOneRegion)
{
	const std::vector<block> blocks{straight(0x1000), straight(0x1010), straight(0x1020)};

	// Below the threshold a straight line stays a plain block, so ask for the
	// region form explicitly here; the threshold is covered separately.
	const auto regions = ppu_region::form(blocks, 256, 8);
	ASSERT_EQ(regions.size(), 1u);
	EXPECT_EQ(regions[0].entry, 0x1000u);
	EXPECT_EQ(regions[0].instructions, 12u);
	EXPECT_FALSE(regions[0].is_loop);
	ASSERT_EQ(regions[0].blocks.size(), 3u);
	EXPECT_EQ(regions[0].blocks[0], 0x1000u);
	EXPECT_EQ(regions[0].blocks[2], 0x1020u);
}

TEST(PpuRegionScan, ShortRegionIsDroppedUnlessItIsALoop)
{
	const std::vector<block> blocks{straight(0x1000), straight(0x1010)};

	EXPECT_TRUE(ppu_region::form(blocks, 256, 32).empty());
	EXPECT_EQ(ppu_region::form(blocks, 256, 8).size(), 1u);
}

TEST(PpuRegionScan, BackEdgeMakesItsTargetALoopHeader)
{
	const std::vector<block> blocks{branch(0x1000, 0x1010), branch(0x1010, 0x1000)};

	const auto headers = ppu_region::loop_headers(blocks);
	ASSERT_EQ(headers.size(), 1u);
	EXPECT_EQ(headers[0], 0x1000u);

	// A loop is formed even below the instruction threshold.
	const auto regions = ppu_region::form(blocks, 256, 4096);
	ASSERT_EQ(regions.size(), 1u);
	EXPECT_TRUE(regions[0].is_loop);
	EXPECT_EQ(regions[0].entry, 0x1000u);
	EXPECT_EQ(regions[0].blocks.size(), 2u);
}

TEST(PpuRegionScan, LoopHeadersAreOrderedByDescendingSpan)
{
	// 0x2000 .. 0x2008 is an 8-instruction loop, 0x1000 .. 0x1040 a 64-instruction
	// one, so the longer loop is formed first.
	const std::vector<block> blocks{
		branch(0x1000, 0x1040), straight(0x1010), straight(0x1020), straight(0x1030), branch(0x1040, 0x1000),
		branch(0x2000, 0x2008), branch(0x2008, 0x2000)};

	const auto headers = ppu_region::loop_headers(blocks);
	ASSERT_EQ(headers.size(), 2u);
	EXPECT_EQ(headers[0], 0x1000u);
	EXPECT_EQ(headers[1], 0x2000u);
}

TEST(PpuRegionScan, BlockWithTwoPredecessorsIsNotAbsorbed)
{
	// Two forward branches into 0x1010: it must keep its own implementation.
	const std::vector<block> blocks{
		branch(0x1000, 0x1010),
		branch(0x1004 + 0, 0x1010),
		straight(0x1010)};

	ppu_region::loop_headers(blocks);

	// 0x1000 and 0x1004 both branch to 0x1010, which therefore has two
	// predecessors and is an entry.
	const auto regions = ppu_region::form(blocks, 256, 0);
	ASSERT_FALSE(regions.empty());

	for (const auto& reg : regions)
	{
		if (reg.entry != 0x1010)
		{
			EXPECT_EQ(std::find(reg.blocks.begin(), reg.blocks.end(), 0x1010u), reg.blocks.end());
		}
	}
}

TEST(PpuRegionScan, WalkStopsAtTheInstructionBudget)
{
	const std::vector<block> blocks{straight(0x1000), straight(0x1010), straight(0x1020)};

	const auto regions = ppu_region::form(blocks, 8, 0);
	ASSERT_FALSE(regions.empty());

	// The first region takes as much as the budget allows; the block left over is
	// still an entry of its own.
	EXPECT_EQ(regions[0].entry, 0x1000u);
	EXPECT_EQ(regions[0].instructions, 8u);
	EXPECT_EQ(regions[0].blocks.size(), 2u);
}

TEST(PpuRegionScan, AnExitBlockEndsTheRegion)
{
	block exit_block = straight(0x1010);
	exit_block.is_exit = true;

	const std::vector<block> blocks{straight(0x1000), exit_block, straight(0x1020)};

	const auto regions = ppu_region::form(blocks, 256, 0);
	ASSERT_FALSE(regions.empty());

	for (const auto& reg : regions)
	{
		if (reg.entry == 0x1000)
		{
			// The exit block closes the region; the block after it is separate.
			EXPECT_EQ(std::find(reg.blocks.begin(), reg.blocks.end(), 0x1020u), reg.blocks.end());
		}
	}
}
