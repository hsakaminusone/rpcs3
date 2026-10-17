#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

namespace mtl
{
	// The latched `sync-flip` selector state (LLD-sync-flip §2.1).  These four
	// selectors persist until rewritten and feed every semaphore action.  The
	// notify and report DMA contexts are absent: they belong to `surface`
	// (dispositions/surface.csv owns M_NV4097_SET_CONTEXT_DMA_NOTIFIES and
	// M_NV4097_SET_CONTEXT_DMA_REPORT).
	struct sync_flip_state
	{
		u32 semaphore_context_dma_406e = 0;
		u32 semaphore_offset_406e = 0;
		u32 semaphore_context_dma_4097 = 0;
		u32 semaphore_offset_4097 = 0;
	};

	sync_flip_state capture_sync_flip_state();

	// The two semaphore engines carry separate selectors and separate alignment
	// rules (LLD-sync-flip §2.1).
	enum class semaphore_engine
	{
		nv406e,
		nv4097,
	};

	u32 get_semaphore_context_dma(const sync_flip_state& state, semaphore_engine engine);
	u32 get_semaphore_offset(const sync_flip_state& state, semaphore_engine engine);

	// The 406e release rejects a 4-byte-misaligned offset (nv406e.cpp:111); the
	// 4097 releases reject a 16-byte-misaligned offset (nv4097.cpp:796,826).
	bool is_semaphore_offset_aligned(u32 offset, semaphore_engine engine);

	// The flip semaphore is offset 0x10 on the semaphore-R context (nv406e.cpp:121).
	// It bypasses the draw-stream label path (LLD-sync-flip invariant 6).
	bool is_flip_semaphore(u32 offset, u32 context_dma);

	// GET_REPORT / CLEAR_REPORT_VALUE type decode (LLD-sync-flip §3.3).  The ZCULL
	// kinds retire through the visibility buffer; `timestamp` is a host write.
	enum class report_kind
	{
		timestamp,
		zpass_pixel_count,
		zcull_stats,
		zcull_stats1,
		zcull_stats2,
		zcull_stats3,
	};

	report_kind get_report_kind(u8 type);
	bool is_zcull_report(report_kind kind);

	// The byte-order swizzle the back-end release applies to its label value
	// (nv4097.cpp:840).
	u32 get_back_end_semaphore_value(u32 arg);

	// The release kinds differ by the flush pair of write_gcm_label<FlushDMA,FlushPipe>
	// (nv47_sync.hpp:32-53).
	enum class release_kind
	{
		nv406e,       // <false, true>  - pipe flush only
		back_end,     // <true,  true>  - pipe and DMA flush
		texture_read, // <true,  false> - DMA flush; strict mode adds the pipe flush
	};

	// The producer barrier a release needs before its label store (LLD-sync-flip
	// §3.2).  The substrate emits it through the barrier chokepoint.
	struct release_barrier
	{
		MTL::Stages after_stages = MTL::StageAll;
		MTL::Stages before_queue_stages = MTL::StageAll;
		MTL4::VisibilityOptions visibility = MTL4::VisibilityOptionDevice;
	};

	release_barrier get_release_barrier(release_kind kind, bool strict);
}

#endif // RSX_ENABLE_METAL4
