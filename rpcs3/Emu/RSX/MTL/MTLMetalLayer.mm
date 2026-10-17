// The AppKit half of the presenter.  This is the only Objective-C++ unit of the
// Metal 4 backend, and it holds no RSX logic: it attaches a CAMetalLayer to the
// Qt NSView and keeps its drawable the size of the client area.
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

#include "MTLMetalLayer.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	void* attach_metal_layer(void* view, void* device)
	{
		if (!view || !device)
		{
			return nullptr;
		}

		NSView* ns_view = static_cast<NSView*>(view);

		CAMetalLayer* layer = [[CAMetalLayer alloc] init];
		layer.device = static_cast<id<MTLDevice>>(device);
		// The drawable is the output pass's render target and is never sampled, so
		// the framebuffer-only fast path stays available (HLD-04 §4.1).
		layer.framebufferOnly = YES;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
		// One drawable per frame slot, so the ring never starves the pool; a vsynced
		// present; and a bounded acquire, because a nil drawable is a dropped frame
		// and not a stall on the RSX thread (HLD-06 §2.2).
		layer.maximumDrawableCount = 3;
		layer.displaySyncEnabled = YES;
		layer.allowsNextDrawableTimeout = YES;

		// The guest output is sRGB-encoded, so the layer carries the sRGB colour
		// space and the display applies no conversion.
		CGColorSpaceRef colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
		layer.colorspace = colorspace;
		CGColorSpaceRelease(colorspace);

		ns_view.wantsLayer = YES;
		ns_view.layer = layer;

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

	void detach_metal_layer(void* view, void* layer)
	{
		if (view)
		{
			NSView* ns_view = static_cast<NSView*>(view);

			if (ns_view.layer == static_cast<CAMetalLayer*>(layer))
			{
				ns_view.layer = nil;
			}
		}

		if (layer)
		{
			[static_cast<CAMetalLayer*>(layer) release];
		}
	}
}

#endif // RSX_ENABLE_METAL4

#pragma GCC diagnostic pop
