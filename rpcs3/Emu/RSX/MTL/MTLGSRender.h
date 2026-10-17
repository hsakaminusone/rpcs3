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
	// `Method` ambiguous in a TU that reaches metal-cpp afterwards.
	class MTLGSRender : public GSRender
	{
	public:
		explicit MTLGSRender(utils::serial* ar) noexcept;
		~MTLGSRender() override;

		u64 get_cycles() override;
		void on_init_thread() override;
		void on_exit() override;

	private:
		std::unique_ptr<device> m_device;
	};
}

#endif // RSX_ENABLE_METAL4
