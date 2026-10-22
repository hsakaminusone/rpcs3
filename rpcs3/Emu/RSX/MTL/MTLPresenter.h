#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	class command_recorder;

	// C7.  Owns the present of the Metal 4 backend (HLD-03 §4.9, HLD-04 §4.1,
	// HLD-06 §2.1): acquire a drawable, hand its texture to the caller as the render
	// target, and present it after the one submit path commits.  The layer belongs to
	// the view's owner when the view already carries one, so the presenter keeps only
	// the drawable and the residency set it needs.
	//
	// The presenter does not own the frame's output.  The output passes draw into
	// the drawable the presenter returns, so the presenter holds no RSX state.
	class presenter
	{
	public:
		~presenter();

		// `view` is the Qt NSView (GSFrameBase::handle).  False when the view is
		// unusable, which is not fatal: the frame then runs without a present.
		bool init(const device& dev, void* view);
		void shutdown();

		bool valid() const { return m_layer != nullptr; }

		// Acquire the frame's drawable, sized to the client area, and return its
		// texture as the render target.  False when no drawable is available, which
		// means the frame is dropped.
		bool acquire(command_recorder& recorder, u32 width, u32 height, MTL::Texture*& target);

		// Submit the frame and present the acquired drawable.  This is the only way a
		// frame ends, because a drawable the queue signalled must reach the display or
		// the layer keeps it for good, so every acquired drawable is presented.
		void present(command_recorder& recorder);

		// Apply the vsync setting to the layer and to the present.  Metal has no swap
		// interval and the setting changes at run time, so it is re-read on an emu
		// flip (VKPresent.cpp:438, GLPresent.cpp:256).
		void update_vsync();

	private:
		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		void* m_view = nullptr;
		void* m_layer = nullptr;
		// True when this backend attached the layer, which is the case only for a view
		// that Qt gave no layer.  A layer Qt owns stays with the view.
		bool m_owns_layer = false;
		// The layer's residency set, which carries the drawable textures.  It is
		// attached to the queue so a drawable is resident before the output pass
		// writes it.
		MTL::ResidencySet* m_drawables = nullptr;
		MTL::Drawable* m_drawable = nullptr;
		u32 m_width = 0;
		u32 m_height = 0;
		MTL::PixelFormat m_format = MTL::PixelFormatBGRA8Unorm;

		// The display-sync state the layer carries, and the interval the present
		// paces to.  The interval is non-zero only for a mode that paces.
		bool m_sync_enabled = true;
		bool m_sync_applied = false;
		f64 m_present_interval = 0.0;

		// The measured interval between the last two presents, and the stamp of the
		// last present.  The acquire-stall record reports the measured interval: the
		// pacing interval above is zero in most modes, so it cannot carry that fact.
		// Both are diagnostic; a build without diagnostics compiles them out of use.
		[[maybe_unused]] f64 m_present_gap_ms = 0.0;
		[[maybe_unused]] u64 m_last_present_ns = 0;
	};
}

#endif // RSX_ENABLE_METAL4
