#include "MTLSurface.h"

#ifdef RSX_ENABLE_METAL4

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
		m_depth = depth;
		surface_width = width;
		surface_height = height;
		set_resolution_scaling_config(scaling);

		// The guest AA mode is always reported, so get_spp() stays the guest sample
		// count (surface_store.h:646).  The host sample count follows the video
		// configuration.
		set_aa_mode(antialias);

		const u8 host_samples = (g_cfg.video.antialiasing_level == msaa_level::_auto) ? rsx::get_format_sample_count(antialias) : 1;
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
		// surface reads it through a swizzled view (MTLHandles.cpp:19).
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
	}

	void surface::create_depth(u32 address, rsx::surface_depth_format2 format, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling)
	{
		set_format(format);
		create(address, width, height, pitch, antialias, scaling, mtl::get_surface_depth_format(format), rsx::get_format_block_size_in_bytes(format), true);
	}

	void surface::memory_barrier(command_recorder& cmd, rsx::surface_access access_type)
	{
		if (!old_contents.empty())
		{
			flush_old_contents(cmd);
		}

		if (needs_cpu_upload())
		{
			load_background(cmd);
			state_flags = rsx::surface_state_flags::ready;
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

	void surface::flush_old_contents(command_recorder& cmd)
	{
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		texture* target = get_surface(rsx::surface_access::transfer_read);
		const u32 first = prepare_rw_barrier_for_transfer(this);

		for (u32 index = first; index < old_contents.size(); ++index)
		{
			auto& region = old_contents[index];
			region.init_transfer(this);

			surface* source = static_cast<surface*>(region.source);
			if (!source || !source->value())
			{
				continue;
			}

			const areai src = region.src_rect();
			const areai dst = region.dst_rect();

			encoder->copyFromTexture(source->get_surface(rsx::surface_access::transfer_read)->value(), 0, 0,
				MTL::Origin::Make(src.x1, src.y1, 0),
				MTL::Size::Make(src.x2 - src.x1, src.y2 - src.y1, 1),
				target->value(), 0, 0,
				MTL::Origin::Make(dst.x1, dst.y1, 0));
		}

		if (sample_count() > 1)
		{
			// The region went into the resolve target, so the multisample image must
			// take it back before a draw (HLD-02 §2.2).
			msaa_flags |= rsx::surface_state_flags::require_unresolve;
		}

		clear_rw_barrier();
	}

	void surface::load_background(command_recorder& cmd)
	{
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		texture* target = get_surface(rsx::surface_access::transfer_read);

		// The guest address is the offset into the guest window (HLD-02 §3.5).
		encoder->copyFromBuffer(m_resources->guest_buffer(), base_addr,
			rsx_pitch, rsx_pitch * get_surface_height<rsx::surface_metrics::samples>(),
			MTL::Size::Make(width(), height(), 1),
			target->value(), 0, 0, MTL::Origin::Make(0, 0, 0));

		if (sample_count() > 1)
		{
			msaa_flags |= rsx::surface_state_flags::require_unresolve;
		}
	}

	void surface::resolve(command_recorder& cmd)
	{
		if (sample_count() == 1 || !resolve_surface || !(msaa_flags & rsx::surface_state_flags::require_resolve))
		{
			return;
		}

		MTL4::RenderPassDescriptor* descriptor = MTL4::RenderPassDescriptor::alloc()->init();
		MTL::RenderPassColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
		attachment->setTexture(value());
		attachment->setResolveTexture(resolve_surface->value());
		attachment->setLoadAction(MTL::LoadActionLoad);
		attachment->setStoreAction(MTL::StoreActionMultisampleResolve);

		cmd.open_pass(descriptor, MTL4::RenderEncoderOptionNone);
		descriptor->release();

		msaa_flags = rsx::surface_state_flags::ready;
	}

	void surface::unresolve(command_recorder& cmd)
	{
		if (sample_count() == 1 || !resolve_surface || !(msaa_flags & rsx::surface_state_flags::require_unresolve))
		{
			return;
		}

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		encoder->copyFromTexture(resolve_surface->value(), 0, 0,
			MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width(), height(), 1),
			value(), 0, 0, MTL::Origin::Make(0, 0, 0));

		msaa_flags = rsx::surface_state_flags::ready;
	}

	void surface::write_to_memory(command_recorder& cmd, MTL::Buffer* target, u32 dst_offset, u32 src_offset, u32 write_length)
	{
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		// src_offset is a byte offset into the surface row space.
		const u32 row = src_offset / rsx_pitch;
		const u32 column = (src_offset % rsx_pitch) / get_bpp();
		const u32 rows = write_length / rsx_pitch;

		encoder->copyFromTexture(get_surface(rsx::surface_access::transfer_read)->value(), 0, 0,
			MTL::Origin::Make(column, row, 0),
			MTL::Size::Make(width() - column, rows, 1),
			target, dst_offset, rsx_pitch, rsx_pitch * rows);
	}
}

#endif // RSX_ENABLE_METAL4
