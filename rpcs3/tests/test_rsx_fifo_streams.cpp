#include <gtest/gtest.h>
#include "Emu/RSX/RSXFIFO.h"

// The parser follows the published end of the command stream it is in, not the
// single guest PUT register. These cases come from Ridge Racer 7, which keeps
// two command buffers about 4 MB apart and alternates PUT between them while
// the parser is still draining the other one.
namespace rsx::FIFO
{
	namespace
	{
		constexpr u32 boot = 0x1000;
		constexpr u32 stream_a = 0xcb0000;
		constexpr u32 stream_b = 0x10a0000;
	}

	TEST(FifoStreams, BootStreamFollowsPut)
	{
		stream_tracker streams;
		streams.reset(boot);

		streams.note_put(boot + 0x3a0, boot);
		EXPECT_EQ(streams.frontier(boot + 0x3a0, boot), boot + 0x3a0);

		// The guest publishes the rest of the boot stream in one move. That is a
		// legitimate advance for a linear stream and must not be treated as a
		// foreign buffer.
		streams.note_put(stream_a + 0x9d4, boot + 0x53c);
		EXPECT_EQ(streams.frontier(stream_a + 0x9d4, boot + 0x53c), stream_a + 0x9d4);

		// The jump enters the first real command buffer and adopts that PUT.
		streams.on_jump(stream_a, stream_a + 0x9d4);
		EXPECT_EQ(streams.frontier(stream_a + 0x9d4, stream_a), stream_a + 0x9d4);
	}

	TEST(FifoStreams, ForeignPutDoesNotAdvanceCurrentStream)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x9d4, boot);
		streams.on_jump(stream_a, stream_a + 0x9d4);

		// Enter B and publish A from B.
		streams.note_put(stream_b + 0xe20, stream_a);
		streams.on_jump(stream_b, stream_b + 0xe20);
		streams.note_put(stream_a + 0x2840, stream_b);

		// The parser is at the published end of A. B's PUT must not make the word
		// at that position readable: it was not written for this pass.
		EXPECT_EQ(streams.frontier(stream_b + 0x28cc, stream_a + 0x2840), stream_a + 0x2840);

		// The guest then finishes the A fill and moves PUT inside A. That is the
		// same stream, so the parser resumes at the old frontier word.
		streams.note_put(stream_a + 0x28cc, stream_b);
		EXPECT_EQ(streams.frontier(stream_b + 0x28cc, stream_a + 0x2840), stream_a + 0x28cc);
	}

	TEST(FifoStreams, ParkedStreamDoesNotWalkIntoStaleTail)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x9d4, boot);
		streams.on_jump(stream_a, stream_a + 0x9d4);
		streams.note_put(stream_b + 0xe20, stream_a);
		streams.on_jump(stream_b, stream_b + 0xe20);

		// A is filled up to this point and the producer has moved on to B. The
		// bytes past it are stale data from an earlier pass, not this one.
		streams.note_put(stream_a + 0xf0dc, stream_b);
		EXPECT_EQ(streams.frontier(stream_b + 0x2000, stream_a + 0xf0dc), stream_a + 0xf0dc);
	}

	TEST(FifoStreams, FrontierAtPutIsTheLinkPosition)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x9d4, boot);
		streams.on_jump(stream_a, stream_a + 0x9d4);

		// The guest publishes a chunk by pointing PUT at its final link. The
		// parser is then standing on exactly that position, so the frontier and
		// PUT are the same address and the link word there is readable. A data
		// word at the same position is not.
		EXPECT_EQ(streams.frontier(stream_a + 0x9d4, stream_a + 0x9d4), stream_a + 0x9d4);
	}

	TEST(FifoStreams, PendingPutSeedsFirstEntry)
	{
		stream_tracker streams;
		streams.reset(boot);

		// PUT names B before the parser has ever jumped there.
		streams.note_put(stream_b + 0x1000, boot);
		streams.on_jump(stream_b, boot);

		EXPECT_EQ(streams.frontier(boot, stream_b), stream_b + 0x1000);
	}

	TEST(FifoStreams, CallUsesLinearRuleAndReturnRestoresStream)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x1000, boot);
		streams.on_jump(stream_a, stream_a + 0x1000);

		const u32 put = stream_b + 0x2000;

		// Static subroutine below every command stream: fully published already.
		streams.on_call(0xca8180, stream_a + 0x100);
		EXPECT_EQ(streams.frontier(put, 0xca8180), put);

		streams.on_return();
		EXPECT_EQ(streams.frontier(put, stream_a + 0x100), stream_a + 0x1000);
	}

	TEST(FifoStreams, RecycledBufferDoesNotRewindBehindParser)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x9d4, boot);
		streams.on_jump(stream_a, stream_a + 0x9d4);
		streams.note_put(stream_b + 0xe20, stream_a);
		streams.on_jump(stream_b, stream_b + 0xe20);
		streams.note_put(stream_a + 0x2000, stream_b);

		// The parser is at 0x1800. A fill that ends behind it is a recycle of the
		// buffer for a later pass and must not shorten what is already published.
		streams.note_put(stream_a + 0x1000, stream_a + 0x1800);
		EXPECT_EQ(streams.frontier(stream_b, stream_a + 0x1800), stream_a + 0x2000);

		// A fill that still covers the parser is a real advance of this stream.
		streams.note_put(stream_a + 0x1900, stream_a + 0x1800);
		EXPECT_EQ(streams.frontier(stream_b, stream_a + 0x1800), stream_a + 0x1900);
	}

	TEST(FifoStreams, ParkAreaAboveStreamsIsReadUnbounded)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x6264, boot);
		streams.on_jump(stream_a, stream_a + 0x6264);

		// Ridge Racer 7 jumps to 0x3020300 with a NOP there and PUT still in the
		// other buffer, then patches the area. The hardware reads the target, so a
		// frontier taken from the target would park on the NOP for ever.
		constexpr u32 park = 0x3020300;
		streams.on_jump(park, stream_b + 0xb0e0);
		EXPECT_EQ(streams.frontier(stream_b + 0xb0e0, park), umax);

		// A jump back into the published buffer keeps its own frontier.
		streams.on_jump(stream_a, stream_b + 0xb0e0);
		EXPECT_EQ(streams.frontier(stream_b + 0xb0e0, stream_a + 0x100), stream_a + 0x6264);
	}

	TEST(FifoStreams, FrontierLinkTestAcceptsOnlyGuestLinkEncodings)
	{
		// The parser consumes the word at a stream's published end only when it
		// is the link the guest writes there. GET keeps that word unacknowledged
		// until the parser takes it, so it is still the guest's link and never
		// next-pass data. Accepting a data word on its encoding alone (the old
		// L138 failure) would jump into guest RAM.
		EXPECT_TRUE(stream_tracker::is_link(0x210a0000));                 // old jump
		EXPECT_TRUE(stream_tracker::is_link(RSX_METHOD_CALL_CMD | 0x1234)); // call
		EXPECT_TRUE(stream_tracker::is_link(RSX_METHOD_RETURN_CMD));      // return

		EXPECT_FALSE(stream_tracker::is_link(0x00040064));                // method
		EXPECT_FALSE(stream_tracker::is_link(0x00000030));                // data
		EXPECT_FALSE(stream_tracker::is_link(0x00002000));                // data
	}

	TEST(FifoStreams, ParkedStreamIsAbandonedWhenProducerPublishesTheOtherBuffer)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x1000, boot);
		streams.on_jump(stream_a, stream_a + 0x1000);

		// The parser consumed A and entered B. B's chunk ends mid-command: the
		// producer pointed PUT at the header's payload and then moved on, so the
		// word at B's frontier is data and the link is never written. Waiting for
		// it parks the parser for ever and keeps GET inside the buffer the guest
		// wants back.
		streams.note_put(stream_b + 0x1000, stream_a + 0x1000);
		streams.on_jump(stream_b, stream_b + 0x1000);
		streams.note_put(stream_a + 0x1200, stream_b + 0x1000);

		u32 target = 0;
		EXPECT_TRUE(streams.redirect(stream_a + 0x1200, stream_b + 0x1000, target));
		EXPECT_EQ(target, stream_a);
	}

	TEST(FifoStreams, RedirectWaitsUntilTheChunkIsExhausted)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x1000, boot);
		streams.on_jump(stream_a, stream_a + 0x1000);
		streams.note_put(stream_b + 0x1000, stream_a + 0x1000);
		streams.on_jump(stream_b, stream_b + 0x1000);

		// The parser is still inside B, so the rest of B is readable even though
		// the producer has published A again.
		streams.note_put(stream_a + 0x1200, stream_b + 0x800);

		u32 target = 0;
		EXPECT_FALSE(streams.redirect(stream_a + 0x1200, stream_b + 0x800, target));
	}

	TEST(FifoStreams, ResyncKeepsCommandBufferStreams)
	{
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x1000, boot);
		streams.on_jump(stream_a, stream_a + 0x1000);
		streams.note_put(stream_b + 0x1000, stream_a + 0x1000);
		streams.on_jump(stream_b, stream_b + 0x1000);

		// A recovery point in static code must not turn A into a fresh stream
		// whose frontier is its own base: the next jump into A would park on the
		// first word instead of reading the chunk the guest already published.
		streams.reset_at(0x154c);
		streams.on_jump(stream_a, stream_b + 0x2000);
		EXPECT_EQ(streams.frontier(stream_b + 0x2000, stream_a + 0x100), stream_a + 0x1000);
	}

	TEST(FifoStreams, NestedCallKeepsTheChunkItReturnsInto)
	{
		// Ridge Racer 7 enters the NV406E_SEMAPHORE_ACQUIRE trampoline from a
		// command chunk, the trampoline jumps into the next chunk, and that
		// chunk carries a CALL of its own (L149). Hardware answers the inner
		// CALL by overwriting its single return-address register, so the outer
		// frame is dead: the parser never returns to the chunk it called from.
		stream_tracker streams;
		streams.reset(boot);
		streams.note_put(stream_a + 0x1000, boot);
		streams.on_jump(stream_a, stream_a + 0x1000);
		streams.note_put(stream_b + 0x1000, stream_a);
		streams.on_jump(stream_b, stream_b + 0x1000);

		const u32 stream_c = 0x2000000;
		const u32 stream_d = 0x3000000;
		const u32 stream_e = 0x900000;
		const u32 stream_f = 0x4a0000;

		streams.note_put(stream_c + 0x1000, stream_b);
		streams.on_jump(stream_c, stream_c + 0x1000);
		// The fifth stream recycles the boot stream, so D takes its slot.
		streams.note_put(stream_d + 0x1000, stream_c + 0x1000);
		streams.on_jump(stream_d, stream_d + 0x1000);

		// D calls the static semaphore trampoline, which jumps into static code
		// below every command stream.
		const u32 put_e = stream_e + 0x1000;
		streams.on_call(0x1544, stream_d + 0x100);
		streams.on_jump(stream_e, put_e);
		// The chunk D jumped into carries a CALL of its own.
		streams.on_call(0xca8180, stream_e + 0x40);
		streams.on_return();

		// F recycles the lowest non-current stream. The parser is inside E, so
		// E must survive; recycling it would orphan the frontier the parser is
		// standing on.
		streams.note_put(stream_f + 0x1000, put_e);
		streams.on_jump(stream_f, stream_f + 0x1000);
		EXPECT_TRUE(streams.is_known(stream_e + 0x40));
	}
}
