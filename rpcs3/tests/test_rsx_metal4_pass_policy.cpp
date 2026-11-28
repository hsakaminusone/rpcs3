#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLPassPolicy.h"

// The draw absorption policy (METAL4-ARCH-REDESIGN-2026-11-09.md C1).
//
// A draw may encode into the open render pass only when the recorder still owns
// the encoder, the pass descriptor would be rebuilt identically, and the draw
// does not sample an attachment of that pass.  The predicate is deliberately a
// plain struct of four bools so the policy can be exhausted here without a
// Metal device.  Correctness does not rest on the predicate: a false positive
// is impossible by construction (all four are checked), and a false negative
// only costs a pass.
#ifdef RSX_ENABLE_METAL4

#include <cstdio>

namespace
{
	mtl::draw_pass_policy policy(bool open, bool owned, bool key, bool samples)
	{
		mtl::draw_pass_policy result;
		result.encoder_open = open;
		result.recorder_owns_encoder = owned;
		result.pass_key_matches = key;
		result.samples_attachment = samples;
		return result;
	}

	const char* describe(const mtl::draw_pass_policy& p)
	{
		static thread_local char text[128];
		std::snprintf(text, sizeof(text), "open=%d owns=%d key=%d samples=%d",
			p.encoder_open, p.recorder_owns_encoder, p.pass_key_matches, p.samples_attachment);
		return text;
	}
}

TEST(MTLPassPolicy, AbsorbsOnlyWhenEveryConditionHolds)
{
	// The one positive case: an open, owned, key-matching pass the draw does not
	// sample from.  Every other row of the 16-case table must refuse.
	for (unsigned bits = 0; bits < 16; ++bits)
	{
		const mtl::draw_pass_policy p = policy(bits & 1u, bits & 2u, bits & 4u, bits & 8u);
		SCOPED_TRACE(describe(p));

		const bool expected = p.encoder_open && p.recorder_owns_encoder
			&& p.pass_key_matches && !p.samples_attachment;

		EXPECT_EQ(p.absorbs(), expected);
	}
}

TEST(MTLPassPolicy, ClosedEncoderNeverAbsorbs)
{
	EXPECT_FALSE(policy(false, true, true, false).absorbs());
}

TEST(MTLPassPolicy, ForeignEncoderNeverAbsorbs)
{
	// A copy, upload, readback, resolve or slot re-entry closed the cached
	// encoder: the pointer is stale, and only the recorder can say so.
	EXPECT_FALSE(policy(true, false, true, false).absorbs());
}

TEST(MTLPassPolicy, ChangedPassKeyNeverAbsorbs)
{
	// A surface swap, a resolve target, a sample-count change or a zcull buffer
	// replacement all rebuild the pass descriptor, so the draw needs its own
	// pass even though the old encoder is still open.
	EXPECT_FALSE(policy(true, true, false, false).absorbs());
}

TEST(MTLPassPolicy, SampledAttachmentNeverAbsorbs)
{
	// Metal has no feedback primitive.  Even with an open, owned, key-matching
	// pass, a draw that samples an attachment needs the encoder closed: the
	// store then holds the pixels and the recorder's boundary barrier orders
	// them before the fresh pass samples them (H2).
	EXPECT_FALSE(policy(true, true, true, true).absorbs());
}

TEST(MTLPassPolicy, FeedbackWinsOverEveryOtherCondition)
{
	// The C3 guard is the only condition that has to be consulted after the
	// other three pass: a feedback draw must split even when nothing else moved.
	for (unsigned bits = 0; bits < 8; ++bits)
	{
		mtl::draw_pass_policy p = policy(bits & 1u, bits & 2u, bits & 4u, true);
		SCOPED_TRACE(describe(p));
		EXPECT_FALSE(p.absorbs());
	}
}

TEST(MTLPassPolicy, ARefusalLeavesTheOldPolicyStateIntact)
{
	// The caller clears encoder_open when it splits.  The predicate must not
	// cache its answer: reusing the struct after that clear must refuse.
	mtl::draw_pass_policy p = policy(true, true, true, true);
	EXPECT_FALSE(p.absorbs());

	p.samples_attachment = false;
	EXPECT_TRUE(p.absorbs());

	p.encoder_open = false;
	EXPECT_FALSE(p.absorbs());
}

#endif // RSX_ENABLE_METAL4
