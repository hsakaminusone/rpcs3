#pragma once

#include "util/types.hpp"
#include "MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

// The AppKit half of the presenter (HLD-04 §4.1).  metal-cpp carries no AppKit, so
// attaching a CAMetalLayer to the Qt NSView needs a .mm.  The interface stays a
// plain C boundary so this header carries no Metal and no Objective-C, and the
// renderer header can name the presenter without reaching either.
namespace mtl
{
	// Attach a CAMetalLayer to `view` and return it.  Null when the view is unusable.
	// The drawable size and format are set by the first present, from the source.
	void* attach_metal_layer(void* view, void* device);

	// Set the drawable size in pixels and its pixel format.  The layer scales the
	// drawable to the view, so the drawable carries the source resolution and the
	// present needs no scale pass.  `format` is an MTL::PixelFormat.
	void set_metal_layer_drawable(void* layer, u32 width, u32 height, u32 format);

	// True when the layer can carry `format` as a drawable.  A CAMetalLayer only
	// accepts the drawable formats the display pipe can scan out.
	bool layer_supports_format(u32 format);

	// Detach and release the layer from `view`.
	void detach_metal_layer(void* view, void* layer);
}

#endif // RSX_ENABLE_METAL4
