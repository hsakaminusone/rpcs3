#include "MTLZCULL.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLCommandRecorder.h"
#include "MTLInstrumentation.h"
#include "MTLResourceManager.h"

#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"

#include "util/logs.hpp"

#include <algorithm>

namespace mtl
{
	namespace
	{
		// One visibility counter is a uint64_t sample count.
		constexpr u32 counter_stride = sizeof(u64);
	}

	zcull::query_counter* zcull::counter(rsx::reports::occlusion_query_info* query)
	{
		if (query->driver_handle >= counter_capacity)
		{
			return nullptr;
		}

		return &m_counters[query->driver_handle];
	}

	void zcull::init(const device& /*dev*/, resource_manager& resources, command_recorder& recorder)
	{
		m_recorder = &recorder;
		m_resources = &resources;
		m_visibility = resources.new_buffer(counter_capacity * counter_stride, MTL::StorageModeShared);

		if (m_visibility)
		{
			resources.make_persistent(m_visibility);
		}

		for (u32 index = 0; index < counter_capacity; ++index)
		{
			m_free[index] = index;
		}
		m_free_count = counter_capacity;

		// The shared report machinery indexes its query pool by driver_handle
		// (VKGSRender.cpp:597-600 does the same).
		for (u32 index = 0; index < rsx::reports::occlusion_query_count; ++index)
		{
			m_occlusion_query_data[index].driver_handle = index;
		}

		m_counters = {};
		m_deferred = {};
		m_deferred_count = 0;
		m_generations = {};
		m_current_slot = 0;
		m_counting = false;
		m_counting_query = nullptr;
		m_exhaustion_logged = false;
	}

	void zcull::shutdown()
	{
		if (m_visibility && m_resources)
		{
			m_resources->defer_release(m_visibility);
		}

		m_visibility = nullptr;
		m_resources = nullptr;
		m_recorder = nullptr;
		m_counters = {};
		m_deferred = {};
		m_deferred_count = 0;
		m_free_count = 0;
		m_generations = {};
		m_counting = false;
		m_counting_query = nullptr;
		m_exhaustion_logged = false;
	}

	MTL::Buffer* zcull::visibility_buffer(u32 /*slot*/) const
	{
		return m_visibility;
	}

	void zcull::on_slot_complete(u32 slot, bool reported)
	{
		if (slot >= frame_slot_count)
		{
			return;
		}

		m_current_slot = slot;

		if (!reported)
		{
			// The commit that used this slot never reported, so its command buffer
			// may still run and may still write a counter that this slot owns.  The
			// generation stays, so every gate that depends on it stays closed until
			// a later commit of the slot reports (Q3).
			return;
		}

		m_generations[slot]++;

		// Submissions are serialized on one MTL4::CommandQueue, so when this slot's
		// frame completes, every earlier frame that wrote a counter has completed
		// too.  A deferred counter whose gate is this slot or earlier is safe to
		// reuse.
		for (u32 index = 0; index < m_deferred_count;)
		{
			const deferred_release& pending = m_deferred[index];
			if (m_generations[pending.slot] > pending.generation)
			{
				m_free[m_free_count++] = pending.index;
				m_deferred[index] = m_deferred[--m_deferred_count];
				continue;
			}

			index++;
		}
	}

	void zcull::apply(MTL4::RenderCommandEncoder* encoder)
	{
		if (!encoder)
		{
			return;
		}

		// Resolve the recorded mode against the buffer.  While counting is on but
		// the pool gave the query no counter, there is nothing to count into, so the
		// mode falls back to Disabled.
		bool counting = m_counting;
		u32 offset = 0;
		query_counter* entry = (counting && m_counting_query) ? counter(m_counting_query) : nullptr;

		if (counting && (!entry || !entry->owned))
		{
			// The query has no counter (the pool was full).  Emit Disabled and do not
			// read a counter that was never allocated.
			counting = false;
		}
		else if (counting)
		{
			offset = entry->index * counter_stride;
		}

		// The mode is encoder state and does not cross encoders.  Every pass opens a
		// new render encoder, and Metal can give that encoder the address of one
		// that ended, so a cache keyed on the pointer is unsound: it can suppress
		// the mode for a whole pass whose draws then count nothing (Q2).  The draw
		// path calls this once per pass, so the emit is not redundant.
		//
		// Boolean mode stops counting at the first passing fragment, which is all
		// the guest needs when precise_zpass_count is off; the shared write path
		// collapses any non-zero count to 0xFFFF in that case (Q5, K1).
		MTL::VisibilityResultMode mode = MTL::VisibilityResultModeDisabled;
		if (counting)
		{
			mode = g_cfg.video.precise_zpass_count ? MTL::VisibilityResultModeCounting : MTL::VisibilityResultModeBoolean;
		}

		encoder->setVisibilityResultMode(mode, offset);

		if (counting)
		{
			// This encoder's frame can now write the counter, so its reuse must wait
			// for the counter's completion gate.
			entry->counted = true;
		}
	}

	void zcull::begin_occlusion_query(rsx::reports::occlusion_query_info* query)
	{
		query->result = 0;

		// TODO(excise): level 2; the ZCULL query stream (area I).
		MTL_PROBE_LATCH(2, "zcull/query began");

		// Record counting on for this query.  apply emits it on the open encoder.
		m_counting = true;
		m_counting_query = query;

		query_counter* entry = counter(query);
		if (!entry || !m_visibility)
		{
			// TODO(excise): level 2; a query with no counter is a different signature.
			MTL_PROBE_LATCH(2, "zcull/query refused");
			return;
		}

		entry->owned = false;

		if (m_free_count == 0)
		{
			// Every counter is live.  The query gets none, apply emits Disabled, and
			// get_result reports zero, which is the VK and GL answer for a query with
			// no counter (Q6).  Log once, because the pool is sized for every live
			// query and exhaustion means the deferred window is leaking.
			MTL_PROBE_LATCH(2, "zcull/counters exhausted");

			if (!m_exhaustion_logged)
			{
				m_exhaustion_logged = true;
				rsx_log.error("Metal 4: the occlusion counter pool is exhausted, a query reports no pixels");
			}

			return;
		}

		const u32 index = m_free[--m_free_count];

		entry->index = index;
		entry->end_slot = 0;
		entry->end_generation = 0;
		entry->end_commit = 0;
		entry->counted = false;
		entry->gated = false;
		entry->owned = true;

		// Every pass carries VisibilityResultTypeAccumulate, so Metal never resets
		// the counter.  Clear it here.  The counter was free, so no in-flight frame
		// writes it.
		static_cast<u64*>(m_visibility->contents())[index] = 0;
	}

	void zcull::end_occlusion_query(rsx::reports::occlusion_query_info* query)
	{
		// Record counting off.  apply emits it on the open encoder before the next
		// draw.
		m_counting = false;
		m_counting_query = nullptr;

		query_counter* entry = counter(query);
		if (!entry || !entry->owned)
		{
			return;
		}

		// The query stops counting in the current frame, so the current slot is the
		// last that can hold a write for it.  The gate passes when a later commit of
		// that slot reports, which the frame's own submit does.  The slot generation
		// is the fallback for a commit whose feedback was lost (Q3, Q4).
		entry->end_slot = m_current_slot;
		entry->end_generation = m_generations[m_current_slot];
		entry->end_commit = m_recorder ? m_recorder->completed_commits(m_current_slot) : 0;
		entry->gated = true;
	}

	bool zcull::query_ready(const query_counter& entry) const
	{
		if (!entry.gated)
		{
			return false;
		}

		// A delivered commit of the slot proves the counters final: the queue is
		// serialized, so every earlier commit of that slot has completed too.
		if (m_recorder && m_recorder->completed_commits(entry.end_slot) > entry.end_commit)
		{
			return true;
		}

		// Fallback: the slot was reused, which begin_frame allows only after its
		// previous frame reported.  A lost feedback therefore closes the gate until
		// the slot comes back around.
		return m_generations[entry.end_slot] > entry.end_generation;
	}

	bool zcull::check_occlusion_query_status(rsx::reports::occlusion_query_info* query)
	{
		const query_counter* entry = counter(query);
		if (!entry || !entry->owned)
		{
			// No counter: nothing to wait for.  get_result reports the sentinel.
			return true;
		}

		return query_ready(*entry);
	}

	void zcull::get_occlusion_query_result(rsx::reports::occlusion_query_info* query)
	{
		query_counter* entry = counter(query);
		if (!entry || !m_visibility)
		{
			// The handle is not a pool index.  -1 is the base class's "unknown /
			// fully visible" sentinel.
			query->result = -1;
			return;
		}

		if (!entry->owned)
		{
			// The query had draws but the pool gave it no counter.  Report zero, as
			// VK and GL do; -1 would run a block the guest expects to skip (Q6).
			query->result = 0;
			return;
		}

		// A forced read bypasses check_occlusion_query_status (RSXZCULL.cpp:616-620),
		// so the read itself must make the value final.  The flush commits the open
		// command buffer and waits for its feedback with a bound; it is the same
		// primitive the texture readback uses (Q1).
		if (entry->counted && !query_ready(*entry) && m_recorder)
		{
			m_recorder->flush_for_readback();

			if (!query_ready(*entry))
			{
				// The feedback did not arrive in the bound.  Report the current value
				// rather than block the RSX thread, and let release_counter defer the
				// counter until a later commit of the slot proves the work done (K8).
				rsx_log.error("Metal 4: the ZPASS readback did not become final in time, the result may be partial");
			}
		}

		const u64 samples = static_cast<u64*>(m_visibility->contents())[entry->index];
		query->result = static_cast<u32>(std::min<u64>(samples, 0xFFFF'FFFFu));
		release_counter(*entry);
	}

	void zcull::discard_occlusion_query(rsx::reports::occlusion_query_info* query)
	{
		query_counter* entry = counter(query);
		if (!entry || !entry->owned)
		{
			return;
		}

		release_counter(*entry);
	}

	void zcull::release_counter(query_counter& entry)
	{
		entry.owned = false;

		if (!entry.counted)
		{
			// Nothing ever counted into it, so no frame can write it.
			m_free[m_free_count++] = entry.index;
			return;
		}

		if (entry.gated && query_ready(entry))
		{
			// The gate already passed, so the value is final and the index is safe.
			m_free[m_free_count++] = entry.index;
			return;
		}

		// The gate has not passed.  Defer the free until the slot that can still
		// write the counter completes, so a reuse cannot corrupt the read.
		deferred_release pending;
		pending.index = entry.index;

		if (entry.gated)
		{
			pending.slot = entry.end_slot;
			pending.generation = entry.end_generation;
		}
		else
		{
			// A discard of an active query recorded no end.  The current slot is the
			// last that can hold a write for it.
			pending.slot = m_current_slot;
			pending.generation = m_generations[m_current_slot];
		}

		m_deferred[m_deferred_count++] = pending;
	}
}

#endif // RSX_ENABLE_METAL4
