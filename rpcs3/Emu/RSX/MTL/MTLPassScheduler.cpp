#include "MTLPassScheduler.h"
#include "MTLInstrumentation.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	MTL::Stages access_stages(rsx::surface_access access, bool vertex_stage)
	{
		if (access == rsx::surface_access::shader_write || access == rsx::surface_access::shader_read)
		{
			return vertex_stage ? MTL::StageVertex : MTL::StageFragment;
		}

		if (access == rsx::surface_access::transfer_write || access == rsx::surface_access::transfer_read)
		{
			return MTL::StageBlit | MTL::StageDispatch;
		}

		if (access == rsx::surface_access::memory_write || access == rsx::surface_access::memory_read)
		{
			return MTL::StageAll;
		}

		// gpu_reference only marks that the GPU refers to the object; it orders
		// nothing, so it maps to an empty stage mask (HLD-03 §2.4).
		return static_cast<MTL::Stages>(0);
	}

	void pass_scheduler::begin_frame()
	{
		m_records.clear();
		m_pending.clear();
		m_carried.clear();
		m_pass = 0;
		m_split = false;
	}

	void pass_scheduler::begin_pass()
	{
		// The per-pass edges and the split request belong to the pass.  The record
		// stays, and so does a carried edge: the caller emits it on the encoder this
		// pass opens.
		m_pending.clear();
		m_pass++;
		m_split = false;
	}

	void pass_scheduler::note_read(MTL::ResourceID resource, MTL::Stages stages)
	{
		// RAW: a completed write must reach this read.  A writer in an earlier encoder
		// takes the queue form.  A writer in the open pass is the TBDR feedback case,
		// which one encoder cannot order, so the edge carries to the encoder that
		// follows this one (HLD-03 §2.4, §2.1 item 5).
		record& entry = m_records[resource._impl];

		if (entry.write_stages)
		{
			m_pending.push_back(barrier_spec{ entry.write_stages, stages, MTL4::VisibilityOptionDevice, entry.write_pass != m_pass });
			entry.write_stages = 0;
		}

		entry.read_stages = stages;
		entry.read_pass = m_pass;
	}

	void pass_scheduler::note_write(MTL::ResourceID resource, MTL::Stages stages)
	{
		// WAR: a completed read must reach this write.
		record& entry = m_records[resource._impl];

		if (entry.read_stages)
		{
			m_pending.push_back(barrier_spec{ entry.read_stages, stages, MTL4::VisibilityOptionDevice, entry.read_pass != m_pass });
			entry.read_stages = 0;
		}

		entry.write_stages = stages;
		entry.write_pass = m_pass;
	}

	void pass_scheduler::flush(MTL4::CommandEncoder* encoder)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "pass/barrier batch");
		if (!encoder)
		{
			m_pending.clear();
			return;
		}

		for (const barrier_spec& spec : m_pending)
		{
			if (emit_barrier(encoder, spec))
			{
				continue;
			}

			// The producer is a fragment or tile stage of this encoder, so the edge
			// cannot be ordered inside it.  The pass splits: the next encoder emits
			// it with the queue form, once this encoder's tile has flushed.
			m_split = true;
			m_carried.push_back(spec);
		}

		m_pending.clear();
	}

	bool pass_scheduler::emit_split_barriers(MTL4::CommandEncoder* encoder)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "pass/split barriers");
		if (!encoder || m_carried.empty())
		{
			return false;
		}

		for (barrier_spec spec : m_carried)
		{
			spec.cross_pass = true;
			emit_barrier(encoder, spec);
		}

		m_carried.clear();
		m_split = false;
		return true;
	}

	void pass_scheduler::request_texture_barrier()
	{
		// The surface's write stages are the fragment and tile stages of the pass
		// that carried it, and the consumer is a sample, which is a vertex or a
		// fragment read.
		m_carried.push_back(barrier_spec{ MTL::StageFragment | MTL::StageTile, MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice, true });
	}
}

#endif // RSX_ENABLE_METAL4
