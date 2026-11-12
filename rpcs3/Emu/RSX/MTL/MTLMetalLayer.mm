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

#include <atomic>
#include <cstdio>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>

#include "MTLMetalLayer.h"


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

	// diagnostic scaffolding for the acquire-stall investigation; remove it with the
	// MTL diagnostics before this backend is proposed for merge.


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

	bool metal_layer_visible(void* view)
	{
		// The window owns the visibility, so a detached view, a hidden view and a
		// window the window server is not compositing all answer false.
		if (!view)
		{
			return false;
		}

		NSView* ns_view = static_cast<NSView*>(view);

		if (ns_view.hiddenOrHasHiddenAncestor)
		{
			return false;
		}

		NSWindow* window = ns_view.window;

		// A miniaturised window still has a layer, but the display takes no drawable
		// from it, which is the case that would wait out the acquire timeout.
		if (!window || !window.visible || window.miniaturized)
		{
			return false;
		}

		// A window the window server reports as not visible is fully covered or off
		// screen, and a drawable it would give is one the display never takes.
		return (window.occlusionState & NSWindowOcclusionStateVisible) != 0;
	}


}

#endif // RSX_ENABLE_METAL4

#pragma GCC diagnostic pop
