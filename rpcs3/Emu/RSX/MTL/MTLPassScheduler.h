#pragma once

#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"

#include <unordered_map>
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
		// A frame boundary retires every record, so an edge cannot leak across it
		// (HLD-03 §5).
		void begin_frame();

		// Start a pass.  A pass boundary orders everything already queued, so the
		// per-pass edges and the split request clear here.  The per-resource record
		// survives, because a read in this pass must still be ordered behind a write
		// in an earlier pass of the frame.
		void begin_pass();

		// Record the read and the write of a resource at stages in the open pass.
		// Each one consumes the opposite record, because the edge it finds is the
		// edge it queues.
		void note_read(MTL::ResourceID resource, MTL::Stages stages);
		void note_write(MTL::ResourceID resource, MTL::Stages stages);

		// Emit every queued edge that the open encoder can order.  An edge it cannot
		// order is carried to the next encoder and latches the split request.
		void flush(MTL4::CommandEncoder* encoder);

		// Emit the carried edges on the encoder the caller opened after the split.
		// Returns false when nothing was carried.  The caller calls this before it
		// records the new pass (HLD-03 §2.4).
		bool emit_split_barriers(MTL4::CommandEncoder* encoder);

		bool split_requested() const { return m_split; }

	private:
		struct record
		{
			MTL::Stages write_stages = 0;
			u32 write_pass = 0;
			MTL::Stages read_stages = 0;
			u32 read_pass = 0;
		};

		// One entry per resource the frame has touched.  A flat map keeps the note
		// path O(1); a linear scan would make the frame's cost quadratic in its
		// working set, which is the load-bearing path of every draw.
		std::unordered_map<u64, record> m_records;
		std::vector<barrier_spec> m_pending;
		std::vector<barrier_spec> m_carried;
		u32 m_pass = 0;
		bool m_split = false;
	};
}

#endif // RSX_ENABLE_METAL4
