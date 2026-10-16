// metal-cpp must be reached before GSRender.h: gcm_enums.h:1183 has
// `using namespace gcm`, which makes Objective-C's `Method` ambiguous in a TU
// that reaches metal-cpp afterwards.
#include "MTLDevice.h"

#include "MTLGSRender.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLCommandRecorder.h"
#include "MTLFill.h"
#include "MTLFramebuffer.h"
#include "MTLPresenter.h"
#include "MTLResourceManager.h"
#include "MTLSurfaceCache.h"
#include "MTLSurfaceState.h"

#include "Emu/RSX/RSXThread.h"

#include <tuple>

namespace mtl
{
	// The backend objects of one renderer instance.  They are built in
	// on_init_thread and torn down in on_exit, in reverse order.
	struct MTLGSRender::frame_resources
	{
		resource_manager resources;
		command_recorder recorder;
		surface_cache cache;
		framebuffer attachments;
		fill_pass fill;
		presenter present;
	};

	MTLGSRender::MTLGSRender(utils::serial* ar) noexcept
		: GSRender(ar)
	{
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender() = default;

	u64 MTLGSRender::get_cycles()
	{
		return thread_ctrl::get_cycles(static_cast<named_thread<MTLGSRender>&>(*this));
	}

	void MTLGSRender::on_init_thread()
	{
		m_device = device::create();
		if (!m_device)
		{
			die("no Metal 4 device is available");
		}

		// The 16-bool backend contract (RSXThread.h:80; 10 §2.2).
		m_device->fill_backend_config(backend_config);

		// The resource layer and the recorder come up with the device, because the
		// guest window joins the persistent residency set and the ring is sized once
		// (HLD-02 I5, HLD-04 §3.2).
		m_frame_resources = std::make_unique<frame_resources>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);
		m_frame_resources->fill.init(*m_device, m_frame_resources->resources);

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = get_frame())
		{
			m_frame_resources->present.init(*m_device, frame->handle());
		}

		GSRender::on_init_thread();
	}

	void MTLGSRender::on_exit()
	{
		if (m_frame_resources)
		{
			// The layer goes first, then the ring drains, then the resources go.
			m_frame_resources->present.shutdown();
			m_frame_resources->fill.shutdown();
			m_frame_resources->recorder.shutdown();
			m_frame_resources->resources.shutdown();
			m_frame_resources.reset();
		}

		m_device.reset();
		GSRender::on_exit();
	}

	bool MTLGSRender::update_scissor(bool clip_viewport)
	{
		areau region;
		if (get_scissor(region, clip_viewport))
		{
			m_scissor = region;
			return true;
		}

		return !m_graphics_state.test(rsx::pipeline_state::scissor_setup_invalid);
	}

	bool MTLGSRender::bind_framebuffer(rsx::framebuffer_creation_context context)
	{
		const bool clipped_scissor = (context == rsx::framebuffer_creation_context::context_draw);

		if (m_frame_resources->attachments.valid() && m_framebuffer_layout.ignore_change)
		{
			// The layout has not changed, so the bound set is still current
			// (VKGSRender.cpp:2618).
			update_scissor(clipped_scissor);
			return true;
		}

		m_current_framebuffer_context = context;

		m_graphics_state.clear(rsx::rtt_config_dirty | rsx::rtt_config_contested | rsx::rtt_config_valid | rsx::rtt_cache_state_dirty);
		get_framebuffer_layout(context, m_framebuffer_layout);

		if (!m_graphics_state.test(rsx::rtt_config_valid))
		{
			return false;
		}

		m_frame_resources->attachments.bind(m_frame_resources->recorder, m_frame_resources->cache, m_framebuffer_layout, resolution_scaling_config);
		update_scissor(clipped_scissor);
		return m_frame_resources->attachments.valid();
	}

	void MTLGSRender::clear_surface(u32 mask)
	{
		if (skip_current_frame || !m_frame_resources)
		{
			return;
		}

		// A stencil clear cannot run with the stencil write mask off
		// (VKGSRender.cpp:1413).
		if (!rsx::method_registers.stencil_mask())
		{
			mask &= ~RSX_GCM_CLEAR_STENCIL_BIT;
		}

		if (!(mask & RSX_GCM_CLEAR_ANY_MASK))
		{
			return;
		}

		// The clear context is what makes the store create the attachments it needs.
		u8 context = rsx::framebuffer_creation_context::context_draw;
		if (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK)
		{
			context |= rsx::framebuffer_creation_context::context_clear_color;
		}

		if (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK)
		{
			context |= rsx::framebuffer_creation_context::context_clear_depth;
		}

		if (!bind_framebuffer(rsx::framebuffer_creation_context{context}))
		{
			return;
		}

		const surface_state state = capture_surface_state();
		framebuffer& attachments = m_frame_resources->attachments;

		if (state.clear_covers_attachment())
		{
			// The clear is the whole attachment, so it is a load action and needs no
			// draw (LLD-surface invariant 4).  The pass is opened so the clear is
			// recorded, then closed.
			attachments.open_pass(m_frame_resources->recorder, mask, state);
			m_frame_resources->recorder.close_encoder();
			return;
		}

		// Metal has no scissored clear, so a partial clear is a full-screen fill
		// confined by the scissor (CONFLICTS.md C1).
		if (!update_scissor(false))
		{
			return;
		}

		MTL4::RenderCommandEncoder* encoder = attachments.open_pass(m_frame_resources->recorder, 0, state);
		if (!encoder)
		{
			return;
		}

		u16 x = static_cast<u16>(m_scissor.x1);
		u16 y = static_cast<u16>(m_scissor.y1);
		u16 width = static_cast<u16>(m_scissor.width());
		u16 height = static_cast<u16>(m_scissor.height());

		std::tie(x, y, width, height) = rsx::clip_region<u16>(attachments.width(), attachments.height(), x, y, width, height, true);

		MTL::ScissorRect scissor;
		scissor.x = x;
		scissor.y = y;
		scissor.width = width;
		scissor.height = height;
		encoder->setScissorRect(scissor);

		const bool write_color = (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0;
		const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK) != 0;
		const std::array<f32, 4> color = {
			static_cast<f32>(state.clear_color.red),
			static_cast<f32>(state.clear_color.green),
			static_cast<f32>(state.clear_color.blue),
			static_cast<f32>(state.clear_color.alpha) };
		const MTL::PixelFormat color_format = attachments.color_count() != 0 ? attachments.color(0)->pixel_format() : MTL::PixelFormatInvalid;

		m_frame_resources->fill.fill(m_frame_resources->recorder.fragment_table(), encoder, color_format, attachments.sample_count(), color, state.clear_depth, write_color, write_depth);
		m_frame_resources->recorder.close_encoder();
	}

	// C7.  The RSX output is the surface at the flip address, and it is presented
	// through the one submit path the recorder owns (HLD-03 §4.9).
	void MTLGSRender::flip(const rsx::display_flip_info_t& info)
	{
		if (m_frame_resources && m_frame_resources->present.valid())
		{
			if (surface* source = m_frame_resources->cache.get_surface_at(info.buffer))
			{
				if (texture* output = source->get_surface(rsx::surface_access::memory_read))
				{
					m_frame_resources->recorder.begin_frame();

					// The presenter submits the frame it fills.  A dropped frame is
					// submitted here instead, so the ring advances either way.
					if (!m_frame_resources->present.present(output->value(), m_frame_resources->recorder))
					{
						m_frame_resources->recorder.submit();
					}
				}
			}
		}

		GSRender::flip(info);
	}
}

#endif // RSX_ENABLE_METAL4
