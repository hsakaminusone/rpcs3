#pragma once

#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"
#include "Utilities/address_range.h"

#include <deque>
#include <unordered_map>
#include <vector>

namespace mtl
{
	// The stage mask of one RSX access (HLD-03 §2.4).  The producer and consumer
	// columns of that table are the same mask for every row, so one function serves
	// both.  vertex_stage selects the vertex half of the two shader rows.
	MTL::Stages access_stages(rsx::surface_access access, bool vertex_stage);

	// The hazard-derived pass scheduler.  It is an optimiser, not the correctness
	// authority (METAL4-ARCH-REDESIGN-2026-11-09.md C6).
	//
	// Metal 4 tracks no hazards anywhere, so correctness comes from the recorder:
	// every encoder ends with a producer-scope ALL->ALL device barrier, and the
	// framebuffer/pass key decides when a draw may share an encoder (C1/C2).  A
	// wrong note can therefore cost a barrier or a pass, never a missing one.
	// Within a pass the scheduler may still order a resource it saw written and
	// then read; outside it the boundary already orders everything.
	//
	// The hazard identity is the guest memory a resource occupies, not the Metal
	// object that backs it.  RSX aliases arbitrary guest ranges: the same address
	// can be a render target in one pass and a sampled texture in the next, and the
	// backend then holds two MTLTextures with two different gpuResourceIDs over the
	// same bytes.  Keying a record by gpuResourceID therefore misses every such
	// write-to-read edge, so each note carries the guest range it covers and two
	// accesses conflict when their resources match or their ranges overlap.
	//
	// Ordering is the only thing that makes a report mean something: a pass
	// boundary orders NOTHING by itself — only barriers do.  begin_pass() clears
	// the per-pass queue and the split latch; the records survive, because a read
	// in this pass must still be ordered behind a write in an earlier pass of the
	// frame.
	class pass_scheduler
	{
	public:
		// A frame boundary retires every record, so an edge cannot leak across it
		// (HLD-03 §5).
		void begin_frame();

		// Start a pass.  A pass boundary orders nothing: only barriers do.
		// begin_pass() clears the per-pass queue (m_pending) and the split latch;
		// the per-resource record survives, because a read in this pass must still
		// be ordered behind a write in an earlier pass of the frame.
		void begin_pass();

		// Record the read and the write of a resource at stages in the open pass.
		// Each one consumes the opposite record for the stages it carries, per
		// consumer class, so a pair emits at most one barrier per bucket per frame;
		// a record's producer mask is the union of every stage-set ever written to
		// it this frame.  A range that is invalid (a resource with no guest bytes,
		// such as a temporary image) matches by resource only.
		void note_read(MTL::ResourceID resource, utils::address_range32 range, MTL::Stages stages);
		void note_write(MTL::ResourceID resource, utils::address_range32 range, MTL::Stages stages);

		// Emit every queued edge that the open encoder can order.  An edge it cannot
		// order is carried to the next encoder and latches the split request.
		// When encoder is null (the framebuffer-invalid clear paths), nothing is
		// emitted and every queued edge is carried into the carried list instead of
		// being destroyed: the next open pass emits them in queue form.
		void flush(MTL4::CommandEncoder* encoder);

		// Emit the carried edges on the encoder the caller opened after the split.
		// Returns false when nothing was carried.  The caller calls this before it
		// records the new pass.
		bool emit_split_barriers(MTL4::CommandEncoder* encoder);

		// An explicit request from the texture cache (I4).  A cyclic reference samples
		// a surface an earlier pass of this frame wrote, so the pair is a queue-scope
		// edge: the writer's tile has flushed and only the encoder that opens after
		// the consuming resolve can carry it.  The request is pushed onto the carried
		// list and the caller emits it from the current pass's encoder at the head of
		// that pass.
		void request_texture_barrier();

		// A hint, never a correctness gate.  The recorder already orders every
		// encoder boundary with ALL->ALL, so a missed split costs performance and a
		// spurious one costs an encoder; neither changes the pixels.  The caller is
		// therefore free to open a pass for reasons the scheduler did not predict
		// (the framebuffer key, a feedback copy, a readback).
		bool split_requested() const { return m_split; }

		// The edges the notes have queued since the last begin_pass.  flush() emits
		// and clears them; the tests read them because a scheduler has no encoder.
		usz queued_edges() const { return m_pending.size(); }

	private:
		struct record
		{
			// The guest footprint of the last ranged access.  An invalid range means
			// the resource has no guest bytes, so only its own id can match.
			utils::address_range32 range;

			// The producer mask is the union of every stage-set written to the
			// resource this frame; adding new bits re-arms the satisfied masks.
			MTL::Stages write_stages = 0;
			u32 write_pass = 0;

			// The union of the read stages noted so far this frame.
			MTL::Stages read_stages = 0;
			u32 read_pass = 0;

			// Per-pass scratch: consumer stages for which an edge naming this
			// record's producer mask has already been emitted in this pass, per
			// bucket.  Cleared by every begin_pass, so an edge a later pass owes a
			// record is never silenced by an earlier pass.
			MTL::Stages write_satisfied_same = 0;
			MTL::Stages write_satisfied_earlier = 0;
			MTL::Stages read_satisfied_same = 0;
			MTL::Stages read_satisfied_earlier = 0;

			bool write_listed = false;
			bool read_indexed = false;
			utils::address_range32 read_indexed_range;
		};

		record* fetch(u64 resource);
		void index_read(record& entry);

		static bool overlaps(const record& entry, const utils::address_range32& range);

		// One record per resource the frame has touched.  A deque keeps the pointers
		// the two indexes hold stable while the map grows.
		std::deque<record> m_records;
		std::unordered_map<u64, record*> m_by_resource;

		// The write candidates of a read: one entry per distinct write resource of
		// the frame.  A frame has a handful (up to four colours, depth, resolves and
		// transfer writes), so a scan beats a range index.
		std::vector<record*> m_write_records;

		// The read candidates of a write, indexed by a 64 KiB guest block.  A frame
		// can sample hundreds of textures, so a linear scan would be quadratic in
		// its working set; the exact range test rejects the block false candidates.
		std::unordered_map<u32, std::vector<record*>> m_read_blocks;

		std::vector<barrier_spec> m_pending;
		std::vector<barrier_spec> m_carried;
		u32 m_pass = 0;
		bool m_split = false;
	};
}

#endif // RSX_ENABLE_METAL4
