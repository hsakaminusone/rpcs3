// The AppKit half of the presenter.  This is the only Objective-C++ unit of the
// Metal 4 backend, and it holds no RSX logic: it takes the CAMetalLayer of the Qt
// NSView, or attaches one when the view carries none, and it keeps the drawable the
// size of the client area.
//
// The project builds this file with -Werror, and the AppKit and QuartzCore
// headers are not ours, so the diagnostics are neutralised locally (the
// VK/vkutils/metal_layer.mm idiom).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>

#include "MTLMetalLayer.h"

#include "MTLInstrumentation.h"

#include "util/logs.hpp"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	// The CAMetalLayer a view presents with.  Qt keeps the layer it gave the window
	// as the view's content layer, and it wraps that layer in a container layer
	// (Qt, qnsview_drawing.mm, setLayer), so the layer is not always the view's own.
	static CAMetalLayer* find_metal_layer(NSView* ns_view)
	{
		CALayer* layer = ns_view.layer;

		if ([layer isKindOfClass:[CAMetalLayer class]])
		{
			return static_cast<CAMetalLayer*>(layer);
		}

		for (CALayer* sublayer in layer.sublayers)
		{
			if ([sublayer isKindOfClass:[CAMetalLayer class]])
			{
				return static_cast<CAMetalLayer*>(sublayer);
			}
		}

		return nil;
	}

	// TODO(excise): everything from here to metal_layer_watch_drawable is temporary
	// diagnostic scaffolding for the acquire-stall investigation; remove it with the
	// MTL diagnostics before this backend is proposed for merge.

#if MTL_DEBUG_LOGS
	// A heartbeat on the main run loop.  It fires only while the main thread is idle,
	// so its lateness is the time the emulator's own GUI work held the main thread,
	// which is a candidate for a stall and for an input that arrives late.
	// TODO(excise): level 4; the main-thread heartbeat.
	static std::atomic<u64> g_main_tick_ns{0};

	static void start_main_heartbeat()
	{
		static std::once_flag once;

		std::call_once(once, []
		{
			dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
			dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), 2ull * NSEC_PER_MSEC, 1ull * NSEC_PER_MSEC);
			dispatch_source_set_event_handler(timer, ^{ g_main_tick_ns.store(probe::now_ns(), std::memory_order_relaxed); });
			dispatch_resume(timer);
		});
	}

	static f64 main_thread_gap_ms()
	{
		const u64 tick = g_main_tick_ns.load(std::memory_order_relaxed);

		// A negative value is a "no sample yet" token, not an age.  The record prints
		// a word for it, so it never reaches the log as a negative age.
		return tick ? static_cast<f64>(probe::now_ns() - tick) * 1e-6 : -1.0;
	}
#endif // MTL_DEBUG_LOGS

#if MTL_DIAGNOSTICS
	// The time the present call left and the time the display showed the drawable,
	// kept per drawable identifier.  The difference is how long the display server
	// held the drawable the acquire waits on.
	// TODO(excise): level 4; the display-side present stamp.
	static std::atomic<u64> g_present_ns[8];
	static std::atomic<u64> g_displayed_ns{0};
#endif // MTL_DIAGNOSTICS

	void* attach_metal_layer(void* view, void* device, bool& owned)
	{
		owned = false;

		if (!view || !device)
		{
			return nullptr;
		}

		NSView* ns_view = static_cast<NSView*>(view);

		// The layer Qt gave the window is the one the display composites, so it is the
		// one to configure and present with.  Only a view without one gets a layer
		// from here, and only that layer is this backend's to release.
		CAMetalLayer* layer = find_metal_layer(ns_view);

		if (!layer)
		{
			layer = [[CAMetalLayer alloc] init];
			ns_view.wantsLayer = YES;
			ns_view.layer = layer;
			owned = true;
		}

		layer.device = static_cast<id<MTLDevice>>(device);
		// The drawable is the output pass's render target and is never sampled, so
		// the framebuffer-only fast path stays available (HLD-04 §4.1).
		layer.framebufferOnly = YES;
		// The guest output is opaque and fills the whole drawable, and an opaque layer
		// is what the direct-to-display path requires (Apple, presenting metal
		// drawables); a non-opaque layer is always composited by the window server.
		layer.opaque = YES;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		// One drawable per frame slot, so the ring never starves the pool; a vsynced
		// present; and a bounded acquire, because a nil drawable is a dropped frame
		// and not a stall on the RSX thread (HLD-06 §2.2).
		layer.maximumDrawableCount = 3;
		layer.displaySyncEnabled = YES;
		layer.allowsNextDrawableTimeout = YES;

		// The guest output is sRGB-encoded, so the layer carries the sRGB colour
		// space and the display applies no conversion.  Qt sets this same colour space
		// on its display updates, from the view's colour space, and both agree.
		CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		layer.colorspace = colorspace;
		CGColorSpaceRelease(colorspace);

#if MTL_DEBUG_LOGS
		// TODO(excise): level 4; starts the main-thread heartbeat.
		if (::mtl::probe::at_least(4))
		{
			start_main_heartbeat();
		}
#endif // MTL_DEBUG_LOGS

		return layer;
	}

	void set_metal_layer_drawable(void* layer, u32 width, u32 height, u32 format)
	{
		if (!layer || !width || !height)
		{
			return;
		}

		CAMetalLayer* metal_layer = static_cast<CAMetalLayer*>(layer);
		metal_layer.pixelFormat = static_cast<MTLPixelFormat>(format);
		metal_layer.drawableSize = CGSizeMake(width, height);
	}

	void set_metal_layer_sync(void* layer, bool enabled)
	{
		if (!layer)
		{
			return;
		}

		static_cast<CAMetalLayer*>(layer).displaySyncEnabled = enabled;
	}

	f64 metal_layer_refresh_interval(void* view)
	{
		constexpr f64 default_interval = 1.0 / 60.0;

		if (!view)
		{
			return default_interval;
		}

		NSScreen* screen = static_cast<NSView*>(view).window.screen;

		if (!screen)
		{
			return default_interval;
		}

		const NSInteger rate = screen.maximumFramesPerSecond;

		return rate > 0 ? 1.0 / static_cast<f64>(rate) : default_interval;
	}

	void detach_metal_layer(void* view, void* layer, bool owned)
	{
		if (!layer)
		{
			return;
		}

		// The layer Qt gave the window belongs to Qt, which tears it down with the
		// window, so only a layer from attach_metal_layer leaves here.
		if (!owned)
		{
			return;
		}

		if (view)
		{
			static_cast<NSView*>(view).layer = nil;
		}

		[static_cast<CAMetalLayer*>(layer) release];
	}

	void* metal_layer_residency_set(void* layer)
	{
		if (!layer)
		{
			return nullptr;
		}

		// The layer keeps its residency set, which carries the drawable textures, only
		// on macOS 26 and later.
		if (@available(macOS 26.0, *))
		{
			return static_cast<void*>(static_cast<CAMetalLayer*>(layer).residencySet);
		}

		return nullptr;
	}

#if MTL_DEBUG_LOGS
	// TODO(excise): level 4; the layer-state walk of one stall.
	void metal_layer_log_state(void* view, void* layer)
	{
		if (!view)
		{
			return;
		}

		NSView* ns_view = static_cast<NSView*>(view);
		NSWindow* window = ns_view.window;
		CAMetalLayer* metal_layer = static_cast<CAMetalLayer*>(layer);
		CAMetalLayer* content_layer = find_metal_layer(ns_view);

		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: view hidden %d, window %p visible %d occluded %d, layer %.0fx%.0f drawable %.0fx%.0f max %u",
			static_cast<int>(ns_view.hiddenOrHasHiddenAncestor),
			static_cast<void*>(window),
			window ? static_cast<int>(window.visible) : -1,
			window ? static_cast<int>((window.occlusionState & NSWindowOcclusionStateVisible) == 0) : -1,
			metal_layer ? metal_layer.bounds.size.width : 0.0,
			metal_layer ? metal_layer.bounds.size.height : 0.0,
			metal_layer ? metal_layer.drawableSize.width : 0.0,
			metal_layer ? metal_layer.drawableSize.height : 0.0,
			metal_layer ? static_cast<unsigned>(metal_layer.maximumDrawableCount) : 0u);

		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: layer %p (%s), view layer %p (%s), view content layer %p, is content layer %d, delegate %s, transaction %d",
			static_cast<void*>(metal_layer), object_getClassName(static_cast<id>(metal_layer)),
			static_cast<void*>(ns_view.layer), object_getClassName(ns_view.layer),
			static_cast<void*>(content_layer), static_cast<int>(content_layer == metal_layer),
			object_getClassName(static_cast<id>(metal_layer ? metal_layer.delegate : nil)),
			static_cast<int>(metal_layer ? metal_layer.presentsWithTransaction : -1));
	}
#endif // MTL_DEBUG_LOGS

#if MTL_DIAGNOSTICS
	// TODO(excise): level 4; the display-side present stamp.
	void metal_layer_note_present(void* drawable)
	{
		if (!drawable)
		{
			return;
		}

		const u64 identifier = static_cast<id<MTLDrawable>>(drawable).drawableID & 7;

		g_present_ns[identifier].store(probe::now_ns(), std::memory_order_relaxed);
	}

	// TODO(excise): level 4; the display-side stamp, and the level-1 present bookkeeping.
	void metal_layer_note_displayed(void* drawable)
	{
		if (!drawable)
		{
			return;
		}

		// TODO(excise): level 1; drawable presented.
		MTL_PERF_NOTE_PRESENTED(1);

		if (!::mtl::probe::at_least(4))
		{
			return;
		}

		const u64 now = probe::now_ns();
		const u64 previous = g_displayed_ns.exchange(now, std::memory_order_relaxed);

#if MTL_PERF_ENABLED
		const u64 identifier = static_cast<id<MTLDrawable>>(drawable).drawableID & 7;
		const u64 start = g_present_ns[identifier].load(std::memory_order_relaxed);

		if (start)
		{
			// TODO(excise): level 4; instantaneous value.
			MTL_PERF_GAUGE(4, "present/displayed latency", (now - start) / 1000);
		}

		// TODO(excise): temporary diagnostic; the gap is the display cadence itself,
		// so a gap far above the refresh interval is the display server and not the
		// emulator, which is what separates the two causes of a drained drawable pool.
		if (previous)
		{
			// TODO(excise): level 4; instantaneous value.
			MTL_PERF_GAUGE(4, "present/display gap", (now - previous) / 1000);
		}

		// TODO(excise): level 4; running total.
		MTL_PERF_COUNT(4, "present/displayed", 1);
#else
		(void)previous;
#endif // MTL_PERF_ENABLED
	}
#endif // MTL_DIAGNOSTICS

#if MTL_DEBUG_LOGS
	// TODO(excise): level 4; the acquire-stall probe.
	void metal_layer_log_stall(void* view, void* layer, f64 elapsed_ms, u64 drawables_in_flight, f64 present_gap_ms)
	{
		if (elapsed_ms < probe::stall_ms())
		{
			return;
		}

		static std::atomic<u32> reported{0};
		const u32 index = reported.fetch_add(1, std::memory_order_relaxed);

		if (index >= 2000)
		{
			return;
		}

		NSView* ns_view = view ? static_cast<NSView*>(view) : nil;
		NSWindow* window = ns_view.window;
		CAMetalLayer* metal_layer = static_cast<CAMetalLayer*>(layer);

		const u64 displayed = g_displayed_ns.load(std::memory_order_relaxed);
		const f64 main_gap_ms = main_thread_gap_ms();

		// TODO(excise): level 2; the stall, its magnitude, and the window state it
		// happened in (area O hang/stall sampler, area L frontend surface).
		MTL_PROBE_LATCH(2, "stall/acquire reported");
		MTL_PROBE_LATCH(2, window && (window.occlusionState & NSWindowOcclusionStateVisible) == 0
			? "window/occluded" : "window/visible");
		MTL_PROBE_HISTOGRAM(2, "stall duration",
			elapsed_ms <= 32.0 ? 0 : elapsed_ms <= 64.0 ? 1 : elapsed_ms <= 128.0 ? 2 : 3, "stall ms bucket");

		// Both ages carry a token when they have no value yet, so a sentinel never
		// reaches the log as a negative age (Rule: two defects in the level-1 probe).
		char present_gap[32] = {};
		char since_display[32] = {};
		char main_gap[32] = {};

		if (present_gap_ms > 0.0)
		{
			std::snprintf(present_gap, sizeof(present_gap), "%.1f ms", present_gap_ms);
		}
		else
		{
			std::snprintf(present_gap, sizeof(present_gap), "none");
		}

		if (displayed)
		{
			std::snprintf(since_display, sizeof(since_display), "%.1f ms",
				static_cast<f64>(probe::now_ns() - displayed) * 1e-6);
		}
		else
		{
			std::snprintf(since_display, sizeof(since_display), "none");
		}

		if (main_gap_ms >= 0.0)
		{
			std::snprintf(main_gap, sizeof(main_gap), "%.1f ms", main_gap_ms);
		}
		else
		{
			std::snprintf(main_gap, sizeof(main_gap), "none");
		}

		mtl_log.notice("Metal 4: acquire stall %u, %.1f ms, drawables in flight %llu, present gap %s, main gap %s, since display %s, view hidden %d, window visible %d occluded %d, bounds %.0fx%.0f drawable %.0fx%.0f max %u sync %d",
			index, elapsed_ms, static_cast<unsigned long long>(drawables_in_flight), present_gap,
			main_gap, since_display,
			static_cast<int>(ns_view ? ns_view.hiddenOrHasHiddenAncestor : 1),
			window ? static_cast<int>(window.visible) : -1,
			window ? static_cast<int>((window.occlusionState & NSWindowOcclusionStateVisible) == 0) : -1,
			metal_layer ? metal_layer.bounds.size.width : 0.0,
			metal_layer ? metal_layer.bounds.size.height : 0.0,
			metal_layer ? metal_layer.drawableSize.width : 0.0,
			metal_layer ? metal_layer.drawableSize.height : 0.0,
			metal_layer ? static_cast<unsigned>(metal_layer.maximumDrawableCount) : 0u,
			metal_layer ? static_cast<int>(metal_layer.displaySyncEnabled) : -1);
	}
#endif // MTL_DEBUG_LOGS

#if MTL_DIAGNOSTICS
	// TODO(excise): level 4; installs the presented callback of one drawable.
	void metal_layer_watch_drawable(void* drawable)
	{
		if (!drawable)
		{
			return;
		}

		static const MTLDrawablePresentedHandler handler = ^(id<MTLDrawable> presented) {
			metal_layer_note_displayed(static_cast<void*>(presented));
			// TODO(excise): level 4; deep-probe line.
			MTL_NOTE(4, "Metal 4: display showed drawable %p", static_cast<void*>(presented));
		};

		[static_cast<id<MTLDrawable>>(drawable) addPresentedHandler:handler];
	}
#endif // MTL_DIAGNOSTICS
}

#endif // RSX_ENABLE_METAL4

#pragma GCC diagnostic pop
