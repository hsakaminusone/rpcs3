#include <gtest/gtest.h>

#include "Utilities/jit_code_space.h"

// CPU-INV-1xx: code-space reuse (§8.5.1). The rule that matters is the epoch
// gate: reusing a chunk that may still have live code pointing into it is a jump
// into garbage, and every other property here is bookkeeping.
namespace
{
	struct code_space_test : ::testing::Test
	{
		void SetUp() override { jit_code_space::reset(); }
		void TearDown() override { jit_code_space::reset(); }
	};

	u8* fake(u64 id)
	{
		return reinterpret_cast<u8*>(id * jit_code_space::chunk_size);
	}
}

TEST_F(code_space_test, AcquireTakesAFreeChunk)
{
	jit_code_space::add(fake(1));

	u8* chunk = jit_code_space::acquire(1);
	ASSERT_EQ(chunk, fake(1));
	EXPECT_EQ(jit_code_space::live_count(), 1u);
	EXPECT_EQ(jit_code_space::free_count(), 0u);
}

TEST_F(code_space_test, AcquireFailsWhenEveryChunkIsLive)
{
	jit_code_space::add(fake(1));
	jit_code_space::add(fake(2));

	ASSERT_NE(jit_code_space::acquire(1), nullptr);
	ASSERT_NE(jit_code_space::acquire(1), nullptr);

	EXPECT_EQ(jit_code_space::acquire(1), nullptr);
}

TEST_F(code_space_test, ReleasedChunkIsReused)
{
	jit_code_space::add(fake(1));

	u8* first = jit_code_space::acquire(5);
	ASSERT_EQ(first, fake(1));

	jit_code_space::release(first, 6);

	EXPECT_EQ(jit_code_space::acquire(7), fake(1));
}

TEST_F(code_space_test, ChunkWrittenAtTheCallersEpochIsNotReused)
{
	jit_code_space::add(fake(1));

	u8* chunk = jit_code_space::acquire(10);
	jit_code_space::release(chunk, 10);

	// Something compiled at epoch 10 may still be reachable.
	EXPECT_EQ(jit_code_space::acquire(10), nullptr);

	// Once the caller is strictly newer, the chunk is safe.
	EXPECT_EQ(jit_code_space::acquire(11), fake(1));
}

TEST_F(code_space_test, OldestFreeChunkIsChosen)
{
	jit_code_space::add(fake(1));
	jit_code_space::add(fake(2));

	u8* a = jit_code_space::acquire(1);
	u8* b = jit_code_space::acquire(1);

	jit_code_space::release(a, 2);
	jit_code_space::release(b, 3);

	EXPECT_EQ(jit_code_space::acquire(4), a);
	EXPECT_EQ(jit_code_space::acquire(4), b);
}

TEST_F(code_space_test, ReleasingAnUnknownChunkIsIgnored)
{
	jit_code_space::add(fake(1));

	jit_code_space::release(fake(9), 4);

	EXPECT_EQ(jit_code_space::free_count(), 1u);
}

TEST_F(code_space_test, ResetForgetsEverything)
{
	jit_code_space::add(fake(1));
	jit_code_space::acquire(1);

	jit_code_space::reset();

	EXPECT_EQ(jit_code_space::live_count(), 0u);
	EXPECT_EQ(jit_code_space::free_count(), 0u);
	EXPECT_EQ(jit_code_space::acquire(1), nullptr);
}
