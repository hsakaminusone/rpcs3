#include "MTLSurface.h"
#include "MTLInstrumentation.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"

#include "Emu/RSX/Common/TextureUtils.h"

namespace mtl
{
	texture* surface::get_surface(rsx::surface_access access_type)
	{
		if (sample_count() == 1 || !access_type.is_transfer())
		{
			return this;
		}

		// A transfer access reads the resolved data, because the multisample image is
		// not a transfer source (texture_cache_helpers.h:752-761).
		return resolve_surface.get();
	}

	void surface::create(u32 address, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, MTL::PixelFormat format, u8 bpp, bool depth)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/create");
		m_depth = depth;
		surface_width = width;
		surface_height = height;
		set_resolution_scaling_config(scaling);

		// The guest AA mode is always reported, so get_spp() stays the guest sample
		// count (surface_store.h:646).  The host sample count follows the video
		// configuration.
		set_aa_mode(antialias);

		const u8 requested_samples = (g_cfg.video.antialiasing_level == msaa_level::_auto) ? rsx::get_format_sample_count(antialias) : 1;
		const u8 host_samples = m_resources->supported_sample_count(requested_samples);
		sample_layout = (host_samples > 1) ? rsx::surface_sample_layout::ps3 : rsx::surface_sample_layout::null;

		// The guest row and the Metal row are two different numbers (HLD-02 §2.3).
		// native_pitch is the exploded row, because get_bpp() divides it by the
		// sampled width (surface_utils.h:256).
		rsx_pitch = pitch;
		native_pitch = static_cast<u32>(width) * bpp * samples_x;
		queue_tag(address);

		const auto [scaled_w, scaled_h] = rsx::apply_resolution_scale<true>(scaling, width, height);

		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(host_samples > 1 ? MTL::TextureType2DMultisample : MTL::TextureType2D);
		descriptor->setPixelFormat(format);
		descriptor->setWidth(scaled_w);
		descriptor->setHeight(scaled_h);
		descriptor->setSampleCount(host_samples);
		// PixelFormatView is required because a texture unit that samples this
		// surface reads it through a swizzled view (MTLHandles.cpp, get_view), and
		// the stencil read needs it too (MTLSurface.cpp, stencil_view).
		descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
		assign(m_resources->new_texture(descriptor, MTL::StorageModePrivate));

		// The resolved image persists and stays samplable (HLD-02 §2.2).
		if (host_samples > 1)
		{
			descriptor->setTextureType(MTL::TextureType2D);
			descriptor->setSampleCount(1);
			resolve_surface = std::make_unique<texture>(m_resources);
			resolve_surface->assign(m_resources->new_texture(descriptor, MTL::StorageModePrivate));
		}

		descriptor->release();

		// Guest memory holds the contents until a draw writes the surface (HLD-02 §2.1).
		memory_usage_flags = rsx::surface_usage_flags::attachment;
		state_flags = rsx::surface_state_flags::erase_bkgnd;
	}

	void surface::create_color(u32 address, rsx::surface_color_format format, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling)
	{
		set_format(format);
		create(address, width, height, pitch, antialias, scaling, mtl::get_surface_color_format(format), rsx::get_format_block_size_in_bytes(format), false);

		// The guest channel meaning of the format, so a sampled surface returns the
		// channels the guest wrote (DEVIATIONS-SURFACE.md, S7).  The resolve image
		// carries the same format and therefore the same map.
		const std::array<MTL::TextureSwizzle, 4> layout = mtl::get_surface_component_layout(format);
		set_native_component_layout(layout);

		if (resolve_surface)
		{
			resolve_surface->set_native_component_layout(layout);
		}
	}

	void surface::create_depth(u32 address, rsx::surface_depth_format2 format, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling)
	{
		set_format(format);
		create(address, width, height, pitch, antialias, scaling, mtl::get_surface_depth_format(format), rsx::get_format_block_size_in_bytes(format), true);
	}

	texture* surface::depth_mirror()
	{
		if (!m_depth)
		{
			return nullptr;
		}

		if (m_depth_mirror)
		{
			return m_depth_mirror.get();
		}

		// The copy that fills the mirror must match the source format and sample
		// count, so the mirror repeats both, and the read is a shader read with no
		// sampler.
		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(sample_count() > 1 ? MTL::TextureType2DMultisample : MTL::TextureType2D);
		descriptor->setPixelFormat(pixel_format());
		descriptor->setWidth(width());
		descriptor->setHeight(height());
		descriptor->setSampleCount(sample_count());
		descriptor->setUsage(MTL::TextureUsageShaderRead);

		m_depth_mirror = std::make_unique<texture>(m_resources);
		m_depth_mirror->assign(m_resources->new_texture(descriptor, MTL::StorageModePrivate));
		descriptor->release();

		return m_depth_mirror.get();
	}

	texture* surface::stencil_view()
	{
		// Only the combined float-stencil format has a castable stencil view.
		if (!m_depth || pixel_format() != MTL::PixelFormat::PixelFormatDepth32Float_Stencil8)
		{
			return nullptr;
		}

		if (m_stencil_view)
		{
			return m_stencil_view.get();
		}

		m_stencil_view = std::make_unique<texture>(m_resources);
		m_stencil_view->assign(value()->newTextureView(MTL::PixelFormat::PixelFormatX32_Stencil8));

		return m_stencil_view.get();
	}

	MTL::ResourceID surface::mirror_depth(command_recorder& cmd)
	{
		texture* mirror = depth_mirror();
		if (!mirror)
		{
			return {};
		}

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return {};
		}

		// The copy reads the surface the previous pass wrote, and this pass reads the
		// mirror, so both edges cross an encoder and are queue-scope.
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
		encoder->copyFromTexture(value(), 0, 0,
			MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width(), height(), 1),
			mirror->value(), 0, 0, MTL::Origin::Make(0, 0, 0));

		return mirror->resource_id();
	}

	void surface::memory_barrier(command_recorder& cmd, rsx::surface_access access_type)
	{
		if (old_contents.empty())
		{
			if (state_flags & rsx::surface_state_flags::erase_bkgnd)
			{
				initialize_memory(cmd, access_type);
				on_write(rsx::get_shared_tag(), static_cast<rsx::surface_state_flags>(msaa_flags));
			}
		}
		else
		{
			flush_old_contents(cmd, access_type);
		}

		if (access_type.is_transfer())
		{
			resolve(cmd);
		}
		else if (access_type == rsx::surface_access::shader_write)
		{
			unresolve(cmd);
		}
	}

	void surface::flush_old_contents(command_recorder& cmd, rsx::surface_access access)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/flush old");
		// TODO(excise): level 2; the surface write-back stream (area G).
		MTL_PROBE_LATCH(2, "surface/flush old");
		texture* target = get_surface(rsx::surface_access::transfer_read);
		const u32 first = prepare_rw_barrier_for_transfer(this);

		// An intact destination accepts every incoming source; otherwise a source
		// that no longer holds its own data must not be blended in
		// (VKRenderTargets.cpp:1049-1054).
		const bool accept_all = last_use_tag && test();

		// The transfers land on top of the background, so it is initialised first,
		// unless one of them supplies every texel (VKRenderTargets.cpp:1073-1087).
		if (state_flags & rsx::surface_state_flags::erase_bkgnd)
		{
			bool covered = false;

			for (u32 index = first; index < old_contents.size() && !covered; ++index)
			{
				auto& region = old_contents[index];
				surface* source = static_cast<surface*>(region.source);
				if (!source || !source->value() || (!accept_all && !source->test()))
				{
					continue;
				}

				region.init_transfer(this);
				const areai dst = region.dst_rect();
				covered = (dst.x1 == 0 && dst.y1 == 0 &&
					static_cast<u32>(dst.x2) == target->value()->width() &&
					static_cast<u32>(dst.y2) == target->value()->height());
			}

			if (covered)
			{
				state_flags &= ~rsx::surface_state_flags::erase_bkgnd;
				msaa_flags = rsx::surface_state_flags::ready;
			}
			else
			{
				initialize_memory(cmd, rsx::surface_access::memory_write);
			}
		}

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		u64 newest_tag = 0;
		bool optimize_copy = true;

		for (u32 index = first; index < old_contents.size(); ++index)
		{
			auto& region = old_contents[index];
			surface* source = static_cast<surface*>(region.source);

			if (!source || !source->value() || (!accept_all && !source->test()))
			{
				continue;
			}

			region.init_transfer(this);
			const areai src = region.src_rect();
			const areai dst = region.dst_rect();

			const bool covers = (dst.x1 == 0 && dst.y1 == 0 &&
				static_cast<u32>(dst.x2) == target->value()->width() &&
				static_cast<u32>(dst.y2) == target->value()->height());

			if (covers)
			{
				// A covering copy defines the whole image, stencil included
				// (VKRenderTargets.cpp:1078-1080).
				stencil_init_flags = source->stencil_init_flags;
			}

			encoder->copyFromTexture(source->get_surface(rsx::surface_access::transfer_read)->value(), 0, 0,
				MTL::Origin::Make(src.x1, src.y1, 0),
				MTL::Size::Make(src.x2 - src.x1, src.y2 - src.y1, 1),
				target->value(), 0, 0,
				MTL::Origin::Make(dst.x1, dst.y1, 0));

			optimize_copy = optimize_copy && !covers;
			newest_tag = source->last_use_tag;
		}

		if (!newest_tag) [[unlikely]]
		{
			// No source held valid data, so the contents are undefined
			// (VKRenderTargets.cpp:1118-1125).
			clear_rw_barrier();
			state_flags |= rsx::surface_state_flags::erase_bkgnd;
			initialize_memory(cmd, access);
			ensure(state_flags == rsx::surface_state_flags::ready);
		}

		on_write_copy(newest_tag, optimize_copy);
	}

	void surface::initialize_memory(command_recorder& cmd, rsx::surface_access access)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/init memory");
		if (needs_cpu_upload())
		{
			load_background(cmd);
		}
		else
		{
			// Default-initialise without loading: a fresh image holds undefined
			// contents and the read-buffers configuration is off
			// (VKRenderTargets.cpp:817-828).
			clear_memory(cmd, this);

			if (sample_count() > 1 && access.is_transfer_or_read())
			{
				// Only the resolve target matters when it is read
				// (VKRenderTargets.cpp:820-825).
				clear_memory(cmd, resolve_surface.get());
			}
		}

		state_flags = rsx::surface_state_flags::ready;
	}

	void surface::clear_memory(command_recorder& cmd, texture* target)
	{
		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
		descriptor->setRenderTargetWidth(target->value()->width());
		descriptor->setRenderTargetHeight(target->value()->height());

		if (is_depth_surface())
		{
			MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment();
			depth->setTexture(target->value());
			depth->setLoadAction(MTL::LoadActionClear);
			depth->setStoreAction(MTL::StoreActionStore);
			depth->setClearDepth(1.0);

			if (rsx::is_depth_stencil_format(get_surface_depth_format()))
			{
				MTL::RenderPassStencilAttachmentDescriptor* stencil = descriptor->stencilAttachment();
				stencil->setTexture(target->value());
				stencil->setLoadAction(MTL::LoadActionClear);
				stencil->setStoreAction(MTL::StoreActionStore);
				stencil->setClearStencil(0xFF);
			}
		}
		else
		{
			MTL::RenderPassColorAttachmentDescriptor* color = descriptor->colorAttachments()->object(0);
			color->setTexture(target->value());
			color->setLoadAction(MTL::LoadActionClear);
			color->setStoreAction(MTL::StoreActionStore);
			color->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));
		}

		cmd.open_pass(descriptor);
		descriptor->release();
	}

	void surface::load_background(command_recorder& cmd)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/load background");
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		texture* target = get_surface(rsx::surface_access::transfer_read);

		const rsx::surface_depth_format2 depth_format = get_surface_depth_format();

		if (m_depth && depth_format_needs_decode(depth_format))
		{
			const u32 plane_pitch = width() * 4;
			MTL::Buffer* staging = m_resources->new_buffer(plane_pitch * height(), MTL::StorageModeShared);
			decode_depth_plane(staging->contents(), static_cast<const u8*>(m_resources->guest_buffer()->contents()) + base_addr,
				width(), height(), rsx_pitch, get_bpp(), depth_format);

			encoder->copyFromBuffer(staging, 0, plane_pitch, plane_pitch * height(),
				MTL::Size::Make(width(), height(), 1),
				target->value(), 0, 0, MTL::Origin::Make(0, 0, 0),
				get_depth_plane_option(target->value()->pixelFormat(), false));

			cmd.resources().defer_release(staging);

			if (rsx::is_depth_stencil_format(depth_format))
			{
				stencil_init_flags |= (1 << 9);
			}
		}
		else
		{
			// The guest address is the offset into the guest window (HLD-02 §3.5).
			encoder->copyFromBuffer(m_resources->guest_buffer(), base_addr,
				rsx_pitch, rsx_pitch * get_surface_height<rsx::surface_metrics::samples>(),
				MTL::Size::Make(width(), height(), 1),
				target->value(), 0, 0, MTL::Origin::Make(0, 0, 0));
		}

		if (sample_count() > 1)
		{
			msaa_flags |= rsx::surface_state_flags::require_unresolve;
		}
	}

	void surface::resolve(command_recorder& cmd)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/resolve");
		if (sample_count() == 1 || !resolve_surface || !(msaa_flags & rsx::surface_state_flags::require_resolve))
		{
			return;
		}

		// TODO(excise): level 2; a multisampled surface resolved (area G).
		MTL_PROBE_LATCH(2, "surface/resolve");
		MTL_PROBE_HISTOGRAM(2, "surface resolve samples", sample_count(), "sample count");

		// A depth or stencil attachment resolves in its own pass, because the resolve
		// only reads the multisampled image after the passes that wrote it have
		// closed.  StoreAndMultisampleResolve keeps the multisampled image, so a later
		// write needs no unresolve.
		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();

		if (m_depth)
		{
			MTL::RenderPassDepthAttachmentDescriptor* depth = descriptor->depthAttachment();
			depth->setTexture(value());
			depth->setResolveTexture(resolve_surface->value());
			depth->setLoadAction(MTL::LoadActionLoad);
			depth->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);

			if (rsx::is_depth_stencil_format(get_surface_depth_format()))
			{
				MTL::RenderPassStencilAttachmentDescriptor* stencil = descriptor->stencilAttachment();
				stencil->setTexture(value());
				stencil->setResolveTexture(resolve_surface->value());
				stencil->setLoadAction(MTL::LoadActionLoad);
				stencil->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
			}
		}
		else
		{
			MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
			attachment->setTexture(value());
			attachment->setResolveTexture(resolve_surface->value());
			attachment->setLoadAction(MTL::LoadActionLoad);
			attachment->setStoreAction(MTL::StoreActionStoreAndMultisampleResolve);
		}

		MTL4::RenderCommandEncoder* encoder = cmd.open_pass(descriptor, MTL4::RenderEncoderOptionNone);
		descriptor->release();

		// A render-pass resolve is ordered for later queue work by the producer form
		// of the barrier.  The consumer form leaves a window in which the resolved
		// image is not yet visible, so the resolve pass declares the edge itself
		// (REVIEW-FINDINGS.md, B6).
		if (encoder)
		{
			emit_producer_barrier(encoder, { MTL::StageAll, MTL::StageAll });
		}

		msaa_flags = rsx::surface_state_flags::ready;
	}

	void surface::unresolve(command_recorder&)
	{
		// Every resolve keeps the multisampled image (StoreAndMultisampleResolve), so
		// there is nothing to copy back.  A 1-sample-to-N-sample blit is not legal
		// anyway, because a Metal blit copy needs matching sample counts.
		msaa_flags &= ~rsx::surface_state_flags::require_unresolve;
	}

	void surface::write_to_memory(command_recorder& cmd, MTL::Buffer* target, u32 dst_offset, u32 src_offset, u32 write_length)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "surface/write to memory");
		// TODO(excise): level 2; a surface written back to guest memory (area G, H).
		MTL_PROBE_LATCH(2, "surface/write back");
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		// src_offset is a byte offset into the surface row space.
		const u32 row = src_offset / rsx_pitch;
		const u32 column = (src_offset % rsx_pitch) / get_bpp();
		u32 rows = write_length / rsx_pitch;

		if (!rows || column >= width())
		{
			return;
		}

		MTL::Texture* source = get_surface(rsx::surface_access::transfer_read)->value();
		if (row >= source->height())
		{
			return;
		}

		// The row pitch is the guest one.  A host surface can be wider than the
		// guest surface under resolution scaling, so the copy is bounded by both the
		// host extent and the guest row (REVIEW-FINDINGS.md, S10).  The scaled case
		// still needs a downscale, but the bit-exact unscaled path must not read past
		// the host texture or write past the guest row.
		const u32 copy_width = std::min<u32>(width() - column, rsx_pitch / get_bpp());
		rows = std::min<u32>(rows, static_cast<u32>(source->height()) - row);
		if (!copy_width || !rows)
		{
			return;
		}

		const MTL::PixelFormat format = source->pixelFormat();
		const rsx::surface_depth_format2 depth_format = get_surface_depth_format();

		// The guest depth pack is not the Metal depth plane.  z16_uint happens to use
		// the same word, but z16_float and both z24s8 forms need the upload decode
		// inverted (B16).  The blit copies the host plane to a shared staging buffer;
		// the frame is committed and waited for, because the CPU encode that follows
		// is the read-back itself.  This is the same bounded wait the texture
		// read-back uses (B4, MTLCommandRecorder::flush_for_readback).
		if (m_depth && depth_format_needs_decode(depth_format))
		{
			const u32 plane_pitch = copy_width * 4;
			MTL::Buffer* plane = m_resources->new_buffer(static_cast<u64>(plane_pitch) * rows, MTL::StorageModeShared);
			if (!plane)
			{
				rsx_log.error("Metal 4: the depth read-back staging buffer could not be created");
				return;
			}

			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(column, row, 0),
				MTL::Size::Make(copy_width, rows, 1),
				plane, 0, plane_pitch, plane_pitch * rows,
				get_depth_plane_option(format, false));

			MTL::Buffer* stencil = nullptr;
			const u32 stencil_pitch = (copy_width + 3u) & ~3u;
			const bool combined = rsx::is_depth_stencil_format(depth_format);
			if (combined)
			{
				stencil = m_resources->new_buffer(static_cast<u64>(stencil_pitch) * rows, MTL::StorageModeShared);
				if (!stencil)
				{
					rsx_log.error("Metal 4: the stencil read-back staging buffer could not be created");
					m_resources->defer_release(plane);
					return;
				}

				encoder->copyFromTexture(source, 0, 0,
					MTL::Origin::Make(column, row, 0),
					MTL::Size::Make(copy_width, rows, 1),
					stencil, 0, stencil_pitch, stencil_pitch * rows,
					get_depth_plane_option(format, true));
			}

			cmd.flush_for_readback();

			u8* const dst = static_cast<u8*>(target->contents()) + dst_offset;
			encode_depth_plane(dst, plane->contents(), copy_width, rows, rsx_pitch, get_bpp(), depth_format,
				stencil ? stencil->contents() : nullptr, stencil ? stencil_pitch : 0);

			m_resources->defer_release(plane);
			if (stencil)
			{
				m_resources->defer_release(stencil);
			}
			return;
		}

		if (format == MTL::PixelFormat::PixelFormatDepth32Float_Stencil8)
		{
			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(column, row, 0),
				MTL::Size::Make(copy_width, rows, 1),
				target, dst_offset, rsx_pitch, rsx_pitch * rows,
				get_depth_plane_option(format, false));
		}
		else
		{
			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(column, row, 0),
				MTL::Size::Make(copy_width, rows, 1),
				target, dst_offset, rsx_pitch, rsx_pitch * rows);
		}
	}
}

#endif // RSX_ENABLE_METAL4
