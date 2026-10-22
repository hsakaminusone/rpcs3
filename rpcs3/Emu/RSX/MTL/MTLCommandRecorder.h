#pragma once

#include "MTLDevice.h"
#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

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
		std::atomic_bool in_flight{false};
		// The commit generation of the slot.  A late feedback carries the generation
		// it belongs to, so a feedback of an abandoned commit cannot clear a slot
		// that a newer commit now owns.  The commit path writes it outside the lock
		// and the feedback handler reads it under the lock, so it is atomic.
		std::atomic<u32> generation{0};
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
		void release_slot(frame_slot& slot);
		void allocate_slot(frame_slot& slot);
		void commit_slot(frame_slot& slot, u32 index, u32 generation);
		void on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback);
		bool ring_idle() const;

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		resource_manager* m_resources = nullptr;
		std::array<frame_slot, frame_slot_count> m_slots{};
		// Delivered commit feedback per slot.  The feedback thread writes it and the
		// RSX thread reads it, so it is atomic.
		std::array<std::atomic<u64>, frame_slot_count> m_completed{};
		u32 m_slot = 0;
		MTL4::CommandEncoder* m_encoder = nullptr;
		MTL4::ComputeCommandEncoder* m_compute_encoder = nullptr;
		MTL::Drawable* m_drawable = nullptr;
		std::mutex m_mutex;
		std::condition_variable m_condition;
	};
}

#endif // RSX_ENABLE_METAL4
