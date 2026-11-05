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
	// The CAMetalLayer this backend presents with, taken from `view` or attached to
	// it.  Qt gives a window of a Metal surface type a CAMetalLayer and keeps it as
	// the view's content layer, possibly below a container layer (Qt,
	// qnsview_drawing.mm, shouldUseMetalLayer).  That layer is the one the display
	// composites, so this backend adopts it and never puts a second layer on the
	// view.  A view with no such layer gets one here, and then this backend owns it,
	// which the out parameter reports.  Null when the view is unusable.  The drawable
	// size and format are set by the first present, from the client area.
	void* attach_metal_layer(void* view, void* device, bool& owned);

	// Set the drawable size in pixels and its pixel format.  The size is the client
	// area, so the output path scales the RSX image into the drawable.  `format` is
	// an MTL::PixelFormat.
	void set_metal_layer_drawable(void* layer, u32 width, u32 height, u32 format);

	// Gate the present on the display's refresh.  A metal layer has one sync state,
	// so this is the whole of vsync (HLD-06 §2.8).
	void set_metal_layer_sync(void* layer, bool enabled);

	// One screen refresh in seconds, which the adaptive present paces to.  Falls
	// back to 1/60 when the view has no screen.  AppKit carries this value and
	// metal-cpp does not, so it is read here (HLD-06 §2.8).
	f64 metal_layer_refresh_interval(void* view);

	// Detach `layer` from `view`.  Only a layer this backend created (`owned`) is
	// released, because a layer Qt owns is torn down with the window.
	void detach_metal_layer(void* view, void* layer, bool owned);

	// The layer's read-only residency set, which carries the drawable textures, as an
	// MTL::ResidencySet*.  The drawables are not in the resource manager's sets, so
	// this one must join the queue (Apple, presenting metal drawables).
	void* metal_layer_residency_set(void* layer);

	// TODO(excise): temporary diagnostic declaration; remove with the MTL diagnostics.
	// The layer-state walk of one acquire stall.  It runs at level 4.
	void metal_layer_log_state(void* view, void* layer);

	// TODO(excise): temporary diagnostic declaration; remove with the MTL diagnostics.
	// A bounded diagnostic for an acquire that stalls, which reports the window and
	// layer state the stall happened in.  It runs at level 4.
	// `drawables_in_flight` is the pool depth the acquire waited on, and
	// `present_gap_ms` is the measured interval between the last two presents.
	void metal_layer_log_stall(void* view, void* layer, f64 elapsed_ms, u64 drawables_in_flight, f64 present_gap_ms);

	// TODO(excise): temporary diagnostic declaration; remove with the MTL diagnostics.
	// Stamps the moment a drawable is handed to the display.  It pairs with the
	// framework's presented callback, which is where the time the display server
	// held the drawable comes from.
	void metal_layer_note_present(void* drawable);

	// TODO(excise): temporary diagnostic declaration; remove with the MTL diagnostics.
	// Installs the framework's presented callback on a drawable.
	void metal_layer_watch_drawable(void* drawable);
}

#endif // RSX_ENABLE_METAL4
