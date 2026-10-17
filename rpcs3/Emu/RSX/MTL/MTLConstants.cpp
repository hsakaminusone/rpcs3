#include "MTLDevice.h"

#include "MTLConstants.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	namespace
	{
		// A texture-buffer view over a whole ring, so a draw's data is a starting
		// offset in attrib_data rather than a new view.  R8Uint makes one texel one
		// byte, which is what the shared GLSL's byte addressing needs (HLD-05 §3.5).
		MTL::Texture* make_stream_view(MTL::Buffer* buffer, u64 length)
		{
			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::textureBufferDescriptor(
				MTL::PixelFormatR8Uint, length, MTL::ResourceStorageModeShared, MTL::TextureUsageShaderRead);

			// A class-method descriptor is autoreleased; hold it for the call only.
			descriptor->retain();

			// For a texture buffer the row stride is the buffer length, and Metal
			// requires it to be at least four bytes.
			MTL::Texture* view = buffer->newTexture(descriptor, 0, std::max<u64>(length, 4));

			descriptor->release();
			return view;
		}
	}

	void stream_ring::init(resource_manager& resources, u64 capacity)
	{
		m_resources = &resources;
		m_capacity = capacity;
		m_buffer = resources.new_buffer(capacity, MTL::StorageModeShared);

		if (m_buffer)
		{
			m_view = make_stream_view(m_buffer, m_buffer->length());
		}

		m_count = 0;
	}

	void stream_ring::shutdown()
	{
		if (m_resources)
		{
			for (MTL::Texture* view : m_retired_views)
			{
				m_resources->defer_release(view);
			}

			for (MTL::Buffer* buffer : m_retired)
			{
				m_resources->defer_release(buffer);
			}

			if (m_view)
			{
				m_resources->defer_release(m_view);
			}

			if (m_buffer)
			{
				m_resources->defer_release(m_buffer);
			}
		}

		m_retired_views.clear();
		m_retired.clear();
		m_view = nullptr;
		m_buffer = nullptr;
		m_resources = nullptr;
	}

	void stream_ring::begin_frame()
	{
		for (MTL::Texture* view : m_retired_views)
		{
			m_resources->defer_release(view);
		}

		for (MTL::Buffer* buffer : m_retired)
		{
			m_resources->defer_release(buffer);
		}

		m_retired_views.clear();
		m_retired.clear();
		m_count = 0;
	}

	void* stream_ring::alloc(u64 size, u32& offset)
	{
		offset = 0;

		if (!m_buffer || size == 0)
		{
			return nullptr;
		}

		if (m_count + size > m_capacity)
		{
			// A new buffer needs a new view.  The retired pair is still referenced
			// by the draws already encoded this frame, so both releases are
			// deferred (HLD-02 I7).
			MTL::Buffer* next = m_resources->new_buffer(m_capacity, MTL::StorageModeShared);
			if (!next)
			{
				return nullptr;
			}

			MTL::Texture* next_view = make_stream_view(next, next->length());
			if (!next_view)
			{
				m_resources->defer_release(next);
				return nullptr;
			}

			m_retired.push_back(m_buffer);
			m_retired_views.push_back(m_view);
			m_buffer = next;
			m_view = next_view;
			m_count = 0;
		}

		offset = static_cast<u32>(m_count);
		void* block = static_cast<std::byte*>(m_buffer->contents()) + m_count;
		m_count += size;
		return block;
	}

	void byte_ring::init(resource_manager& resources, u64 capacity)
	{
		m_resources = &resources;
		m_capacity = capacity;
		m_buffer = resources.new_buffer(capacity, MTL::StorageModeShared);
		m_count = 0;
	}

	void byte_ring::shutdown()
	{
		if (m_resources)
		{
			for (MTL::Buffer* buffer : m_retired)
			{
				m_resources->defer_release(buffer);
			}

			if (m_buffer)
			{
				m_resources->defer_release(m_buffer);
			}
		}

		m_retired.clear();
		m_buffer = nullptr;
		m_resources = nullptr;
	}

	void byte_ring::begin_frame()
	{
		for (MTL::Buffer* buffer : m_retired)
		{
			m_resources->defer_release(buffer);
		}

		m_retired.clear();
		m_count = 0;
	}

	void* byte_ring::alloc(u64 size, u32& offset)
	{
		offset = 0;

		if (!m_buffer || size == 0)
		{
			return nullptr;
		}

		if (m_count + size > m_capacity)
		{
			MTL::Buffer* next = m_resources->new_buffer(m_capacity, MTL::StorageModeShared);
			if (!next)
			{
				return nullptr;
			}

			m_retired.push_back(m_buffer);
			m_buffer = next;
			m_count = 0;
		}

		offset = static_cast<u32>(m_count);
		void* block = static_cast<std::byte*>(m_buffer->contents()) + m_count;
		m_count += size;
		return block;
	}

	void constants_heap::init(resource_manager& resources, u32 frame_count)
	{
		// The heap sizes to the recorder's ring, because a slot's blocks are reused
		// only after that slot's frame has completed (HLD-04 §3.2).
		ensure(frame_count == frame_slot_count);

		for (ring_set& set : m_sets)
		{
			set.vertex_context.init(resources, draw_capacity);
			set.draw_parameters.init(resources, draw_capacity);
			set.fragment_context.init(resources, draw_capacity);
			set.sampler_info.init(resources, draw_capacity);
			set.stipple.init(resources, draw_capacity);
			set.blend_values.init(resources, draw_capacity);
			set.fill_values.init(resources, fill_capacity);
			set.vertex_draw_params.init(resources, draw_capacity);
			set.fragment_constants.init(resources, constant_capacity);
			// One change is one 468-vec4 file, so the ring holds the changes, not
			// the vec4 (MTLConstants.h:256).
			set.transform_constants.init(resources, transform_capacity * vertex_constant_count);
			set.instancing_constants.init(resources, constant_capacity);
			set.instancing_lookup.init(resources, constant_capacity);
		}

		for (stream_set& set : m_streams)
		{
			set.persistent.init(resources, stream_capacity);
			set.volatile_stream.init(resources, volatile_capacity);
			set.index.init(resources, index_capacity);
			set.overlay_vertices.init(resources, overlay_vertex_capacity);
			set.overlay_uniforms.init(resources, overlay_uniform_capacity);
		}
	}

	void constants_heap::shutdown()
	{
		for (ring_set& set : m_sets)
		{
			set.vertex_context.shutdown();
			set.draw_parameters.shutdown();
			set.fragment_context.shutdown();
			set.sampler_info.shutdown();
			set.stipple.shutdown();
			set.blend_values.shutdown();
			set.fill_values.shutdown();
			set.vertex_draw_params.shutdown();
			set.fragment_constants.shutdown();
			set.transform_constants.shutdown();
			set.instancing_constants.shutdown();
			set.instancing_lookup.shutdown();
		}

		for (stream_set& set : m_streams)
		{
			set.persistent.shutdown();
			set.volatile_stream.shutdown();
			set.index.shutdown();
			set.overlay_vertices.shutdown();
			set.overlay_uniforms.shutdown();
		}

		m_slot = 0;
	}

	void constants_heap::begin_frame(u32 slot)
	{
		m_slot = slot % frame_slot_count;

		ring_set& set = m_sets[m_slot];
		set.vertex_context.begin_frame();
		set.draw_parameters.begin_frame();
		set.fragment_context.begin_frame();
		set.sampler_info.begin_frame();
		set.stipple.begin_frame();
		set.blend_values.begin_frame();
		set.fill_values.begin_frame();
		set.vertex_draw_params.begin_frame();
		set.fragment_constants.begin_frame();
		set.transform_constants.begin_frame();
		set.instancing_constants.begin_frame();
		set.instancing_lookup.begin_frame();

		stream_set& streams = m_streams[m_slot];
		streams.persistent.begin_frame();
		streams.volatile_stream.begin_frame();
		streams.index.begin_frame();
		streams.overlay_vertices.begin_frame();
		streams.overlay_uniforms.begin_frame();
	}
}

#endif // RSX_ENABLE_METAL4
