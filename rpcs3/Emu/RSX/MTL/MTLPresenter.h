#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	class command_recorder;

	// C7.  Owns the CAMetalLayer and the drawable of the present (HLD-03 §4.9,
	// HLD-04 §4.1, HLD-06 §2.1): acquire a drawable, hand its texture to the caller
	// as the render target, and present it after the one submit path commits.
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

		// Submit the frame and present the acquired drawable.
		void present(command_recorder& recorder);

		// Give up the acquired drawable without presenting it.  The frame is still
		// submitted, because the ring must advance.
		void drop() { m_drawable = nullptr; }

	private:
		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		void* m_view = nullptr;
		void* m_layer = nullptr;
		MTL::Drawable* m_drawable = nullptr;
		u32 m_width = 0;
		u32 m_height = 0;
		MTL::PixelFormat m_format = MTL::PixelFormatBGRA8Unorm;
	};
}

#endif // RSX_ENABLE_METAL4
