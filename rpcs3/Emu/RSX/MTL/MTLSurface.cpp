#include "MTLSurface.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"

#include "Emu/RSX/Common/TextureUtils.h"

#include "util/logs.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace mtl
{
	// A content transfer fills a surface in its own encoder.  The heaps are untracked
	// and Metal 4 encoders may overlap, so the transfer must complete before the
	// encoder that next samples or loads the surface (HLD-03 §3.2).  The producer
	// form is the one a render pass can encode: its own tile store follows the
	// barrier, so the consumer form would leave the window the resolve path already
	// closes this way (REVIEW-FINDINGS.md, B6).
	static void emit_content_barrier(MTL4::CommandEncoder* encoder, [[maybe_unused]] const surface& target, [[maybe_unused]] const char* reason)
	{
		emit_producer_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageAll });
	}

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

		// The copies read source surfaces an earlier pass wrote, so that edge crosses
		// an encoder too.  It is the queue-scope form every other copy in the backend
		// takes (mirror_depth, copy_transfer_regions_impl).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit | MTL::StageDispatch, MTL4::VisibilityOptionDevice, true });

		u64 newest_tag = 0;
		bool optimize_copy = true;
		bool copied = false;

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
			copied = true;
		}

		// The copies landed in the target image, and the next encoder samples or
		// loads it, so they must complete first.
		if (copied)
		{
			emit_content_barrier(encoder, *this, "copy in");
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

		// The clear lands in the attachment, and the next encoder samples or loads it.
		if (MTL4::RenderCommandEncoder* encoder = cmd.open_pass(descriptor))
		{
			emit_content_barrier(encoder, *this, "clear");
		}

		descriptor->release();
	}

	void surface::load_background(command_recorder& cmd)
	{
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
		else if (!m_depth && packed_format_needs_convert(get_surface_color_format()))
		{
			// The host stores blue in the high field, so the image
			// cannot take the guest bytes as-is (and the x1/o1 bit would move).  The decode
			// is the inverse of the depth upload above: decode into a shared staging
			// buffer, then fill the image from it.
			const u32 plane_pitch = width() * 2;
			MTL::Buffer* staging = m_resources->new_buffer(static_cast<u64>(plane_pitch) * height(), MTL::StorageModeShared);

			if (!staging)
			{
				rsx_log.error("Metal 4: the packed colour background staging buffer could not be created");
				return;
			}

			decode_packed16_plane(staging->contents(), static_cast<const u8*>(m_resources->guest_buffer()->contents()) + base_addr,
				width(), height(), rsx_pitch, get_surface_color_format());

			encoder->copyFromBuffer(staging, 0, plane_pitch, plane_pitch * height(),
				MTL::Size::Make(width(), height(), 1),
				target->value(), 0, 0, MTL::Origin::Make(0, 0, 0));

			cmd.resources().defer_release(staging);
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

		// The background copy is the surface's first content, and the next encoder
		// samples or loads it.
		emit_content_barrier(encoder, *this, "background");
	}

	void surface::resolve(command_recorder& cmd)
	{
		if (sample_count() == 1 || !resolve_surface || !(msaa_flags & rsx::surface_state_flags::require_resolve))
		{
			return;
		}


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

	surface_write_range clip_surface_write_range(u32 src_offset, u32 write_length, u32 pitch, u32 height)
	{
		surface_write_range range{};

		if (!write_length || !pitch || !height)
		{
			return range;
		}

		const u64 begin = src_offset;
		const u64 end = begin + write_length;
		const u64 first_row = std::min<u64>(begin / pitch, height);
		const u64 last_row = std::min<u64>((end + pitch - 1) / pitch, height);

		if (first_row >= last_row)
		{
			return range;
		}

		range.first_row = static_cast<u32>(first_row);
		range.rows = static_cast<u32>(last_row - first_row);
		range.row_offset = static_cast<u32>(begin - first_row * pitch);

		const u64 span = static_cast<u64>(range.rows) * pitch;
		range.bytes = static_cast<u32>(std::min<u64>(write_length, span - range.row_offset));
		return range;
	}

	// Nearest-neighbour resample of one row between planes of different widths.
	// The scaled read-back and the format decodes both land here.
	static void resample_plane_row(u8* dst, const u8* src, u32 dst_width, u32 src_width, u32 bytes_per_pixel)
	{
		if (dst_width == src_width)
		{
			std::memcpy(dst, src, static_cast<usz>(dst_width) * bytes_per_pixel);
			return;
		}

		for (u32 x = 0; x < dst_width; ++x)
		{
			const u32 source_x = static_cast<u32>(static_cast<u64>(x) * src_width / dst_width);
			std::memcpy(dst + static_cast<usz>(x) * bytes_per_pixel,
				src + static_cast<usz>(source_x) * bytes_per_pixel, bytes_per_pixel);
		}
	}

	// The guest asks for a *byte range* of the surface in guest row layout
	// (surface_store.h:995-1014): src_offset, dst_offset and write_length are
	// arbitrary bytes, so a range can start and end inside a row.  Mirror Vulkan
	// (VKRenderTargets.h:635-698): materialise the rows the range touches at the
	// guest pitch, then move exactly write_length bytes.  A row rectangle would
	// drop a partial row and shift a range whose destination is not row aligned.
	void surface::write_to_memory(command_recorder& cmd, MTL::Buffer* target, u32 dst_offset, u32 src_offset, u32 write_length)
	{

		if (!target || !write_length || !rsx_pitch || !surface_width || !surface_height)
		{
			return;
		}

		const surface_write_range range = clip_surface_write_range(src_offset, write_length, rsx_pitch, surface_height);

		if (!range.rows)
		{
			return;
		}

		const u64 first_row = range.first_row;
		const u32 row_offset = range.row_offset;
		u32 rows = range.rows;

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		MTL::Texture* source = get_surface(rsx::surface_access::transfer_read)->value();
		if (!source)
		{
			return;
		}

		const u32 host_width = static_cast<u32>(source->width());
		const u32 host_height = static_cast<u32>(source->height());
		const bool scaled = host_width != surface_width || host_height != surface_height;

		// The host rows those guest rows read.  Unscaled, one guest row is one
		// host row; scaled, a guest row samples the nearest host row.
		const u32 host_first = scaled
			? std::min<u32>(static_cast<u32>(static_cast<u64>(first_row) * host_height / surface_height), host_height - 1)
			: static_cast<u32>(first_row);
		const u64 guest_end_row = first_row + rows;
		const u32 host_last = scaled
			? std::min<u32>(host_height, static_cast<u32>((guest_end_row * host_height + surface_height - 1) / surface_height))
			: std::min<u32>(static_cast<u32>(guest_end_row), host_height);

		if (host_first >= host_last)
		{
			return;
		}

		rows = std::min<u32>(rows, host_last - host_first);
		const u64 span = static_cast<u64>(rows) * rsx_pitch;
		const u32 bytes = static_cast<u32>(std::min<u64>(write_length, span - row_offset));

		if (!bytes)
		{
			return;
		}

		const u32 host_rows = host_last - host_first;
		const u32 bpp = get_bpp();
		if (!bpp)
		{
			return;
		}

		const MTL::PixelFormat format = source->pixelFormat();
		const rsx::surface_depth_format2 depth_format = get_surface_depth_format();
		const rsx::surface_color_format color_format = get_surface_color_format();
		const bool depth_decode = m_depth && depth_format_needs_decode(depth_format);
		const bool packed_convert = !m_depth && packed_format_needs_convert(color_format);

		// The host row a guest row samples, relative to host_first.
		const auto host_row_offset = [&](u32 guest_row)
		{
			return scaled
				? static_cast<u32>(static_cast<u64>(guest_row) * host_height / surface_height) - host_first
				: guest_row - static_cast<u32>(first_row);
		};

		// Fetch the host rows of one plane into a shared buffer.  The copy is
		// asynchronous; every path below waits once before the CPU touches it.
		const auto fetch_host_rows = [&](u32 bytes_per_pixel, MTL::BlitOption option) -> MTL::Buffer*
		{
			MTL::Buffer* plane = m_resources->new_buffer(
				static_cast<u64>(host_width) * host_rows * bytes_per_pixel, MTL::StorageModeShared);
			if (!plane)
			{
				return nullptr;
			}

			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(0, host_first, 0),
				MTL::Size::Make(host_width, host_rows, 1),
				plane, 0, host_width * bytes_per_pixel, host_width * bytes_per_pixel * host_rows,
				option);
			return plane;
		};

		// The guest bytes of the requested rows, in guest row layout.  The last
		// memcpy takes the requested subrange out of it.
		std::vector<u8> staging(static_cast<usz>(span));

		if (depth_decode)
		{
			MTL::Buffer* plane = fetch_host_rows(4, get_depth_plane_option(format, false));
			if (!plane)
			{
				rsx_log.error("Metal 4: the depth read-back staging buffer could not be created");
				return;
			}

			const bool combined = rsx::is_depth_stencil_format(depth_format);
			MTL::Buffer* stencil = combined ? fetch_host_rows(1, get_depth_plane_option(format, true)) : nullptr;
			if (combined && !stencil)
			{
				rsx_log.error("Metal 4: the stencil read-back staging buffer could not be created");
				m_resources->defer_release(plane);
				return;
			}

			cmd.flush_for_readback();

			std::vector<u8> depth_row(static_cast<usz>(surface_width) * 4);
			std::vector<u8> stencil_row(combined ? surface_width : 0);

			for (u32 y = 0; y < rows; ++y)
			{
				const u32 source_row = host_row_offset(static_cast<u32>(first_row) + y);
				const u8* const plane_row = static_cast<const u8*>(plane->contents()) + static_cast<usz>(source_row) * host_width * 4;
				resample_plane_row(depth_row.data(), plane_row, surface_width, host_width, 4);

				if (combined)
				{
					const u8* const stencil_plane_row = static_cast<const u8*>(stencil->contents()) + static_cast<usz>(source_row) * host_width;
					resample_plane_row(stencil_row.data(), stencil_plane_row, surface_width, host_width, 1);
				}

				encode_depth_plane(staging.data() + static_cast<usz>(y) * rsx_pitch, depth_row.data(),
					surface_width, 1, rsx_pitch, bpp, depth_format,
					combined ? stencil_row.data() : nullptr, surface_width);
			}

			m_resources->defer_release(plane);
			if (stencil)
			{
				m_resources->defer_release(stencil);
			}
		}
		else if (packed_convert)
		{
			MTL::Buffer* plane = fetch_host_rows(2, MTL::BlitOptionNone);
			if (!plane)
			{
				rsx_log.error("Metal 4: the packed colour read-back staging buffer could not be created");
				return;
			}

			cmd.flush_for_readback();

			std::vector<u8> packed_row(static_cast<usz>(surface_width) * 2);

			for (u32 y = 0; y < rows; ++y)
			{
				const u32 source_row = host_row_offset(static_cast<u32>(first_row) + y);
				resample_plane_row(packed_row.data(),
					static_cast<const u8*>(plane->contents()) + static_cast<usz>(source_row) * host_width * 2,
					surface_width, host_width, 2);
				encode_packed16_plane(staging.data() + static_cast<usz>(y) * rsx_pitch, packed_row.data(),
					surface_width, 1, rsx_pitch, color_format);
			}

			m_resources->defer_release(plane);
		}
		else if (scaled)
		{
			MTL::Buffer* plane = fetch_host_rows(bpp, MTL::BlitOptionNone);
			if (!plane)
			{
				rsx_log.error("Metal 4: the scaled read-back staging buffer could not be created");
				return;
			}

			cmd.flush_for_readback();

			for (u32 y = 0; y < rows; ++y)
			{
				const u32 source_row = host_row_offset(static_cast<u32>(first_row) + y);
				resample_plane_row(staging.data() + static_cast<usz>(y) * rsx_pitch,
					static_cast<const u8*>(plane->contents()) + static_cast<usz>(source_row) * host_width * bpp,
					surface_width, host_width, bpp);
			}

			m_resources->defer_release(plane);
		}
		else
		{
			// Unscaled colour: the guest row is the host row and the guest pitch is
			// the blit row pitch, so the GPU can fill the staging buffer directly.
			MTL::Buffer* scratch = m_resources->new_buffer(span, MTL::StorageModeShared);
			if (!scratch)
			{
				rsx_log.error("Metal 4: the surface read-back staging buffer could not be created");
				return;
			}

			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(0, host_first, 0),
				MTL::Size::Make(surface_width, rows, 1),
				scratch, 0, rsx_pitch, rsx_pitch * rows);

			// A buffer-to-buffer blit needs four-byte aligned offsets and length.
			// When the guest range is aligned the copy stays in the command stream
			// exactly where it used to be, with no extra drain; otherwise the CPU
			// takes the bytes after one bounded wait.
			if (!((dst_offset | row_offset | bytes) & 3u))
			{
				encoder->copyFromBuffer(scratch, row_offset, target, dst_offset, bytes);
				m_resources->defer_release(scratch);
				return;
			}

			cmd.flush_for_readback();

			if (u8* const dst = static_cast<u8*>(target->contents()))
			{
				std::memcpy(dst + dst_offset, static_cast<u8*>(scratch->contents()) + row_offset, bytes);
			}

			m_resources->defer_release(scratch);
			return;
		}

		if (u8* const dst = static_cast<u8*>(target->contents()))
		{
			std::memcpy(dst + dst_offset, staging.data() + row_offset, bytes);
		}
	}

}

#endif // RSX_ENABLE_METAL4
