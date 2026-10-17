#pragma once

#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"

#include <vector>

namespace mtl
{
	// The stage mask of one RSX access (HLD-03 §2.4).  The producer and consumer
	// columns of that table are the same mask for every row, so one function serves
	// both.  vertex_stage selects the vertex half of the two shader rows.
	MTL::Stages access_stages(rsx::surface_access access, bool vertex_stage);

	// The hazard-derived pass scheduler (HLD-01 C3, HLD-03 §2.4).  It owns the
	// encoder boundaries and every barrier.  It does not open or close encoders: it
	// answers, and the caller acts.
	class pass_scheduler
	{
	public:
		// Start a pass.  A pass boundary orders every resource, so the touch sets and
		// the split request clear here.  The caller calls this when it opens an
		// encoder, including the first encoder of a frame.
		void begin_pass();

		// Record a write of resource by stages in the open pass.  Returns false when
		// the write cannot join the pass.
		bool note_write(MTL::ResourceID resource, MTL::Stages stages);

		// Record a read of resource by stages in the open pass.  Returns false when
		// the read cannot join the pass.
		bool note_read(MTL::ResourceID resource, MTL::Stages stages);

		// Queue one explicit edge for a resource whose writer is in another pass.
		void note_edge(const barrier_spec& spec);

		// Emit every queued barrier that is legal inside one encoder.  A barrier that
		// is not legal latches a split request.
		void flush(MTL4::CommandEncoder* encoder);

		bool split_requested() const { return m_split; }

	private:
		struct touch
		{
			MTL::ResourceID resource;
			MTL::Stages stages;
		};

		// Capacity rule: the touch sets stay bounded so the linear scans stay cheap.
		// At the cap the pass splits, and begin_pass() clears both sets (HLD-03 §2.4).
		static constexpr usz max_tracked_touches = 64;

		std::vector<touch> m_writers;
		std::vector<touch> m_readers;
		std::vector<barrier_spec> m_pending;
		bool m_split = false;
	};
}

#endif // RSX_ENABLE_METAL4
