#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/RSXZCULL.h"

#include <array>

namespace mtl
{
	class command_recorder;
	class resource_manager;

	// The Metal 4 ZCULL backend (HLD-04 §3.8).  It owns one persistent
	// visibility-result buffer and implements the five shared ZCULL_control hooks
	// with its counters.
	//
	// A counter is one uint64_t at a fixed offset in the buffer, and a counter is
	// owned by a query for the query's whole life.  Every pass descriptor carries
	// this buffer with VisibilityResultTypeAccumulate, so a counter never resets and
	// the draws of a query accumulate into it across passes, frames and slots.  The
	// RSX query that spans a frame therefore keeps all of its samples.
	//
	// Recording is a state and applying it is lazy.  The shared report machinery
	// calls begin_occlusion_query / end_occlusion_query between draws, when no
	// encoder is open, so those hooks only record the counting mode.  The draw path
	// holds the current encoder and calls apply, which emits the mode on it.
	//
	// Every call site is the RSX thread: the shared report machinery calls the five
	// overrides and the draw path calls apply and on_slot_complete (HLD-04 §2.3).
	// The state therefore needs no lock.
	class zcull : public rsx::reports::ZCULL_control
	{
	public:
		// One counter per live query, and the shared pool holds every query, so the
		// pool is exactly occlusion_query_count counters (16 KiB).
		static constexpr u32 counter_capacity = rsx::reports::occlusion_query_count;

		// Create the visibility buffer and make it persistent.  A Metal 4 command
		// buffer holds no strong reference, so the buffer must stay resident for the
		// application lifetime.  The recorder supplies the completion gate and the
		// bounded flush that a forced read needs (Q1, Q4).
		void init(const device& dev, resource_manager& resources, command_recorder& recorder);
		void shutdown();

		// The one buffer holds every counter, so every slot returns it.  The parent
		// still passes the open slot, and the parameter is kept so its call site is
		// unchanged.  The pass descriptor must carry this buffer before open_pass,
		// with VisibilityResultTypeAccumulate.
		MTL::Buffer* visibility_buffer(u32 slot) const;

		// Apply the pending counting mode to `encoder`.  The parent calls it once
		// after it opens a pass and once before each draw, so a query that starts or
		// ends between draws lands on the encoder that carries the following draws.
		// It is a no-op when neither the mode nor the encoder changed.
		void apply(MTL4::RenderCommandEncoder* encoder);

		// The slot's previous frame has completed when `reported` is true.  Bump its
		// generation and free the counters whose completion gate has passed.  On an
		// abandoned frame neither happens, because the abandoned command buffer can
		// still write its counter (Q3).
		void on_slot_complete(u32 slot, bool reported);

	private:
		// One RSX query's counter.  `end_slot`/`end_generation` are the completion
		// gate, captured when the query stopped counting.
		struct query_counter
		{
			u32 index = 0;
			u32 end_slot = 0;
			u64 end_generation = 0;
			// The delivered-commit count of `end_slot` when the query stopped
			// counting.  The value is final once a later commit of the same slot
			// reports (Q4).
			u64 end_commit = 0;
			bool owned = false;
			bool counted = false;
			bool gated = false;
		};

		// A counter whose gate had not passed when it was released.  It returns to
		// the pool from on_slot_complete, after the slot that could still write it
		// completes.
		struct deferred_release
		{
			u32 index = 0;
			u32 slot = 0;
			u64 generation = 0;
		};

		// The five shared hooks.
		void begin_occlusion_query(rsx::reports::occlusion_query_info* query) override;
		void end_occlusion_query(rsx::reports::occlusion_query_info* query) override;
		bool check_occlusion_query_status(rsx::reports::occlusion_query_info* query) override;
		void get_occlusion_query_result(rsx::reports::occlusion_query_info* query) override;
		void discard_occlusion_query(rsx::reports::occlusion_query_info* query) override;

		// The counter a query owns, or null when its handle is not a pool index.
		query_counter* counter(rsx::reports::occlusion_query_info* query);
		// True when the value of a stopped query is final.
		bool query_ready(const query_counter& entry) const;
		// Return a query's counter to the pool, or defer it until its gate passes.
		void release_counter(query_counter& entry);

		command_recorder* m_recorder = nullptr;
		resource_manager* m_resources = nullptr;
		MTL::Buffer* m_visibility = nullptr;
		std::array<u64, frame_slot_count> m_generations{};
		std::array<query_counter, counter_capacity> m_counters{};
		std::array<u32, counter_capacity> m_free{};
		u32 m_free_count = 0;
		std::array<deferred_release, counter_capacity> m_deferred{};
		u32 m_deferred_count = 0;

		u32 m_current_slot = 0;

		// The recorded mode.  begin_occlusion_query turns counting on for a query,
		// end_occlusion_query turns it off; apply emits it.
		bool m_counting = false;
		rsx::reports::occlusion_query_info* m_counting_query = nullptr;
		// The exhaustion log fires once, because a full pool can repeat every query.
		bool m_exhaustion_logged = false;
	};
}

#endif // RSX_ENABLE_METAL4
