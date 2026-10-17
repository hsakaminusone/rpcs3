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
		void begin() override;
		void end() override;
		void emit_geometry(u32 sub_index) override;
		void flip(const rsx::display_flip_info_t& info) override;
		void clear_surface(u32 mask) override;

		// The sync hooks (HLD-04 §4).  The guest can wait on a label that only the
		// GPU can release, so a mid-frame wait must submit; the texture cache owns
		// the invalidation of the guest ranges the CPU writes.
		void on_semaphore_acquire_wait() override;
		void write_barrier(u32 memory_address, u32 memory_range) override;
		bool on_access_violation(u32 address, bool is_writing) override;
		void on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause) override;
		void notify_tile_unbound(u32 tile) override;

	private:
		// The resource layer, the command recorder, the surface store, the pass the
		// attachments belong to, the built-in fill, the presenter and the draw path.
		// Defined in the .cpp.
		struct frame_resources;

		// Open the frame's command buffer on the next ring slot.  Called once per
		// frame, before the first pass, because the ring's slot is only reusable
		// after its previous frame has completed (HLD-04 §3.2).
		void open_frame();

		// Bind the surfaces the context names and refresh the scissor.  False when
		// there is no valid configuration to draw into.
		bool bind_framebuffer(rsx::framebuffer_creation_context context);
		// Refresh the cached scissor.  get_scissor() returns false both for an
		// unchanged region and for an invalid one, so the flag decides
		// (RSXThread.cpp:2095).
		bool update_scissor(bool clip_viewport);

		// Resolve the referenced texture units of the open clause and record the
		// Metal image and sampler of each.  It runs before the programs are built,
		// because the builders read the resolved descriptors (RSXThread.cpp:2488).
		void load_texture_env();

		std::unique_ptr<device> m_device;
		std::unique_ptr<frame_resources> m_frame_resources;

		// The current scissor rectangle, in attachment pixels (MTLRasterState.h).
		areau m_scissor;

		// The frame's command buffer is open.  A frame opens lazily on its first
		// pass and closes at the flip, which submits it.
		bool m_frame_open = false;
	};
}

#endif // RSX_ENABLE_METAL4
