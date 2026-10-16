#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	class command_recorder;

	// C7.  Owns the CAMetalLayer and the present path (HLD-03 §4.9, HLD-04 §4.1):
	// acquire a drawable, wait for it, copy the RSX output into it, and hand it to
	// the recorder so the commit and the signal stay on the one submit path.
	//
	// The presenter does not own the output texture.  The frame resolves it and
	// passes it in, so the presenter holds no RSX state.
	class presenter
	{
	public:
		~presenter();

		// `view` is the Qt NSView (GSFrameBase::handle).  False when the view is
		// unusable, which is not fatal: the frame then runs without a present.
		bool init(const device& dev, void* view);
		void shutdown();

		bool valid() const { return m_layer != nullptr; }

		// Present `source` for the frame the recorder has already opened.  False
		// when no drawable is available, which means the frame is dropped.
		bool present(MTL::Texture* source, command_recorder& recorder);

	private:
		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		void* m_view = nullptr;
		void* m_layer = nullptr;
		u32 m_width = 0;
		u32 m_height = 0;
		MTL::PixelFormat m_format{};
	};
}

#endif // RSX_ENABLE_METAL4
