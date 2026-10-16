#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include "util/logs.hpp"

#include <algorithm>

namespace mtl
{
	MTL::ResourceOptions storage_options(MTL::StorageMode storage)
	{
		return storage == MTL::StorageModePrivate ? MTL::ResourceStorageModePrivate : MTL::ResourceStorageModeShared;
	}

	void heap_pool::init(MTL::Device* device, MTL::StorageMode storage, u64 block_size)
	{
		m_device = device;
		m_storage = storage;
		m_block_size = block_size;
	}

	void heap_pool::shutdown()
	{
		for (const block& entry : m_blocks)
		{
			entry.heap->release();
		}

		m_blocks.clear();
		m_device = nullptr;
	}

	MTL::Heap* heap_pool::create_heap(u64 size)
	{
		MTL::HeapDescriptor* descriptor = MTL::HeapDescriptor::alloc()->init();
		descriptor->setType(MTL::HeapTypePlacement);
		descriptor->setStorageMode(m_storage);
		// The pass scheduler inserts the barriers, so a heap does not track hazards.
		descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
		descriptor->setSize(size);

		MTL::Heap* heap = m_device->newHeap(descriptor);
		descriptor->release();
		return heap;
	}

	MTL::Heap* heap_pool::allocate(u64 size, u64 alignment, u64& offset)
	{
		// A request over half a block gets a dedicated heap (01 §3.2), so it cannot
		// fragment the pool.
		if (size * 2 > m_block_size)
		{
			MTL::Heap* dedicated = create_heap(size);
			if (!dedicated)
			{
				return nullptr;
			}

			offset = 0;
			m_blocks.push_back({ dedicated, {} });
			return dedicated;
		}

		for (block& entry : m_blocks)
		{
			for (auto span = entry.free_spans.begin(); span != entry.free_spans.end(); ++span)
			{
				const u64 aligned = (span->first + alignment - 1) & ~(alignment - 1);
				const u64 padding = aligned - span->first;
				if (span->second < padding + size)
				{
					continue;
				}

				const u64 remaining = span->second - padding - size;

				if (padding > 0)
				{
					span->second = padding;
					entry.free_spans.insert(span + 1, { aligned + size, remaining });
				}
				else if (remaining > 0)
				{
					span->first = aligned + size;
					span->second = remaining;
				}
				else
				{
					entry.free_spans.erase(span);
				}

				offset = aligned;
				return entry.heap;
			}
		}

		MTL::Heap* heap = create_heap(m_block_size);
		if (!heap)
		{
			return nullptr;
		}

		// Offset zero is aligned, and the request is at most half a block.
		block& entry = m_blocks.emplace_back();
		entry.heap = heap;
		entry.free_spans.push_back({ size, m_block_size - size });

		offset = 0;
		return heap;
	}

	void heap_pool::free(MTL::Heap* heap, u64 offset, u64 size)
	{
		for (block& entry : m_blocks)
		{
			if (entry.heap != heap)
			{
				continue;
			}

			const auto at = std::lower_bound(entry.free_spans.begin(), entry.free_spans.end(), offset,
				[](const std::pair<u64, u64>& span, u64 value) { return span.first < value; });

			auto span = entry.free_spans.insert(at, { offset, size });

			// Join the next span, then the previous one.
			if (span + 1 != entry.free_spans.end() && span->first + span->second == (span + 1)->first)
			{
				span->second += (span + 1)->second;
				entry.free_spans.erase(span + 1);
			}

			if (span != entry.free_spans.begin())
			{
				auto previous = span - 1;
				if (previous->first + previous->second == span->first)
				{
					previous->second += span->second;
					entry.free_spans.erase(span);
				}
			}

			return;
		}
	}

	namespace
	{
		MTL::ResidencySet* create_residency_set(MTL::Device* device, u32 capacity)
		{
			MTL::ResidencySetDescriptor* descriptor = MTL::ResidencySetDescriptor::alloc()->init();
			descriptor->setInitialCapacity(capacity);

			MTL::ResidencySet* set = device->newResidencySet(descriptor, nullptr);
			descriptor->release();
			return set;
		}
	}

	void residency_manager::init(MTL::Device* device, MTL4::CommandQueue* queue, u32 frame_count)
	{
		m_device = device;
		m_queue = queue;

		m_persistent = create_residency_set(device, 256);
		m_queue->addResidencySet(m_persistent);

		m_per_frame.resize(frame_count);
		m_tracked.resize(frame_count);

		for (u32 index = 0; index < frame_count; ++index)
		{
			m_per_frame[index] = create_residency_set(device, 256);
			m_queue->addResidencySet(m_per_frame[index]);
		}
	}

	void residency_manager::shutdown()
	{
		if (!m_queue)
		{
			return;
		}

		if (m_persistent)
		{
			m_queue->removeResidencySet(m_persistent);
			m_persistent->release();
			m_persistent = nullptr;
		}

		for (MTL::ResidencySet* set : m_per_frame)
		{
			m_queue->removeResidencySet(set);
			set->release();
		}

		m_per_frame.clear();
		m_tracked.clear();
		m_device = nullptr;
		m_queue = nullptr;
	}

	void residency_manager::make_persistent(MTL::Allocation* allocation)
	{
		if (!allocation || !m_persistent)
		{
			return;
		}

		m_persistent->addAllocation(allocation);
		m_persistent->commit();
	}

	void residency_manager::on_frame_begin(u32 index)
	{
		m_current = index;
	}

	void residency_manager::on_bind(MTL::Allocation* allocation)
	{
		if (!allocation || m_per_frame.empty())
		{
			return;
		}

		const u32 slot = m_current % static_cast<u32>(m_per_frame.size());
		m_per_frame[slot]->addAllocation(allocation);
		m_tracked[slot].insert(allocation);
	}

	void residency_manager::commit_frame()
	{
		if (m_per_frame.empty())
		{
			return;
		}

		m_per_frame[m_current % static_cast<u32>(m_per_frame.size())]->commit();
	}

	void residency_manager::on_commit_complete(u32 index)
	{
		if (m_per_frame.empty())
		{
			return;
		}

		const u32 slot = index % static_cast<u32>(m_per_frame.size());

		for (MTL::Allocation* allocation : m_tracked[slot])
		{
			m_per_frame[slot]->removeAllocation(allocation);
		}

		m_tracked[slot].clear();
		m_per_frame[slot]->commit();
	}

	void resource_manager::init(const device& dev, u32 frame_count)
	{
		m_device = dev.dev();
		m_shared.init(m_device, MTL::StorageModeShared, shared_block_size);
		m_private.init(m_device, MTL::StorageModePrivate, private_block_size);
		m_residency.init(m_device, dev.queue(), frame_count);

		// The guest window is an application-lifetime resource, and every buffer
		// binding refers to it (HLD-02 I5).
		m_residency.make_persistent(dev.window().buffer);
	}

	void resource_manager::shutdown()
	{
		garbage_collect();
		m_residency.shutdown();
		m_shared.shutdown();
		m_private.shutdown();
		m_device = nullptr;
	}

	MTL::Buffer* resource_manager::new_buffer(u64 length, MTL::StorageMode storage)
	{
		const MTL::ResourceOptions options = storage_options(storage);
		const MTL::SizeAndAlign required = m_device->heapBufferSizeAndAlign(length, options);

		heap_pool& pool = (storage == MTL::StorageModePrivate) ? m_private : m_shared;

		u64 offset = 0;
		MTL::Heap* heap = pool.allocate(required.size, required.align, offset);
		if (!heap)
		{
			rsx_log.error("Metal 4: heap growth failed for a %llu byte buffer", length);
			return nullptr;
		}

		MTL::Buffer* buffer = heap->newBuffer(length, options, offset);
		if (!buffer)
		{
			pool.free(heap, offset, required.size);
			rsx_log.error("Metal 4: buffer allocation failed for %llu bytes", length);
			return nullptr;
		}

		m_residency.on_bind(buffer);
		return buffer;
	}

	MTL::Texture* resource_manager::new_texture(MTL::TextureDescriptor* descriptor, MTL::StorageMode storage)
	{
		descriptor->setStorageMode(storage);

		const MTL::SizeAndAlign required = m_device->heapTextureSizeAndAlign(descriptor);

		heap_pool& pool = (storage == MTL::StorageModePrivate) ? m_private : m_shared;

		u64 offset = 0;
		MTL::Heap* heap = pool.allocate(required.size, required.align, offset);
		if (!heap)
		{
			rsx_log.error("Metal 4: heap growth failed for a texture");
			return nullptr;
		}

		MTL::Texture* texture = heap->newTexture(descriptor, offset);
		if (!texture)
		{
			pool.free(heap, offset, required.size);
			rsx_log.error("Metal 4: texture allocation failed");
			return nullptr;
		}

		m_residency.on_bind(texture);
		return texture;
	}

	void resource_manager::make_persistent(MTL::Allocation* allocation)
	{
		m_residency.make_persistent(allocation);
	}

	void resource_manager::on_frame_begin(u32 index)
	{
		m_residency.on_frame_begin(index);
	}

	void resource_manager::commit_frame()
	{
		m_residency.commit_frame();
	}

	void resource_manager::on_commit_complete(u32 index)
	{
		m_residency.on_commit_complete(index);
	}

	void resource_manager::defer_release(MTL::Allocation* allocation)
	{
		if (allocation)
		{
			m_deferred.push_back(allocation);
		}
	}

	void resource_manager::garbage_collect()
	{
		for (MTL::Allocation* allocation : m_deferred)
		{
			allocation->release();
		}

		m_deferred.clear();
	}
}

#endif // RSX_ENABLE_METAL4
