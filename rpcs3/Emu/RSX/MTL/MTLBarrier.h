#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	// One hazard edge: the stages that must complete, the stages that must observe
	// the completion, and the scope of the observation (HLD-03 §3.2).  The default
	// scope is the device, because a weaker one under-orders an ordinary edge
	// (HLD-03 §7 R4).  `.resourceAlias` is only for simultaneously aliased
	// addresses, and flushes to system memory, so it is never ordinary.
	struct barrier_spec
	{
		MTL::Stages producer = 0;
		MTL::Stages consumer = 0;
		MTL4::VisibilityOptions visibility = MTL4::VisibilityOptionDevice;

		// The producer already ran in a closed encoder, so the queue-scope form
		// orders it (HLD-03 §3.2).  A producer in the open encoder uses the cheaper
		// encoder-scope form, when its stages allow it.
		bool cross_pass = false;
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

	// The queue-scope form (HLD-03 §3.2).  It orders work that already ran in an
	// earlier encoder before the consumer stages of the open encoder.  The presenter
	// uses it to order the frame's render work before its own copy.
	void emit_queue_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec);
}

#endif // RSX_ENABLE_METAL4
