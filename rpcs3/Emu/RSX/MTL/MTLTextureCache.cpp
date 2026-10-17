#include "MTLTextureCache.h"

#ifdef RSX_ENABLE_METAL4

#include "util/asm.hpp"
#include "util/logs.hpp"

namespace mtl
{
	void texture_cache::upload_texture_data(command_recorder& cmd, texture* dst, u32 gcm_format, bool is_swizzled, const std::vector<rsx::subresource_layout>& layouts) const
	{
		rsx::texture_uploader_capabilities caps
		{
			.supports_byteswap = false,
			.supports_vtc_decoding = false,
			.supports_hw_deswizzle = false,
			.supports_zero_copy = false,
			.supports_dxt = true,
			.alignment = 4
		};

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		for (const rsx::subresource_layout& layout : layouts)
		{
			if (layout.level >= dst->value()->mipmapLevelCount())
			{
				rsx_log.error("Metal 4: invalid subresource definition for the output texture. Mip level does not exist.");
				continue;
			}

			u32 row_pitch;
			u32 rows;

			if (gcm_format == CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8 || gcm_format == CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8)
			{
				// The decode path emits one BGRA texel per block texel.
				row_pitch = utils::align<u32>(layout.width_in_texel * 4, 4);
				rows = layout.height_in_texel;
			}
			else
			{
				row_pitch = utils::align<u32>(layout.width_in_block * rsx::get_format_block_size_in_bytes(gcm_format), 4);
				rows = layout.height_in_block * layout.depth;
			}

			const u32 size = row_pitch * rows;
			MTL::Buffer* staging = cmd.resources().new_buffer(size, MTL::StorageModeShared);

			rsx::io_buffer io_buf(staging->contents(), size);
			rsx::upload_texture_subresource(io_buf, layout, gcm_format, is_swizzled, caps);

			const MTL::TextureType type = dst->value()->textureType();
			const NS::UInteger slice = (type == MTL::TextureTypeCube || type == MTL::TextureType2DArray) ? layout.layer : 0;
			const NS::UInteger z = (type == MTL::TextureType3D) ? layout.layer : 0;

			rsx::surface_depth_format2 depth_format{};
			if (get_depth_texture_decode_format(gcm_format, depth_format) && depth_format_needs_decode(depth_format))
			{
				const u32 plane_pitch = layout.width_in_texel * 4;
				const u32 plane_rows = layout.height_in_texel * layout.depth;
				MTL::Buffer* plane = cmd.resources().new_buffer(plane_pitch * plane_rows, MTL::StorageModeShared);

				decode_depth_plane(plane->contents(), staging->contents(),
					layout.width_in_texel, plane_rows, row_pitch,
					rsx::get_format_block_size_in_bytes(gcm_format), depth_format);

				encoder->copyFromBuffer(plane, 0, plane_pitch, plane_pitch * plane_rows,
					MTL::Size::Make(layout.width_in_texel, layout.height_in_texel, layout.depth),
					dst->value(), slice, layout.level, MTL::Origin::Make(0, 0, z),
					get_depth_plane_option(dst->value()->pixelFormat(), false));

				cmd.resources().defer_release(plane);
				cmd.resources().defer_release(staging);
			}
			else
			{
				encoder->copyFromBuffer(staging, 0, row_pitch, row_pitch * rows,
					MTL::Size::Make(layout.width_in_texel, layout.height_in_texel, layout.depth),
					dst->value(), slice, layout.level, MTL::Origin::Make(0, 0, z));

				cmd.resources().defer_release(staging);
			}
		}
	}

	void cached_texture_section::copy_texture(command_recorder& cmd, bool miss)
	{
		ensure(exists());

		if (!miss) [[likely]]
		{
			baseclass::on_speculative_flush();
		}
		else
		{
			baseclass::on_miss();
		}

		texture* target = vram_texture;
		u32 transfer_width = width;
		u32 transfer_height = height;
		u32 transfer_x = 0;
		u32 transfer_y = 0;

		if (context == rsx::texture_upload_context::framebuffer_storage)
		{
			auto* surf = static_cast<surface*>(vram_texture);
			surf->memory_barrier(cmd, rsx::surface_access::transfer_read);

			// The resolved image is pixel-sized, not sample-sized: Metal resolves a
			// multisample image in place, so the region needs no sample scaling
			// (VKTextureCache.h:241-261 scales because its image is sample-sized).
			target = surf->get_surface(rsx::surface_access::transfer_read);
		}

		const auto valid_range = get_confirmed_range();
		if (const auto section_range = get_section_range(); section_range != valid_range)
		{
			if (const auto offset = (valid_range.start - get_section_base()))
			{
				transfer_y = offset / rsx_pitch;
				transfer_x = (offset % rsx_pitch) / rsx::get_format_block_size_in_bytes(gcm_format);

				ensure(transfer_width >= transfer_x);
				ensure(transfer_height >= transfer_y);
				transfer_width -= transfer_x;
				transfer_height -= transfer_y;
			}

			if (const auto tail = (section_range.end - valid_range.end))
			{
				const auto row_count = tail / rsx_pitch;

				ensure(transfer_height >= row_count);
				transfer_height -= row_count;
			}
		}

		const u32 staging_size = std::max<u32>(rsx_pitch * height, static_cast<u32>(get_section_size()));
		if (!m_staging || m_staging->value()->length() < staging_size)
		{
			m_staging.reset();
			m_staging = std::make_unique<buffer>(&cmd.resources());
			m_staging->assign(cmd.resources().new_buffer(staging_size, MTL::StorageModeShared));
		}

		real_pitch = rsx_pitch;

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		// The staging buffer keeps the guest row pitch, so the base imp_flush is a
		// straight memcpy (texture_cache_utils.h:1612).
		const u32 dst_offset = transfer_y * rsx_pitch + transfer_x * rsx::get_format_block_size_in_bytes(gcm_format);

		MTL::Texture* source = target->value();
		const MTL::PixelFormat format = source->pixelFormat();

		if (format == MTL::PixelFormat::PixelFormatDepth32Float_Stencil8)
		{
			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(transfer_x, transfer_y, 0),
				MTL::Size::Make(transfer_width, transfer_height, 1),
				m_staging->value(), dst_offset, rsx_pitch, rsx_pitch * transfer_height,
				get_depth_plane_option(format, false));
		}
		else
		{
			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(transfer_x, transfer_y, 0),
				MTL::Size::Make(transfer_width, transfer_height, 1),
				m_staging->value(), dst_offset, rsx_pitch, rsx_pitch * transfer_height);
		}

		m_readback_cmd = &cmd;

		synchronized = true;
		sync_timestamp = rsx::get_shared_tag();
	}

	void* cached_texture_section::map_synchronized(u32 offset, u32 size)
	{
		ensure(m_staging);
		ensure(offset + size <= m_staging->value()->length());

		if (m_readback_cmd)
		{
			m_readback_cmd->flush_for_readback();
			m_readback_cmd = nullptr;
		}

		return static_cast<u8*>(m_staging->value()->contents()) + offset;
	}

	texture* texture_cache::create_temporary_subresource_impl(command_recorder& cmd, texture* src, MTL::PixelFormat format, MTL::TextureType dst_type,
		u32 gcm_format, u16 width, u16 height, u16 depth, u8 mipmaps, const rsx::texture_channel_remap_t& remap, const copy_region_descriptor* copy)
	{
		if (format == MTL::PixelFormat::PixelFormatInvalid)
		{
			format = get_texture_format(gcm_format);
		}

		temporary_texture* dst = nullptr;
		const u64 match_key = encode_properties(format, dst_type, width, height, depth, mipmaps);

		for (auto& e : m_temporary_surfaces)
		{
			if (e->has_refs())
			{
				continue;
			}

			if (e->properties_encoding == match_key)
			{
				dst = e.get();
				break;
			}
		}

		if (!dst)
		{
			auto data = std::make_unique<temporary_texture>(&cmd.resources());

			MTL::TextureDescriptor* descriptor = make_descriptor(format, dst_type, width, height, depth, mipmaps);
			data->assign(cmd.resources().new_texture(descriptor, MTL::StorageModePrivate));
			descriptor->release();

			data->set_format_class(rsx::classify_format(gcm_format));
			dst = data.get();
			dst->properties_encoding = match_key;
			m_temporary_surfaces.emplace_back(std::move(data));
		}

		dst->add_ref();

		if (copy)
		{
			rsx::simple_array<copy_region_descriptor> region = { *copy };
			copy_transfer_regions_impl(cmd, dst, region);
		}

		if (!src || src->pixel_format() != format)
		{
			// A data cast was done, so apply the base component map.
			dst->set_native_component_layout(get_component_mapping(gcm_format, rsx::component_order::default_));
		}
		else
		{
			dst->set_native_component_layout(src->get_native_component_layout());
		}

		return dst->get_view(remap);
	}

	void texture_cache::copy_transfer_regions_impl(command_recorder& cmd, texture* dst_image, const rsx::simple_array<copy_region_descriptor>& sources) const
	{
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		const bool dst_3d = dst_image->value()->textureType() == MTL::TextureType3D;

		for (const auto& slice : sources)
		{
			if (!slice.src)
			{
				continue;
			}

			if (slice.src->pixel_format() != dst_image->pixel_format())
			{
				rsx_log.error("Metal 4: typeless texture copy is not supported");
				continue;
			}

			if (slice.src_w != slice.dst_w || slice.src_h != slice.dst_h)
			{
				rsx_log.error("Metal 4: scaled texture copy is not supported");
				continue;
			}

			const NS::UInteger dst_slice = dst_3d ? 0 : slice.dst_z;
			const NS::UInteger dst_z = dst_3d ? slice.dst_z : 0;

			encoder->copyFromTexture(slice.src->value(), 0, 0,
				MTL::Origin::Make(slice.src_x, slice.src_y, 0),
				MTL::Size::Make(slice.src_w, slice.src_h, 1),
				dst_image->value(), dst_slice, slice.level,
				MTL::Origin::Make(slice.dst_x, slice.dst_y, dst_z));
		}
	}

	texture_bind texture_cache::resolve_texture_bind(const rsx::sampled_image_descriptor_base* descriptor, MTL::SamplerState* sampler)
	{
		if (!descriptor || !sampler)
		{
			return {};
		}

		const auto* desc = static_cast<const sampled_image_descriptor*>(descriptor);
		if (!desc->image_handle)
		{
			return {};
		}

		return { desc->image_handle->resource_id(), sampler->gpuResourceID() };
	}
}

#endif // RSX_ENABLE_METAL4
