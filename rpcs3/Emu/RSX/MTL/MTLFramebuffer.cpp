#include "MTLDevice.h"
#include "MTLInstrumentation.h"

#include "MTLFramebuffer.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLSurface.h"

// metal-cpp is already reached through the header above, so the RSX headers that
// pull `using namespace gcm` can follow (gcm_enums.h:1183).
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Utils/rsx_utils.h"

#include <tuple>

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

	bool framebuffer::pass_key::matches(const pass_key& other) const
	{
		// The layout comparison is the same one bind() uses, so a set that bind()
		// considers unchanged is a set this key considers unchanged.  The texture
		// identities cover the cases the layout cannot: a surface swap, a resolve
		// target appearing or changing, and a zcull buffer replacement.
		return same_framebuffer(layout, other.layout)
			&& color == other.color
			&& resolve == other.resolve
			&& depth == other.depth
			&& depth_resolve == other.depth_resolve
			&& scratch_depth == other.scratch_depth
			&& visibility == other.visibility
			&& color_count == other.color_count
			&& sample_count == other.sample_count
			&& width == other.width
			&& height == other.height;
	}

	framebuffer::pass_key framebuffer::current_key() const
	{
		pass_key key;
		key.layout = m_layout;
		key.color_count = m_color_count;
		key.sample_count = m_sample_count;
		key.width = m_width;
		key.height = m_height;
		key.visibility = m_visibility;

		for (u32 index = 0; index < m_color_count; ++index)
		{
			if (!m_color[index])
			{
				continue;
			}

			key.color[index] = m_color[index]->value();

			if (texture* resolve = m_color[index]->resolve_surface.get())
			{
				key.resolve[index] = resolve->value();
			}
		}

		if (m_depth)
		{
			key.depth = m_depth->value();

			if (texture* depth_resolve = m_depth->resolve_surface.get())
			{
				key.depth_resolve = depth_resolve->value();
			}
		}

		key.scratch_depth = m_scratch_depth;

		return key;
	}

	bool framebuffer::can_absorb() const
	{
		return m_has_pass_key && m_pass_key.matches(current_key());
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
		m_has_pass_key = false;
	}

	void framebuffer::bind(command_recorder& cmd, surface_cache& cache, const rsx::framebuffer_layout& layout, const rsx::surface_scaling_config_t& scaling)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/store bind");
		// The pass extent is the attachment extent, and the store creates the
		// attachments at the scaled size (MTLSurface.cpp:74-80).  VK builds its
		// framebuffer at the same scaled size (VKGSRender.cpp:2821).
		const auto [scaled_width, scaled_height] = rsx::apply_resolution_scale<true>(scaling, layout.width, layout.height);

		// The bound set is still current, so the store walk is not repeated.  This is
		// the per-draw path, so it must not cost a surface lookup.  The scaled
		// extent is part of the key: a resolution-scale change keeps the guest
		// layout but needs a new pass and new attachments.
		if (m_has_layout && same_framebuffer(m_layout, layout) &&
			m_width == scaled_width && m_height == scaled_height)
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
		m_width = scaled_width;
		m_height = scaled_height;

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
		else
		{
			// No attachment at all.  The guest still rasterises here (framebufferless
			// draws update ZPASS counters), so the set gets a depth-only scratch image
			// and stays valid: the draw path otherwise opens no pass and NOPs.
			ensure_scratch_depth(cmd, rsx::get_format_sample_count(layout.aa_mode));
			m_sample_count = m_scratch_samples;
		}
	}

	void framebuffer::ensure_scratch_depth(command_recorder& cmd, u8 samples)
	{
		samples = std::max<u8>(samples, 1);

		if (m_scratch_depth && m_scratch_width == m_width && m_scratch_height == m_height &&
			m_scratch_samples == samples)
		{
			return;
		}

		if (m_scratch_depth)
		{
			// A command buffer holds no strong reference (HLD-02 I7), so the old
			// image leaves through the frame slot that may still reference it.
			cmd.resources().defer_release(m_scratch_depth);
			m_scratch_depth = nullptr;
		}

		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(MTL::TextureType2D);
		descriptor->setPixelFormat(MTL::PixelFormatDepth32Float);
		descriptor->setWidth(std::max<u16>(m_width, 1));
		descriptor->setHeight(std::max<u16>(m_height, 1));
		descriptor->setSampleCount(samples);
		descriptor->setMipmapLevelCount(1);
		descriptor->setArrayLength(1);
		descriptor->setUsage(MTL::TextureUsageRenderTarget);

		m_scratch_depth = cmd.resources().new_texture(descriptor, MTL::StorageModePrivate);
		descriptor->release();

		if (!m_scratch_depth)
		{
			rsx_log.error("Metal 4: the %ux%u framebufferless pass image could not be created", m_width, m_height);
			m_scratch_width = 0;
			m_scratch_height = 0;
			m_scratch_samples = 0;
			return;
		}

		m_scratch_width = m_width;
		m_scratch_height = m_height;
		m_scratch_samples = samples;
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

		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
		// The render area may not exceed the smallest attachment, and a pass built
		// from a layout that never set a clip size would otherwise name a zero
		// extent the 1x1 scratch image cannot cover.
		descriptor->setRenderTargetWidth(std::max<u16>(m_width, 1));
		descriptor->setRenderTargetHeight(std::max<u16>(m_height, 1));

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
		// The caller's mask says whether a clear was asked for at all: a partial
		// clear is a scissored fill and passes 0 here, and a load action would wipe
		// everything outside that scissor (MTLFramebuffer.h:27-31).
		//
		// EXCISE-BEGIN sentinel-pass
		// TODO(excise): level 5; MTL_PROBE_VARIANT=sentinel.  Every colour attachment of
		// every pass loads as a clear of the sentinel instead of loading its
		// content.  A pixel that reaches the presented image therefore comes from
		// some draw of this frame, and a pixel that is not reached by any draw stays
		// the sentinel: this is the only way to separate "content is stale" from
		// "content is absent", which is the first fork of the glitch triage.  The
		// value is magenta, which no scene surface is, and the run is not a
		// shipping frame.  Depth and stencil are not touched.
		const bool sentinel_passes = probe::variant("sentinel");
		// EXCISE-END sentinel-pass

		bool clear_color = ((clear_mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0 &&
			state.clear_color_fast && (state.clear_colormask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0);

		// EXCISE-BEGIN sentinel-clear
		if (sentinel_passes)
		{
			clear_color = true;
		}
		// EXCISE-END sentinel-clear
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

			// EXCISE-BEGIN sentinel-clear-color
			if (sentinel_passes)
			{
				attachment->setClearColor(MTL::ClearColor::Make(1.0, 0.0, 1.0, 1.0));
			}
			else
			// EXCISE-END sentinel-clear-color
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
		else if (m_scratch_depth)
		{
			// A framebufferless pass.  The image is not guest data, and nothing reads
			// it, so a depth-only pass with both actions DontCare is the whole pass:
			// Metal still rasterises and still counts the occlusion query (probe D).
			MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment();
			depth->setTexture(m_scratch_depth);
			depth->setLoadAction(MTL::LoadActionDontCare);
			depth->setStoreAction(MTL::StoreActionDontCare);
		}

		MTL4::RenderCommandEncoder* encoder = cmd.open_pass(descriptor);
		descriptor->release();

		// The pass is now the one a later draw may share.  Capture its inputs after
		// the open: the recorder closed the previous encoder inside open_pass, and a
		// draw that finds this key unchanged can encode into this pass without a
		// boundary (C1).
		m_pass_key = current_key();
		m_has_pass_key = encoder != nullptr;
		return encoder;
	}
}

#endif // RSX_ENABLE_METAL4
