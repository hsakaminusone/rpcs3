#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mtl
{
	// The pool sizes follow the heap suballocator survey (01 §3.2).
	constexpr u64 shared_block_size = 32u * 1024u * 1024u;
	constexpr u64 private_block_size = 128u * 1024u * 1024u;

	// One placement heap class and its free list (HLD-02 §2.1).  A sub-allocation is
	// an offset into a heap.  A heap never moves, so the offset stays valid for the
	// life of the allocation.
	class heap_pool
	{
	public:
		void init(MTL::Device* device, MTL::StorageMode storage, u64 block_size);
		void shutdown();

		// Return the heap that holds the span, or nullptr when the pool cannot grow.
		MTL::Heap* allocate(u64 size, u64 alignment, u64& offset);
		void free(MTL::Heap* heap, u64 offset, u64 size);

	private:
		struct block
		{
			MTL::Heap* heap = nullptr;
			// (offset, size) spans, offset-sorted.
			std::vector<std::pair<u64, u64>> free_spans;
		};

		MTL::Heap* create_heap(u64 size);

		MTL::Device* m_device = nullptr;
		MTL::StorageMode m_storage = MTL::StorageModeShared;
		u64 m_block_size = 0;
		std::vector<block> m_blocks;
	};

	// Explicit residency (HLD-02 §2.6).  A Metal 4 command buffer holds no strong
	// reference to its resources, so every reachable allocation must be resident.
	// The persistent set holds the long-lived objects; a ring of per-frame sets holds
	// the transient ones.
	class residency_manager
	{
	public:
		void init(MTL::Device* device, MTL4::CommandQueue* queue, u32 frame_count);
		void shutdown();

		void make_persistent(MTL::Allocation* allocation);
		void on_frame_begin(u32 index);
		void on_bind(MTL::Allocation* allocation);
		void commit_frame();
		void on_commit_complete(u32 index);

	private:
		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		MTL::ResidencySet* m_persistent = nullptr;
		std::vector<MTL::ResidencySet*> m_per_frame;
		std::vector<std::unordered_set<MTL::Allocation*>> m_tracked;
		u32 m_current = 0;
	};

	// The resource layer (HLD-01 C5, HLD-02 §2.1).  A buffer or a texture is
	// sub-allocated from the placement heap for its storage class.  Guest memory is
	// never a texture source, so surfaces and textures are copies (§2.1).
	class resource_manager
	{
	public:
		void init(const device& dev, u32 frame_count);
		void shutdown();

		MTL::Buffer* new_buffer(u64 length, MTL::StorageMode storage);
		MTL::Texture* new_texture(MTL::TextureDescriptor* descriptor, MTL::StorageMode storage);

		// The one no-copy buffer over the guest block (HLD-02 §3.5).  A guest address
		// is the offset into it, so a load needs no staging copy.
		MTL::Buffer* guest_buffer() const { return m_guest; }

		// Metal's sample-count support is per device: Apple's table lists 4 as
		// universal and 2 as "some devices", so a host request the device rejects
		// folds up to 4 and keeps the antialiasing.
		u8 supported_sample_count(u8 count) const;

		void make_persistent(MTL::Allocation* allocation);
		void on_frame_begin(u32 index);
		void commit_frame();
		void on_commit_complete(u32 index);

		// Deferred release (HLD-02 I7).  A Metal 4 command buffer holds no strong
		// reference, so an object must survive until every frame that can reference it
		// completes.  A release is queued on the frame slot that asks for it and runs
		// when that slot's frame completes, which is the first moment nothing on the
		// queue can still reference it.
		void defer_release(MTL::Allocation* allocation);
		void garbage_collect();

	private:
		// The heap span of one allocation, so a release returns it to its pool.
		struct allocation_span
		{
			heap_pool* pool = nullptr;
			MTL::Heap* heap = nullptr;
			u64 offset = 0;
			u64 size = 0;
		};

		void track(MTL::Allocation* allocation, heap_pool& pool, MTL::Heap* heap, u64 offset, u64 size);
		void release_list(std::vector<MTL::Allocation*>& list);

		MTL::Device* m_device = nullptr;
		MTL::Buffer* m_guest = nullptr;
		residency_manager m_residency;
		heap_pool m_shared;
		heap_pool m_private;
		std::unordered_map<MTL::Allocation*, allocation_span> m_spans;
		std::vector<std::vector<MTL::Allocation*>> m_deferred;
		u32 m_frame_slot = 0;
	};

	// A private placement heap holds GPU-only resources; a shared one holds the
	// resources the CPU writes.
	MTL::ResourceOptions storage_options(MTL::StorageMode storage);
}

#endif // RSX_ENABLE_METAL4
