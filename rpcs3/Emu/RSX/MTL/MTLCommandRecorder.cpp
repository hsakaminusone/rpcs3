#include "MTLCommandRecorder.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLInstrumentation.h"

#include "util/logs.hpp"

#include <chrono>
#include <mach/mach_time.h>

namespace mtl
{
	// The bound on the commit-feedback wait.  Vulkan bounds the same wait with
	// FRAME_PRESENT_TIMEOUT, ten seconds (VKPresent.cpp:250).
	static constexpr u32 recorder_feedback_timeout = 10;

#if MTL_DIAGNOSTICS
	// TODO(excise): level 1; the wall clock the commit feedback is measured against.
	// The level-1 feedback gauges need it, so it sits under the wider switch.
	static f64 debug_now_seconds()
	{
		static const mach_timebase_info_data_t timebase = [] { mach_timebase_info_data_t value{}; mach_timebase_info(&value); return value; }();

		return static_cast<f64>(mach_absolute_time()) * static_cast<f64>(timebase.numer) / static_cast<f64>(timebase.denom) * 1e-9;
	}
#endif // MTL_DIAGNOSTICS

	void command_recorder::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_queue = dev.queue();
		m_resources = &resources;

		for (frame_slot& slot : m_slots)
		{
			allocate_slot(slot);
		}

		for (std::atomic<u64>& completed : m_completed)
		{
			completed.store(0, std::memory_order_relaxed);
		}
	}

	u64 command_recorder::completed_commits(u32 slot) const
	{
		if (slot >= frame_slot_count)
		{
			return 0;
		}

		return m_completed[slot].load(std::memory_order_acquire);
	}

	// Drop the Metal objects of one slot.  A committed command buffer and its
	// allocator are retained by the queue until their work completes, so releasing
	// this reference is safe even when the commit never reported back; resetting
	// the allocator is not (DEVIATIONS-SYNC.md, SY3).
	void command_recorder::release_slot(frame_slot& slot)
	{
		if (slot.interpreter_fragment_table)
		{
			slot.interpreter_fragment_table->release();
			slot.interpreter_fragment_table = nullptr;
		}

		if (slot.interpreter_vertex_table)
		{
			slot.interpreter_vertex_table->release();
			slot.interpreter_vertex_table = nullptr;
		}

		if (slot.fragment_table)
		{
			slot.fragment_table->release();
			slot.fragment_table = nullptr;
		}

		if (slot.vertex_table)
		{
			slot.vertex_table->release();
			slot.vertex_table = nullptr;
		}

		if (slot.command_buffer)
		{
			slot.command_buffer->release();
			slot.command_buffer = nullptr;
		}

		if (slot.allocator)
		{
			slot.allocator->release();
			slot.allocator = nullptr;
		}
	}

	// Build the Metal objects of one slot.  The slots are built here and rebuilt
	// when a commit never reports its feedback (HLD-04 §3.1).
	void command_recorder::allocate_slot(frame_slot& slot)
	{
		release_slot(slot);

		MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
		descriptor->setMaxBufferBindCount(argument_buffer_count);
		descriptor->setMaxTextureBindCount(argument_texture_count);
		descriptor->setMaxSamplerStateBindCount(argument_sampler_count);
		// An unbound slot reads a deterministic null (HLD-05 §4.10).
		descriptor->setInitializeBindings(true);

		slot.allocator = m_device->newCommandAllocator();
		slot.command_buffer = m_device->newCommandBuffer();
		slot.vertex_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.fragment_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.interpreter_vertex_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.interpreter_fragment_table = m_device->newArgumentTable(descriptor, nullptr);

		descriptor->release();

		if (!slot.allocator || !slot.command_buffer || !slot.vertex_table || !slot.fragment_table ||
			!slot.interpreter_vertex_table || !slot.interpreter_fragment_table)
		{
			die("command recorder allocation failed");
		}
	}

	bool command_recorder::wait_idle()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/wait idle");

		std::unique_lock lock(m_mutex);
		return m_condition.wait_for(lock, std::chrono::seconds(recorder_feedback_timeout),
			[this] { return ring_idle(); });
	}

	void command_recorder::shutdown()
	{
		// Drain the ring before the objects go away (HLD-04 §2.4).  The wait is
		// bounded: a commit whose feedback never arrives must not hold the shutdown
		// for ever, because the window close that owns this call would never return.
		bool idle = true;
		{
			std::unique_lock lock(m_mutex);

			if (!m_condition.wait_for(lock, std::chrono::seconds(recorder_feedback_timeout), [this] { return ring_idle(); }))
			{
				u32 in_flight = 0;

				for (const frame_slot& slot : m_slots)
				{
					in_flight += slot.in_flight.load() ? 1u : 0u;
				}

				rsx_log.error("Metal 4: the frame ring did not drain in %u s, %u slot(s) still in flight; their objects are leaked rather than released",
					recorder_feedback_timeout, in_flight);
				idle = false;
			}
		}

		// Releasing a slot whose GPU work is unfinished frees memory the queue may
		// still read (DEVIATIONS-SYNC.md, SY3).  The process is closing, so leaking
		// the objects is the safe choice.
		if (idle)
		{
			for (frame_slot& slot : m_slots)
			{
				release_slot(slot);
			}
		}

		m_device = nullptr;
		m_queue = nullptr;
		m_resources = nullptr;
	}

	bool command_recorder::begin_frame()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/begin frame");
		frame_slot& slot = m_slots[m_slot];

		// The slot returns only after its commit feedback (HLD-04 §3.2).  The wait
		// is bounded, because a feedback that never arrives must not stop the RSX
		// thread (VKPresent.cpp:250 bounds the same wait with
		// FRAME_PRESENT_TIMEOUT).
		bool reported = true;
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/wait feedback");
			std::unique_lock lock(m_mutex);

			if (slot.in_flight.load())
			{
				// TODO(excise): level 4; deep-probe line.
				MTL_NOTE(4, "Metal 4: slot %u is in flight, begin_frame waits for its feedback", m_slot);
			}

			reported = m_condition.wait_for(lock, std::chrono::seconds(recorder_feedback_timeout),
				[&slot] { return !slot.in_flight.load(); });
		}

		if (!reported)
		{
			// The commit never reported back, so its allocator cannot be reset:
			// resetting an allocator whose GPU work is unfinished is undefined.  The
			// slot takes fresh objects, the generation advances so the late feedback
			// is ignored, and the stalled objects are abandoned rather than reset.
			// The frame ring keeps moving and the RSX thread keeps running.
			rsx_log.error("Metal 4: slot %u did not report its feedback in %u s, the slot takes fresh objects",
				m_slot, recorder_feedback_timeout);
			allocate_slot(slot);
			slot.generation.fetch_add(1, std::memory_order_relaxed);
			slot.in_flight.store(false);
		}

		m_resources->on_frame_begin(m_slot);

		if (reported)
		{
			// The slot is idle, so the frame that used it is complete: its deferred
			// objects can go (HLD-02 I7).
			m_resources->on_commit_complete(m_slot);
		}
		else
		{
			// The GPU may still read the objects this slot deferred, so they wait
			// for an idle ring instead of being released now (DEVIATIONS-SYNC.md,
			// SY2).
			m_resources->quarantine(m_slot);
		}

		// The ring is idle, so nothing can reference a deferred object (HLD-02 §4).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/gc");

			if (ring_idle())
			{
				m_resources->garbage_collect();
				m_resources->drain_quarantine();
			}
		}

		// The commit options live for the recorder's lifetime, so nothing to
		// recycle here (HLD-04 §3.2).

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/allocator");

			// Never reset an allocator while its GPU work is in flight (HLD-01 §4.7).
			slot.allocator->reset();
			slot.command_buffer->beginCommandBuffer(slot.allocator);
			m_encoder = nullptr;
		}

#if MTL_PERF_ENABLED
		u32 in_flight = 0;

		for (const frame_slot& other : m_slots)
		{
			in_flight += other.in_flight.load() ? 1u : 0u;
		}

		// TODO(excise): level 1; instantaneous value.
		MTL_PERF_GAUGE(1, "recorder/ring in flight", in_flight);
#endif // MTL_PERF_ENABLED

		return reported;
	}

	void command_recorder::submit()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/submit");

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/submit/close encoder");
			close_encoder();
		}

		const u32 index = m_slot;
		frame_slot& slot = m_slots[index];

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/submit/end buffer");
			slot.command_buffer->endCommandBuffer();
		}

		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
		slot.in_flight.store(true);

		commit_slot(slot, index, generation);

#if MTL_DIAGNOSTICS
		// TODO(excise): level 1; the commit stamp the feedback lag is measured from.
		slot.debug_commit_time = debug_now_seconds();
#endif
		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: slot %u committed at %.3f, %s %p", m_slot, slot.debug_commit_time, m_drawable ? "with drawable" : "no drawable", static_cast<const void*>(m_drawable));

		// The drawable is signalled only after the commit that fills it, so the
		// present is ordered behind the frame (C7, HLD-03 §4.9).
		if (m_drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/submit/signal drawable");
			m_queue->signalDrawable(m_drawable);
			m_drawable = nullptr;
		}

		m_slot = (m_slot + 1) % frame_slot_count;
	}

	void command_recorder::flush_for_readback()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/readback wait");

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/readback wait/close encoder");
			close_encoder();
		}

		frame_slot& slot = m_slots[m_slot];

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/readback wait/end buffer");
			slot.command_buffer->endCommandBuffer();
		}

		slot.in_flight.store(true);

		// Each commit takes a generation of its own, so a late feedback of the
		// frame's earlier commit cannot clear this one.
		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
		commit_slot(slot, m_slot, generation);

		bool reported = true;
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/readback wait/gpu wait");
			std::unique_lock lock(m_mutex);
			reported = m_condition.wait_for(lock, std::chrono::seconds(recorder_feedback_timeout),
				[&slot] { return !slot.in_flight.load(); });
		}

		if (!reported)
		{
			// A readback commit that never reports cannot block the RSX thread for
			// ever (Vulkan bounds the same wait with GENERAL_WAIT_TIMEOUT,
			// VKTextureCache.cpp:1472).  The slot takes fresh objects because its
			// allocator must not be reset while the abandoned command buffer may
			// still run; the readback may then carry stale data.
			rsx_log.error("Metal 4: the readback commit did not report its feedback in %u s, the slot takes fresh objects",
				recorder_feedback_timeout);
			allocate_slot(slot);
			slot.generation.fetch_add(1, std::memory_order_relaxed);
			slot.in_flight.store(false);
		}

		// The slot came back, so its GPU work is complete and the same slot serves
		// the rest of the frame (HLD-04 §3.1).
		slot.allocator->reset();
		slot.command_buffer->beginCommandBuffer(slot.allocator);
		m_encoder = nullptr;
	}

	void command_recorder::set_present_drawable(MTL::Drawable* drawable)
	{
		// The wait must precede the encoding, because the drawable's texture is the
		// copy destination.
		if (drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/present drawable wait");
			m_queue->wait(drawable);
		}

		m_drawable = drawable;
	}

	MTL4::RenderCommandEncoder* command_recorder::open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/open pass");
		close_encoder();

		MTL4::RenderCommandEncoder* encoder = m_slots[m_slot].command_buffer->renderCommandEncoder(descriptor, options);
		m_encoder = encoder;
		return encoder;
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_compute()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/open compute");
		// A compute encoder is reused for consecutive copies, so a texture upload
		// of N units costs one encoder rather than N (HLD-04 §3.1).
		if (m_compute_encoder)
		{
			return m_compute_encoder;
		}

		close_encoder();

		MTL4::ComputeCommandEncoder* encoder = m_slots[m_slot].command_buffer->computeCommandEncoder();
		m_encoder = encoder;
		m_compute_encoder = encoder;
		return encoder;
	}

	void command_recorder::close_encoder()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/close encoder");
		if (m_encoder)
		{
			m_encoder->endEncoding();
			m_encoder = nullptr;
		}

		m_compute_encoder = nullptr;
	}

	void command_recorder::commit_slot(frame_slot& slot, u32 index, u32 generation)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/commit slot");
		// A commit takes its own options, and Metal keeps them until the feedback is
		// delivered, so one instance serves one commit (Apple, MTL4CommitOptions and
		// MTL4CommandQueue::commit; KK builds one per submit in kk_queue_commit).  A
		// reused instance delivers feedback for its first commit alone, which leaves
		// the slot in flight and stops the frame ring (REVIEW-FINDINGS.md, F17).
		MTL4::CommitOptions* options = nullptr;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/commit slot/options");
			options = MTL4::CommitOptions::alloc()->init();
			options->addFeedbackHandler([this, index, generation](MTL4::CommitFeedback* feedback) { on_feedback(index, generation, feedback); });
		}

		const MTL4::CommandBuffer* buffers[] = { slot.command_buffer };

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/commit slot/queue commit");
			m_queue->commit(buffers, 1, options);
		}

		options->release();

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "recorder/commits", 1);
	}

	void command_recorder::on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/feedback");
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "recorder/feedback events", 1);
		// TODO(excise): level 2; the commit feedback arrived (area M session classifier).
		MTL_PROBE_LATCH(2, "frame/feedback delivered");

		if (NS::Error* error = feedback->error())
		{
			// TODO(excise): level 2; the commit failed (area F, Metal faults).
			MTL_PROBE_LATCH(2, "fault/commit failed");
			rsx_log.error("Metal 4: commit failed: %s", error->localizedDescription()->utf8String());
		}

		if (feedback->GPUEndTime() > 0.0)
		{
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "recorder/feedback lag us", (debug_now_seconds() - feedback->GPUEndTime()) * 1e6);
		}

		// TODO(excise): temporary diagnostic; the frame's GPU execution and the time
		// it queued behind earlier frames tell a GPU-bound frame from a display-bound
		// one, which the present rows alone cannot.
		if (feedback->GPUEndTime() > feedback->GPUStartTime() && feedback->GPUStartTime() > 0.0)
		{
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "frame/gpu us", (feedback->GPUEndTime() - feedback->GPUStartTime()) * 1e6);
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "frame/gpu wait us", (feedback->GPUStartTime() - m_slots[index].debug_commit_time) * 1e6);
		}

		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: feedback for slot %u: gpu %.3f..%.3f, commit %.3f, start lag %.3f, delivery lag %.3f",
			index, feedback->GPUStartTime(), feedback->GPUEndTime(), m_slots[index].debug_commit_time,
			feedback->GPUStartTime() - m_slots[index].debug_commit_time, debug_now_seconds() - feedback->GPUEndTime());

		{
			std::lock_guard lock(m_mutex);

			// A feedback of a commit the ring already abandoned carries an older
			// generation, so it must not release the slot a newer commit now owns.
			if (m_slots[index].generation.load(std::memory_order_relaxed) != generation)
			{
				return;
			}

			// The commit is done.  A later commit of this slot proves every earlier
			// commit of the slot done as well, because the queue is serialized (Q4).
			m_completed[index].fetch_add(1, std::memory_order_release);
			m_slots[index].in_flight.store(false);
		}

		m_condition.notify_one();
	}

	bool command_recorder::ring_idle() const
	{
		for (const frame_slot& slot : m_slots)
		{
			if (slot.in_flight.load())
			{
				return false;
			}
		}

		return true;
	}
}

#endif // RSX_ENABLE_METAL4
