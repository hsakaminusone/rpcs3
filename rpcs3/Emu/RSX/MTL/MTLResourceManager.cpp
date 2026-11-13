#include "MTLResourceManager.h"
#include "MTLInstrumentation.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/Memory/vm.h"
#include "util/logs.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

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

	void heap_pool::set_residency(residency_manager* residency)
	{
		m_residency = residency;
	}

	MTL::Heap* heap_pool::create_heap(u64 size)
	{
		MTL::HeapDescriptor* descriptor = MTL::HeapDescriptor::alloc()->init();
		descriptor->setType(MTL::HeapTypePlacement);
		descriptor->setStorageMode(m_storage);
		// The pass scheduler inserts the barriers, so a heap does not track hazards.
		// TODO(excise): a diagnostic that hands the ordering back to the driver.  A
		// tracked heap delays every access to a sub-allocation until the conflicting
		// access completes, so a run with this on answers one question: is a visual
		// defect a missing edge (the defect goes) or content (the defect stays)?  It
		// changes what the backend emits, so the boot states it.
		descriptor->setHazardTrackingMode(probe::variant("heaps")
			? MTL::HazardTrackingModeTracked
			: MTL::HazardTrackingModeUntracked);
		descriptor->setSize(size);

		MTL::Heap* heap = m_device->newHeap(descriptor);
		descriptor->release();

		// A sub-allocation is reachable only through its heap, so the heap is the
		// unit of residency.  It is released at shutdown, after the set is gone.
		if (heap && m_residency)
		{
			m_residency->make_persistent(heap);
		}

		return heap;
	}

	MTL::Heap* heap_pool::allocate(u64 size, u64 alignment, u64& offset, u32& block_index)
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
			block_index = static_cast<u32>(m_blocks.size());
			m_blocks.push_back({ dedicated, {} });
			return dedicated;
		}

		for (usz index = 0; index < m_blocks.size(); ++index)
		{
			block& entry = m_blocks[index];

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
				block_index = static_cast<u32>(index);
				return entry.heap;
			}
		}

		MTL::Heap* heap = create_heap(m_block_size);
		if (!heap)
		{
			return nullptr;
		}

		// Offset zero is aligned, and the request is at most half a block.
		block_index = static_cast<u32>(m_blocks.size());
		block& entry = m_blocks.emplace_back();
		entry.heap = heap;
		entry.free_spans.push_back({ size, m_block_size - size });

		offset = 0;
		return heap;
	}

	void heap_pool::free(u32 block_index, u64 offset, u64 size)
	{
		// The index is the one the allocation recorded.  A block is only dropped at
		// shutdown, so it still names the same heap (P3).
		if (static_cast<usz>(block_index) >= m_blocks.size())
		{
			return;
		}

		std::vector<std::pair<u64, u64>>& spans = m_blocks[block_index].free_spans;
		const auto at = std::lower_bound(spans.begin(), spans.end(), offset,
			[](const std::pair<u64, u64>& span, u64 value) { return span.first < value; });

		auto span = spans.insert(at, { offset, size });

		// Join the next span, then the previous one.
		if (span + 1 != spans.end() && span->first + span->second == (span + 1)->first)
		{
			span->second += (span + 1)->second;
			spans.erase(span + 1);
		}

		if (span != spans.begin())
		{
			auto previous = span - 1;
			if (previous->first + previous->second == span->first)
			{
				previous->second += span->second;
				spans.erase(span);
			}
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

	void residency_manager::init(MTL::Device* device, MTL4::CommandQueue* queue)
	{
		m_device = device;
		m_queue = queue;

		// The set is attached before any heap exists, so no allocation can become
		// reachable before the set that covers it.
		m_persistent = create_residency_set(device, 256);
		m_queue->addResidencySet(m_persistent);
	}

	void residency_manager::shutdown()
	{
		if (!m_queue)
		{
			return;
		}

		if (m_persistent)
		{
			// The set goes before its allocations are released (HLD-02 I7).
			m_queue->removeResidencySet(m_persistent);
			m_persistent->release();
			m_persistent = nullptr;
		}

		m_published.clear();
		m_dirty = false;
		m_device = nullptr;
		m_queue = nullptr;
	}

	void residency_manager::make_persistent(MTL::Allocation* allocation)
	{
		if (!allocation || !m_persistent)
		{
			return;
		}

		// The commit is deferred: a run of heap creations is one driver call, and the
		// flush before the next command buffer commit publishes them all (P6).
		m_persistent->addAllocation(allocation);
		m_published.insert(allocation);
		m_dirty = true;
	}

	void residency_manager::flush()
	{
		if (!m_dirty || !m_persistent)
		{
			return;
		}

		m_persistent->commit();
		m_dirty = false;
	}

	void residency_manager::remove(MTL::Allocation* allocation)
	{
		if (!allocation || !m_persistent)
		{
			return;
		}

		// Only a published allocation is in the set, and a repeated removal must not
		// touch the set again.
		if (m_published.erase(allocation) == 0)
		{
			return;
		}

		m_persistent->removeAllocation(allocation);
		m_persistent->commit();
	}

	bool residency_manager::is_persistent(MTL::Allocation* allocation) const
	{
		return m_published.count(allocation) != 0;
	}

	void resource_manager::init(const device& dev, u32 frame_count)
	{
		m_device = dev.dev();
		m_guest = dev.window().buffer;
		m_supports_bc = m_device->supportsBCTextureCompression();

		// TODO(excise): level 2; the compression capability decides the DXT path.
		MTL_NOTE(2, "Metal 4: BC texture compression is %s", m_supports_bc ? "available" : "unavailable");

		m_shared.init(m_device, MTL::StorageModeShared, shared_block_size);
		m_private.init(m_device, MTL::StorageModePrivate, private_block_size);
		m_residency.init(m_device, dev.queue());
		m_deferred.resize(frame_count);

		// Every heap the pools create is published to the persistent set, so heap
		// residency covers each sub-allocation for its whole life (HLD-02 §2.6 I5).
		m_shared.set_residency(&m_residency);
		m_private.set_residency(&m_residency);

		// The guest window is an application-lifetime resource, and every buffer
		// binding refers to it (HLD-02 I5).
		m_residency.make_persistent(dev.window().buffer);
	}

	void resource_manager::shutdown()
	{
		garbage_collect();

		// The label imports leave the residency set before their memory goes (L136).
		for (label_page& page : m_label_pages)
		{
			m_residency.remove(page.buffer);
			page.buffer->release();
		}

		m_label_pages.clear();
		m_residency.shutdown();
		m_shared.shutdown();
		m_private.shutdown();
		m_device = nullptr;
	}

	MTL::Buffer* resource_manager::new_buffer(u64 length, MTL::StorageMode storage)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "resource/new buffer");
		// TODO(excise): level 2; the buffer lifetime and a coarse size bucket (area G).
		MTL_PROBE_LATCH(2, "resource/buffer created");
		MTL_PROBE_HISTOGRAM(2, "resource buffer size",
			length <= 4096 ? 0 : length <= 65536 ? 1 : length <= 1048576 ? 2 : 3, "buffer size bucket");
		const MTL::ResourceOptions options = storage_options(storage);

		// The answer depends only on the length and the storage options, and the
		// pool asks for the same handful of sizes repeatedly (rings, staging, the
		// per-frame blocks), so the driver call is memoised (P4).
		const auto memo_key = std::pair<u64, u64>(length, static_cast<u64>(options));
		MTL::SizeAndAlign required{};
		const auto memo = m_buffer_size_align.find(memo_key);

		if (memo != m_buffer_size_align.end())
		{
			required = memo->second;
		}
		else
		{
			required = m_device->heapBufferSizeAndAlign(length, options);

			if (m_buffer_size_align.size() < 1024)
			{
				m_buffer_size_align.emplace(memo_key, required);
			}
		}

		heap_pool& pool = (storage == MTL::StorageModePrivate) ? m_private : m_shared;

		u64 offset = 0;
		u32 block = 0;
		MTL::Heap* heap = pool.allocate(required.size, required.align, offset, block);
		if (!heap)
		{
			rsx_log.error("Metal 4: heap growth failed for a %llu byte buffer", length);
			return nullptr;
		}

		MTL::Buffer* buffer = heap->newBuffer(length, options, offset);
		if (!buffer)
		{
			pool.free(block, offset, required.size);
			rsx_log.error("Metal 4: buffer allocation failed for %llu bytes", length);
			return nullptr;
		}

		track(buffer, pool, block, offset, required.size);
#ifndef NDEBUG
		// The invariant is structural, so it belongs to a build that can assert it
		// rather than to the allocation path (P7).
		check_residency(heap);
#endif

		// TODO(excise): level 5; MTL_PROBE_VARIANT=poison.  A shared buffer is CPU
		// reachable, so a never-written allocation is filled here with the word of
		// its own creation index, and the log line carries the word; the GPU-side
		// allocations are filled in the texture cache instead.  A buffer whose
		// supposedly dead region keeps the fill was never written, which is the
		// other half of the unwritten-resource question.
		if (probe::variant("poison") && storage == MTL::StorageModeShared)
		{
			static std::atomic<u32> diagnostic_buffer_index{0};
			const u32 index = diagnostic_buffer_index.fetch_add(1, std::memory_order_relaxed);
			const u32 word = probe::poison_word(index + 1u);

			if (void* bytes = buffer->contents())
			{
				probe::fill_words(bytes, length, word);

				static std::atomic<u32> diagnostic_buffer_logs{0};
				if (diagnostic_buffer_logs.fetch_add(1, std::memory_order_relaxed) < 64)
				{
					MTL_NOTE(2, "Metal 4: poison buffer %u, %llu bytes, word 0x%08x",
						index + 1u, static_cast<unsigned long long>(length), word);
				}
			}
		}
		return buffer;
	}

	MTL::Buffer* resource_manager::label_buffer(u32 address, u32& offset)
	{
		// The host page is the unit of a guest mapping, so it is the unit of the
		// import.  A page re-mapped after this call would keep the GPU on the old
		// physical page, which is why the import is made here, after the page exists,
		// rather than with the application-lifetime window (L136).
		constexpr u32 page_size = 0x10000;
		const u32 base = address & ~(page_size - 1);

		// A guest address is 32 bits, so its page always lies inside the imported
		// window; the invariant is stated for the reader, not enforced at run time.
		ensure(static_cast<u64>(base) + page_size <= guest_address_space);

		for (const label_page& page : m_label_pages)
		{
			if (page.base == base)
			{
				offset = address - base;
				return page.buffer;
			}
		}

		MTL::Buffer* buffer = m_device->newBuffer(vm::g_sudo_addr + base, page_size,
			MTL::ResourceStorageModeShared, nullptr);
		if (!buffer)
		{
			rsx_log.error("Metal 4: the label import at 0x%x could not be created", base);
			return nullptr;
		}

		m_residency.make_persistent(buffer);
		m_label_pages.push_back({ base, buffer });
		offset = address - base;
		return buffer;
	}

	u8 resource_manager::supported_sample_count(u8 count) const
	{
		// Apple lists 4 samples as universal and 2 as "some devices", so a request
		// the device rejects folds up to 4 and keeps the antialiasing.
		if (count > 1 && !m_device->supportsTextureSampleCount(count))
		{
			return 4;
		}

		return count;
	}

	MTL::Texture* resource_manager::new_texture(MTL::TextureDescriptor* descriptor, MTL::StorageMode storage)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "resource/new texture");
		// TODO(excise): level 2; the texture lifetime and its pixel format (area G).
		MTL_PROBE_LATCH(2, "resource/texture created");
		MTL_PROBE_HISTOGRAM(2, "resource texture format", static_cast<u32>(descriptor->pixelFormat()), "metal pixel format");
		descriptor->setStorageMode(storage);

		const MTL::SizeAndAlign required = m_device->heapTextureSizeAndAlign(descriptor);

		heap_pool& pool = (storage == MTL::StorageModePrivate) ? m_private : m_shared;

		u64 offset = 0;
		u32 block = 0;
		MTL::Heap* heap = pool.allocate(required.size, required.align, offset, block);
		if (!heap)
		{
			rsx_log.error("Metal 4: heap growth failed for a texture");
			return nullptr;
		}

		MTL::Texture* texture = heap->newTexture(descriptor, offset);
		if (!texture)
		{
			pool.free(block, offset, required.size);
			rsx_log.error("Metal 4: texture allocation failed");
			return nullptr;
		}

		track(texture, pool, block, offset, required.size);
#ifndef NDEBUG
		check_residency(heap);
#endif
		return texture;
	}

	void resource_manager::make_persistent(MTL::Allocation* allocation)
	{
		m_residency.make_persistent(allocation);
	}

	void resource_manager::flush_residency()
	{
		m_residency.flush();
	}

	void resource_manager::on_frame_begin(u32 index)
	{
		m_frame_slot = index;
	}

	void resource_manager::on_commit_complete(u32 index)
	{
		if (m_deferred.empty())
		{
			return;
		}

		// The slot that just went idle is the one the frame that just completed
		// used, so the objects deferred during that frame are safe to release
		// (HLD-02 I7).  The frames on later slots may still be in flight.
		release_list(m_deferred[index]);
	}

	void resource_manager::quarantine(u32 index)
	{
		if (m_deferred.empty())
		{
			return;
		}

		std::vector<MTL::Allocation*>& list = m_deferred[index];

		if (list.empty())
		{
			return;
		}

		m_quarantine.insert(m_quarantine.end(), list.begin(), list.end());
		list.clear();
	}

	void resource_manager::drain_quarantine()
	{
		release_list(m_quarantine);
	}

	void resource_manager::check_residency(MTL::Heap* heap)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "resource/residency");
		if (m_residency.is_persistent(heap))
		{
			return;
		}

		// A sub-allocation of an unpublished heap reads zeros in silence: Metal
		// raises no validation error and no GPU fault (HLD-02 F1).  Report it here,
		// where the cause is, rather than at the draw that shows the symptom.
		static bool reported = false;
		if (!reported)
		{
			reported = true;
			rsx_log.error("Metal 4: heap %p is not in the persistent residency set, so its allocations read zeros",
				static_cast<const void*>(heap));
		}
	}

	void resource_manager::track(MTL::Allocation* allocation, heap_pool& pool, u32 block, u64 offset, u64 size)
	{
		m_spans.emplace(allocation, allocation_span{ &pool, block, offset, size });
	}

	void resource_manager::defer_release(MTL::Allocation* allocation)
	{
		if (!allocation)
		{
			return;
		}

		if (m_deferred.empty())
		{
			allocation->release();
			return;
		}

		m_deferred[m_frame_slot % static_cast<u32>(m_deferred.size())].push_back(allocation);
	}

	void resource_manager::garbage_collect()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "resource/gc");
		for (std::vector<MTL::Allocation*>& list : m_deferred)
		{
			release_list(list);
		}
	}

	void resource_manager::release_list(std::vector<MTL::Allocation*>& list)
	{
		for (MTL::Allocation* allocation : list)
		{
			if (const auto found = m_spans.find(allocation); found != m_spans.end())
			{
				const allocation_span& span = found->second;
				span.pool->free(span.block, span.offset, span.size);
				m_spans.erase(found);
			}

			// The object is about to be released, so the set must stop naming it;
			// otherwise a later allocation that reuses its span is covered by a stale
			// entry (DEVIATIONS-SYNC.md, SY5).
			m_residency.remove(allocation);
			allocation->release();
		}

		list.clear();
	}
}

#endif // RSX_ENABLE_METAL4
