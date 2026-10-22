#include "MTLTextureCache.h"
#include "MTLBarrier.h"
#include "MTLInstrumentation.h"

#ifdef RSX_ENABLE_METAL4

#include "util/asm.hpp"
#include "util/logs.hpp"
#include "Emu/RSX/Utils/algorithm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mtl
{
	texture_extent normalize_texture_extent(MTL::TextureType type, u16 width, u16 height, u16 depth, u8 mipmaps)
	{
		texture_extent extent;
		extent.width = std::max<u16>(width, 1);
		extent.height = std::max<u16>(height, 1);
		extent.depth = (type == MTL::TextureType3D) ? std::max<u16>(depth, 1) : 1;

		if (type == MTL::TextureTypeCube)
		{
			// The RSX reads the face size from the width field and a cube face is
			// square, so the height follows.
			extent.height = extent.width;
		}

		const u32 largest = std::max<u32>(extent.width, std::max<u32>(extent.height, extent.depth));
		const u32 levels = rsx::floor_log2(largest) + 1;
		extent.mipmaps = static_cast<u8>(std::clamp<u32>(std::max<u32>(mipmaps, 1), 1, levels));
		return extent;
	}

	void texture_cache::upload_texture_data(command_recorder& cmd, texture* dst, u32 gcm_format, bool is_swizzled, const std::vector<rsx::subresource_layout>& layouts) const
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/upload");
		rsx::texture_uploader_capabilities caps
		{
			.supports_byteswap = false,
			.supports_vtc_decoding = false,
			.supports_hw_deswizzle = false,
			.supports_zero_copy = false,
			.supports_dxt = cmd.resources().supports_bc_texture_compression(),
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

			const bool decoded_dxt = !caps.supports_dxt &&
				(gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
				 gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
				 gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45);

			if (decoded_dxt ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8 ||
				gcm_format == CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8)
			{
				// The decode path emits one BGRA texel per guest texel.  A DXT
				// block is four by four, so its compressed row is much shorter than
				// the expanded row the uploader writes.
				row_pitch = utils::align<u32>(layout.width_in_texel * 4, 4);
				rows = layout.height_in_texel * layout.depth;
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

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the guest bytes and the converted bytes of the
			// uploads.  A swizzled compressed upload looks identical in the host
			// image whether the guest layout was interpreted or not, so the raw and
			// the staging bytes have to be captured together.
			if (probe::at_least(5))
			{
				static u32 diagnostic_uploads = 0;
				static u32 diagnostic_dumps = 0;
				static u64 diagnostic_dump_bytes = 0;
				static u64 diagnostic_keys[512] = {};
				static u32 diagnostic_key_count = 0;

				const u32 address = static_cast<u32>(reinterpret_cast<uintptr_t>(layout.data.data()));
				const u64 key = (static_cast<u64>(address) << 32) ^
					(static_cast<u64>(gcm_format) << 24) ^
					(static_cast<u64>(layout.width_in_texel) << 12) ^
					(static_cast<u64>(layout.height_in_texel) << 4) ^
					(static_cast<u64>(layout.level) << 1) ^
					static_cast<u64>(layout.layer);

				bool seen = false;
				for (u32 index = 0; index < diagnostic_key_count; ++index)
				{
					seen |= diagnostic_keys[index] == key;
				}

				const usz raw_size = layout.data.size();
				const usz scan_size = std::min<usz>({ raw_size, static_cast<usz>(size), 1024u * 1024u });
				const u8* raw = layout.data.data();
				const u8* converted = static_cast<const u8*>(staging->contents());
				u32 raw_nonzero = 0;
				u32 converted_nonzero = 0;
				u32 raw_distinct = 0;
				u32 converted_distinct = 0;
				u32 raw_first[4] = {};
				u32 converted_first[4] = {};

				for (usz index = 0; index < scan_size; index += sizeof(u32))
				{
					if (!raw)
					{
						break;
					}

					u32 word = 0;
					std::memcpy(&word, raw + index, std::min<usz>(sizeof(word), scan_size - index));
					raw_nonzero += word ? 1u : 0u;
					if (index < 16)
					{
						raw_first[index / 4] = word;
					}
				}

				for (usz index = 0; index < scan_size; index += sizeof(u32))
				{
					if (!converted)
					{
						break;
					}

					u32 word = 0;
					std::memcpy(&word, converted + index, std::min<usz>(sizeof(word), scan_size - index));
					converted_nonzero += word ? 1u : 0u;
					if (index < 16)
					{
						converted_first[index / 4] = word;
					}
				}

				if (raw)
				{
					u8 seen_bytes[256] = {};
					for (usz index = 0; index < raw_size && index < 1024u * 1024u; ++index)
					{
						if (!seen_bytes[raw[index]])
						{
							seen_bytes[raw[index]] = 1;
							raw_distinct++;
						}
					}
				}

				if (converted)
				{
					u8 seen_bytes[256] = {};
					for (u32 index = 0; index < size; ++index)
					{
						if (!seen_bytes[converted[index]])
						{
							seen_bytes[converted[index]] = 1;
							converted_distinct++;
						}
					}
				}

				MTL_NOTE(5, "Metal 4: upload %u, address 0x%x fmt 0x%x swizzled %u level %u layer %u, tex %ux%u block %ux%u depth %u pitch %u rows %u size %u raw %llu, guest %u/%llu nonzero distinct %u first %08x %08x %08x %08x, staging %u/%u nonzero distinct %u first %08x %08x %08x %08x",
					diagnostic_uploads++, address, gcm_format, is_swizzled ? 1u : 0u, layout.level, layout.layer,
					static_cast<u32>(layout.width_in_texel), static_cast<u32>(layout.height_in_texel),
					static_cast<u32>(layout.width_in_block), static_cast<u32>(layout.height_in_block),
					static_cast<u32>(layout.depth), row_pitch, rows, size,
					static_cast<unsigned long long>(raw_size),
					raw_nonzero, static_cast<unsigned long long>(std::max<usz>(raw_size, 1) / 4u), raw_distinct,
					raw_first[0], raw_first[1], raw_first[2], raw_first[3],
					converted_nonzero, size ? size / 4u : 0u, converted_distinct,
					converted_first[0], converted_first[1], converted_first[2], converted_first[3]);

				if (!seen && diagnostic_key_count < 512 && layout.level == 0)
				{
					diagnostic_keys[diagnostic_key_count++] = key;

					if (diagnostic_dumps < 512 && diagnostic_dump_bytes + raw_size + size <= 256u * 1024u * 1024u)
					{
						const u32 index = diagnostic_dumps++;
						const usz raw_write_size = std::min<usz>(raw_size, 16u * 1024u * 1024u);
						diagnostic_dump_bytes += raw_size + size;
						char path[192];

						std::snprintf(path, sizeof(path), "/tmp/mtl4-dump/guest-%u-addr%x-fmt%x-sw%u-%ux%u-sz%llu.bin",
							index, address, gcm_format, is_swizzled ? 1u : 0u,
							static_cast<u32>(layout.width_in_texel), static_cast<u32>(layout.height_in_texel),
							static_cast<unsigned long long>(raw_size));
						if (std::FILE* out = std::fopen(path, "wb"))
						{
							if (raw)
							{
								std::fwrite(raw, 1, raw_write_size, out);
							}
							std::fclose(out);
						}

						std::snprintf(path, sizeof(path), "/tmp/mtl4-dump/conv-%u-addr%x-fmt%x-sw%u-%ux%u-sz%u.bin",
							index, address, gcm_format, is_swizzled ? 1u : 0u,
							static_cast<u32>(layout.width_in_texel), static_cast<u32>(layout.height_in_texel), size);
						if (std::FILE* out = std::fopen(path, "wb"))
						{
							if (converted)
							{
								std::fwrite(converted, 1, size, out);
							}
							std::fclose(out);
						}
					}
				}
			}
#endif // MTL_DEBUG_LOGS

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

	void texture_cache::clear_texture(command_recorder& cmd, const texture* image, u32 gcm_format, u16 width, u16 height, u16 depth, u8 first_level, u8 level_count) const
	{
		if (!image || !level_count)
		{
			return;
		}

		const bool compressed =
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45;

		const u32 block_size = rsx::get_format_block_size_in_bytes(gcm_format);
		const MTL::TextureType type = image->value()->textureType();

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		for (u32 level = first_level; level < first_level + level_count; ++level)
		{
			if (level >= image->value()->mipmapLevelCount())
			{
				break;
			}

			// The level is a quarter of the previous one in each direction, and a
			// compressed level holds four by four blocks.
			const u32 level_width = std::max<u32>(1u, static_cast<u32>(width) >> level);
			const u32 level_height = std::max<u32>(1u, static_cast<u32>(height) >> level);

			u32 row_bytes = 0;
			u32 rows = 0;

			if (compressed)
			{
				row_bytes = (level_width / 4) * block_size;
				rows = level_height / 4;
			}
			else
			{
				row_bytes = level_width * block_size;
				rows = level_height;
			}

			if (!row_bytes || !rows)
			{
				continue;
			}

			const u32 slices = (type == MTL::TextureType3D) ? std::max<u32>(1u, depth) : 1u;
			const u64 size = static_cast<u64>(row_bytes) * rows * slices;

			// A shared buffer's contents are undefined too, so the zeroing is
			// explicit rather than an assumption about the allocator.
			MTL::Buffer* staging = cmd.resources().new_buffer(size, MTL::StorageModeShared);
			if (!staging)
			{
				return;
			}

			if (void* bytes = staging->contents())
			{
				std::memset(bytes, 0, size);
			}

			encoder->copyFromBuffer(staging, 0, row_bytes, row_bytes * rows,
				MTL::Size::Make(level_width, level_height, slices),
				image->value(), 0, level, MTL::Origin::Make(0, 0, 0));

			cmd.resources().defer_release(staging);
		}
	}

	void cached_texture_section::copy_texture(command_recorder& cmd, bool miss)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/readback copy");
		ensure(exists());

		// TODO(excise): level 2; a host image read back to guest memory, with the
		// guest format.  A depth guest format here is the depth-plane path.  One
		// latch per branch, because a latch registers its name once per call site.
		if (miss)
		{
			MTL_PROBE_LATCH(2, "texture/readback on miss");
		}
		else
		{
			MTL_PROBE_LATCH(2, "texture/readback on flush");
		}

		MTL_PROBE_HISTOGRAM(2, "texture readback format", static_cast<u32>(gcm_format), "gcm readback format");
		MTL_PROBE_HISTOGRAM(2, "texture readback context", static_cast<u32>(context), "readback context");

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

			{
				// TODO(excise): level 1; block timer.
				MTL_PERF_SCOPE(1, "texture/readback copy/barrier");
				surf->memory_barrier(cmd, rsx::surface_access::transfer_read);
			}

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
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "texture/readback copy/staging");
			m_staging.reset();
			m_staging = std::make_unique<buffer>(&cmd.resources());
			m_staging->assign(cmd.resources().new_buffer(staging_size, MTL::StorageModeShared));
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "texture/readback staging bytes", staging_size);
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

		// The guest depth pack is not the Metal depth plane (TX12, the texture-cache
		// site of SF9).  z16_uint happens to share the word, but z16_float and both
		// z24s8 forms need the upload decode inverted.  The plane copy is staged and
		// waited for, then the CPU encodes it into the guest-pitch staging row.
		rsx::surface_depth_format2 depth_format{};
		bool is_depth_section = false;

		if (context == rsx::texture_upload_context::framebuffer_storage)
		{
			depth_format = static_cast<surface*>(vram_texture)->get_surface_depth_format();
			is_depth_section = true;
		}
		else
		{
			is_depth_section = get_depth_texture_decode_format(gcm_format, depth_format);
		}

		if (is_depth_section && is_depth_texture() && depth_format_needs_decode(depth_format))
		{
			const u32 bpp = rsx::get_format_block_size_in_bytes(gcm_format);
			const u32 plane_pitch = transfer_width * 4;
			const u32 plane_rows = transfer_height;
			MTL::Buffer* plane = cmd.resources().new_buffer(static_cast<u64>(plane_pitch) * plane_rows, MTL::StorageModeShared);

			if (!plane)
			{
				rsx_log.error("Metal 4: the depth read-back staging buffer could not be created");
				return;
			}

			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(transfer_x, transfer_y, 0),
				MTL::Size::Make(transfer_width, transfer_height, 1),
				plane, 0, plane_pitch, plane_pitch * plane_rows,
				get_depth_plane_option(format, false));

			MTL::Buffer* stencil = nullptr;
			const u32 stencil_pitch = (transfer_width + 3u) & ~3u;
			const bool combined = rsx::is_depth_stencil_format(depth_format);
			if (combined)
			{
				stencil = cmd.resources().new_buffer(static_cast<u64>(stencil_pitch) * plane_rows, MTL::StorageModeShared);
				if (!stencil)
				{
					rsx_log.error("Metal 4: the stencil read-back staging buffer could not be created");
					cmd.resources().defer_release(plane);
					return;
				}

				encoder->copyFromTexture(source, 0, 0,
					MTL::Origin::Make(transfer_x, transfer_y, 0),
					MTL::Size::Make(transfer_width, transfer_height, 1),
					stencil, 0, stencil_pitch, stencil_pitch * plane_rows,
					get_depth_plane_option(format, true));
			}

			cmd.flush_for_readback();
			m_readback_cmd = nullptr;

			encode_depth_plane(static_cast<u8*>(m_staging->value()->contents()) + dst_offset,
				plane->contents(), transfer_width, transfer_height, rsx_pitch, bpp, depth_format,
				stencil ? stencil->contents() : nullptr, stencil ? stencil_pitch : 0);

			cmd.resources().defer_release(plane);
			if (stencil)
			{
				cmd.resources().defer_release(stencil);
			}

			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "texture/readback bytes", static_cast<u64>(rsx_pitch) * transfer_height);

			synchronized = true;
			sync_timestamp = rsx::get_shared_tag();
			return;
		}

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "texture/readback copy/encode");

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
		}

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "texture/readback bytes", static_cast<u64>(rsx_pitch) * transfer_height);

		m_readback_cmd = &cmd;

		synchronized = true;
		sync_timestamp = rsx::get_shared_tag();
	}

	void* cached_texture_section::map_synchronized(u32 offset, u32 size)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/readback wait");
		ensure(m_staging);
		ensure(offset + size <= m_staging->value()->length());

		if (m_readback_cmd)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "texture/readback wait/flush");
			m_readback_cmd->flush_for_readback();
			m_readback_cmd = nullptr;
		}

		return static_cast<u8*>(m_staging->value()->contents()) + offset;
	}

	texture* texture_cache::create_temporary_subresource_impl(command_recorder& cmd, texture* src, MTL::PixelFormat format, MTL::TextureType dst_type,
		u32 gcm_format, u16 width, u16 height, u16 depth, u8 mipmaps, const rsx::texture_channel_remap_t& remap, const copy_region_descriptor* copy)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/temp subresource");
		if (format == MTL::PixelFormat::PixelFormatInvalid)
		{
			format = get_texture_format(gcm_format, cmd.resources().supports_bc_texture_compression());
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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/transfer");
		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return;
		}

		// The destination is a pooled temporary, so an earlier draw of this frame may
		// still be sampling the image this copy overwrites.  The heaps are untracked,
		// so nothing orders that pair on its own, and both halves cross an encoder:
		// the edge is queue-scope, the same form every other copy in this backend
		// takes (MTLSurface.cpp:155, REVIEW-FINDINGS.md I9).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });

		const bool dst_3d = dst_image->value()->textureType() == MTL::TextureType3D;

		for (const auto& slice : sources)
		{
			if (!slice.src)
			{
				continue;
			}

			if (slice.src_w != slice.dst_w || slice.src_h != slice.dst_h)
			{
				rsx_log.error("Metal 4: scaled texture copy is not supported");
				continue;
			}

			const NS::UInteger dst_slice = dst_3d ? 0 : slice.dst_z;
			const NS::UInteger dst_z = dst_3d ? slice.dst_z : 0;

			const MTL::PixelFormat src_format = slice.src->pixel_format();
			const MTL::PixelFormat dst_format = dst_image->pixel_format();

			if (src_format != dst_format)
			{
				// Metal's texture copy needs one pixel format on both sides.  A
				// typeless copy is a bit copy between two layouts of the same texel
				// size, so stage it through a buffer: the two blits preserve the
				// guest words, and the destination view names the new format.
				const u32 src_texel = get_texel_size(src_format);
				const u32 dst_texel = get_texel_size(dst_format);

				if (src_texel == 0 || src_texel != dst_texel)
				{
					rsx_log.error("Metal 4: typeless texture copy from format %u to %u has no bit-preserving route",
						static_cast<u32>(src_format), static_cast<u32>(dst_format));
					continue;
				}

				// A 256-byte row keeps the blit inside Apple's alignment rule for
				// every texel size this backend creates.
				const u32 row_bytes = utils::align(slice.src_w * src_texel, 256u);
				const u32 image_bytes = row_bytes * slice.src_h;

				MTL::Buffer* staging = cmd.resources().new_buffer(utils::align<u64>(image_bytes, 0x1000), MTL::StorageModeShared);
				if (!staging)
				{
					rsx_log.error("Metal 4: typeless texture copy staging allocation failed");
					continue;
				}

				encoder->copyFromTexture(slice.src->value(), 0, 0,
					MTL::Origin::Make(slice.src_x, slice.src_y, 0),
					MTL::Size::Make(slice.src_w, slice.src_h, 1),
					staging, 0, row_bytes, image_bytes);

				// The staging buffer is in an untracked heap, so the second blit
				// needs an explicit edge after the first one.
				if (!emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageBlit }))
				{
					rsx_log.error("Metal 4: typeless texture copy barrier was refused");
					cmd.resources().defer_release(staging);
					continue;
				}

				encoder->copyFromBuffer(staging, 0, row_bytes, image_bytes,
					MTL::Size::Make(slice.src_w, slice.src_h, 1),
					dst_image->value(), dst_slice, slice.level,
					MTL::Origin::Make(slice.dst_x, slice.dst_y, dst_z));

				cmd.resources().defer_release(staging);
				continue;
			}

			encoder->copyFromTexture(slice.src->value(), 0, 0,
				MTL::Origin::Make(slice.src_x, slice.src_y, 0),
				MTL::Size::Make(slice.src_w, slice.src_h, 1),
				dst_image->value(), dst_slice, slice.level,
				MTL::Origin::Make(slice.dst_x, slice.dst_y, dst_z));
		}
	}

	texture_bind texture_cache::resolve_texture_bind(command_recorder& cmd, rsx::sampled_image_descriptor_base* descriptor,
		MTL::SamplerState* sampler, bool& deferred)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "texture/bind");
		deferred = false;

		if (!descriptor || !sampler)
		{
			return {};
		}

		auto* desc = static_cast<sampled_image_descriptor*>(descriptor);

		// TODO(excise): level 2; how each unit's image was classified.  The
		// texture that renders as a lit surface and the one that renders as an
		// emissive decal differ here, so this is the family that separates them.
		MTL_PROBE_HISTOGRAM(2, "texture bind context", static_cast<u32>(desc->upload_context), "upload context");
		MTL_PROBE_HISTOGRAM(2, "texture bind dimension", static_cast<u32>(desc->image_type), "bind dimension");
		MTL_PROBE_HISTOGRAM(2, "texture bind format class", static_cast<u32>(desc->format_class), "format class");

		if (desc->is_cyclic_reference)
		{
			// TODO(excise): level 2; the section is both a render target and a
			// sampled input, the case that needs an explicit pass boundary (I4).
			MTL_PROBE_LATCH(2, "texture/bind cyclic");
		}

		if (!desc->image_handle)
		{
			// TODO(excise): level 2; which deferred materialisation the unit took.
			MTL_PROBE_HISTOGRAM(2, "texture bind deferred op", static_cast<u32>(desc->external_subresource_desc.op), "deferred op");

			if (desc->external_subresource_desc.op == rsx::deferred_request_command::nop)
			{
				// TODO(excise): level 2; the unit is referenced but has no image.
				MTL_PROBE_LATCH(2, "texture/bind null");
				return {};
			}

			// The descriptor is legal without an image when it carries a deferred
			// copy or gather (`sampled_image_descriptor::validate`, texture_cache.h:570).
			// Materialize it before the render pass opens; the copy encoder is a
			// queue-form producer, and the caller records its write below.
			texture* image = create_temporary_subresource(cmd, desc->external_subresource_desc);
			if (!image)
			{
				// TODO(excise): level 2; a legal descriptor produced no image.
				MTL_PROBE_LATCH(2, "texture/bind dropped");
				return {};
			}

			// TODO(excise): level 2; a deferred copy or gather ran at bind.
			MTL_PROBE_LATCH(2, "texture/bind deferred");
			deferred = true;
			return { image->resource_id(), sampler->gpuResourceID() };
		}

		return { desc->image_handle->resource_id(), sampler->gpuResourceID() };
	}
}

#endif // RSX_ENABLE_METAL4
