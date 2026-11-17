#include <gtest/gtest.h>

#include "Utilities/jit_page_index.h"

// CPU-INV-0xx: the per-page invalidation index and its epoch (§8.1.4, §8.5.1).
// A link cell or an IBTC entry may only be trusted across an epoch that has not
// moved, so the epoch has to be bumped exactly once per invalidation and the
// index has to forget a module when that module is released.
namespace
{
	constexpr u32 guest_page = 0x1000;

	struct page_index_test : ::testing::Test
	{
		void SetUp() override { jit_page::reset(); }
		void TearDown() override { jit_page::reset(); }
	};
}

TEST_F(page_index_test, RecordsCoveragePerPage)
{
	jit_page::note_compiled(0x100000, guest_page, 7);

	const auto modules = jit_page::modules_over(0x100000 >> 12);
	ASSERT_EQ(modules.size(), 1u);
	EXPECT_EQ(modules[0], 7u);

	EXPECT_TRUE(jit_page::modules_over((0x200000 >> 12)).empty());
}

TEST_F(page_index_test, RecordingTheSameModuleTwiceDoesNotDuplicate)
{
	jit_page::note_compiled(0x100000, guest_page, 7);
	jit_page::note_compiled(0x100000, guest_page, 7);

	EXPECT_EQ(jit_page::modules_over(0x100000 >> 12).size(), 1u);
}

TEST_F(page_index_test, RangeCoveringSeveralPagesIsRecordedOnEach)
{
	jit_page::note_compiled(0x100000, guest_page * 4, 3);

	for (u32 page = 0x100000 >> 12; page < (0x100000 >> 12) + 4; page++)
	{
		const auto modules = jit_page::modules_over(page);
		ASSERT_EQ(modules.size(), 1u) << "page " << page;
		EXPECT_EQ(modules[0], 3u);
	}
}

TEST_F(page_index_test, SeveralModulesCanCoverOnePage)
{
	jit_page::note_compiled(0x100000, guest_page, 1);
	jit_page::note_compiled(0x100000 + 8, 4, 2);

	const auto modules = jit_page::modules_over(0x100000 >> 12);
	ASSERT_EQ(modules.size(), 2u);
	EXPECT_EQ(modules[0], 1u);
	EXPECT_EQ(modules[1], 2u);
}

TEST_F(page_index_test, InvalidationBumpsTheEpochOnce)
{
	const u64 before = jit_page::epoch();
	const u64 after = jit_page::note_invalidated(0x100000, guest_page * 2);

	EXPECT_GT(after, before);
	EXPECT_EQ(after, jit_page::epoch());
	EXPECT_EQ(jit_page::note_invalidated(0x100000, 4), after + 1);
}

TEST_F(page_index_test, InvalidationKeepsTheCoverageItInvalidated)
{
	// The recompile asks the index which implementation to replace, so the
	// coverage outlives the invalidation; only releasing the module drops it.
	jit_page::note_compiled(0x100000, guest_page, 5);
	jit_page::note_invalidated(0x100000, guest_page);

	EXPECT_EQ(jit_page::modules_over(0x100000 >> 12).size(), 1u);
}

TEST_F(page_index_test, ForgettingAModuleDropsItsPages)
{
	jit_page::note_compiled(0x100000, guest_page, 1);
	jit_page::note_compiled(0x200000, guest_page, 2);

	jit_page::forget_module(1);

	EXPECT_TRUE(jit_page::modules_over(0x100000 >> 12).empty());
	EXPECT_EQ(jit_page::modules_over(0x200000 >> 12).size(), 1u);
}

TEST_F(page_index_test, ForgettingAModuleOnASharedPageKeepsTheOther)
{
	jit_page::note_compiled(0x100000, guest_page, 1);
	jit_page::note_compiled(0x100000, guest_page, 2);

	jit_page::forget_module(1);

	const auto modules = jit_page::modules_over(0x100000 >> 12);
	ASSERT_EQ(modules.size(), 1u);
	EXPECT_EQ(modules[0], 2u);
}

TEST_F(page_index_test, ResetClearsCoverageAndTheEpoch)
{
	jit_page::note_compiled(0x100000, guest_page, 1);
	jit_page::note_invalidated(0x100000, guest_page);

	jit_page::reset();

	EXPECT_EQ(jit_page::epoch(), 1u);
	EXPECT_TRUE(jit_page::modules_over(0x100000 >> 12).empty());
}
