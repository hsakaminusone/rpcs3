#include <gtest/gtest.h>
#include "Emu/RSX/RSXFIFO.h"

// The parser has one cursor and one producer position (PUT). PUT is the
// producer's watermark and it is exclusive: the word at PUT is not published.
// The parser reads while it is not on PUT and idles when it is.
//
// There is deliberately no exception for a word that looks like a link. That
// exception was tried and failed on device (2026-11-11, Demon's Souls): the
// parser parked at 0x3f454, where PUT had just arrived, read the previous-lap
// word 0x20040000 as OLD_JUMP|0x40000, walked unwritten memory and desynced at
// 0x19300000. A word at PUT is stale until the producer writes it.
namespace rsx::FIFO
{
	namespace
	{
		using action = cursor_rules::action;

		constexpr u32 boot = 0x1000;
		constexpr u32 ring_start = 0x8000;
		constexpr u32 ring_end = 0x5fefc4;
	}

	TEST(FifoStreams, CursorBehindProducerReads)
	{
		// The normal case: the producer has published past the cursor.
		EXPECT_EQ(cursor_rules::decide(boot, boot + 0x3a0), action::read);
		EXPECT_EQ(cursor_rules::decide(boot, boot + 0x40), action::read);
	}

	TEST(FifoStreams, CursorAheadOfProducerStillReads)
	{
		// A jump target the producer has not reached yet. Hardware walks it;
		// the read is not gated by PUT.
		constexpr u32 park = 0x3020300;
		EXPECT_EQ(cursor_rules::decide(park, ring_start), action::read);
	}

	TEST(FifoStreams, CursorOnProducerIdles)
	{
		// PUT is exclusive. Nothing at the cursor is published.
		EXPECT_EQ(cursor_rules::decide(0x5fd77c, 0x5fd77c), action::idle);
		EXPECT_EQ(cursor_rules::decide(ring_end, ring_end), action::idle);
	}

	TEST(FifoStreams, StaleLinkAtPutIsNotConsumed)
	{
		// The 2026-11-11 regression. The word at the producer's write head was
		// 0x20040000 - a syntactically valid OLD_JUMP to 0x40000, but stale
		// previous-lap data. It must not be consumed: the decision does not
		// look at the word at all.
		EXPECT_EQ(cursor_rules::decide(0x3f454, 0x3f454), action::idle);
	}

	TEST(FifoStreams, PublishedLinkIsReadNormally)
	{
		// Once the producer has published past the link it is an ordinary
		// command and the normal read path follows it.
		EXPECT_EQ(cursor_rules::decide(ring_end, ring_end + 4), action::read);
	}

	// Recovery must not trust a bare address. The guest recycles the ring, so a
	// position that was a packet boundary when the rollback point was recorded
	// may hold the next generation's data by the time recovery runs. RR7
	// re-entered 0x10a588c twenty times in two seconds for exactly that reason.

	TEST(FifoRecovery, RollsBackToNewestIntactBoundary)
	{
		// Newest first. The two newest headers were rewritten by the guest (the
		// ring now holds 0x02062000 there); 0x1074 is the newest one that is
		// still the packet that was parsed.
		const packet_boundary executed[] = {
			{0x1084, 0x00040068},
			{0x107c, 0x00040064},
			{0x1074, 0x00041d70},
			{0x106c, 0x00041d6c},
		};

		const auto read_word = [](u32 pos) -> u32
		{
			switch (pos)
			{
			case 0x1074: return 0x00041d70;
			case 0x106c: return 0x00041d6c;
			default: return 0x02062000;
			}
		};

		EXPECT_EQ(recovery_rules::select(executed, 4, 0x108c, read_word), 0x1074u);
	}

	TEST(FifoRecovery, NeverReentersTheFailingPacket)
	{
		// A boundary at the failing position is not a rollback target even when
		// its header still matches: the parse is deterministic, so the packet
		// fails identically and the recovery becomes a loop.
		const packet_boundary executed[] = {{0x108c, 0x02062000}};
		const auto read_word = [](u32) -> u32 { return 0x02062000; };

		EXPECT_EQ(recovery_rules::select(executed, 1, 0x108c, read_word), umax);
	}

	TEST(FifoRecovery, IgnoresBoundariesAboveTheFailure)
	{
		// A boundary in the buffer the parser jumped out of is not a rollback
		// target: resuming there would re-execute a chunk that is already done.
		const packet_boundary executed[] = {{0x10a0000, 0x00040064}};
		const auto read_word = [](u32) -> u32 { return 0x00040064; };

		EXPECT_EQ(recovery_rules::select(executed, 1, 0xcb588c, read_word), umax);
	}

	TEST(FifoRecovery, AllowsBoundaryWhenHeaderWasRewrittenAway)
	{
		// The word is the discriminator, not the address: the same position is
		// usable again once the producer has written a packet there.
		const packet_boundary executed[] = {{0x1074, 0x00041d70}};
		const auto read_word = [](u32) -> u32 { return 0x00041bc8; };

		EXPECT_EQ(recovery_rules::select(executed, 1, 0x108c, read_word), umax);
	}
}
