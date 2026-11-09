#pragma once

// The guard below is defined by MTLAvailability.h, not by the compiler command
// line, so the header must include it: a translation unit that includes this
// header alone would otherwise compile the policy away.
#include "MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	// Whether a draw may join the render encoder that is already open (C1).
	//
	// The four conditions are independent, and each is a different way the open
	// pass stopped being the pass this draw would build:
	//
	//   encoder_open          the instrumented path may have ended the encoder
	//                         after the previous draw.
	//   recorder_owns_encoder a copy, upload, readback, resolve or slot re-entry
	//                         closed it behind the cached pointer.  The recorder
	//                         is the authority; the cache's pointer is a hint.
	//   pass_key_matches      the attachment set, resolve targets, visibility
	//                         buffer, sample count and extent are unchanged, so
	//                         the pass descriptor would be byte-for-byte the same.
	//   samples_attachment    no sampled vertex or fragment texture overlaps an
	//                         attachment of the open pass.  Metal has no feedback
	//                         primitive, so such a draw needs a boundary (H2).
	//
	// Correctness never rests on this predicate alone: a false positive would
	// encode into a pass that does not hold the draw's attachments, which is why
	// the caller also validates the recorder and the pass key.  The predicate is
	// pure so the policy is testable without a Metal device.
	struct draw_pass_policy
	{
		bool encoder_open = false;
		bool recorder_owns_encoder = false;
		bool pass_key_matches = false;
		bool samples_attachment = false;

		bool absorbs() const
		{
			return encoder_open && recorder_owns_encoder && pass_key_matches && !samples_attachment;
		}
	};
}

#endif // RSX_ENABLE_METAL4
