#include "MTLCommandRecorder.h"

#ifdef RSX_ENABLE_METAL4

#include "util/logs.hpp"

namespace mtl
{
	void command_recorder::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_queue = dev.queue();
		m_resources = &resources;

		MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
		descriptor->setMaxBufferBindCount(argument_buffer_count);
		descriptor->setMaxTextureBindCount(argument_texture_count);
		descriptor->setMaxSamplerStateBindCount(argument_sampler_count);
		// An unbound slot reads a deterministic null (HLD-05 §4.10).
		descriptor->setInitializeBindings(true);

		for (frame_slot& slot : m_slots)
		{
			slot.allocator = m_device->newCommandAllocator();
			slot.command_buffer = m_device->newCommandBuffer();
			slot.vertex_table = m_device->newArgumentTable(descriptor, nullptr);
			slot.fragment_table = m_device->newArgumentTable(descriptor, nullptr);

			if (!slot.allocator || !slot.command_buffer || !slot.vertex_table || !slot.fragment_table)
			{
				die("command recorder allocation failed");
			}

			// The commit options are per slot and the slot index is fixed, so the
			// handler is registered once here and never rebuilt (HLD-04 §3.2).
			const u32 index = static_cast<u32>(&slot - m_slots.data());
			slot.commit_options = MTL4::CommitOptions::alloc()->init();
			slot.commit_options->addFeedbackHandler([this, index](MTL4::CommitFeedback* feedback) { on_feedback(index, feedback); });
		}

		descriptor->release();
	}

	void command_recorder::shutdown()
	{
		// Drain the ring before the objects go away (HLD-04 §2.4).
		{
			std::unique_lock lock(m_mutex);
			m_condition.wait(lock, [this] { return ring_idle(); });
		}

		for (frame_slot& slot : m_slots)
		{
			if (slot.commit_options)
			{
				slot.commit_options->release();
			}

			slot.fragment_table->release();
			slot.vertex_table->release();
			slot.command_buffer->release();
			slot.allocator->release();
		}

		m_device = nullptr;
		m_queue = nullptr;
		m_resources = nullptr;
	}

	void command_recorder::begin_frame()
	{
		frame_slot& slot = m_slots[m_slot];

		// The slot returns only after its commit feedback (HLD-04 §3.2).
		{
			std::unique_lock lock(m_mutex);
			m_condition.wait(lock, [&slot] { return !slot.in_flight.load(); });
		}

		// The slot is idle, so the frame that used it is complete: its residency
		// entries and its deferred objects can go (HLD-02 I7).
		m_resources->on_frame_begin(m_slot);
		m_resources->on_commit_complete(m_slot);

		// The ring is idle, so nothing can reference a deferred object (HLD-02 §4).
		if (ring_idle())
		{
			m_resources->garbage_collect();
		}

		// The commit options live for the recorder's lifetime, so nothing to
		// recycle here (HLD-04 §3.2).

		// Never reset an allocator while its GPU work is in flight (HLD-01 §4.7).
		slot.allocator->reset();
		slot.command_buffer->beginCommandBuffer(slot.allocator);
		m_encoder = nullptr;
	}

	void command_recorder::submit()
	{
		close_encoder();

		frame_slot& slot = m_slots[m_slot];
		slot.command_buffer->endCommandBuffer();

		// Every bound allocation must be resident before the commit (HLD-02 I5).
		m_resources->commit_frame();

		slot.in_flight.store(true);

		const MTL4::CommandBuffer* buffers[] = { slot.command_buffer };
		m_queue->commit(buffers, 1, slot.commit_options);

		// The drawable is signalled only after the commit that fills it, so the
		// present is ordered behind the frame (C7, HLD-03 §4.9).
		if (m_drawable)
		{
			m_queue->signalDrawable(m_drawable);
			m_drawable = nullptr;
		}

		m_slot = (m_slot + 1) % frame_slot_count;
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
		close_encoder();

		MTL4::RenderCommandEncoder* encoder = m_slots[m_slot].command_buffer->renderCommandEncoder(descriptor, options);
		m_encoder = encoder;
		return encoder;
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_compute()
	{
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
		if (m_encoder)
		{
			m_encoder->endEncoding();
			m_encoder = nullptr;
		}

		m_compute_encoder = nullptr;
	}

	void command_recorder::on_feedback(u32 index, MTL4::CommitFeedback* feedback)
	{
		if (NS::Error* error = feedback->error())
		{
			rsx_log.error("Metal 4: commit failed: %s", error->localizedDescription()->utf8String());
		}

		{
			std::lock_guard lock(m_mutex);
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
