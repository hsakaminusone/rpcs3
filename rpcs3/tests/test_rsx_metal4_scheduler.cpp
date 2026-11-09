#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLBarrier.h"
#include "Emu/RSX/MTL/MTLPassScheduler.h"

// The Metal 4 pass scheduler: the hazard optimiser.
//
// The scheduler is not the correctness authority.  Every encoder is closed with
// a producer-scope ALL->ALL device barrier by the command recorder (C2), and the
// framebuffer pass key decides when two draws share an encoder (C1).  The
// scheduler only avoids the barriers that are provably redundant inside a pass;
// a missed note can therefore cost a barrier, never a pixel.
//
// Every test here exercises the scheduler's decision procedure: which accesses
// conflict, which direction the hazard runs, which pass each access was noted
// in, and which barrier form (if any) the chokepoint emits.  No test opens an
// MTL::Device.  On Apple silicon this file installs a fake MTL4::CommandEncoder
// as an ObjC object that records every barrierAfter* message it receives, so
// the exact specs the backend would emit can be asserted.  The cast is legal
// because metal-cpp dispatches every call through objc_msgSend; the fake object
// never calls into Metal.  Hosts without RSX_ENABLE_METAL4 compile this file
// away, like the rest of the backend.
#ifdef RSX_ENABLE_METAL4

#include "Utilities/address_range.h"

#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	// The stage masks used by the sweeps.  StageAll also covers the high bits.
	constexpr MTL::Stages k0 = static_cast<MTL::Stages>(0);
	constexpr MTL::Stages kV = MTL::StageVertex;
	constexpr MTL::Stages kF = MTL::StageFragment;
	constexpr MTL::Stages kT = MTL::StageTile;
	constexpr MTL::Stages kB = MTL::StageBlit;
	constexpr MTL::Stages kD = MTL::StageDispatch;
	constexpr MTL::Stages kVF = static_cast<MTL::Stages>(MTL::StageVertex | MTL::StageFragment);
	constexpr MTL::Stages kFT = static_cast<MTL::Stages>(MTL::StageFragment | MTL::StageTile);
	constexpr MTL::Stages kBD = static_cast<MTL::Stages>(MTL::StageBlit | MTL::StageDispatch);
	constexpr MTL::Stages kVFT = static_cast<MTL::Stages>(MTL::StageVertex | MTL::StageFragment | MTL::StageTile);
	constexpr MTL::Stages kAll = MTL::StageAll;

	constexpr std::array<MTL::Stages, 10> kSweepStages = { k0, kV, kF, kT, kB, kD, kVF, kFT, kBD, kAll };

	// Every recorded barrier message, in emission order.  `kind` names which of
	// the three barrierAfter* forms was called; the rest are the exact arguments.
	enum class barrier_kind : u8
	{
		after_encoder_stages,
		after_queue_stages,
		after_stages,
	};

	struct recorded_barrier
	{
		barrier_kind kind = barrier_kind::after_encoder_stages;
		MTL::Stages producer = k0;
		MTL::Stages consumer = k0;
		u64 visibility = 0;
		id encoder = nullptr; // which fake object received the message
	};

	const char* barrier_kind_name(barrier_kind kind)
	{
		switch (kind)
		{
		case barrier_kind::after_encoder_stages: return "barrierAfterEncoderStages";
		case barrier_kind::after_queue_stages: return "barrierAfterQueueStages";
		case barrier_kind::after_stages: return "barrierAfterStages";
		}

		return "<unknown>";
	}

	std::vector<recorded_barrier>& recorded_barriers()
	{
		static std::vector<recorded_barrier> calls;
		return calls;
	}

	void reset_recorded_barriers()
	{
		recorded_barriers().clear();
	}

	void record_barrier(id self, barrier_kind kind, u64 after, u64 before, u64 visibility)
	{
		recorded_barriers().push_back({ kind, static_cast<MTL::Stages>(after), static_cast<MTL::Stages>(before), visibility, self });
	}

	void fake_barrier_after_encoder_stages(id self, SEL /*_cmd*/, u64 after, u64 before, u64 visibility)
	{
		record_barrier(self, barrier_kind::after_encoder_stages, after, before, visibility);
	}

	void fake_barrier_after_queue_stages(id self, SEL /*_cmd*/, u64 after, u64 before, u64 visibility)
	{
		record_barrier(self, barrier_kind::after_queue_stages, after, before, visibility);
	}

	void fake_barrier_after_stages(id self, SEL /*_cmd*/, u64 after, u64 before, u64 visibility)
	{
		record_barrier(self, barrier_kind::after_stages, after, before, visibility);
	}

	// Two fake encoder objects; a test names which one received a message, so
	// emission on the pass that carries an edge can be distinguished from emission
	// on the pass that produced it.  The class implements exactly the three
	// selectors metal-cpp's barrier calls dispatch under; a selector it lacks is
	// unrecognised, like it would be to a real Metal object.
	MTL4::CommandEncoder* fake_encoder(u32 index = 0)
	{
		static std::array<MTL4::CommandEncoder*, 2> encoders = []() -> std::array<MTL4::CommandEncoder*, 2>
		{
			Class cls = objc_allocateClassPair(objc_getClass("NSObject"), "MTL4FakeCommandEncoderForTests", 0);
			class_addMethod(cls, sel_registerName("barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:"),
				reinterpret_cast<IMP>(fake_barrier_after_encoder_stages), "v@:QQQ");
			class_addMethod(cls, sel_registerName("barrierAfterQueueStages:beforeStages:visibilityOptions:"),
				reinterpret_cast<IMP>(fake_barrier_after_queue_stages), "v@:QQQ");
			class_addMethod(cls, sel_registerName("barrierAfterStages:beforeQueueStages:visibilityOptions:"),
				reinterpret_cast<IMP>(fake_barrier_after_stages), "v@:QQQ");
			objc_registerClassPair(cls);

			return { reinterpret_cast<MTL4::CommandEncoder*>(class_createInstance(cls, 0)),
					 reinterpret_cast<MTL4::CommandEncoder*>(class_createInstance(cls, 0)) };
		}();

		return encoders[index & 1u];
	}

	void expect_recorded_barriers(std::initializer_list<recorded_barrier> expected)
	{
		const std::vector<recorded_barrier>& got = recorded_barriers();
		ASSERT_EQ(got.size(), expected.size());

		std::size_t index = 0;
		for (const recorded_barrier& want : expected)
		{
			const recorded_barrier& have = got[index];
			SCOPED_TRACE("barrier call " + std::to_string(index));
			EXPECT_EQ(want.kind, have.kind) << barrier_kind_name(want.kind) << " expected";
			EXPECT_EQ(want.producer, have.producer) << "producer mismatch";
			EXPECT_EQ(want.consumer, have.consumer) << "consumer mismatch";
			EXPECT_EQ(want.visibility, have.visibility) << "visibility mismatch";
			EXPECT_EQ(want.encoder, have.encoder) << "encoder mismatch";
			++index;
		}
	}

	void expect_eq_queued(const mtl::pass_scheduler& scheduler, usz expected)
	{
		EXPECT_EQ(scheduler.queued_edges(), expected);
	}

	void expect_no_barriers_recorded()
	{
		EXPECT_TRUE(recorded_barriers().empty()) << "expected no barrier calls, got " << recorded_barriers().size();
	}

	utils::address_range32 range(u32 start, u32 length)
	{
		return utils::address_range32::start_length(start, length);
	}

	MTL::ResourceID resource(u64 id)
	{
		return MTL::ResourceID{ id };
	}

	void fresh_frame_pass(mtl::pass_scheduler& scheduler)
	{
		scheduler.begin_frame();
		scheduler.begin_pass();
	}
}

// =============================================================================
// 1. Guest-range identity: resource id, overlap, containment
// =============================================================================

TEST(MTLSchedulerRange, TwoIdsOverlappingRangesEdgeAtAnyOffset)
{
	// One resource written in an earlier pass, a different resource overlapping
	// its guest range in a later pass: the guest bytes are the identity, not
	// the Metal object.
	const std::pair<u32, u32> cases[] = { { 0x1000, 0x2000 }, { 0x1000, 0x1800 }, { 0x1800, 0x2000 }, { 0x1000, 0x1000 } };
	for (const auto& [read_start, read_len] : cases)
	{
		SCOPED_TRACE("read " + std::to_string(read_start) + "+" + std::to_string(read_len));

		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), range(0x1000, 0x3000), kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), range(read_start, read_len), kV);

		expect_eq_queued(scheduler, 1u);
	}
}

TEST(MTLSchedulerRange, TwoIdsDisjointRangesNoEdge)
{
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x50), range(0x2000, 0x1000), kV);
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerRange, OneByteOverlapEdges)
{
	// One byte of shared footprint is an edge; one byte of separation is not.
	const std::array<std::array<u32, 5>, 6> cases = { {
		{ 0x1000, 0x1fff, 0x1fff, 0x2fff, 1 }, // one byte shared
		{ 0x1000, 0x1fff, 0x2000, 0x2fff, 0 }, // adjacent, no shared byte
		{ 0x0000, 0xffff, 0xffff, 0xffff, 1 }, // one byte shared at a block face
		{ 0x0001, 0xffff, 0x0000, 0x0000, 0 }, // adjacent, no shared byte
		{ 0x0000, 0x0000, 0x0001, 0x0001, 0 }, // adjacent single bytes
		{ 0x1000, 0xffff, 0x1000, 0x9fff, 1 }, // containment
	} };

	for (const auto& c : cases)
	{
		SCOPED_TRACE("write " + std::to_string(c[0]) + "-" + std::to_string(c[1]) +
			" read " + std::to_string(c[2]) + "-" + std::to_string(c[3]));

		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), utils::address_range32::start_end(c[0], c[1]), kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), utils::address_range32::start_end(c[2], c[3]), kV);

		expect_eq_queued(scheduler, c[4]);
	}
}

TEST(MTLSchedulerRange, SameObjectMatchesAcrossDisjointRanges)
{
	// The same Metal object is always a hazard, even with disjoint footprints:
	// the resource is the identity when both notes carry one.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x77), range(0x1000, 0x1000), kF);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x77), range(0xf0000000, 0x1000), kV);
	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerRange, SameObjectMatchesWhenOneNoteHasNoRange)
{
	// A temporary image has no guest bytes; only its resource id can match, and
	// only in the direction where one of the two notes lacks a footprint.
	{
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x77), {}, kT);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x77), range(0x1000, 0x1000), kF);
		expect_eq_queued(scheduler, 1u);
	}
	{
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x78), range(0x1000, 0x1000), kT);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x78), {}, kF);
		expect_eq_queued(scheduler, 1u);
	}
}

TEST(MTLSchedulerRange, DifferentObjectsWithInvalidRangesDoNotEdge)
{
	// Invalid ranges cannot alias another object; they carry no guest bytes.
	{
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), {}, kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), range(0x1000, 0x1000), kV);
		expect_eq_queued(scheduler, 0u);
	}
	{
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x41), range(0x1000, 0x1000), kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x51), {}, kV);
		expect_eq_queued(scheduler, 0u);
	}
}

TEST(MTLSchedulerRange, InvalidRangesBothSidesSameObjectEdges)
{
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x77), {}, kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x77), {}, kV);
	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerRange, StartLengthZeroIsAnInvalidRange)
{
	// start_length(x, 0) yields the default invalid range, so neither note
	// carries a footprint and the different resources cannot match.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x50), range(0x1000, 0), kV);
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerRange, StartLengthOverflowYieldsAnInvalidRange)
{
	// 0xffffffff + 3 wraps below the start, which is how the class reports it.
	const utils::address_range32 wrapped = utils::address_range32::start_length(0xffff'ffffu, 4);
	EXPECT_FALSE(wrapped.valid());

	// The wrapped range can therefore never produce a phantom edge.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), wrapped, kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x50), wrapped, kV);
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerRange, WholeAddressSpaceRangeAliasesEverything)
{
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), utils::address_range32::start_end(0x0u, 0xffff'ffffu), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x50), range(0xdead'beefu, 0x1000), kV);
	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerRange, TouchingRangesNeverConflictAtAnyLength)
{
	// [a, a+n) and [a+n, a+2n) share no byte; the scheduler must never call
	// them an edge, at any footprint or at any alignment.
	for (const u32 length : { 1u, 0x10u, 0x1000u, 0x10000u })
	{
		for (const u32 offset : { 0x0u, 0xffffu, 0x10000u })
		{
			SCOPED_TRACE("length " + std::to_string(length) + " offset " + std::to_string(offset));

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_write(resource(0x40), range(offset, length), kBD);

			scheduler.begin_pass();
			scheduler.note_read(resource(0x50), range(offset + length, length), kV);

			expect_eq_queued(scheduler, 0u);
		}
	}
}

TEST(MTLSchedulerRange, NestedRangesEdgeInBothContainmentOrders)
{
	{
		// Read inside write.
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), range(0x1000, 0x8000), kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), range(0x2000, 0x1000), kV);
		expect_eq_queued(scheduler, 1u);
	}
	{
		// Write inside read.
		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), range(0x2000, 0x1000), kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), range(0x1000, 0x8000), kV);
		expect_eq_queued(scheduler, 1u);
	}
}

TEST(MTLSchedulerRange, RangesStraddling64KiBBlockEdgesEdge)
{
	// The read index works in 64 KiB blocks; a footprint that straddles a
	// block edge must be found by a write that overlaps only one side of it.
	const utils::address_range32 read = range(0x0001'0000u - 0x8000u, 0xc000u); // 0x8000..0x14000, blocks 0 and 1

	for (const u32 write_start : { 0x0000'0000u, 0x0000'f000u, 0x0001'0000u, 0x0001'3000u, 0x0001'4000u, 0x0001'5000u })
	{
		SCOPED_TRACE("write start " + std::to_string(write_start));

		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_read(resource(0x50), read, kV);

		scheduler.begin_pass();
		scheduler.note_write(resource(0x40), range(write_start, 0x1000), kBD);

		const bool want_conflict = write_start <= read.end && write_start + 0x0fff >= read.start;
		expect_eq_queued(scheduler, want_conflict ? 1u : 0u);
	}
}

TEST(MTLSchedulerRange, SingleByteAtEveryBlockBoundaryEdges)
{
	// One byte overlapping each 64 KiB boundary, sampled at both faces of it.
	for (const u32 boundary : { 0x1'0000u, 0x2'0000u, 0x3'0000u })
	{
		for (const s32 offset : { -1, 0, 1 })
		{
			SCOPED_TRACE("boundary " + std::to_string(boundary) + " offset " + std::to_string(offset));
			const u32 address = static_cast<u32>(static_cast<s64>(boundary) + offset);

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_write(resource(0x40), range(address, 1), kBD);

			scheduler.begin_pass();
			scheduler.note_read(resource(0x50), range(boundary - 1, 2), kV); // straddles the boundary

			const bool want_conflict = address >= boundary - 1 && address <= boundary;
			expect_eq_queued(scheduler, want_conflict ? 1u : 0u);
		}
	}
}

TEST(MTLSchedulerRange, ReadIndexNeverMissesAnOverlappingWrite)
{
	// Reads that span or touch block edges must be found on a shared block by
	// any overlapping write.  The exact range predicate is the oracle.
	const utils::address_range32 single = range(0x0000'9000u, 0x1000u);        // block 0
	const utils::address_range32 multi = range(0x0000'9000u, 0xd000u);         // blocks 0 and 1
	const utils::address_range32 writes[] = {
		range(0x0000'8000u, 0x2000),  // block 0, overlaps both (shares 0x9000..0x9fff)
		range(0x0000'8000u, 0x10000), // block 0, overlaps both
		range(0x0000'9000u, 0x1000),  // identical footprint
		range(0x0001'4000u, 0x1000),  // block 1, overlaps multi only
		range(0x0001'5fffu, 0x1),     // last byte of multi, block 1
		range(0x0001'6000u, 0x1000),  // block 1, just past multi
		range(0xffff'f000u, 0x1000),  // block 65535, no overlap
		range(0x0000'1000u, 0),       // invalid: zero length, no footprint
	};

	for (const utils::address_range32 write : writes)
	{
		for (const auto& [test_range, name] : { std::pair{ single, std::string{ "single" } }, std::pair{ multi, std::string{ "multi" } } })
		{
			SCOPED_TRACE(name + " " + write.str());

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_read(resource(0x50), test_range, kV);

			scheduler.begin_pass();
			scheduler.note_write(resource(0x40), write, kBD);

			const bool want_conflict = write.valid() && test_range.valid() && write.overlaps(test_range);
			expect_eq_queued(scheduler, want_conflict ? 1u : 0u);
		}
	}
}

TEST(MTLSchedulerRange, RandomisedRangePairsMatchTheBytePredicate)
{
	// Deterministic random pairs; the oracle is the exact range predicate.
	u32 seed = 0x5eed'1234u;
	const auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

	for (u32 iteration = 0; iteration < 400; ++iteration)
	{
		SCOPED_TRACE("iteration " + std::to_string(iteration));
		const u32 w_start = next() % 0x0040'0000u;
		const u32 w_len = 1 + (next() % 0x0002'0000u);
		const u32 r_start = next() % 0x0040'0000u;
		const u32 r_len = 1 + (next() % 0x0002'0000u);

		const utils::address_range32 write = range(w_start, w_len);
		const utils::address_range32 read = range(r_start, r_len);
		const bool want_conflict = write.valid() && read.valid() && write.overlaps(read);

		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), write, kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(0x50), read, kV);

		expect_eq_queued(scheduler, want_conflict ? 1u : 0u);
	}
}

TEST(MTLSchedulerRange, RandomisedMixedIdentityStillNeedsAnOverlap)
{
	// Random pairs where the resource ids may or may not match; identity alone
	// always conflicts, different ids still need a shared byte.
	u32 seed = 0xc0ff'eee0u;
	const auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed; };

	for (u32 iteration = 0; iteration < 400; ++iteration)
	{
		SCOPED_TRACE("iteration " + std::to_string(iteration));
		const u32 w_start = next() % 0x0010'0000u;
		const u32 w_len = 1 + (next() % 0x0001'0000u);
		const u32 r_start = next() % 0x0010'0000u;
		const u32 r_len = 1 + (next() % 0x0001'0000u);
		const bool same_resource = (next() & 1u) != 0;

		const utils::address_range32 write = range(w_start, w_len);
		const utils::address_range32 read = range(r_start, r_len);
		const bool range_conflict = write.valid() && read.valid() && write.overlaps(read);
		const bool want_conflict = same_resource || range_conflict;

		mtl::pass_scheduler scheduler;
		fresh_frame_pass(scheduler);
		scheduler.note_write(resource(0x40), write, kBD);

		scheduler.begin_pass();
		scheduler.note_read(resource(same_resource ? 0x40u : 0x50u), read, kV);

		expect_eq_queued(scheduler, want_conflict ? 1u : 0u);
	}
}

// =============================================================================
// 2. RAW: write then read — the cross-encoder hazard
// =============================================================================

TEST(MTLSchedulerRaw, EarlierPassEmitsQueueBarrierWithExactStages)
{
	// The common case: the transfer that filled an image is long done; the
	// consumer pass carries one queue-form producer edge for it.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));
	expect_no_barriers_recorded();

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerRaw, EarlierPassFragmentConsumerCarriesDeviceVisibility)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kT, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerRaw, EarlierPassTileProducerStaysQueueFormAcrossPasses)
{
	// The producer form is chosen at note time; pass distance does not change it.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);

	for (u32 pass = 0; pass < 3; ++pass)
	{
		scheduler.begin_pass(); // filler passes, no accesses
		scheduler.flush(fake_encoder(0));
	}

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kT, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerRaw, FrameBoundaryRetiresTheWriteSoNoBarrierIsEmitted)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerRaw, SamePassVertexProducerEmitsIntraPassBarrier)
{
	// A vertex-stage producer of this encoder is legal to order inside it.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassBlitDispatchProducerEmitsIntraPassBarrier)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassFragmentProducerEmitsNothingAndRequestsTheSplit)
{
	// A fragment-stage producer of this encoder cannot be ordered inside it:
	// flush() must carry the edge instead.  The carried edge must not appear on
	// this encoder at all.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
	EXPECT_TRUE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassTileProducerEmitsNothingAndRequestsTheSplit)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassStageAllProducerEmitsNothingAndRequestsTheSplit)
{
	// StageAll carries the fragment and tile bits; it can never be intra-pass.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kAll);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassVertexWithTileProducerEmitsNothingAndRequestsTheSplit)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kVFT);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, SamePassZeroProducerIsSkippedAndEdgesNothing)
{
	// The candidate scan skips a zero-stage producer; the same-pass bucket stays
	// empty and no edge, barrier or split results.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, CarriedEdgeIsEmittedOnTheNextEncoderAsAQueueBarrier)
{
	// flush() carries the edge; emit_split_barriers() is where it lands.  The
	// next encoder observes the queue-form of it, with cross_pass forced true.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));
	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_pass();
	EXPECT_FALSE(scheduler.split_requested()); // begin_pass clears the latch...
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1))); // ...but not the carried edge

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, CarriedEdgeIsEmittedExactlyOnce)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	scheduler.begin_pass();
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1)));
	EXPECT_FALSE(scheduler.emit_split_barriers(fake_encoder(1)));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

TEST(MTLSchedulerRaw, CarriedEdgeIsNotReemittedAfterTheNextFlush)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	scheduler.begin_pass();
	// The caller re-enters record_pass_hazards for the next pass, which emits
	// the split barriers before its own flush.
	scheduler.emit_split_barriers(fake_encoder(1));
	scheduler.flush(fake_encoder(1)); // nothing pending now

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kT, kF, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

TEST(MTLSchedulerRaw, CarriedEdgeIsDroppedByBeginFrame)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_frame();
	scheduler.begin_pass();
	EXPECT_FALSE(scheduler.emit_split_barriers(fake_encoder(1))); // nothing carried anymore
	EXPECT_FALSE(scheduler.split_requested());

	expect_no_barriers_recorded();
}

TEST(MTLSchedulerRaw, CarriedEdgeSurvivesNullEncoderOfTheNextPass)
{
	// emit_split_barriers(nullptr) refuses and keeps the carried edge; the
	// following real encoder still receives it.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_pass();
	EXPECT_FALSE(scheduler.split_requested());    // begin_pass clears the latch...
	EXPECT_FALSE(scheduler.emit_split_barriers(nullptr)); // ...and the null emit refuses to touch the carried edge

	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1)));
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerRaw, ZeroConsumerStageEmitsNoEdge)
{
	// A gpu_reference-class read (an empty stage mask) produces no edge: a
	// barrier whose consumer mask is zero orders nothing.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
}

TEST(MTLSchedulerRaw, ZeroProducerStageProducesNoEdgeAcrossPasses)
{
	// Pin the as-is behaviour: a zero-stage write is invisible to the RAW
	// candidate scan, so a later read finds no producer at all.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), k0);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerRaw, ProducerMaskUnionsAcrossNotes)
{
	// Two writes of the same resource in two passes: the record keeps the union
	// of every stage mask ever noted, so a later read orders behind all of them.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD); // pass 1, blit

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);  // pass 2, fragment

	scheduler.begin_pass(); // pass 3
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, static_cast<MTL::Stages>(kBD | kF), kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerRaw, TwoCandidatesOneSamePassOneEarlierQueueBothBuckets)
{
	// One read needs to wait on a same-pass producer (intra-pass legal) and an
	// earlier-pass producer (queue form); both must be emitted, in order.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD); // earlier-pass candidate

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x50), range(0x1000, 0x1000), kV);  // same-pass candidate

	scheduler.note_read(resource(0x60), range(0x1000, 0x1000), kF);   // reads both, by range
	scheduler.flush(fake_encoder(0));

	// Specs are emitted per candidate, in note (m_write_records) order: the
	// earlier-pass candidate was noted first.
	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerRaw, SamePassProducersOfTwoResourcesEmitPerCandidate)
{
	// Specs are emitted per candidate — a legal producer is never carried along
	// by an illegal one in the same bucket.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_write(resource(0x50), range(0x2000, 0x1000), kBD);

	scheduler.note_read(resource(0x60), range(0x1000, 0x2000), kF);   // both candidates overlap the footprint
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_encoder_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerRaw, StageSweepEarlierPassProducerAgainstConsumer)
{
	// The full product of the stage masks, earlier-pass bucket: every legal
	// combination must emit exactly one queue-form barrier with the noted pair.
	for (const MTL::Stages producer : kSweepStages)
	{
		for (const MTL::Stages consumer : kSweepStages)
		{
			SCOPED_TRACE("producer " + std::to_string(producer) + " consumer " + std::to_string(consumer));
			reset_recorded_barriers();

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_write(resource(0x40), range(0x1000, 0x1000), producer);

			scheduler.begin_pass();
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), consumer);
			scheduler.flush(fake_encoder(0));

			if (producer == k0 || consumer == k0)
			{
				expect_no_barriers_recorded(); // zero on either side produces no edge
			}
			else
			{
				expect_recorded_barriers({ { barrier_kind::after_queue_stages, producer, consumer, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
			}
		}
	}
}

TEST(MTLSchedulerRaw, StageSweepSamePassDecidesByFragmentTileBits)
{
	// The same full product, same-pass bucket: the legality rule decides the
	// form — intra-pass for no fragment|tile bit, nothing now and a carried
	// queue-form edge for the next encoder otherwise.
	for (const MTL::Stages producer : kSweepStages)
	{
		for (const MTL::Stages consumer : kSweepStages)
		{
			SCOPED_TRACE("producer " + std::to_string(producer) + " consumer " + std::to_string(consumer));
			reset_recorded_barriers();

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_write(resource(0x40), range(0x1000, 0x1000), producer);
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), consumer);
			scheduler.flush(fake_encoder(0));

			const bool legal = (producer & kFT) == 0;
			if (producer == k0 || consumer == k0)
			{
				expect_no_barriers_recorded(); // zero on either side produces no edge
				EXPECT_FALSE(scheduler.split_requested());
			}
			else if (legal)
			{
				expect_recorded_barriers({ { barrier_kind::after_encoder_stages, producer, consumer, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
			}
			else
			{
				expect_no_barriers_recorded();
				EXPECT_TRUE(scheduler.split_requested());

				scheduler.begin_pass();
				scheduler.emit_split_barriers(fake_encoder(1));
				expect_recorded_barriers({ { barrier_kind::after_queue_stages, producer, consumer, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
			}
		}
	}
}

TEST(MTLSchedulerRaw, StageSweepSamePassConsumesProducerExactlyOnce)
{
	// Every same-pass pair emits at most once per write note; the second read
	// finds the producer already consumed.
	for (const MTL::Stages producer : kSweepStages)
	{
		for (const MTL::Stages consumer : kSweepStages)
		{
			SCOPED_TRACE("producer " + std::to_string(producer) + " consumer " + std::to_string(consumer));
			reset_recorded_barriers();

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_write(resource(0x40), range(0x1000, 0x1000), producer);
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), consumer);
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), consumer); // the write is consumed by the first read

			scheduler.flush(fake_encoder(0));

			const bool legal = (producer & kFT) == 0;
			const usz wanted = (producer == k0 || consumer == k0) ? 0u : (legal ? 1u : 0u);
			EXPECT_EQ(recorded_barriers().size(), wanted);
		}
	}
}

// =============================================================================
// 3. WAR: read then write — the overwrite hazard
// =============================================================================

TEST(MTLSchedulerWar, EarlierPassEmitsQueueBarrierWithExactStages)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));
	expect_no_barriers_recorded();

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWar, EarlierPassVertexReadBarrierIsQueueForm)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWar, SamePassVertexReadEmitsIntraPassBarrier)
{
	// A vertex-stage read of this encoder can be ordered inside it, before the
	// write the pass is about to issue.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWar, SamePassBlitDispatchReadEmitsIntraPassBarrier)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWar, SamePassFragmentReadEmitsNothingAndRequestsTheSplit)
{
	// The TBDR feedback case: the read is a fragment stage of this encoder,
	// so the write cannot be ordered after it inside the encoder.  The edge is
	// carried to the next one.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_pass();
	scheduler.emit_split_barriers(fake_encoder(1));
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kF, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

TEST(MTLSchedulerWar, SamePassTileReadEmitsNothingAndRequestsTheSplit)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kT);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_pass();
	scheduler.emit_split_barriers(fake_encoder(1));
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kT, kBD, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

TEST(MTLSchedulerWar, ReadMaskUnionsAcrossNotes)
{
	// Two reads of the same resource: the record keeps the union of every
	// stage mask ever noted, so a later write orders behind all of them.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF); // pass 1

	scheduler.begin_pass(); // pass 2
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);  // pass 2

	scheduler.begin_pass(); // pass 3
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, static_cast<MTL::Stages>(kF | kV), kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWar, OneReadEmitsWarAtEachConsumingWrite)
{
	// A write event orders behind every conflicting earlier read; two writes
	// in two passes both need their edges, and the satisfied scratch is cleared
	// per pass so the second write is not silenced by the first.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	scheduler.begin_pass(); // pass 3
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kF, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerWar, StageSweepEarlierPassProducerAgainstConsumer)
{
	for (const MTL::Stages read_stages : kSweepStages)
	{
		for (const MTL::Stages write_stages : kSweepStages)
		{
			SCOPED_TRACE("read " + std::to_string(read_stages) + " write " + std::to_string(write_stages));
			reset_recorded_barriers();

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), read_stages);

			scheduler.begin_pass();
			scheduler.note_write(resource(0x40), range(0x1000, 0x1000), write_stages);
			scheduler.flush(fake_encoder(0));

			if (read_stages == k0 || write_stages == k0)
			{
				expect_no_barriers_recorded(); // zero on either side produces no edge
			}
			else
			{
				expect_recorded_barriers({ { barrier_kind::after_queue_stages, read_stages, write_stages, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
			}
		}
	}
}

TEST(MTLSchedulerWar, StageSweepSamePassDecidesByFragmentTileBits)
{
	for (const MTL::Stages read_stages : kSweepStages)
	{
		for (const MTL::Stages write_stages : kSweepStages)
		{
			SCOPED_TRACE("read " + std::to_string(read_stages) + " write " + std::to_string(write_stages));
			reset_recorded_barriers();

			mtl::pass_scheduler scheduler;
			fresh_frame_pass(scheduler);
			scheduler.note_read(resource(0x40), range(0x1000, 0x1000), read_stages);
			scheduler.note_write(resource(0x40), range(0x1000, 0x1000), write_stages);
			scheduler.flush(fake_encoder(0));

			const bool legal = (read_stages & kFT) == 0;
			if (read_stages == k0 || write_stages == k0)
			{
				expect_no_barriers_recorded(); // zero on either side produces no edge
				EXPECT_FALSE(scheduler.split_requested());
			}
			else if (legal)
			{
				expect_recorded_barriers({ { barrier_kind::after_encoder_stages, read_stages, write_stages, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
			}
			else
			{
				expect_no_barriers_recorded();
				EXPECT_TRUE(scheduler.split_requested());

				scheduler.begin_pass();
				scheduler.emit_split_barriers(fake_encoder(1));
				expect_recorded_barriers({ { barrier_kind::after_queue_stages, read_stages, write_stages, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
			}
		}
	}
}

TEST(MTLSchedulerWar, TwoCandidatesOneSamePassOneEarlierQueueBothBuckets)
{
	// An earlier-pass read (queue form) and a same-pass read (intra-pass legal
	// vertex producer) both have to be ordered behind the write.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);           // earlier-pass candidate

	scheduler.begin_pass(); // pass 2
	scheduler.note_read(resource(0x50), range(0x1000, 0x1000), kBD);          // same-pass candidate

	scheduler.note_write(resource(0x60), range(0x1000, 0x1000), kT);
	scheduler.flush(fake_encoder(0));

	// Specs are emitted per read candidate, in note (m_read_blocks) order: the
	// earlier-pass candidate was noted first.
	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kV, kT, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_encoder_stages, kBD, kT, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerWar, SamePassReadsEmitPerCandidate)
{
	// Specs are emitted per read candidate — a legal producer is never carried
	// along by an illegal one in the same bucket.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x50), range(0x2000, 0x1000), kBD);

	scheduler.note_write(resource(0x60), range(0x1000, 0x2000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_encoder_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerWar, ZeroReadStageSkippedNothingPhantom)
{
	// A read noted with a zero stage mask does not produce a producer mask, so
	// a later write finds nothing to wait for.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), k0);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	expect_recorded_barriers({});
	expect_eq_queued(scheduler, 0u);
}

// =============================================================================
// 4. WAW: write then write — the scheduler does not track these at all
// =============================================================================

TEST(MTLSchedulerWaw, TwoWritesInTwoPassesEmitNothing)
{
	// The property, not a preference: the scheduler has no write-write edge.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerWaw, TwoWritesInOnePassEmitNothing)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerWaw, MixedMaskWritesAcrossPassesEmitNothing)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
}

TEST(MTLSchedulerWaw, WriteAfterWriteDoesNotOrderTheSecondWrite)
{
	// Reads interposed between the writes each find their producer; the pair
	// of writes never finds each other.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
}

TEST(MTLSchedulerWaw, OverlappingWriteThenReadSeesTheUnionOfMasks)
{
	// Three writes then one read: the barrier producer is the union of every
	// mask ever written to the resource in the frame.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kT);   // pass 1
	scheduler.begin_pass();                                            // pass 2
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);   // pass 2
	scheduler.begin_pass();                                            // pass 3
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);   // pass 3

	scheduler.begin_pass(); // pass 4
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, static_cast<MTL::Stages>(kT | kF | kV), kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerWaw, DisjointWriteThenWriteAlsoEmitsNothing)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x50), range(0x4000, 0x1000), kT);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
}

// =============================================================================
// 5. Consumption precision: exactly the overlapping partners pay the edge
// =============================================================================

TEST(MTLSchedulerConsumption, OneWriteConsumesExactlyTheOverlappingReads)
{
	// Two reads in disjoint ranges; the write overlaps only the first.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);  // overlapping read
	scheduler.note_read(resource(0x42), range(0x8000, 0x1000), kF);  // disjoint read

	scheduler.begin_pass();
	scheduler.note_write(resource(0x43), range(0x1000, 0x1000), kBD);

	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerConsumption, ALaterWriteConsumesTheReadTheFirstWriteDidNot)
{
	// The disjoint read survives the first write and is consumed by the second.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x42), range(0x8000, 0x1000), kF);

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x43), range(0x1000, 0x1000), kBD);   // consumes read A
	scheduler.flush(fake_encoder(0));

	scheduler.begin_pass(); // pass 3
	scheduler.note_write(resource(0x44), range(0x8000, 0x1000), kBD);   // consumes read B
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kV, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerConsumption, OneReadConsumesExactlyTheOverlappingWrites)
{
	// Two writes in disjoint ranges; the read overlaps only the first, and the
	// second write is still available to the next read.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.note_write(resource(0x42), range(0x8000, 0x1000), kF);
	scheduler.flush(fake_encoder(0)); // nothing pending; no reads yet

	scheduler.begin_pass(); // pass 2
	scheduler.note_read(resource(0x43), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	scheduler.begin_pass(); // pass 3
	scheduler.note_read(resource(0x43), range(0x8000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerConsumption, UnionProducerStagesAcrossOverlappingReads)
{
	// A write overlapping a vertex-read resource and a differently-ranged
	// fragment-read resource emits one queue-form spec per read candidate, in
	// note order.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x42), range(0x2000, 0x1000), kF);

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x43), range(0x1000, 0x2000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kV, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerConsumption, DisjointSecondReadIsNotConsumedByTheWrite)
{
	// The producer mask carries only the consumed read's stages, not every read
	// in the frame.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);   // overlapping
	scheduler.note_read(resource(0x42), range(0x8000, 0x1000), kF);   // disjoint

	scheduler.begin_pass();
	scheduler.note_write(resource(0x43), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kV, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerConsumption, ConsumedBySameObjectWriteWithoutAnyRange)
{
	// Range-free consumption works across both notes regardless of footprints.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x41), {}, kBD);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kV, kBD, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerConsumption, BurstReadsOfThreeResourcesConsumeOncePerWrite)
{
	// Three differently-ranged resources read in one pass; a later write
	// overlapping all three emits one queue-form spec per read candidate, in
	// note order.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x42), range(0x2000, 0x1000), kF);
	scheduler.note_read(resource(0x43), range(0x3000, 0x1000), kBD);

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x44), range(0x1000, 0x3000), kT);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kV, kT, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kT, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kBD, kT, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerConsumption, PartialOverlapAcrossBlockBoundaryStillConsumes)
{
	// A read straddling a block edge is consumed by a write overlapping one
	// byte of it, and only by that write.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x41), range(0xfffe'f000u, 0x2000u), kV); // blocks 0xfffe and 0xffff, 0xfffef000..0xffff0fff

	scheduler.begin_pass();
	scheduler.note_write(resource(0x42), range(0xfffd'0000u, 0x1000u), kBD); // block 0xfffd, disjoint — no consume
	scheduler.note_write(resource(0x43), range(0xffff'0fffu, 1), kBD);       // last byte of the read — consumes it

	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerConsumption, SamePassMixedCandidatesSplitPerCandidate)
{
	// Per-candidate emission splits the legal (vertex) producer from the illegal
	// (fragment) one: the legal half is emitted intra-pass, the illegal half is
	// carried to the next encoder.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.note_write(resource(0x42), range(0x2000, 0x1000), kF);

	scheduler.note_read(resource(0x43), range(0x1000, 0x2000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
	EXPECT_TRUE(scheduler.split_requested());

	scheduler.begin_pass();
	scheduler.emit_split_barriers(fake_encoder(1));
	expect_recorded_barriers({
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kF, kF, MTL4::VisibilityOptionDevice, fake_encoder(1) },
	});
}

// =============================================================================
// 6. Frame and pass lifetime, split latch, flush semantics
// =============================================================================

TEST(MTLSchedulerLifetime2, BeginFrameRetiresTheRecordsBothDirections)
{
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	expect_eq_queued(scheduler, 0u);

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_write(resource(0x41), range(0x1000, 0x1000), kBD);
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kF);
	expect_eq_queued(scheduler, 1u); // this frame works normally
}

TEST(MTLSchedulerLifetime2, PendingEdgesDroppedByBeginPassWithoutFlush)
{
	// A pending edge that is never flushed is dropped by the next begin_pass —
	// but the satisfied scratch is per-pass too, so the next read emits fresh
	// edges for its own consumer classes instead of inheriting consumed state.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	expect_eq_queued(scheduler, 1u);

	scheduler.begin_pass(); // no flush
	expect_eq_queued(scheduler, 0u);

	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV); // a fresh consumer class: the vertex edge is emitted
	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerLifetime2, FlushNullCarriesPendingEdgesToTheNextPass)
{
	// flush(nullptr) is the encoder-less path: nothing is emitted now, but
	// every queued edge is carried into the carried list and emitted in queue
	// form by the next open pass's emit_split_barriers — never destroyed.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(nullptr);

	expect_eq_queued(scheduler, 0u);
	expect_no_barriers_recorded();
	EXPECT_FALSE(scheduler.split_requested()); // the null flush does not latch the split

	scheduler.begin_pass();
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1))); // the edge rides the carried list
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });

	// The carried list is cleared by the emission; a fresh pass owes nothing else.
	scheduler.begin_pass();
	EXPECT_FALSE(scheduler.emit_split_barriers(fake_encoder(1)));
}

TEST(MTLSchedulerLifetime2, FlushNullThenRealFlushDoesNotEmitOldEdges)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);

	scheduler.flush(nullptr);
	scheduler.flush(fake_encoder(0)); // the pending batch was carried, not left in the queue

	expect_no_barriers_recorded();
}

TEST(MTLSchedulerLifetime2, FlushNullCarriesButNeverLatchesTheSplit)
{
	// The null flush does not latch the split (a fragment-producer edge that a
	// real flush would carry), but it does move the batch to the carried list,
	// and that list is drained in queue form on the next pass open.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);

	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.begin_pass();
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1)));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

TEST(MTLSchedulerLifetime2, RecordsSurviveAcrossFlushesAndPasses)
{
	// flush() does not retire records; a write noted in pass 1 is still found
	// by a read in pass 3 after two intermediate flushes.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.flush(fake_encoder(0)); // nothing pending (no reads yet)
	scheduler.begin_pass();           // pass 2
	scheduler.flush(fake_encoder(0)); // still nothing
	scheduler.begin_pass();           // pass 3
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerLifetime2, PassCounterAdvancesOncePerBeginPass)
{
	// The same-pass/earlier-pass classification depends on m_pass: a write
	// noted before a read in the same pass classifies as same-pass; across a
	// begin_pass it never does.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV); // pass 1
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);  // pass 1, same pass

	mtl::pass_scheduler scheduler2;
	fresh_frame_pass(scheduler2); // pass 1
	scheduler2.note_write(resource(0x40), range(0x1000, 0x1000), kV); // pass 1
	scheduler2.begin_pass();                                         // pass 2
	scheduler2.note_read(resource(0x40), range(0x1000, 0x1000), kF);  // pass 2, earlier pass

	expect_eq_queued(scheduler, 1u);
	expect_eq_queued(scheduler2, 1u);
}

TEST(MTLSchedulerLifetime2, SamePassVsEarlierPassSpecsDiffer)
{
	// The classification is observable through the emitted kind: same-pass is
	// intra-pass (or carried), earlier-pass is the queue form.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);  // same pass, legal producer
	scheduler.flush(fake_encoder(0));

	mtl::pass_scheduler scheduler2;
	fresh_frame_pass(scheduler2);
	scheduler2.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler2.begin_pass();
	scheduler2.note_read(resource(0x40), range(0x1000, 0x1000), kF);  // earlier pass
	scheduler2.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerLifetime2, SplitBarriersAreEmittedBeforePendingBarriers)
{
	// The backend emits carried edges on the next encoder before its own
	// pending batch; the emission order must be preserved.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kF);  // illegal same-pass producer
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);   // carried at flush
	scheduler.flush(fake_encoder(0));                                 // carries the edge

	scheduler.begin_pass(); // pass 2
	scheduler.note_write(resource(0x41), range(0x2000, 0x1000), kBD); // earlier-pass candidate for pass 2's read
	scheduler.note_read(resource(0x41), range(0x2000, 0x1000), kF);
	// record_pass_hazards order: split barriers first, then the pending batch.
	scheduler.emit_split_barriers(fake_encoder(1));
	scheduler.flush(fake_encoder(1));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) },   // carried from pass 1
		{ barrier_kind::after_encoder_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(1) }, // pass 2's pending batch (same-pass, legal producer)
	});
}

TEST(MTLSchedulerLifetime2, CargoCarriedCyclicThenFlushPendingOrder)
{
	// record_pass_hazards order with a cyclic request as well: the cyclic edge
	// is pushed to the carried list before the flush; both are emitted in the
	// order the backend emits them.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler); // pass 1
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD); // earlier-pass for pass 2's read

	scheduler.begin_pass(); // pass 2
	scheduler.request_texture_barrier();   // pushes the cyclic spec
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);  // pending earlier-pass spec
	// record_pass_hazards: emit_split_barriers first, then flush.
	scheduler.emit_split_barriers(fake_encoder(1));
	scheduler.flush(fake_encoder(1));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(1) }, // the cyclic spec
		{ barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(1) },  // pending
	});
}

TEST(MTLSchedulerLifetime2, FlushClearsPendingExactlyOnce)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerLifetime2, EmptyFlushEmitsNothing)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.flush(fake_encoder(0));
	EXPECT_TRUE(recorded_barriers().empty());
}

TEST(MTLSchedulerLifetime2, ManyPassesManyFramesDoesNotCorrupt)
{
	// Interleave frames and passes; every emission must be one of the three
	// known forms, and the recorded count must equal the per-pass edge count.
	// Per pass the read consumes the same-pass write (one intra-pass barrier
	// each pass), and the re-noted write also consumes the previous pass's read
	// (one earlier-pass WAR queue barrier from pass 2 of each frame).  Frames:
	// pass 1 contributes 1 barrier, passes 2-6 contribute 2 each.
	reset_recorded_barriers();
	mtl::pass_scheduler scheduler;
	for (u32 frame = 0; frame < 8; ++frame)
	{
		scheduler.begin_frame();

		for (u32 pass = 0; pass < 6; ++pass)
		{
			scheduler.begin_pass();
			scheduler.note_write(resource(0x100), range(0x1000, 0x1000), kBD);
			scheduler.note_read(resource(0x100), range(0x1000, 0x1000), kV);
			scheduler.flush(fake_encoder(0));
		}
	}

	EXPECT_EQ(recorded_barriers().size(), 8u * (1u + 5u * 2u));
}

TEST(MTLSchedulerLifetime2, NoteBeforeFirstBeginPassStillClassifies)
{
	// m_pass is 0 until the first begin_pass; two before it are same-pass.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kV, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerLifetime2, BeginFrameOnANonEmptySchedulerRetiresEverything)
{
	mtl::pass_scheduler scheduler;

	for (u32 frame = 0; frame < 4; ++frame)
	{
		scheduler.begin_frame();
		scheduler.begin_pass();
		scheduler.note_write(resource(0x40 + frame), range(0x1000 * (frame + 1), 0x1000), kBD);
		scheduler.note_read(resource(0x40 + frame), range(0x1000 * (frame + 1), 0x1000), kV);
	}

	// After the last frame's notes, the next frame must see nothing.
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_read(resource(0x43), range(0x4000, 0x1000), kV);
	expect_eq_queued(scheduler, 0u);
}

// =============================================================================
// 7. The deferred-upload hypothesis, exercised at the scheduler level
// =============================================================================

TEST(MTLSchedulerHypothesis, DeferredUploadRouteCapturesTheQueueFormBarrier)
{
	// The exact flow the backend runs for a deferred materialization: the
	// upload is noted before the consuming pass opens; the read is noted inside
	// it.  The edge must be the queue form on the consuming pass's encoder.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	// resolve phase of the clause (before record_pass_hazards runs): the upload
	// blits are noted as a transfer write of the bound image.
	scheduler.note_write(resource(0x501), range(0x5011'0000u, 0x600000u), kBD);

	// record_pass_hazards of the same clause: begin_pass bumps the pass counter,
	// the sample is noted, the flush runs on the pass's encoder.
	scheduler.begin_pass();
	scheduler.note_read(resource(0x501), range(0x5011'0000u, 0x600000u), kF);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerHypothesis, RetainedPooledTemporaryRematerializesWithAnEdgeEveryClause)
{
	// A retained temporary cached at bind time is re-materialised (the copies
	// re-run) when the descriptor expires; every clause that re-materialises
	// must re-note the write, so the edge is re-established every clause.
	for (u32 clause = 0; clause < 3; ++clause)
	{
		SCOPED_TRACE("clause " + std::to_string(clause));
		reset_recorded_barriers();

		mtl::pass_scheduler scheduler;
		scheduler.begin_frame();

		// The resolve of the clause; the retained temp is re-written by the copies.
		scheduler.note_write(resource(0x601), range(0x6020'0000u, 0x100000u), kBD);
		scheduler.begin_pass();
		scheduler.note_read(resource(0x601), range(0x6020'0000u, 0x100000u), kF);
		scheduler.flush(fake_encoder(0));

		expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	}
}

TEST(MTLSchedulerHypothesis, RetainedDeferredUsesViewIdentityForBothNotes)
{
	// The copy wrote the base image; the bind returns a swizzled view.  The
	// caller notes the write and the read against the view's resource id, so
	// identity matches even though the guest footprint is identical anyway.
	// Metal barriers are stage-global, so the queue form covers the base image
	// as well; the edge would be missing only if the notes disagreed with
	// each other, which they do not.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.note_write(resource(0x701), range(0x7030'0000u, 0x100000u), kBD); // the view's id, as noted at bind

	scheduler.begin_pass();
	scheduler.note_read(resource(0x701), range(0x7030'0000u, 0x100000u), kF);   // the same id, as noted in hazards
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerHypothesis, UnnotedTransferIsUnordered)
{
	// The counter-example the claim predicts, demonstrated: a transfer whose
	// write the caller never reports produces no edge at all, even when the
	// guest range is identical.  The scheduler is only as strong as the notes,
	// which is exactly why the recorder's encoder boundary, not this oracle, is
	// what makes the backend correct (C6).
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	// No note_write anywhere in the frame: the transfer is invisible to the
	// hazard oracle.
	scheduler.begin_pass();
	scheduler.note_read(resource(0x801), range(0x8040'0000u, 0x100000u), kF);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerHypothesis, SharedDeferredUploadOrdersVertexAndFragment)
{
	// The backend notes the vertex-stage materialisation write too, so both
	// stages' uploads are recorded.  The fragment and vertex reads of the same
	// materialised image each get their own edge, per consumer class — the
	// fragment read can no longer starve the vertex read.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.note_write(resource(0x901), range(0x9050'0000u, 0x100000u), kBD); // the materialised image's id, noted at bind (fragment)

	scheduler.note_write(resource(0x901), range(0x9050'0000u, 0x100000u), kBD); // same image noted again at bind (vertex)

	scheduler.begin_pass();
	scheduler.note_read(resource(0x901), range(0x9050'0000u, 0x100000u), kF);
	scheduler.note_read(resource(0x901), range(0x9050'0000u, 0x100000u), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

TEST(MTLSchedulerHypothesis, VertexOnlyMaterialisationStillOrdersVertex)
{
	// The starvation needs the fragment stage to consume the shared record
	// first; an unreferenced fragment program or a vertex-only sample orders
	// the vertex stage correctly.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.note_write(resource(0x901), range(0x9050'0000u, 0x100000u), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x901), range(0x9050'0000u, 0x100000u), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerHypothesis, TwoDeferredMaterialsInOneClauseEachEdge)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.note_write(resource(0xa01), range(0xa050'0000u, 0x100000u), kBD); // materialised texture 1
	scheduler.note_write(resource(0xa02), range(0xa060'0000u, 0x100000u), kBD); // materialised texture 2

	scheduler.begin_pass();
	scheduler.note_read(resource(0xa01), range(0xa050'0000u, 0x100000u), kF);
	scheduler.note_read(resource(0xa02), range(0xa060'0000u, 0x100000u), kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) },
		{ barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) },
	});
}

// =============================================================================
// 8. Emit-path API: classify and observe every barrier form directly
// =============================================================================

TEST(MTLSchedulerEmit, EmitQueueBarrierEmitsQueueFormPassthrough)
{
	reset_recorded_barriers();

	mtl::emit_queue_barrier(fake_encoder(0), { kBD, kF, MTL4::VisibilityOptionDevice, true });

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerEmit, EmitProducerBarrierEmitsAfterStagesPassthrough)
{
	reset_recorded_barriers();

	mtl::emit_producer_barrier(fake_encoder(0), { kAll, kAll, MTL4::VisibilityOptionResourceAlias });

	expect_recorded_barriers({ { barrier_kind::after_stages, kAll, kAll, MTL4::VisibilityOptionResourceAlias, fake_encoder(0) } });
}

TEST(MTLSchedulerEmit, EmitBarrierCrossPassIgnoresStageLegality)
{
	// The queue form never refuses: cross_pass wins over the intra-pass rule.
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kF, kV, MTL4::VisibilityOptionDevice, true /* cross_pass */ });
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kF, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_TRUE(emitted);
}

TEST(MTLSchedulerEmit, EmitBarrierIntraPassRefusesIllegalProducer)
{
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kF, kV, MTL4::VisibilityOptionDevice, false /* cross_pass */ });

	expect_no_barriers_recorded();
	EXPECT_FALSE(emitted);
}

TEST(MTLSchedulerEmit, EmitBarrierIntraPassRefusesTileProducer)
{
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kT, kV, MTL4::VisibilityOptionDevice, false });

	expect_no_barriers_recorded();
	EXPECT_FALSE(emitted);
}

TEST(MTLSchedulerEmit, EmitBarrierIntraPassRefusesMixedLegalAndIllegalProducer)
{
	// One illegal bit anywhere in the mask refuses the whole spec.
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kVFT, kV, MTL4::VisibilityOptionDevice, false });

	expect_no_barriers_recorded();
	EXPECT_FALSE(emitted);
}

TEST(MTLSchedulerEmit, EmitBarrierIntraPassAcceptsLegalProducer)
{
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kBD, kF, MTL4::VisibilityOptionDevice, false });

	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kBD, kF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_TRUE(emitted);
}

TEST(MTLSchedulerEmit, EmitBarrierCrossPassForcesQueueFormAtSplitEmission)
{
	// What emit_split_barriers does with a carried spec: flips cross_pass to
	// true, so even a stage-legal carried spec would observe the queue form.
	reset_recorded_barriers();

	const bool emitted = mtl::emit_barrier(fake_encoder(0), { kV, kV, MTL4::VisibilityOptionDevice, true });

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kV, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_TRUE(emitted);
}

TEST(MTLSchedulerEmit, CyclicRequestEmitsTheProducerFormOnTheNextEmit)
{
	// The cyclic-reference latch: one request produces one queue-form edge with
	// the fragment|tile producer used for the pass that wrote the surface.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();

	EXPECT_FALSE(scheduler.split_requested());       // the request alone never latches the split
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(1)));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
	EXPECT_FALSE(scheduler.emit_split_barriers(fake_encoder(1)));
}

TEST(MTLSchedulerEmit, CyclicRequestEmitsNothingWithoutTheEmit)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();
	scheduler.flush(fake_encoder(0)); // pending only; the carried list is untouched

	expect_no_barriers_recorded();
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(0)));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerEmit, CyclicRequestIsSingleShotPerRequest)
{
	// consume_texture_barrier_request is single-read in the cache; the scheduler
	// side has no dedup of its own — two requests produce two carried edges.
	// The backend therefore emits one edge per cyclic sample, not one per frame.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();
	scheduler.request_texture_barrier();

	scheduler.emit_split_barriers(fake_encoder(1));

	expect_recorded_barriers({
		{ barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(1) },
		{ barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(1) },
	});
}

TEST(MTLSchedulerEmit, CyclicRequestDroppedByBeginFrame)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();

	scheduler.begin_frame();
	EXPECT_FALSE(scheduler.emit_split_barriers(fake_encoder(0)));
	expect_no_barriers_recorded();
}

TEST(MTLSchedulerEmit, EmitSplitBarriersNullReturnsFalseKeepsRequest)
{
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();

	EXPECT_FALSE(scheduler.emit_split_barriers(nullptr));
	EXPECT_TRUE(scheduler.emit_split_barriers(fake_encoder(0)));
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kFT, kVF, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerEmit, SamePassCarriedWrapperForcesCrossPassEvenForLegalStages)
{
	// flush() never carries a stage-legal spec; the carry can only be observed
	// with an illegal producer, so this test pins the queue-form forcing the
	// emit_split_barriers path applies to carried specs.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kFT);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0)); // carries it

	scheduler.begin_pass();
	scheduler.emit_split_barriers(fake_encoder(1));

	// The emitted kind is the queue form even though kFT could never have been
	// refused had cross_pass been set at flush time.
	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kFT, kV, MTL4::VisibilityOptionDevice, fake_encoder(1) } });
}

// =============================================================================
// 9. Degenerate and adversarial notes
// =============================================================================

TEST(MTLSchedulerDegenerate, ZeroStageBothNotesSamePassStillEmitsNothing)
{
	// Pin as-is: a zero-stage write is invisible to the candidate scan, so an
	// empty-stage read of the same resource produces no edge and no barrier.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLSchedulerDegenerate, ZeroStageWriteAndZeroStageReadAcrossPassesStillEmitsNothing)
{
	// Pin as-is: a zero-stage producer is skipped, so an empty-stage read
	// across passes produces no edge either.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), k0);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerDegenerate, ZeroStageReadProducesNoEdge)
{
	// A gpu_reference-class read (an empty stage mask) produces no edge and
	// consumes nothing: a barrier whose consumer mask is zero orders nothing.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);

	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), k0);
	scheduler.flush(fake_encoder(0));

	expect_no_barriers_recorded();
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerDegenerate, NullResourceIdCollidesAcrossDifferentCallers)
{
	// The null id is a valid key; two unrelated notes under it match by
	// identity.  The backend never notes id 0 (every call site guards on
	// texture._impl), so this is an API-level hazard, pinned.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_write(MTL::ResourceID{}, {}, kBD);

	scheduler.begin_pass();
	scheduler.note_read(MTL::ResourceID{}, {}, kV);
	scheduler.flush(fake_encoder(0));

	expect_recorded_barriers({ { barrier_kind::after_queue_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
}

TEST(MTLSchedulerDegenerate, NoteBeforeAnyFrameOrPassNeverCrashes)
{
	// A bare scheduler (no begin_frame, no begin_pass) accepts notes, flushes
	// to nothing, and resets cleanly afterwards.
	reset_recorded_barriers();

	mtl::pass_scheduler scheduler;
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.note_read(resource(0x41), range(0x1000, 0x1000), kV);
	scheduler.flush(fake_encoder(0));

	// The pre-frame note is a cross-note overlap: one same-pass spec, one
	// intra-pass barrier, legal producer.
	expect_recorded_barriers({ { barrier_kind::after_encoder_stages, kBD, kV, MTL4::VisibilityOptionDevice, fake_encoder(0) } });
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	expect_eq_queued(scheduler, 0u); // begin_frame retired the pre-frame note
}

TEST(MTLSchedulerDegenerate, BeginFrameTwiceRetiresEverythingBothTimes)
{
	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_write(resource(0x40), range(0x1000, 0x1000), kBD);
	scheduler.begin_frame();

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.note_read(resource(0x40), range(0x1000, 0x1000), kV);
	expect_eq_queued(scheduler, 0u);
}

TEST(MTLSchedulerDegenerate, HugeRangeIndexStressDoesNotCorrupt)
{
	// A footprint covering the whole 4 GiB address space is indexed across
	// every 64 KiB block, then a small write finds it.  The index is the
	// largest structure a note can build; this pins the absence of an overrun.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), utils::address_range32::start_end(0u, 0xffff'ffffu), kV);

	scheduler.begin_pass();
	scheduler.note_write(resource(0x50), range(0xdead'beefu, 0x1000u), kBD);

	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerDegenerate, WriteScanAcrossEvery64KiBBlockFindsTheRead)
{
	// A write spanning many blocks finds a read indexed at either extreme.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);
	scheduler.note_read(resource(0x40), range(0x10'0000u, 0x1000u), kV); // block 16

	scheduler.begin_pass();
	scheduler.note_write(resource(0x50), range(0x0, 0x0100'0000u), kBD); // covers every block up to block 256

	expect_eq_queued(scheduler, 1u);
}

TEST(MTLSchedulerDegenerate, InterleavedWritesAndReadsAcrossRangesNoPhantoms)
{
	// A frame's worth of interleaved notes on disjoint ranges produces no
	// edges, then one overlapping pair produces exactly one.
	mtl::pass_scheduler scheduler;
	fresh_frame_pass(scheduler);

	for (u32 index = 0; index < 16; ++index)
	{
		scheduler.note_write(resource(0x1000 + index), range(0x1'0000u * (index + 1), 0x8000u), kBD);
		scheduler.note_read(resource(0x2000 + index), range(0x1'0000u * (index + 1) + 0x8000u, 0x4000u), kV);
	}

	expect_eq_queued(scheduler, 0u);

	scheduler.begin_pass();
	// A read of a resource written earlier in the frame matches by identity;
	// the WAW second write in that position would match nothing (pinned in §4).
	scheduler.note_read(resource(0x1000), range(0x10000u, 0x1000u), kF);

	expect_eq_queued(scheduler, 1u);
}

#endif // RSX_ENABLE_METAL4
