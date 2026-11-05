#include "MTLDevice.h"

#include "MTLPresenter.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"
#include "MTLCommandRecorder.h"
#include "MTLMetalLayer.h"

#include "MTLInstrumentation.h"

#include "Emu/system_config.h"

#include "util/logs.hpp"

#include <mach/mach_time.h>

#include <Foundation/NSAutoreleasePool.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace mtl
{
#if MTL_DEBUG_LOGS
	// TODO(excise): level 3; the wall clock the acquire and present traces stamp.
	static f64 debug_now_seconds()
	{
		static const mach_timebase_info_data_t timebase = [] { mach_timebase_info_data_t value{}; mach_timebase_info(&value); return value; }();

		return static_cast<f64>(mach_absolute_time()) * static_cast<f64>(timebase.numer) / static_cast<f64>(timebase.denom) * 1e-9;
	}
#endif // MTL_DEBUG_LOGS
	presenter::~presenter()
	{
		shutdown();
	}

	bool presenter::init(const device& dev, void* view)
	{
		if (!view)
		{
			return false;
		}

		m_device = dev.dev();
		m_queue = dev.queue();
		m_view = view;

		// The view's own CAMetalLayer is the layer the display composites, so it is
		// the one to present with, and Qt owns it.  Only a view without one gets a
		// layer here, and only that layer is this backend's to release.
		m_layer = attach_metal_layer(view, m_device, m_owns_layer);

		if (!m_layer)
		{
			return false;
		}

		// The drawable's backing texture belongs to no resource-manager set, so the
		// layer's own read-only set carries it: a drawable that is not resident can
		// fault the GPU when the output pass writes it, and the fault shows up as a
		// commit that never completes (Apple, presenting metal drawables; KK does the
		// same in wsi_metal_layer_make_queue_resident).
		m_drawables = static_cast<MTL::ResidencySet*>(metal_layer_residency_set(m_layer));

		if (m_drawables)
		{
			m_queue->addResidencySet(m_drawables);
			m_drawables->requestResidency();
		}

		return true;
	}

	void presenter::shutdown()
	{
		// A drawable of a frame that never presented still holds the reference the
		// acquire took.
		if (m_drawable)
		{
			m_drawable->release();
			m_drawable = nullptr;
		}

		if (m_drawables)
		{
			m_queue->removeResidencySet(m_drawables);
			m_drawables = nullptr;
		}

		if (m_layer)
		{
			detach_metal_layer(m_view, m_layer, m_owns_layer);
			m_layer = nullptr;
		}

		m_view = nullptr;
		m_device = nullptr;
		m_queue = nullptr;
	}

	bool presenter::acquire(command_recorder& recorder, u32 width, u32 height, MTL::Texture*& target)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/acquire");
		target = nullptr;

		if (!m_layer || !m_queue || !width || !height)
		{
			return false;
		}

		// The drawable is the client area, so a resize sizes the drawable and the
		// output path scales the RSX image into it (HLD-06 §2.3).
		if (width != m_width || height != m_height)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/acquire/resize");
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/acquire/resize events", 1);
			set_metal_layer_drawable(m_layer, width, height, static_cast<u32>(m_format));
			m_width = width;
			m_height = height;
		}

#if MTL_DEBUG_LOGS
		const f64 debug_start = debug_now_seconds();
#endif
		CA::MetalDrawable* drawable = nullptr;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/acquire/next drawable");

			// The RSX thread runs no autorelease pool, and the framework calls here
			// return retained-until-drained objects, so this bounds them.  The
			// drawable outlives the pool, so the acquire takes its own reference
			// until the present.
			NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

			drawable = static_cast<CA::MetalLayer*>(m_layer)->nextDrawable();

			if (drawable)
			{
				drawable->retain();
			}

			pool->release();
		}

#if MTL_DEBUG_LOGS
		const f64 acquire_ms = (debug_now_seconds() - debug_start) * 1000.0;

		// TODO(excise): level 3; the acquire timing.
		MTL_NOTE(3, "Metal 4: acquire at %.3f took %.3f ms, drawable %p", debug_start, acquire_ms, static_cast<void*>(drawable));

		// TODO(excise): level 4; the acquire-stall probe.  A stall here is the
		// drawable pool pacing the RSX thread.  The bounded probe reports the window
		// state it happened in, so an occlusion is told apart from a pool that ran
		// dry.
		if (::mtl::probe::at_least(4))
		{
			u64 in_flight = 0;
#if MTL_PERF_ENABLED
			in_flight = ::mtl::perf::drawables_in_flight();
#endif
			metal_layer_log_stall(m_view, m_layer, acquire_ms, in_flight, m_present_gap_ms);
			metal_layer_log_state(m_view, m_layer);
		}
#endif // MTL_DEBUG_LOGS

		if (!drawable)
		{
			// The drawable pool is empty.  The frame is dropped, not presented late.
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/acquire/dropped", 1);
			return false;
		}

		// TODO(excise): level 1; drawable acquired.
		MTL_PERF_NOTE_ACQUIRED(1);
		// TODO(excise): level 1; instantaneous value.
		MTL_PERF_GAUGE(1, "present/drawables in flight", ::mtl::perf::drawables_in_flight());

#if MTL_DIAGNOSTICS
		// TODO(excise): level 1; installs the presented callback of this drawable.
		if (::mtl::probe::at_least(1))
		{
			metal_layer_watch_drawable(drawable);
		}
#endif // MTL_DIAGNOSTICS

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/acquire/set drawable");

			// The recorder waits for the drawable before it encodes and signals it
			// after the commit, so the present never leaves the submit path
			// (HLD-03 §4.9).
			recorder.set_present_drawable(drawable);
		}

		m_drawable = drawable;
		target = drawable->texture();

		if (!target)
		{
			// The drawable carries no texture, so the frame cannot draw into it.  The
			// drawable is still this frame's, and a drawable the queue signalled that
			// never reaches the display leaves the pool for good, so the frame presents
			// it empty.
			return false;
		}

		return true;
	}

	void presenter::update_vsync()
	{
		const vsync_mode mode = g_cfg.video.vsync;
		const bool sync = (mode == vsync_mode::full);

		// Metal has one display-sync state, so only a full sync is a true vsync.
		// The other two modes present as soon as the drawable is ready, and adaptive
		// paces the present to one refresh interval, which is the closest available
		// behaviour to a sync that yields when a frame is late (HLD-06 §2.8).
		m_present_interval = (mode == vsync_mode::adaptive) ? metal_layer_refresh_interval(m_view) : 0.0;

		if (m_sync_applied && sync == m_sync_enabled)
		{
			return;
		}

		set_metal_layer_sync(m_layer, sync);
		m_sync_enabled = sync;
		m_sync_applied = true;
	}

	void presenter::present(command_recorder& recorder)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/display");
		MTL::Drawable* drawable = m_drawable;
		m_drawable = nullptr;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/display/submit");
			recorder.submit();
		}

		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: present at %.3f, drawable %p, paced %d", debug_now_seconds(), static_cast<void*>(drawable), m_present_interval > 0.0 ? 1 : 0);

#if MTL_DIAGNOSTICS
		// The measured interval between presents, sampled here so the next acquire
		// reports a real age and the cadence histogram sees a real value.  It is zero
		// until the second present of the run.
		{
			const u64 now_ns = ::mtl::probe::now_ns();

			if (m_last_present_ns)
			{
				m_present_gap_ms = static_cast<f64>(now_ns - m_last_present_ns) * 1e-6;
			}

			m_last_present_ns = now_ns;
		}

		// TODO(excise): level 2; the present cadence (area D).  The bucket is 0 up to
		// 8 ms, 1 to 17 ms, 2 to 34 ms, 3 to 67 ms and 4 above.
		MTL_PROBE_HISTOGRAM(2, "present cadence",
			m_present_gap_ms <= 8.0 ? 0 : m_present_gap_ms <= 17.0 ? 1 : m_present_gap_ms <= 34.0 ? 2 : m_present_gap_ms <= 67.0 ? 3 : 4,
			"present gap bucket");
#endif // MTL_DIAGNOSTICS

		if (!drawable)
		{
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/display/dropped", 1);
		}

		// The present is a framework call on the RSX thread, which runs no pool.
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		if (drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/display/present call");

#if MTL_DIAGNOSTICS
			// TODO(excise): level 4; stamps the drawable before the present.
			if (::mtl::probe::at_least(4))
			{
				metal_layer_note_present(drawable);
			}
#endif // MTL_DIAGNOSTICS

			if (m_present_interval > 0.0)
			{
				// The paced present of the adaptive mode.  The layer carries no
				// display sync in that mode, so this is what bounds the rate.
				drawable->presentAfterMinimumDuration(m_present_interval);
			}
			else
			{
				drawable->present();
			}

			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/presented", 1);
		}

		pool->release();

		// The present has been handed over, so the acquire's reference goes.
		if (drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/display/release");
			drawable->release();
		}
	}
}

#endif // RSX_ENABLE_METAL4
