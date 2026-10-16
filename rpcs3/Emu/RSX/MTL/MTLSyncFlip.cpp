#include "MTLSyncFlip.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "util/logs.hpp"

namespace mtl
{
	sync_flip_state capture_sync_flip_state()
	{
		sync_flip_state state;

		state.semaphore_context_dma_406e = rsx::method_registers.semaphore_context_dma_406e();
		state.semaphore_offset_406e = rsx::method_registers.semaphore_offset_406e();
		state.semaphore_context_dma_4097 = rsx::method_registers.semaphore_context_dma_4097();
		state.semaphore_offset_4097 = rsx::method_registers.semaphore_offset_4097();

		return state;
	}

	u32 get_semaphore_context_dma(const sync_flip_state& state, semaphore_engine engine)
	{
		return engine == semaphore_engine::nv406e ? state.semaphore_context_dma_406e : state.semaphore_context_dma_4097;
	}

	u32 get_semaphore_offset(const sync_flip_state& state, semaphore_engine engine)
	{
		return engine == semaphore_engine::nv406e ? state.semaphore_offset_406e : state.semaphore_offset_4097;
	}

	bool is_semaphore_offset_aligned(u32 offset, semaphore_engine engine)
	{
		const u32 alignment = engine == semaphore_engine::nv406e ? 4 : 16;
		return (offset % alignment) == 0;
	}

	bool is_flip_semaphore(u32 offset, u32 context_dma)
	{
		return offset == 0x10 && context_dma == CELL_GCM_CONTEXT_DMA_SEMAPHORE_R;
	}

	report_kind get_report_kind(u8 type)
	{
		switch (type)
		{
		case CELL_GCM_ZPASS_PIXEL_CNT: return report_kind::zpass_pixel_count;
		case CELL_GCM_ZCULL_STATS: return report_kind::zcull_stats;
		case CELL_GCM_ZCULL_STATS1: return report_kind::zcull_stats1;
		case CELL_GCM_ZCULL_STATS2: return report_kind::zcull_stats2;
		case CELL_GCM_ZCULL_STATS3: return report_kind::zcull_stats3;
		default:
			// get_report falls back to a timestamp-only write for an unknown type
			// (nv4097.cpp:665-672).
			rsx_log.error("Metal 4: unknown report type 0x%x", type);
			return report_kind::timestamp;
		}
	}

	bool is_zcull_report(report_kind kind)
	{
		return kind != report_kind::timestamp;
	}

	u32 get_back_end_semaphore_value(u32 arg)
	{
		return (arg & 0xff00ff00) | ((arg & 0xff) << 16) | ((arg >> 16) & 0xff);
	}

	release_barrier get_release_barrier(release_kind kind, bool strict)
	{
		release_barrier barrier;

		// The non-strict texture-read release flushes the DMA path only, so the
		// barrier orders the sampler reads alone (nv4097.cpp:810-817; LLD-sync-flip
		// §3.2).  Every other release flushes the whole pipe.
		if (kind == release_kind::texture_read && !strict)
		{
			barrier.after_stages = MTL::StageFragment;
		}

		return barrier;
	}
}

#endif // RSX_ENABLE_METAL4
