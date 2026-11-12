#include "MTLCommandRecorder.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"

#include "Emu/Memory/vm.h"
#include "util/asm.hpp"
#include "util/logs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mach/mach_time.h>
#include <thread>

namespace mtl
{
	// The bound on the commit-feedback wait.  Vulkan bounds the same wait with
	// FRAME_PRESENT_TIMEOUT, ten seconds (VKPresent.cpp:250).
	static constexpr u32 recorder_feedback_timeout = 10;


	void command_recorder::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_queue = dev.queue();
		m_resources = &resources;

		// One timeline for every commit.  The queue signals it after the commit's
		// work, so the CPU can read completion without the feedback handler's lock
		// (SY14; metal-cpp MTL4CommandQueue.hpp:142).  A device that cannot create a
		// shared event cannot report completion at all, which is a backend fault.
		m_completion_event = m_device->newSharedEvent();
		if (!m_completion_event)
		{
			die("Metal 4: the completion event could not be created");
		}

		for (frame_slot& slot : m_slots)
		{
			allocate_slot(slot);
		}

		for (std::atomic<u64>& completed : m_completed)
		{
			completed.store(0, std::memory_order_relaxed);
		}

		m_label_staging = resources.new_buffer(static_cast<u64>(frame_slot_count) * label_staging_bytes, MTL::StorageModeShared);
		if (!m_label_staging)
		{
			die("Metal 4: the label staging buffer could not be allocated");
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
		// The slot is being abandoned or rebuilt, so its label addresses no longer
		// describe anything the GPU can still write.
		slot.labels.clear();

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

	bool command_recorder::slot_idle(const frame_slot& slot) const
	{
		return !slot.completion_value || !m_completion_event ||
			slot.completion_value <= m_completion_event->signaledValue();
	}

	bool command_recorder::await_value(u64 value, const char* what)
	{
		if (!value || !m_completion_event || m_completion_event->signaledValue() >= value)
		{
			return true;
		}

		// The wait is bounded, because a signal that never arrives must not stop
		// the RSX thread (VKPresent.cpp:250 bounds the same wait with
		// FRAME_PRESENT_TIMEOUT).  A commit finishes in microseconds to
		// milliseconds, so a short pause spin covers the common case and lets the
		// thread sleep only when the GPU is genuinely behind.  The clock is read
		// on the sleep path only, so the spin costs nothing but the event read.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(recorder_feedback_timeout);
		u32 spins = 0;

		while (m_completion_event->signaledValue() < value)
		{
			if (++spins < 256)
			{
				utils::pause();
				continue;
			}

			if (std::chrono::steady_clock::now() >= deadline)
			{
				rsx_log.error("Metal 4: %s waited %u s for completion value %llu, the event is at %llu",
					what, recorder_feedback_timeout, value, m_completion_event->signaledValue());
				return false;
			}

			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}

		return true;
	}

	bool command_recorder::await_slot(const frame_slot& slot, const char* what)
	{
		return await_value(slot.completion_value, what);
	}

	bool command_recorder::wait_idle()
	{

		u64 target = 0;

		for (const frame_slot& slot : m_slots)
		{
			target = std::max(target, slot.completion_value);
		}

		return await_value(target, "wait idle");
	}

	void command_recorder::shutdown()
	{
		// Drain the ring before the objects go away (HLD-04 §2.4).  The wait is
		// bounded: a commit whose signal never arrives must not hold the shutdown
		// for ever, because the window close that owns this call would never return.
		u64 target = 0;

		for (const frame_slot& slot : m_slots)
		{
			target = std::max(target, slot.completion_value);
		}

		const bool idle = await_value(target, "shutdown");

		if (!idle)
		{
			u32 in_flight = 0;

			for (const frame_slot& slot : m_slots)
			{
				in_flight += slot_idle(slot) ? 0u : 1u;
			}

			rsx_log.error("Metal 4: the frame ring did not drain in %u s, %u slot(s) still in flight; their objects are leaked rather than released",
				recorder_feedback_timeout, in_flight);
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

			// The staging buffer is read by the GPU, so it goes with the slots.
			if (m_label_staging)
			{
				m_resources->defer_release(m_label_staging);
				m_label_staging = nullptr;
			}

			// The queue would signal a released event on a late commit.
			if (m_completion_event)
			{
				m_completion_event->release();
				m_completion_event = nullptr;
			}
		}

		m_device = nullptr;
		m_queue = nullptr;
		m_resources = nullptr;
	}

	bool command_recorder::begin_frame()
	{
		frame_slot& slot = m_slots[m_slot];

		// The slot returns only after its commit feedback (HLD-04 §3.2).  The wait
		// is bounded, because a feedback that never arrives must not stop the RSX
		// thread (VKPresent.cpp:250 bounds the same wait with
		// FRAME_PRESENT_TIMEOUT).
		bool reported = true;
		{

			if (!slot_idle(slot))
			{
				reported = await_slot(slot, "begin frame");
			}
		}

		if (!reported)
		{
			// The commit never completed, so its allocator cannot be reset:
			// resetting an allocator whose GPU work is unfinished is undefined.  The
			// slot takes fresh objects, the generation advances so the late feedback
			// is ignored, and the stalled objects are abandoned rather than reset.
			// The frame ring keeps moving and the RSX thread keeps running.
			rsx_log.error("Metal 4: slot %u did not complete in %u s, the slot takes fresh objects",
				m_slot, recorder_feedback_timeout);
			allocate_slot(slot);
			slot.generation.fetch_add(1, std::memory_order_relaxed);
			slot.completion_value = 0;
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

			if (ring_idle())
			{
				m_resources->garbage_collect();
				m_resources->drain_quarantine();
			}
		}

		// The commit options live for the recorder's lifetime, so nothing to
		// recycle here (HLD-04 §3.2).

		{

			// Never reset an allocator while its GPU work is in flight (HLD-01 §4.7).
			slot.allocator->reset();
			slot.command_buffer->beginCommandBuffer(slot.allocator);
			slot.buffer_open = true;
			slot.label_staging_offset = 0;
			slot.labels.clear();
			m_pending_count = 0;
			m_reenter_pending = false;
			m_encoder = nullptr;
			m_compute_encoder = nullptr;
		}


		return reported;
	}

	void command_recorder::submit()
	{

		if (m_reenter_pending)
		{
			// A label commit already submitted the frame's last commands, so only the
			// drawable is left; a commit with nothing in it would be pure overhead.
			m_reenter_pending = false;
			m_slots[m_slot].buffer_open = false;

			if (m_drawable)
			{
				m_queue->signalDrawable(m_drawable);
				m_drawable = nullptr;
			}

			m_slot = (m_slot + 1) % frame_slot_count;
			return;
		}

		{
			// A label recorded after the last pass belongs to this commit.
			flush_label_writes();
			close_encoder();
		}

		const u32 index = m_slot;
		frame_slot& slot = m_slots[index];

		{
			slot.command_buffer->endCommandBuffer();
			slot.buffer_open = false;
		}

		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;

		commit_slot(slot, index, generation);


		// The drawable is signalled only after the commit that fills it, so the
		// present is ordered behind the frame (C7, HLD-03 §4.9).
		if (m_drawable)
		{
			m_queue->signalDrawable(m_drawable);
			m_drawable = nullptr;
		}

		m_slot = (m_slot + 1) % frame_slot_count;
	}

	void command_recorder::flush_for_readback()
	{

		if (m_reenter_pending)
		{
			reenter_slot();
		}

		{
			// A pending label must reach the commit; a readback may be what the guest
			// uses to learn that the GPU finished.
			flush_label_writes();
			close_encoder();
		}

		frame_slot& slot = m_slots[m_slot];

		{
			slot.command_buffer->endCommandBuffer();
			slot.buffer_open = false;
		}

		// Each commit takes a generation of its own, so a late feedback of the
		// frame's earlier commit cannot clear this one.
		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
		commit_slot(slot, m_slot, generation);

		// The slot came back, so its GPU work is complete and the same slot serves
		// the rest of the frame (HLD-04 §3.1).
		reenter_slot();
	}

	void command_recorder::reenter_slot()
	{

		frame_slot& slot = m_slots[m_slot];

		// One atomic load when the commit has already finished, which is the common
		// case: the frame committed, the GPU ran it, and the next encoder re-enters.
		bool reported = true;
		{
			reported = await_slot(slot, "re-enter slot");
		}

		if (!reported)
		{
			// The commit never completed, and an allocator must not be reset while its
			// command buffer may still run (Vulkan bounds the same wait with
			// GENERAL_WAIT_TIMEOUT, VKTextureCache.cpp:1472).  The slot takes fresh
			// objects.
			allocate_slot(slot);
			slot.generation.fetch_add(1, std::memory_order_relaxed);
			slot.completion_value = 0;
		}


		slot.allocator->reset();
		slot.command_buffer->beginCommandBuffer(slot.allocator);
		slot.buffer_open = true;

		// A batch that was staged but not yet encoded is not covered by the
		// feedback of the commit that just completed, so it is kept: the buffer
		// that has just begun is the one that will carry its copy.  The staging
		// offset goes with it, because the staged words are still in the region.
		if (!m_pending_count)
		{
			slot.label_staging_offset = 0;
			// The feedback proves every label of the committed command buffer
			// visible, so the next acquire on one of them needs no wait.
			slot.labels.clear();
		}

		m_reenter_pending = false;
		m_encoder = nullptr;
		m_compute_encoder = nullptr;
	}

	void command_recorder::set_present_drawable(MTL::Drawable* drawable)
	{
		// The wait must precede the encoding, because the drawable's texture is the
		// copy destination.
		if (drawable)
		{
			m_queue->wait(drawable);
		}

		m_drawable = drawable;
	}

	MTL4::RenderCommandEncoder* command_recorder::open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options)
	{

		if (m_reenter_pending)
		{
			reenter_slot();
		}

		frame_slot& slot = m_slots[m_slot];

		if (!slot.buffer_open)
		{
			// A pass opened outside a frame would be encoded on an ended buffer;
			// re-enter the slot instead (L159).
			reenter_slot();
		}

		// The pass boundary is the natural place for a label batch: no open pass is
		// split, and the writes are ordered before the pass's load actions.
		flush_label_writes();
		close_encoder();

		MTL4::RenderCommandEncoder* encoder = m_slots[m_slot].command_buffer->renderCommandEncoder(descriptor, options);
		m_encoder = encoder;
		return encoder;
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_compute()
	{

		if (m_reenter_pending)
		{
			reenter_slot();
		}

		flush_label_writes();
		return open_copy_encoder();
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_copy_encoder()
	{
		// A compute encoder is reused for consecutive copies, so a texture upload
		// of N units costs one encoder rather than N (HLD-04 §3.1).
		if (m_compute_encoder)
		{
			return m_compute_encoder;
		}

		frame_slot& open_slot = m_slots[m_slot];

		if (!open_slot.buffer_open)
		{
			// The slot's command buffer was ended (e.g. the frame was submitted)
			// while a copy outside the frame was still queued; encoding on an
			// ended buffer faults inside Metal.  Re-enter the slot first (L159).
			reenter_slot();
		}

		close_encoder();

		MTL4::ComputeCommandEncoder* encoder = m_slots[m_slot].command_buffer->computeCommandEncoder();
		m_encoder = encoder;
		m_compute_encoder = encoder;
		return encoder;
	}

	bool command_recorder::write_label(u32 address, u32 value)
	{
		MTL::Buffer* guest = m_resources->guest_buffer();

		// The guest window is one buffer and a guest address is its offset
		// (HLD-02 §3.5).  A misaligned or out-of-window label cannot be written by
		// the GPU, so the caller stores it on the CPU instead.
		if (!guest || (address & 3) || address > guest->length() || guest->length() - address < sizeof(u32))
		{
			return false;
		}

		if (m_reenter_pending)
		{
			reenter_slot();
		}

		frame_slot& slot = m_slots[m_slot];

		if (!slot.buffer_open)
		{
			// A staged label belongs to the command buffer whose copy will carry
			// it, so the buffer is opened before the word is staged.  A label
			// staged into an ended buffer is written by nobody, and the guest then
			// waits on a word that never changes.
			reenter_slot();
		}

		if (slot.label_staging_offset + sizeof(u32) > label_staging_bytes)
		{
			// Emitting frees nothing, because the GPU still reads the region, so the
			// slot commits and comes back with a clean one.

			flush_label_writes();
			flush_for_readback();
		}

		const u32 offset = m_slot * label_staging_bytes + slot.label_staging_offset;
		std::memcpy(static_cast<u8*>(m_label_staging->contents()) + offset, &value, sizeof(u32));
		m_pending_labels[m_pending_count++] = address;
		slot.labels.push_back(address);
		slot.label_staging_offset += sizeof(u32);


		return true;
	}

	void command_recorder::commit_pending_labels()
	{

		if (m_reenter_pending)
		{
			reenter_slot();
		}

		{
			flush_label_writes();
			close_encoder();
		}

		frame_slot& slot = m_slots[m_slot];

		if (!slot.buffer_open)
		{
			// Nothing has been encoded since the last commit, so there is no buffer
			// to end; the staged batch above was empty for the same reason.
			return;
		}

		{
			slot.command_buffer->endCommandBuffer();
			slot.buffer_open = false;
		}

		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
		commit_slot(slot, m_slot, generation);

		// The GPU writes the labels when it runs this commit.  The guest is the one
		// waiting, so the RSX thread does not: the slot comes back on the next
		// encoder, after the completion has almost certainly arrived.
		m_reenter_pending = true;
	}

	bool command_recorder::sync_label(u32 address)
	{
		bool waited = false;

		for (u32 index = 0; index < frame_slot_count; ++index)
		{
			frame_slot& slot = m_slots[index];

			bool has_address = false;
			for (const u32 label : slot.labels)
			{
				if (label == address)
				{
					has_address = true;
					break;
				}
			}

			if (!has_address)
			{
				continue;
			}

			if (index == m_slot)
			{
				// The label is in the open command buffer: either still staged, or
				// already flushed at a pass boundary.  Neither has reached the GPU,
				// so commit the buffer.  A buffer that was committed already is
				// waiting for its slot to come back.  Both end in re-enter, which is
				// the wait for this slot's feedback.
				if (!m_reenter_pending)
				{
					commit_pending_labels();
				}

				if (m_reenter_pending)
				{
					reenter_slot();
					waited = true;
				}
				else
				{
					slot.labels.clear();
				}
			}
			else if (!slot_idle(slot))
			{
				wait_for_slot(index);
				slot.labels.clear();
				waited = true;
			}
			else
			{
				// This slot's commit has completed, so its label is visible.
				slot.labels.clear();
			}
		}

		return waited;
	}

	bool command_recorder::wait_for_slot(u32 index)
	{
		return await_slot(m_slots[index], "label wait");
	}

	void command_recorder::flush_label_writes()
	{
		if (!m_pending_count)
		{
			return;
		}


		// The encoder opens first: opening it can re-enter the slot, and the batch
		// that survives that is the one this copy must encode.
		MTL4::ComputeCommandEncoder* encoder = open_copy_encoder();

		frame_slot& slot = m_slots[m_slot];
		const u32 first = slot.label_staging_offset - m_pending_count * sizeof(u32);


		// A label is a release, so the write must not run before the work encoded
		// ahead of it.  One queue-scope barrier covers the whole batch (HLD-03 §2.1).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });

		for (u32 i = 0; i < m_pending_count; ++i)
		{
			// A label page has its own import, made after the CPU mapped the page,
			// because the application-lifetime window keeps the GPU on the physical
			// page that existed when it was created (L136).  An address outside the
			// window falls back to the window itself.
			u32 dst_offset = 0;
			MTL::Buffer* dst = m_resources->label_buffer(m_pending_labels[i], dst_offset);

			if (!dst)
			{
				dst = m_resources->guest_buffer();
				dst_offset = m_pending_labels[i];
			}

			encoder->copyFromBuffer(m_label_staging, m_slot * label_staging_bytes + first + i * sizeof(u32),
				dst, dst_offset, sizeof(u32));
		}


		m_pending_count = 0;
	}

	void command_recorder::close_encoder()
	{
		if (m_encoder)
		{
			// The encoder boundary is the backstop the rest of the backend relies on:
			// Metal 4 tracks no hazards, so once this encoder ends, nothing else in the
			// backend is obliged to reconstruct what it wrote.  StageAll includes the
			// tile stage, so a stored attachment is ordered even when the producer was
			// a render pass's tile flush (L202 question (a); KK does the same in
			// end_encoder, kk_cmd_buffer.c:266).
			emit_producer_barrier(m_encoder,
				barrier_spec{ MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionDevice, true });

			m_encoder->endEncoding();
			m_encoder = nullptr;
		}

		m_compute_encoder = nullptr;
	}

	void command_recorder::commit_slot(frame_slot& slot, u32 index, u32 generation)
	{
		// A commit takes its own options, and Metal keeps them until the feedback is
		// delivered, so one instance serves one commit (Apple, MTL4CommitOptions and
		// MTL4CommandQueue::commit; KK builds one per submit in kk_queue_commit).  A
		// reused instance delivers feedback for its first commit alone, which leaves
		// the slot in flight and stops the frame ring (REVIEW-FINDINGS.md, F17).
		MTL4::CommitOptions* options = nullptr;

		{
			options = MTL4::CommitOptions::alloc()->init();
			options->addFeedbackHandler([this, index, generation](MTL4::CommitFeedback* feedback) { on_feedback(index, generation, feedback); });
		}

		const MTL4::CommandBuffer* buffers[] = { slot.command_buffer };

		{
			m_queue->commit(buffers, 1, options);
		}

		options->release();

		// The queue signals the event after this commit's work, so one monotone
		// value per commit tells the CPU when the slot has run.  This is the only
		// completion point the waits use; the feedback handler stays for the
		// commit-diagnostic counters and the query gate (SY14).
		const u64 completion = ++m_completion_sequence;
		slot.completion_value = completion;
		m_queue->signalEvent(m_completion_event, completion);

	}

	void command_recorder::on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback)
	{

		if (NS::Error* error = feedback->error())
		{
			rsx_log.error("Metal 4: commit failed: %s", error->localizedDescription()->utf8String());
		}


		// A feedback of a commit the ring already abandoned carries an older
		// generation, so it must not count against the slot a newer commit now owns.
		if (m_slots[index].generation.load(std::memory_order_relaxed) != generation)
		{
			return;
		}

		// The commit is done.  A later commit of this slot proves every earlier
		// commit of the slot done as well, because the queue is serialized (Q4).
		m_completed[index].fetch_add(1, std::memory_order_release);
	}

	bool command_recorder::ring_idle() const
	{
		for (const frame_slot& slot : m_slots)
		{
			if (!slot_idle(slot))
			{
				return false;
			}
		}

		return true;
	}
}

#endif // RSX_ENABLE_METAL4
