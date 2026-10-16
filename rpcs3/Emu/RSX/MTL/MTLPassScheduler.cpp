#include "MTLPassScheduler.h"

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

	void pass_scheduler::begin_pass()
	{
		m_writers.clear();
		m_readers.clear();
		m_pending.clear();
		m_split = false;
	}

	bool pass_scheduler::note_write(MTL::ResourceID resource, MTL::Stages stages)
	{
		// WAR: an earlier read must complete before this write.  If the read cannot
		// be ordered from inside the encoder, the pass must split instead.
		for (const touch& reader : m_readers)
		{
			if (reader.resource._impl == resource._impl)
			{
				if (stages_legal_intra_pass(reader.stages))
				{
					m_pending.push_back(barrier_spec{ reader.stages, stages, MTL4::VisibilityOptionNone });
				}
				else
				{
					m_split = true;
				}

				break;
			}
		}

		// Record the write, replacing an earlier writer of the same resource so each
		// resource keeps one current stage set.
		bool recorded = false;

		for (touch& writer : m_writers)
		{
			if (writer.resource._impl == resource._impl)
			{
				writer.stages = stages;
				recorded = true;
				break;
			}
		}

		if (!recorded)
		{
			m_writers.push_back(touch{ resource, stages });
		}

		if (m_writers.size() + m_readers.size() >= max_tracked_touches)
		{
			m_split = true;
		}

		return !m_split;
	}

	bool pass_scheduler::note_read(MTL::ResourceID resource, MTL::Stages stages)
	{
		// RAW: an earlier write must complete before this read.  A resource that is
		// both an attachment and a sampled texture is exactly this case, so the TBDR
		// feedback rule needs no separate branch.  If the write cannot be ordered
		// from inside the encoder, the pass must split instead.
		for (const touch& writer : m_writers)
		{
			if (writer.resource._impl == resource._impl)
			{
				if (stages_legal_intra_pass(writer.stages))
				{
					m_pending.push_back(barrier_spec{ writer.stages, stages, MTL4::VisibilityOptionNone });
				}
				else
				{
					m_split = true;
				}

				break;
			}
		}

		// Record the read, replacing an earlier reader of the same resource so each
		// resource keeps one current stage set.
		bool recorded = false;

		for (touch& reader : m_readers)
		{
			if (reader.resource._impl == resource._impl)
			{
				reader.stages = stages;
				recorded = true;
				break;
			}
		}

		if (!recorded)
		{
			m_readers.push_back(touch{ resource, stages });
		}

		if (m_writers.size() + m_readers.size() >= max_tracked_touches)
		{
			m_split = true;
		}

		return !m_split;
	}

	void pass_scheduler::note_edge(const barrier_spec& spec)
	{
		m_pending.push_back(spec);
	}

	void pass_scheduler::flush(MTL4::CommandEncoder* encoder)
	{
		if (!encoder)
		{
			return;
		}

		for (const barrier_spec& spec : m_pending)
		{
			if (!emit_barrier(encoder, spec))
			{
				m_split = true;
			}
		}

		m_pending.clear();
	}
}

#endif // RSX_ENABLE_METAL4
