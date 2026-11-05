#pragma once

#include "MTLDevice.h"
#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"

#include <array>
#include <atomic>
#include <vector>

namespace mtl
{
	// One frame slot: one allocator, one reusable command buffer, and one argument
	// table per stage (HLD-04 §3.1).
	struct frame_slot
	{
		MTL4::CommandAllocator* allocator = nullptr;
		MTL4::CommandBuffer* command_buffer = nullptr;
		MTL4::ArgumentTable* vertex_table = nullptr;
		MTL4::ArgumentTable* fragment_table = nullptr;
		// The interpreter draws from its own tables so its compact texture and
		// sampler bindings never collide with a regular draw's bindings in the
		// shared tables (LLD-shader-interpreter §4.2, §7.1).
		MTL4::ArgumentTable* interpreter_vertex_table = nullptr;
		MTL4::ArgumentTable* interpreter_fragment_table = nullptr;
		// The completion-event value this slot's last commit reaches when the GPU
		// has finished it.  Zero means the slot is idle.  The queue signals in
		// commit order, so one monotone value covers every slot (SY14).  Only the
		// RSX thread writes or reads it; the event's own value is the cross-thread
		// signal the wait polls.
		u64 completion_value = 0;
		// The commit generation of the slot.  A late feedback carries the generation
		// it belongs to, so a feedback of an abandoned commit cannot clear a slot
		// that a newer commit now owns.  The commit path writes it and the feedback
		// handler reads it on another thread, so it is atomic.
		std::atomic<u32> generation{0};
		// The bytes of the slot's label-staging region that its command buffers
		// already reference.  The region is reset only when no command buffer of the
		// slot can still run, because the GPU reads the staged words at execution
		// time, not at encode time (HLD-04 §4 invariant 2).
		u32 label_staging_offset = 0;
		// The addresses whose label writes are in this slot's command buffer (or
		// its last commit, until that commit reports back).  A guest acquire on one
		// of them must wait for this slot before it reads the old guest word.
		std::vector<u32> labels;
		f64 debug_commit_time = 0.0;
	};

	// The command recorder (HLD-01 C4, HLD-04 §3.1).  It is the single chokepoint
	// for the frame's encoder stream: one open encoder at a time, one way to open a
	// pass, one way to submit.  Encoding stays on the RSX thread (HLD-04 §2.3).
	class command_recorder
	{
	public:
		void init(const device& dev, resource_manager& resources);
		void shutdown();

		// Wait for the slot to come back from the GPU, recycle its frame resources,
		// then open the slot's command buffer (HLD-04 §3.2).  Returns false when the
		// previous commit of this slot did not report its feedback: the slot then
		// holds fresh objects and its previous work is abandoned, so a consumer that
		// tracks the slot must not treat that work as complete (Q3).
		bool begin_frame();
		void submit();

		// The number of commits of `slot` whose feedback has been delivered.  The
		// count advances in commit order on one queue, so a later commit proves
		// every earlier commit of the same slot complete.  The query gate uses it so
		// a result is readable in the frame that produced it (Q4).
		u64 completed_commits(u32 slot) const;

		// Wait for every committed slot to report its feedback.  A CPU-visible
		// memory write outside a frame uses this before it publishes the value.
		bool wait_idle();

		// A CPU readback must observe the copy it encoded, and a Metal 4 command
		// buffer is the unit of submission, so the frame commits early and waits for
		// the slot to come back.  The slot is re-entered, because the frame owns
		// exactly one slot, and the present drawable is left for the frame's own
		// submit (REVIEW-FINDINGS.md, B4).
		void flush_for_readback();

		// Write `value` (the guest word, in memory order) to `address` on the GPU, at
		// the current point in the command stream.  The write is recorded, not
		// submitted, so the caller never waits for the GPU to reach it.  False when
		// the address is not in the guest window; the caller then stores it on the
		// CPU after a drain.  A label is the only signal the GPU gives the guest, so
		// a CPU store cannot be ordered against the work it releases (L130).
		bool write_label(u32 address, u32 value);

		// True while a label write is recorded in a command buffer that has not been
		// submitted: a guest that waits for it would wait for ever.
		bool label_writes_pending() const { return m_pending_count != 0; }

		// Submit the frame's recorded work so the pending label writes become
		// visible, and leave the slot to be re-entered by the next command.  A guest
		// waiting on a label needs this; it does not wait for the GPU, the guest's
		// own wait is the wait (HLD-04 §4.3).
		void commit_pending_labels();

		// A semaphore acquire read the pre-release word, and a label write to
		// `address` is still recorded or in flight.  Commit the batch and wait for
		// the slot that carries it, then return true so the caller re-reads the
		// word.  False when no write to this address is pending, which is the
		// common case and costs one scan of the per-slot address lists.
		bool sync_label(u32 address);

		MTL4::RenderCommandEncoder* open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options = MTL4::RenderEncoderOptionNone);
		MTL4::ComputeCommandEncoder* open_compute();
		void close_encoder();

		MTL4::ArgumentTable* vertex_table() const { return m_slots[m_slot].vertex_table; }
		MTL4::ArgumentTable* fragment_table() const { return m_slots[m_slot].fragment_table; }
		MTL4::ArgumentTable* interpreter_vertex_table() const { return m_slots[m_slot].interpreter_vertex_table; }
		MTL4::ArgumentTable* interpreter_fragment_table() const { return m_slots[m_slot].interpreter_fragment_table; }

		// The open slot, so the per-frame rings size and reset with it.
		u32 slot() const { return m_slot; }

		// The drawable this frame presents (C7).  The recorder waits for it before
		// it encodes and signals it after the commit, so a present stays on the one
		// submit path (HLD-03 §4.9).  Null drops the present.
		void set_present_drawable(MTL::Drawable* drawable);

		// The resource layer, for the traits that allocate a surface.
		resource_manager& resources() const { return *m_resources; }

	private:
		// A label batch is staged per frame slot; the GPU reads the staged words when
		// the command buffer runs, so a region is only rewritten once its slot has
		// completed every commit that references it.
		static constexpr u32 label_staging_capacity = 4096;
		static constexpr u32 label_staging_bytes = label_staging_capacity * sizeof(u32);

		void release_slot(frame_slot& slot);
		void allocate_slot(frame_slot& slot);
		void commit_slot(frame_slot& slot, u32 index, u32 generation);
		void on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback);

		// True when the slot's last commit has reached the GPU.  One atomic load,
		// so a wait that is already satisfied costs the same as a test (SY14).
		bool slot_idle(const frame_slot& slot) const;
		bool ring_idle() const;

		// Poll the completion event until `value` is reached.  False on timeout.
		// The feedback handler is not on this path: the event is the GPU's own
		// signal, and a condition variable would add a lock and a wake-up to a
		// check that is almost always already true.
		bool await_value(u64 value, const char* what);
		bool await_slot(const frame_slot& slot, const char* what);

		// Wait for the slot's commit, reset its allocator and open a new command
		// buffer.  The caller must know the slot's command buffer is closed.
		void reenter_slot();

		// Wait for a slot other than the open one to complete its commit.  The
		// caller has already found a label write in that slot.
		bool wait_for_slot(u32 index);

		// Open the compute encoder that carries copies, closing an open pass.  A
		// consecutive copy reuses it, so a batch costs one encoder (HLD-04 §3.1).
		MTL4::ComputeCommandEncoder* open_copy_encoder();

		// Emit the pending label writes into one compute encoder.  One queue barrier
		// orders the whole batch after the work encoded ahead of it.
		void flush_label_writes();

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		resource_manager* m_resources = nullptr;
		std::array<frame_slot, frame_slot_count> m_slots{};
		// Delivered commit feedback per slot.  The feedback thread writes it and the
		// RSX thread reads it, so it is atomic.
		std::array<std::atomic<u64>, frame_slot_count> m_completed{};
		// The GPU completion timeline.  Every commit takes the next value and the
		// queue signals it once that commit's work has run, so a slot is complete
		// when `signaledValue` reaches its value (metal-cpp MTL4CommandQueue.hpp:142;
		// KK signals the same way in kk_queue.c:230).
		MTL::SharedEvent* m_completion_event = nullptr;
		u64 m_completion_sequence = 0;
		MTL::Buffer* m_label_staging = nullptr;
		// TODO(excise): level 2; one word per slot, the GPU's own copy of the last
		// label it wrote.  It separates "the copy did not land" from "the guest
		// consumed the word".
		MTL::Buffer* m_label_probe = nullptr;
		std::array<u32, label_staging_capacity> m_pending_labels{};
		u32 m_pending_count = 0;
		// TODO(excise): level 2; the last GPU label write, verified after feedback.
		u32 m_diag_label_address = 0;
		u32 m_diag_label_value = 0;
		u32 m_slot = 0;
		MTL4::CommandEncoder* m_encoder = nullptr;
		MTL4::ComputeCommandEncoder* m_compute_encoder = nullptr;
		// Set when a commit left the slot closed; the next command re-enters it.
		bool m_reenter_pending = false;
		MTL::Drawable* m_drawable = nullptr;
	};
}

#endif // RSX_ENABLE_METAL4
