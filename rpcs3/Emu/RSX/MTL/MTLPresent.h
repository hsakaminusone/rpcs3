#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "../Common/simple_array.hpp"
#include "MTLOverlays.h"

#include <array>

class GSFrameBase;

namespace rsx::overlays
{
	class display_manager;
}

namespace mtl
{
	class command_recorder;
	class constants_heap;
	class resource_manager;

	// The frame's RSX output (VKPresent.cpp:544).  `image2` is the second eye of a
	// stereo frame and stays null otherwise.
	struct present_source
	{
		MTL::Texture* image = nullptr;
		MTL::Texture* image2 = nullptr;
		u32 width = 0;
		u32 height = 0;
	};

	// The output tier of the present (HLD-06 §2.4).  It owns the scale, calibration
	// and UI passes, plus the frame capture, and it holds no RSX state: the flip
	// gives it the source and the two rectangles.  A frame is one render pass on the
	// drawable, so every output draw encodes into the pass the caller opened.
	class frame_output
	{
	public:
		bool init(const device& dev, resource_manager& resources);
		void shutdown();

		// Compose `source` into the open pass.  `aspect` is the source's aspect-fit
		// rectangle inside the drawable and `display_area` is the client area the
		// overlays lay out in (VKPresent.cpp:674).
		void compose(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, MTL::Texture* target,
			const present_source& source, const areau& aspect, const areau& display_area,
			f32 gamma, bool limited_range, bool stereo, rsx::overlays::display_manager* overlays);

		// Encode the readback of `source` into the current frame.  Metal 4 does not
		// stop the CPU at the encode, so the bytes are delivered once the slot that
		// wrote them has completed (HLD-06 §2.7).
		void request_capture(command_recorder& recorder, constants_heap& constants,
			const present_source& source, bool screenshot, bool with_overlays,
			rsx::overlays::display_manager* overlays);

		// The slot's previous frame has completed, so its capture is readable.
		void on_slot_complete(u32 slot);

		// Hand the slot's completed capture to the frame.  False when it holds none.
		bool deliver(u32 slot, GSFrameBase& frame);

	private:
		// One pending capture per frame slot, so a readback never races the frame
		// that wrote it and a screenshot costs the RSX thread no GPU wait.
		struct capture
		{
			MTL::Buffer* buffer = nullptr;
			u32 width = 0;
			u32 height = 0;
			bool is_bgra = false;
			bool screenshot = false;
			bool ready = false;
		};

		// The image a capture with overlays composites into before the readback.
		// It is a member because it survives the frame that made it.
		MTL::Texture* m_capture_target = nullptr;

		resource_manager* m_resources = nullptr;

		std::array<capture, frame_slot_count> m_captures{};

		present_scale_pass m_scale;
		present_scale_pass m_nearest;
		video_out_calibration_pass m_calibration;
		ui_overlay_renderer m_ui;
	};
}

#endif // RSX_ENABLE_METAL4
