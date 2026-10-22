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

	class residency_manager;

	// One placement heap class and its free list (HLD-02 §2.1).  A sub-allocation is
	// an offset into a heap.  A heap never moves, so the offset stays valid for the
	// life of the allocation.
	class heap_pool
	{
	public:
		void init(MTL::Device* device, MTL::StorageMode storage, u64 block_size);
		void shutdown();

		// The pool publishes every heap it creates to the persistent residency set.
		// A heap outlives all of its sub-allocations, so heap residency covers each
		// of them for its whole life (HLD-02 §2.6 Tier 1).
		void set_residency(residency_manager* residency);

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
		residency_manager* m_residency = nullptr;
	};

	// Explicit residency (HLD-02 §2.6).  A Metal 4 command buffer holds no strong
	// reference to its resources, so every reachable allocation must be resident in
	// a set that was committed and attached to the queue before the commit.
	//
	// Residency here follows resource lifetime, not frame lifetime.  The backend's
	// resources are all cache-owned and outlive the frame that creates them, and a
	// set that drops an allocation which is still reachable leaves the GPU reading
	// zeros in silence (HLD-02 F1).  Every heap and every direct allocation is
	// therefore made persistent at creation and removed only when it is released.
	class residency_manager
	{
	public:
		void init(MTL::Device* device, MTL4::CommandQueue* queue);
		void shutdown();

		void make_persistent(MTL::Allocation* allocation);
		// Drop one allocation from the set when its owner is released.  A set that
		// keeps an allocation forever retains a heap span that a later allocation
		// can reuse (DEVIATIONS-SYNC.md, SY5).
		void remove(MTL::Allocation* allocation);
		bool is_persistent(MTL::Allocation* allocation) const;

	private:
		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		MTL::ResidencySet* m_persistent = nullptr;
		std::unordered_set<MTL::Allocation*> m_published;
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

		// A DXT texture is BC on a device that supports it and expanded BGRA8 on a
		// device that does not.  The uploader and the format table must agree, or the
		// texture descriptor names a format the device cannot allocate.
		bool supports_bc_texture_compression() const { return m_supports_bc; }

		void make_persistent(MTL::Allocation* allocation);

		// The frame slot that is recording, so a release lands on the slot whose
		// command buffer can still reference the object.
		void on_frame_begin(u32 index);
		void on_commit_complete(u32 index);

		// A slot whose commit never reported its feedback still owns a deferred
		// list, and the GPU may still read those objects.  Move the list aside and
		// release it only when the ring is idle, which is the first moment nothing
		// can reference it (DEVIATIONS-SYNC.md, SY2).
		void quarantine(u32 index);
		void drain_quarantine();

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
		void check_residency(MTL::Heap* heap);

		MTL::Device* m_device = nullptr;
		MTL::Buffer* m_guest = nullptr;
		bool m_supports_bc = true;
		residency_manager m_residency;
		heap_pool m_shared;
		heap_pool m_private;
		std::unordered_map<MTL::Allocation*, allocation_span> m_spans;
		std::vector<std::vector<MTL::Allocation*>> m_deferred;
		std::vector<MTL::Allocation*> m_quarantine;
		u32 m_frame_slot = 0;
	};

	// A private placement heap holds GPU-only resources; a shared one holds the
	// resources the CPU writes.
	MTL::ResourceOptions storage_options(MTL::StorageMode storage);
}

#endif // RSX_ENABLE_METAL4
