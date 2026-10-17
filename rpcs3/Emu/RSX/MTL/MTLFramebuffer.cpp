#include "MTLDevice.h"

#include "MTLFramebuffer.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLSurface.h"

// metal-cpp is already reached through the header above, so the RSX headers that
// pull `using namespace gcm` can follow (gcm_enums.h:1183).
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Common/TextureUtils.h"

namespace mtl
{
	bool same_framebuffer(const rsx::framebuffer_layout& a, const rsx::framebuffer_layout& b)
	{
		return a.width == b.width
			&& a.height == b.height
			&& a.color_addresses == b.color_addresses
			&& a.actual_color_pitch == b.actual_color_pitch
			&& a.zeta_address == b.zeta_address
			&& a.actual_zeta_pitch == b.actual_zeta_pitch
			&& a.target == b.target
			&& a.color_format == b.color_format
			&& a.depth_format == b.depth_format
			&& a.aa_mode == b.aa_mode
			&& a.raster_type == b.raster_type;
	}

	void framebuffer::reset()
	{
		m_color = {};
		m_depth = nullptr;
		m_color_count = 0;
		m_sample_count = 1;
		m_width = 0;
		m_height = 0;
		m_has_layout = false;
	}

	void framebuffer::bind(command_recorder& cmd, surface_cache& cache, const rsx::framebuffer_layout& layout, const rsx::surface_scaling_config_t& scaling)
	{
		// The bound set is still current, so the store walk is not repeated.  This is
		// the per-draw path, so it must not cost a surface lookup.
		if (m_has_layout && same_framebuffer(m_layout, layout))
		{
			return;
		}

		reset();

		// The store owns the surfaces: it reuses a match, splits an overlap and
		// allocates when there is none.  It also makes every new attachment ready for
		// drawing (surface_store.h:1013).
		cache.prepare_render_target(cmd,
			layout.color_format, layout.depth_format,
			layout.width, layout.height,
			layout.target, layout.aa_mode, layout.raster_type,
			layout.color_addresses, layout.zeta_address,
			layout.actual_color_pitch, layout.actual_zeta_pitch,
			scaling);

		m_layout = layout;
		m_has_layout = true;
		m_width = layout.width;
		m_height = layout.height;

		// The RSX renumbers the enabled targets into draw slots 0..n-1, so the guest
		// holes are removed: a surface_b draw is draw slot 0 and resolves to guest
		// target 1 (GLRenderTargets.cpp:291-294; VKGSRender.cpp:295,
		// `m_draw_buffers`/`get_rtt_indexes`).  The pass descriptor and the PSO index
		// the same slots, so a hole left at index 0 would bind no attachment there
		// (and a `surface_b`-only draw left `m_color[0]` null).
		u8 color_count = 0;
		for (u32 index = 0; index < 4; ++index)
		{
			if (surface* target = cache.m_bound_render_targets[index].second)
			{
				m_color[color_count++] = target;
			}
		}
		m_color_count = color_count;

		m_depth = cache.m_bound_depth_stencil.second;

		if (m_color_count != 0)
		{
			m_sample_count = m_color[0]->samples();
		}
		else if (m_depth)
		{
			m_sample_count = m_depth->samples();
		}
	}

	MTL4::RenderCommandEncoder* framebuffer::open_pass(command_recorder& cmd, u32 clear_mask, const surface_state& state)
	{
		if (!valid())
		{
			return nullptr;
		}

		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
		descriptor->setRenderTargetWidth(m_width);
		descriptor->setRenderTargetHeight(m_height);

		if (m_visibility)
		{
			// Accumulate, because a query's counter must survive every pass of the
			// query and the reset form would clear it at each pass start
			// (MTLZCULL.h).  The default with no counting enabled is disabled.
			descriptor->setVisibilityResultBuffer(m_visibility);
			descriptor->setVisibilityResultType(MTL::VisibilityResultTypeAccumulate);
		}

		const bool clear_color = (clear_mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0;
		const bool clear_depth = (clear_mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
		const bool clear_stencil = (clear_mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0;

		for (u32 index = 0; index < m_color_count; ++index)
		{
			MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(index);
			attachment->setTexture(m_color[index]->value());
			attachment->setLoadAction(clear_color ? MTL::LoadActionClear : MTL::LoadActionLoad);
			attachment->setStoreAction(MTL::StoreActionStore);

			// A multisampled attachment resolves on store, so the resolved image is
			// the samplable one (HLD-02 §2.2).  The multisampled image survives the
			// resolve, so a later write needs no unresolve.
			if (texture* resolve = m_color[index]->resolve_surface.get())
			{
				attachment->setResolveTexture(resolve->value());
				attachment->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
			}

			if (clear_color)
			{
				attachment->setClearColor(state.clear_color);
			}
		}

		if (m_depth)
		{
			MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment();
			depth->setTexture(m_depth->value());
			depth->setLoadAction(clear_depth ? MTL::LoadActionClear : MTL::LoadActionLoad);
			// The depth attachment always stores.  A depth or stencil render-pass
			// resolve is not ordered by a Metal producer barrier, so the depth half of
			// a multisampled surface resolves through a meta resolve instead, outside
			// this pass (Mesa kk_cmd_buffer.c:1000-1005).
			depth->setStoreAction(MTL::StoreActionStore);

			if (clear_depth)
			{
				depth->setClearDepth(state.clear_depth);
			}

			if (rsx::is_depth_stencil_format(state.depth_format))
			{
				MTL::RenderPassStencilAttachmentDescriptor* stencil = descriptor->stencilAttachment();
				stencil->setTexture(m_depth->value());
				stencil->setLoadAction(clear_stencil ? MTL::LoadActionClear : MTL::LoadActionLoad);
				stencil->setStoreAction(MTL::StoreActionStore);

				if (clear_stencil)
				{
					stencil->setClearStencil(state.clear_stencil);
				}
			}
		}

		MTL4::RenderCommandEncoder* encoder = cmd.open_pass(descriptor);
		descriptor->release();
		return encoder;
	}
}

#endif // RSX_ENABLE_METAL4
