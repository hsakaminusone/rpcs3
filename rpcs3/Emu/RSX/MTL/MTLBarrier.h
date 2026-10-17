#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	// One hazard edge: the stages that must complete, the stages that must observe
	// the completion, and the scope of the observation (HLD-03 §3.2).  The visibility
	// default is the same-encoder scope, because the only form this chokepoint emits
	// orders two sides of one encoder.
	struct barrier_spec
	{
		MTL::Stages producer = 0;
		MTL::Stages consumer = 0;
		MTL4::VisibilityOptions visibility = MTL4::VisibilityOptionNone;
	};

	// A render encoder cannot order work that follows its own fragment or tile
	// stages: the tile is already flushed when the barrier runs.  Such an edge needs
	// a pass split (HLD-03 §2.1 item 5; HLD-01 §4 invariant 3).
	bool stages_legal_intra_pass(MTL::Stages producer);

	// The single barrier chokepoint (HLD-03 §3.4).  Every barrier in the backend is
	// emitted here, so the hazard decisions stay recordable.  A false result is a
	// pass-split request: the scheduler owns the producer/consumer pair that replaces
	// it, because it owns the pass nodes.
	bool emit_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec);
}

#endif // RSX_ENABLE_METAL4
