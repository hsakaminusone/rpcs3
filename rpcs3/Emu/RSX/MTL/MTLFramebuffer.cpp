#include "MTLDevice.h"
#include "MTLInstrumentation.h"

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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/store bind");
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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/open pass");
		if (!valid())
		{
			// TODO(excise): level 2; the pass that never opened (area B).
			MTL_PROBE_LATCH(2, "framebuffer/pass refused");
			return nullptr;
		}

		// TODO(excise): level 2; the pass stream, its clear mask and its targets.
		MTL_PROBE_LATCH(2, "framebuffer/pass opened");
		MTL_PROBE_HISTOGRAM(2, "framebuffer pass clear", clear_mask & 0x7u, "clear mask bits");
		MTL_PROBE_HISTOGRAM(2, "framebuffer pass colors", m_color_count, "color attachment count");
		// TODO(excise): level 2; the depth-stencil feature of the pass (area I).
		if (m_depth)
		{
			MTL_PROBE_LATCH(2, "feature/depth attachment");
		}
		else
		{
			MTL_PROBE_LATCH(2, "feature/no depth attachment");
		}
		// TODO(excise): level 2; the visibility query the pass carries (area I).
		if (m_visibility)
		{
			MTL_PROBE_LATCH(2, "feature/zcull query");
		}
		else
		{
			MTL_PROBE_LATCH(2, "feature/no zcull query");
		}

		// TODO(excise): level 1; a pass with no attachment and no depth cannot render
		// anything, so the probe reports it rather than encoding a silent empty pass.
		if (!m_color_count && !m_depth)
		{
			MTL_ERROR(1, "Metal 4: render pass %ux%u opened with no color and no depth attachment", m_width, m_height);
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

		// The colour mask is the format-aware one the caller resolved; a full
		// MTLLoadActionClear is only legal when it covers every stored channel.
		const bool clear_color = state.clear_color_fast && (state.clear_colormask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0;
		const bool clear_depth = (clear_mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
		const bool clear_stencil = (clear_mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0 && state.clear_stencil_fast;

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
			// The depth attachment always stores, and resolves in its own pass
			// (surface::resolve), because a clause pass is not the last writer of the
			// multisampled image.
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
