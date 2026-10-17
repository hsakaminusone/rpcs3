#include "MTLZCULL.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLResourceManager.h"

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

	void zcull::init(const device& /*dev*/, resource_manager& resources)
	{
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
		m_applied_encoder = nullptr;
		m_applied_offset = 0;
		m_applied_counting = false;
	}

	void zcull::shutdown()
	{
		if (m_visibility && m_resources)
		{
			m_resources->defer_release(m_visibility);
		}

		m_visibility = nullptr;
		m_resources = nullptr;
		m_counters = {};
		m_deferred = {};
		m_deferred_count = 0;
		m_free_count = 0;
		m_generations = {};
		m_counting = false;
		m_counting_query = nullptr;
		m_applied_encoder = nullptr;
		m_applied_offset = 0;
		m_applied_counting = false;
	}

	MTL::Buffer* zcull::visibility_buffer(u32 /*slot*/) const
	{
		return m_visibility;
	}

	void zcull::on_slot_complete(u32 slot)
	{
		if (slot >= frame_slot_count)
		{
			return;
		}

		m_generations[slot]++;
		m_current_slot = slot;

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

		// The desired mode is the recorded state; begin_occlusion_query and
		// end_occlusion_query change it.  Encoder state does not cross encoders, so
		// a new encoder always emits.
		if (encoder == m_applied_encoder && counting == m_applied_counting && offset == m_applied_offset)
		{
			return;
		}

		encoder->setVisibilityResultMode(counting ? MTL::VisibilityResultModeCounting : MTL::VisibilityResultModeDisabled, offset);

		if (counting)
		{
			// This encoder's frame can now write the counter, so its reuse must wait
			// for the counter's completion gate.
			entry->counted = true;
		}

		m_applied_encoder = encoder;
		m_applied_counting = counting;
		m_applied_offset = offset;
	}

	void zcull::begin_occlusion_query(rsx::reports::occlusion_query_info* query)
	{
		query->result = 0;

		// Record counting on for this query.  apply emits it on the open encoder.
		m_counting = true;
		m_counting_query = query;

		query_counter* entry = counter(query);
		if (!entry || !m_visibility)
		{
			return;
		}

		entry->owned = false;

		if (m_free_count == 0)
		{
			// Every counter is live.  The query gets none, apply emits Disabled, and
			// get_result reports the base class's "unknown" sentinel.
			return;
		}

		const u32 index = m_free[--m_free_count];

		entry->index = index;
		entry->end_slot = 0;
		entry->end_generation = 0;
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
		// last that can hold a write for it.  The gate passes when that slot
		// completes.  Submissions are serialized on one queue, so this slot's
		// completion also covers every earlier frame of the query.
		entry->end_slot = m_current_slot;
		entry->end_generation = m_generations[m_current_slot];
		entry->gated = true;
	}

	bool zcull::check_occlusion_query_status(rsx::reports::occlusion_query_info* query)
	{
		const query_counter* entry = counter(query);
		if (!entry || !entry->owned)
		{
			// No counter: nothing to wait for.  get_result reports the sentinel.
			return true;
		}

		if (!entry->gated)
		{
			return false;
		}

		// The value is final once the end slot's frame completed after the query
		// stopped counting.  on_slot_complete bumps the slot generations.
		return m_generations[entry->end_slot] > entry->end_generation;
	}

	void zcull::get_occlusion_query_result(rsx::reports::occlusion_query_info* query)
	{
		query_counter* entry = counter(query);
		if (!entry || !entry->owned || !m_visibility)
		{
			// The query never got a counter.  -1 is the base class's "unknown / fully
			// visible" sentinel.
			query->result = -1;
			return;
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

		if (entry.gated && m_generations[entry.end_slot] > entry.end_generation)
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
