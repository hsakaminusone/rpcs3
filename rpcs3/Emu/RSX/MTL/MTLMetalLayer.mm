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
		// The drawable is only ever a copy destination, never a sampled source, so
		// the fast path is available (HLD-04 §4.1).
		layer.framebufferOnly = YES;
		layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

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

	// The formats a CAMetalLayer accepts as a drawable.  The set is the one the
	// display pipe can scan out; anything else must be converted before the present.
	bool layer_supports_format(u32 format)
	{
		switch (static_cast<MTLPixelFormat>(format))
		{
		case MTLPixelFormatBGRA8Unorm:
		case MTLPixelFormatBGRA8Unorm_sRGB:
		case MTLPixelFormatRGBA8Unorm:
		case MTLPixelFormatRGBA8Unorm_sRGB:
		case MTLPixelFormatRGBA16Float:
		case MTLPixelFormatBGRA10_XR:
		case MTLPixelFormatBGRA10_XR_sRGB:
			return true;
		default:
			return false;
		}
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
