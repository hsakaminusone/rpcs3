#include "MTLPassScheduler.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	namespace
	{
		// The granularity of the read index.  The exact range test below makes the
		// block only a candidate filter, so a coarse block keeps the index small
		// without ever turning a non-overlapping pair into an edge.
		constexpr u32 hazard_block_shift = 16;
	}

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
		m_by_resource.clear();
		m_write_records.clear();
		m_read_blocks.clear();
		m_pending.clear();
		m_carried.clear();
		m_pass = 0;
		m_split = false;
	}

	void pass_scheduler::begin_pass()
	{
		// The per-pass edges and the split request belong to the pass.  The records
		// stay, and so does a carried edge: the caller emits it on the encoder this
		// pass opens.  The satisfied masks are per-pass scratch — they suppress
		// duplicate emission of one hazard within one pass, and they must not
		// silence the edge a later pass owes the same record, so every pass starts
		// with them clear.
		for (record& entry : m_records)
		{
			entry.write_satisfied_same = 0;
			entry.write_satisfied_earlier = 0;
			entry.read_satisfied_same = 0;
			entry.read_satisfied_earlier = 0;
		}

		m_pending.clear();
		m_pass++;
		m_split = false;
	}

	pass_scheduler::record* pass_scheduler::fetch(u64 resource)
	{
		record*& entry = m_by_resource[resource];

		if (!entry)
		{
			entry = &m_records.emplace_back();
		}

		return entry;
	}

	bool pass_scheduler::overlaps(const record& entry, const utils::address_range32& range)
	{
		return entry.range.valid() && range.valid() && entry.range.overlaps(range);
	}

	void pass_scheduler::index_read(record& entry)
	{
		if (!entry.range.valid())
		{
			return;
		}

		// The same unit is noted on every draw of the frame.  The record only needs
		// one index entry per footprint, so the repeat path stays a comparison.
		if (entry.read_indexed && entry.read_indexed_range == entry.range)
		{
			return;
		}

		entry.read_indexed = true;
		entry.read_indexed_range = entry.range;

		const u32 first = entry.range.start >> hazard_block_shift;
		const u32 last = entry.range.end >> hazard_block_shift;

		for (u32 block = first; block <= last; ++block)
		{
			m_read_blocks[block].push_back(&entry);
		}
	}

	void pass_scheduler::note_read(MTL::ResourceID resource, utils::address_range32 range, MTL::Stages stages)
	{
		// RAW: a completed write must reach this read.  A writer in an earlier encoder
		// takes the queue form.  A writer in the open pass is the TBDR feedback case,
		// which one encoder cannot order, so the edge carries to the encoder that
		// follows this one (HLD-03 §2.4, §2.1 item 5).
		record& entry = *fetch(resource._impl);

		if (range.valid())
		{
			entry.range = range;
		}

		// Each conflicting write is its own edge, per bucket.  The satisfied mask is
		// per candidate, so a same-object conflict and a range-alias conflict from
		// two different images never share a suppression.  Emitting per candidate —
		// instead of unioning into one spec — keeps a legal intra-pass producer
		// from being carried along by an illegal one in the same bucket.
		for (record* candidate : m_write_records)
		{
			if (!candidate->write_stages)
			{
				continue;
			}

			// A different Metal object over the same guest bytes is the aliasing case
			// this scheduler exists to catch; the same object matches by identity even
			// when the two notes carry different footprints.
			if (candidate != &entry && !overlaps(*candidate, range))
			{
				continue;
			}

			const bool same_bucket = candidate->write_pass == m_pass;
			MTL::Stages& satisfied = same_bucket ? candidate->write_satisfied_same : candidate->write_satisfied_earlier;
			const MTL::Stages missing = stages & ~satisfied;

			if (!missing)
			{
				continue;
			}

			satisfied |= stages;

			m_pending.push_back(barrier_spec{ candidate->write_stages, missing, MTL4::VisibilityOptionDevice, !same_bucket });
		}

		entry.read_stages |= stages;
		entry.read_pass = m_pass;
		index_read(entry);
	}

	void pass_scheduler::note_write(MTL::ResourceID resource, utils::address_range32 range, MTL::Stages stages)
	{
		// WAR: a completed read must reach this write.
		record& entry = *fetch(resource._impl);

		if (range.valid())
		{
			entry.range = range;
		}

		const auto consume = [&](record& candidate)
		{
			if (!candidate.read_stages)
			{
				return;
			}

			const bool same_bucket = candidate.read_pass == m_pass;
			MTL::Stages& satisfied = same_bucket ? candidate.read_satisfied_same : candidate.read_satisfied_earlier;
			const MTL::Stages missing = stages & ~satisfied;

			if (!missing)
			{
				return;
			}

			satisfied |= stages;

			m_pending.push_back(barrier_spec{ candidate.read_stages, missing, MTL4::VisibilityOptionDevice, !same_bucket });
		};

		// The same object is always a hazard, even when the two notes carry different
		// footprints, so it never depends on the range index.
		consume(entry);

		if (range.valid())
		{
			const u32 first = range.start >> hazard_block_shift;
			const u32 last = range.end >> hazard_block_shift;

			for (u32 block = first; block <= last; ++block)
			{
				const auto it = m_read_blocks.find(block);

				if (it == m_read_blocks.end())
				{
					continue;
				}

				for (record* candidate : it->second)
				{
					if (candidate == &entry || !candidate->read_stages || !overlaps(*candidate, range))
					{
						continue;
					}

					consume(*candidate);
				}
			}
		}

		// The producer mask unions across notes: every stage-set ever written to the
		// resource this frame is part of the barrier producer.  The satisfied masks
		// are per-pass scratch (see begin_pass), so a repeated note across passes
		// re-emits the edges that pass owes.
		entry.write_stages |= stages;
		entry.write_pass = m_pass;

		if (!entry.write_listed)
		{
			entry.write_listed = true;
			m_write_records.push_back(&entry);
		}
	}

	void pass_scheduler::flush(MTL4::CommandEncoder* encoder)
	{
		if (!encoder)
		{
			// No encoder exists, so nothing can be emitted now — but the edges were
			// computed from real hazards, and destroying them here would silently
			// drop them for good.  Carry them into the split list: the next open
			// pass emits them in queue form, the safest form.
			m_carried.insert(m_carried.end(), m_pending.begin(), m_pending.end());
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
