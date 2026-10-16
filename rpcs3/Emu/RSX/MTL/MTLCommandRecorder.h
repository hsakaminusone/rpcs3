#pragma once

#include "MTLDevice.h"
#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace mtl
{
	// The drawable ring bounds the frame slots (HLD-03 §4.9; HLD-04 §3.2).
	constexpr u32 frame_slot_count = 3;

	// The argument-table shape (HLD-04 §3.3, HLD-05 §3.2).  The RSX slot map is one
	// index space per stage.
	constexpr u32 argument_buffer_count = 16;
	constexpr u32 argument_texture_count = 28;
	constexpr u32 argument_sampler_count = 28;

	// One frame slot: one allocator, one reusable command buffer, and one argument
	// table per stage (HLD-04 §3.1).
	struct frame_slot
	{
		MTL4::CommandAllocator* allocator = nullptr;
		MTL4::CommandBuffer* command_buffer = nullptr;
		MTL4::ArgumentTable* vertex_table = nullptr;
		MTL4::ArgumentTable* fragment_table = nullptr;
		MTL4::CommitOptions* commit_options = nullptr;
		std::atomic_bool in_flight{false};
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
		// then open the slot's command buffer (HLD-04 §3.2).
		void begin_frame();
		void submit();

		MTL4::RenderCommandEncoder* open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options = MTL4::RenderEncoderOptionNone);
		MTL4::ComputeCommandEncoder* open_compute();
		void close_encoder();

		MTL4::ArgumentTable* vertex_table() const { return m_slots[m_slot].vertex_table; }
		MTL4::ArgumentTable* fragment_table() const { return m_slots[m_slot].fragment_table; }

		// The resource layer, for the traits that allocate a surface.
		resource_manager& resources() const { return *m_resources; }

	private:
		void on_feedback(u32 index, MTL4::CommitFeedback* feedback);
		bool ring_idle() const;

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		resource_manager* m_resources = nullptr;
		std::array<frame_slot, frame_slot_count> m_slots{};
		u32 m_slot = 0;
		MTL4::CommandEncoder* m_encoder = nullptr;
		std::mutex m_mutex;
		std::condition_variable m_condition;
	};
}

#endif // RSX_ENABLE_METAL4
