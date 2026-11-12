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
}
