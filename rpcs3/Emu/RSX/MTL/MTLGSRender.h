#pragma once

#include "MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/GSRender.h"

#include <memory>

namespace mtl
{
	class device;

	// The Metal 4 renderer (docs/research/metal4/08 §2.1; 10 §4.7).  This is the
	// rsx::thread spine: it brings up the device, fills the backend contract and
	// owns the frame loop.  The per-cluster work lands in the MTL modules.
	//
	// This header stays free of metal-cpp so the UI can name the renderer:
	// gcm_enums.h:1183 has `using namespace gcm`, which makes Objective-C's
	// `Method` ambiguous in a TU that reaches metal-cpp afterwards.  The backend
	// objects sit behind one opaque type for the same reason, so this header also
	// stays free of the RSX headers the UI must not reach.
	class MTLGSRender : public GSRender
	{
	public:
		explicit MTLGSRender(utils::serial* ar) noexcept;
		~MTLGSRender() override;

		u64 get_cycles() override;
		void on_init_thread() override;
		void on_exit() override;
		void flip(const rsx::display_flip_info_t& info) override;
		void clear_surface(u32 mask) override;

	private:
		// The resource layer, the command recorder, the surface store, the pass the
		// attachments belong to, the built-in fill and the presenter.  Defined in
		// the .cpp.
		struct frame_resources;

		// Bind the surfaces the context names and refresh the scissor.  False when
		// there is no valid configuration to draw into.
		bool bind_framebuffer(rsx::framebuffer_creation_context context);

		// Refresh the cached scissor.  get_scissor() returns false both for an
		// unchanged region and for an invalid one, so the flag decides
		// (RSXThread.cpp:2095).
		bool update_scissor(bool clip_viewport);

		std::unique_ptr<device> m_device;
		std::unique_ptr<frame_resources> m_frame_resources;

		// The current scissor rectangle, in attachment pixels (MTLRasterState.h).
		areau m_scissor;
	};
}

#endif // RSX_ENABLE_METAL4
