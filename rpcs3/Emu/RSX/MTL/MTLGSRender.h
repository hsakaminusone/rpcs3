#pragma once

#include "MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Core/RSXRenderBackend.h"

#include <memory>

// The present source and the CPU-upload scratch images are the only metal-cpp
// types this header names, and it must not reach metal-cpp itself: the UI
// includes it before any metal-cpp include, and gcm_enums.h:1183's
// `using namespace gcm` would then make Objective-C's `Method` ambiguous.
namespace MTL
{
	class Texture;
}

namespace mtl
{
	class device;

	// The Metal 4 render backend (docs/research/metal4/08 §2.1; 10 §4.7).  The
	// core owns the frame and draw lifecycle and hands this backend decoded state;
	// the per-cluster work lands in the MTL modules.
	//
	// This header stays free of metal-cpp so the UI can name the renderer:
	// gcm_enums.h:1183 has `using namespace gcm`, which makes Objective-C's
	// `Method` ambiguous in a TU that reaches metal-cpp afterwards.  The backend
	// objects sit behind one opaque type for the same reason.
	class MTLGSRender final : public rsx::render_backend
	{
	public:
		MTLGSRender(rsx::context* ctx, rsx::display_output* display) noexcept;
		~MTLGSRender() override;

		bool is_current_program_interpreted() const override;

		// Device name, in the same shape as Vulkan's "Vulkan | <gpu>".
		std::string logical_id() const override;
		std::string configured_logical_id() const override;

		// The sync hooks (HLD-04 §4).  A release this backend still has in flight
		// must complete before a semaphore acquire reads the guest word.
		void write_barrier(u32 memory_address, u32 memory_range) override;
		bool release_GCM_label(u32 type, u32 address, u32 data) override;
		bool sync_label(u32 address) override;

		// The FIFO desync capture (RR7-FREEZE-FORENSICS.md §8.1).  The shared
		// FIFO gathers the data and calls this; the backend prints it.

	protected:
		void on_init() override;
		void on_shutdown() override;
		rsx::device_status check_device_status() const override;

		std::unique_ptr<rsx::context_resources> create_context_resources(rsx::context* ctx) override;
		void on_release_context_resources(rsx::context* ctx) override;
		void on_bind_context(rsx::context* ctx) override;
		void on_unbind_context(rsx::context* ctx) override;

		bool on_framebuffer_bound(const rsx::framebuffer_layout& layout) override;
		bool is_framebuffer_bound() const override;

		void begin_draw() override;
		bool end_draw() override;
		void clear() override;
		void emit_geometry(u32 sub_index) override;
		bool resolve_sampler_state(rsx::flags32_t& invalidated_programs) override;

		bool present_frame(const rsx::display_flip_info_t& info) override;
		void post_present() override;

		bool on_access_violation(rsx::context* ctx, u32 address, bool is_writing) override;
		void on_invalidate_memory_range(rsx::context* ctx, const utils::address_range32& range, rsx::invalidation_cause cause) override;
		void on_unmap_memory_range(const utils::address_range32& range) override;

		void notify_tile_unbound(u32 tile) override;
		bool scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate) override;
		void on_semaphore_acquire_wait() override;
		bool on_local_task(rsx::FIFO::state state) override;

		rsx::reports::occlusion_query_provider* occlusion_queries() override;
		void patch_transform_constants(rsx::context* ctx, u32 index, u32 count) override;

	private:
		// The resource layer, the command recorder, the surface store, the pass the
		// attachments belong to, the built-in fill, the presenter and the draw path.
		// Defined in the .cpp.
		struct frame_resources;

		// The deferred flush queue.  It is not part of the frame resources, because
		// a request outlives the frame that is open when it arrives.  Defined in
		// the .cpp, so the header stays free of the RSX texture-cache headers.
		struct flush_queue;

		// Open the frame's command buffer on the next ring slot.  Called once per
		// frame, before the first pass, because the ring's slot is only reusable
		// after its previous frame has completed (HLD-04 §3.2).
		void open_frame();

		// Resolve the image that carries the frame's RSX output.  The surface store
		// answers first, then the texture cache, then a CPU upload
		// (GLPresent.cpp:57-139).  `width`/`height` come back as the image's extent.
		void get_present_source(u32 address, u32& width, u32& height, u32 pitch, u32 format, u8 eye,
			MTL::Texture*& image);

		// Write the bound surfaces into the context's layout identity and tell the
		// caches which ranges the surface store now owns.
		void reconcile_render_targets();

		// Mark every texture unit dirty so the next draw re-resolves it.
		void invalidate_samplers();

		// Record the open pass's hazard set and emit the barriers it needs.  The
		// attachment set is the pass's write and the resolved units its read, so a
		// resource the frame already touched becomes a queue barrier (HLD-03 §2.4).
		// `sample_textures` is false for a pass that samples nothing, such as a clear.
		void record_pass_hazards(bool sample_textures);

		std::unique_ptr<device> m_device;
		std::unique_ptr<frame_resources> m_frame_resources;
		std::unique_ptr<flush_queue> m_flush_queue;

		// The scratch images of the CPU upload path, one per stereo eye.
		std::array<MTL::Texture*, 2> m_flip_scratch{};

		// The frame's command buffer is open.  A frame opens lazily on its first
		// pass and closes at the flip, which submits it.
		bool m_frame_open = false;
	};
}

#endif // RSX_ENABLE_METAL4
