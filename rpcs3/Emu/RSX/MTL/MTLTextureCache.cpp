#include "MTLTextureCache.h"
#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

#include "util/asm.hpp"
#include "util/logs.hpp"
#include "Emu/RSX/Utils/algorithm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mtl
{
	// A clear writes the guest's fill word into a staging buffer and copies the
	// level whole, so the row padding carries the fill too.  A byte memset cannot
	// express an arbitrary word, so the fill is word-wise with a short tail (a
	// level byte size is not always a multiple of four).
	static void fill_staging_words(void* output, u64 size, u32 word)
	{
		u8* out = static_cast<u8*>(output);

		for (u64 index = 0; index < size;)
		{
			const u32 chunk = static_cast<u32>(std::min<u64>(size - index, sizeof(word)));
			std::memcpy(out + index, &word, chunk);
			index += chunk;
		}
	}

	texture_extent normalize_texture_extent(MTL::TextureType type, u16 width, u16 height, u16 depth, u8 mipmaps)
	{
		texture_extent extent;
		extent.width = std::max<u16>(width, 1);
		extent.height = std::max<u16>(height, 1);
		extent.depth = (type == MTL::TextureType3D) ? std::max<u16>(depth, 1) : 1;

		if (type == MTL::TextureType1D)
		{
			// A 1-D Metal texture has exactly one row; the guest's height field
			// carries no meaning for it.
			extent.height = 1;
		}

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

	texture_level_extent get_texture_level_extent(MTL::TextureType type, u32 width, u32 height, u32 depth, u32 level, u32 block_size, bool compressed)
	{
		// A mip level halves in each direction down to one texel, and a 3-D level
		// also halves in depth.  The clear path used the base depth for every level
		// and overran the last slices of a volume.
		const u32 shift = std::min<u32>(level, 15);
		const u32 level_width = std::max<u32>(1u, width >> shift);
		const u32 level_height = std::max<u32>(1u, height >> shift);
		const u32 level_depth = (type == MTL::TextureType3D) ? std::max<u32>(1u, depth >> shift) : 1u;

		texture_level_extent extent;
		extent.width = level_width;
		extent.height = level_height;
		extent.depth = level_depth;

		if (!compressed)
		{
			extent.row_bytes = level_width * block_size;
			extent.rows = level_height;
			return extent;
		}

		// Every DXT format is four by four.  A level smaller than one block is
		// still stored as one whole block, so the block count is the ceiling and
		// never zero (TextureUtils.cpp:664-668).
		constexpr u32 block_edge = 4;
		const u32 blocks_x = std::max<u32>(1u, (level_width + block_edge - 1) / block_edge);
		const u32 blocks_y = std::max<u32>(1u, (level_height + block_edge - 1) / block_edge);
		extent.row_bytes = blocks_x * block_size;
		extent.rows = blocks_y;
		return extent;
	}

	texture_readback_region get_texture_readback_region(u32 width, u32 height, u32 pitch, u32 block_size, bool compressed, u32 byte_offset, u32 tail)
	{
		texture_readback_region region;
		region.width = width;
		region.height = height;

		// A compressed guest row is a row of blocks, so both the row index and the
		// x byte offset are block indices; multiply by the block edge to reach the
		// texel origin Metal takes.
		const u32 block_edge = compressed ? 4u : 1u;

		if (byte_offset && pitch)
		{
			region.y = (byte_offset / pitch) * block_edge;
			region.x = ((byte_offset % pitch) / block_size) * block_edge;
			region.width = (width > region.x) ? width - region.x : 0;
			region.height = (height > region.y) ? height - region.y : 0;

			// The staging buffer keeps the guest layout from the section base, so
			// the destination is the raw byte offset, not the texel origin.
			region.byte_offset = byte_offset;
		}

		if (tail && pitch)
		{
			const u32 rows = (tail / pitch) * block_edge;
			region.height = (region.height > rows) ? region.height - rows : 0;
		}

		return region;
	}

	void texture_cache::upload_texture_data(command_recorder& cmd, texture* dst, u32 gcm_format, bool is_swizzled, const std::vector<rsx::subresource_layout>& layouts) const
	{
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

		// The uploader decodes one subresource per call, so every level has the
		// same decoded row shape; only the extent changes.  Size them all first and
		// carve one staging buffer: per level, a separate allocation also costs a
		// heap search, a residency commit and a free-list splice (P2).
		const bool decoded_dxt = !caps.supports_dxt &&
			(gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
			 gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
			 gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45);

		// On Apple the uploader expands the packed R6G5B5 upload to one BGRA8
		// texel per guest texel (Metal does not hold the format); the staging
		// buffer must cover the expanded rows.
#if defined(__APPLE__)
		const bool expanded_r6g5b5 = gcm_format == CELL_GCM_TEXTURE_R6G5B5;
#else
		const bool expanded_r6g5b5 = false;
#endif
		const bool expanded_blocks = decoded_dxt || expanded_r6g5b5 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8;

		struct upload_span
		{
			u64 offset = 0;
			u32 size = 0;
			u32 row_pitch = 0;
			u32 rows = 0;
		};

		std::vector<upload_span> spans(layouts.size());
		u64 total_size = 0;

		for (usz index = 0; index < layouts.size(); ++index)
		{
			const rsx::subresource_layout& layout = layouts[index];
			upload_span& span = spans[index];

			if (layout.level >= dst->value()->mipmapLevelCount())
			{
				rsx_log.error("Metal 4: invalid subresource definition for the output texture. Mip level does not exist.");
				continue;
			}

			if (expanded_blocks)
			{
				// The decode path emits one BGRA texel per guest texel.  A DXT block
				// is four by four, so its compressed row is much shorter than the
				// expanded row the uploader writes.
				span.row_pitch = utils::align<u32>(layout.width_in_texel * 4, 4);
				span.rows = layout.height_in_texel * layout.depth;
			}
			else
			{
				span.row_pitch = utils::align<u32>(layout.width_in_block * rsx::get_format_block_size_in_bytes(gcm_format), 4);
				span.rows = layout.height_in_block * layout.depth;
			}

			span.size = span.row_pitch * span.rows;

			if (!span.size)
			{
				continue;
			}

			span.offset = total_size;
			total_size = utils::align<u64>(total_size + span.size, 16);
		}

		if (!total_size)
		{
			return;
		}

		MTL::Buffer* staging = cmd.resources().new_buffer(total_size, MTL::StorageModeShared);
		if (!staging)
		{
			// A failed allocation skips the upload rather than faulting: the
			// destination keeps what it held, which is what the clear path does.
			rsx_log.error("Metal 4: the %llu byte upload staging buffer could not be created",
				static_cast<unsigned long long>(total_size));
			return;
		}

		u8* const staging_base = static_cast<u8*>(staging->contents());

		for (usz index = 0; index < layouts.size(); ++index)
		{
			const rsx::subresource_layout& layout = layouts[index];
			const upload_span& span = spans[index];

			if (!span.size)
			{
				continue;
			}

			// The uploader writes the whole subresource, so every level starts at its
			// own carved offset and the copies below can run asynchronously.
			u8* const staging_data = staging_base + span.offset;
			rsx::io_buffer io_buf(staging_data, span.size);
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

				decode_depth_plane(plane->contents(), staging_data,
					layout.width_in_texel, plane_rows, span.row_pitch,
					rsx::get_format_block_size_in_bytes(gcm_format), depth_format);

				encoder->copyFromBuffer(plane, 0, plane_pitch, plane_pitch * plane_rows,
					MTL::Size::Make(layout.width_in_texel, layout.height_in_texel, layout.depth),
					dst->value(), slice, layout.level, MTL::Origin::Make(0, 0, z),
					get_depth_plane_option(dst->value()->pixelFormat(), false));

				cmd.resources().defer_release(plane);
			}
			else
			{
				encoder->copyFromBuffer(staging, span.offset, span.row_pitch, span.row_pitch * span.rows,
					MTL::Size::Make(layout.width_in_texel, layout.height_in_texel, layout.depth),
					dst->value(), slice, layout.level, MTL::Origin::Make(0, 0, z));
			}
		}

		cmd.resources().defer_release(staging);
	}

	void texture_cache::clear_texture(command_recorder& cmd, const texture* image, u32 gcm_format, u8 first_level, u8 level_count, u32 fill_word) const
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

		// The host image defines the region a clear may touch.  The guest fields
		// can describe a cube's six faces or a mip chain the host clamped, so a
		// clear driven by them can address past a level or outside the allocation.
		const u32 base_width = static_cast<u32>(image->value()->width());
		const u32 base_height = static_cast<u32>(image->value()->height());
		const u32 base_depth = (type == MTL::TextureType3D) ? static_cast<u32>(image->value()->depth()) : 1u;

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
			// compressed level holds four by four blocks.  A level below one block
			// still occupies a whole block, and a 3-D level shrinks in depth too.
			const texture_level_extent extent = get_texture_level_extent(
				type, base_width, base_height, base_depth, level, block_size, compressed);

			if (!extent.row_bytes || !extent.rows)
			{
				continue;
			}

			const u64 size = static_cast<u64>(extent.row_bytes) * extent.rows * extent.depth;

			// A shared buffer's contents are undefined too, so the fill is
			// explicit rather than an assumption about the allocator.
			MTL::Buffer* staging = cmd.resources().new_buffer(size, MTL::StorageModeShared);
			if (!staging)
			{
				return;
			}

			if (void* bytes = staging->contents())
			{
				fill_staging_words(bytes, size, fill_word);
			}

			// A cube keeps its six faces in the slice dimension.  A 3-D volume
			// carries its depth in the copy size.  Only the cube repeats the copy.
			const u32 slices = (type == MTL::TextureTypeCube) ? 6u : 1u;

			for (u32 slice = 0; slice < slices; ++slice)
			{
				encoder->copyFromBuffer(staging, 0, extent.row_bytes, extent.row_bytes * extent.rows,
					MTL::Size::Make(extent.width, extent.height, extent.depth),
					image->value(), slice, level, MTL::Origin::Make(0, 0, 0));
			}

			cmd.resources().defer_release(staging);
		}

		// The reused image may hold tile content of the pass that wrote it, and the copies above are blits:
		// the producer-side queue edge makes this encoder's writes visible to everything after it, matching
		// the resolve path (MTLSurface.cpp:533) and clear_memory/load_background.  On untracked heaps there
		// is no other edge before the next sample of this image.
		emit_producer_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionDevice });
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

		if (context == rsx::texture_upload_context::framebuffer_storage)
		{
			auto* surf = static_cast<surface*>(vram_texture);

			{
				surf->memory_barrier(cmd, rsx::surface_access::transfer_read);
			}

			// The resolved image is pixel-sized, not sample-sized: Metal resolves a
			// multisample image in place, so the region needs no sample scaling
			// (VKTextureCache.h:241-261 scales because its image is sample-sized).
			target = surf->get_surface(rsx::surface_access::transfer_read);
		}

		const bool compressed =
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 ||
			gcm_format == CELL_GCM_TEXTURE_COMPRESSED_DXT45;
		const u32 block_size = rsx::get_format_block_size_in_bytes(gcm_format);

		const auto valid_range = get_confirmed_range();
		u32 transfer_offset = 0;
		u32 transfer_tail = 0;
		if (const auto section_range = get_section_range(); section_range != valid_range)
		{
			transfer_offset = valid_range.start - get_section_base();
			transfer_tail = section_range.end - valid_range.end;
		}

		const texture_readback_region region = get_texture_readback_region(
			width, height, rsx_pitch, block_size, compressed, transfer_offset, transfer_tail);
		const u32 transfer_width = region.width;
		const u32 transfer_height = region.height;
		const u32 transfer_x = region.x;
		const u32 transfer_y = region.y;
		ensure(transfer_width && transfer_height);

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
		const u32 dst_offset = region.byte_offset;

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


			synchronized = true;
			sync_timestamp = rsx::get_shared_tag();
			return;
		}

		// A framebuffer-storage section is the surface itself; a packed colour surface
		// stores blue in the high field of the Metal packed word, so the raw row copy
		// misaligns the guest pack (TX12 is the depth site of the same boundary; the
		// encode is its colour counterpart).
		if (context == rsx::texture_upload_context::framebuffer_storage)
		{
			if (auto* surf = static_cast<surface*>(vram_texture); surf && !surf->is_depth_surface())
			{
				const rsx::surface_color_format color_format = surf->get_surface_color_format();

				if (packed_format_needs_convert(color_format))
				{
					const u32 plane_pitch = transfer_width * 2;
					const u32 plane_rows = transfer_height;
					MTL::Buffer* plane = cmd.resources().new_buffer(static_cast<u64>(plane_pitch) * plane_rows, MTL::StorageModeShared);

					if (!plane)
					{
						rsx_log.error("Metal 4: the packed colour read-back staging buffer could not be created");
						return;
					}

					encoder->copyFromTexture(source, 0, 0,
						MTL::Origin::Make(transfer_x, transfer_y, 0),
						MTL::Size::Make(transfer_width, transfer_height, 1),
						plane, 0, plane_pitch, plane_pitch * plane_rows);

					cmd.flush_for_readback();
					m_readback_cmd = nullptr;

					encode_packed16_plane(static_cast<u8*>(m_staging->value()->contents()) + dst_offset,
						plane->contents(), transfer_width, transfer_height, rsx_pitch, color_format);

					cmd.resources().defer_release(plane);


					synchronized = true;
					sync_timestamp = rsx::get_shared_tag();
					return;
				}
			}
		}

		// An R6G5B5 texture read back leaving a shader_read section takes the same
		// boundary as the packed colour surface: the Apple upload expands the guest
		// pack to BGRA8 (TX20), so the host image holds one expanded texel per guest
		// texel and the raw row copy returns bytes the guest cannot read.  The encode
		// narrows the plane back into the guest pack; this is the texture arm of the
		// SF14 packed16 pair (the surface arm is the framebuffer_storage branch
		// above).  Every other format keeps the raw path: the packed 16-bit texture
		// hosts stay packed on Apple, with the host word layout noted at
		// get_texture_format (MTLFormats.cpp, the r5g6b5 host layout note).
#if defined(__APPLE__)
		if (context == rsx::texture_upload_context::shader_read && gcm_format == CELL_GCM_TEXTURE_R6G5B5)
		{
			const u32 plane_pitch = transfer_width * 4;
			const u32 plane_rows = transfer_height;
			MTL::Buffer* plane = cmd.resources().new_buffer(static_cast<u64>(plane_pitch) * plane_rows, MTL::StorageModeShared);

			if (!plane)
			{
				rsx_log.error("Metal 4: the r6g5b5 read-back staging buffer could not be created");
				return;
			}

			encoder->copyFromTexture(source, 0, 0,
				MTL::Origin::Make(transfer_x, transfer_y, 0),
				MTL::Size::Make(transfer_width, transfer_height, 1),
				plane, 0, plane_pitch, plane_pitch * plane_rows);

			cmd.flush_for_readback();
			m_readback_cmd = nullptr;

			encode_r6g5b5_plane(static_cast<u8*>(m_staging->value()->contents()) + dst_offset,
				plane->contents(), transfer_width, transfer_height, rsx_pitch);

			cmd.resources().defer_release(plane);


			synchronized = true;
			sync_timestamp = rsx::get_shared_tag();
			return;
		}
#endif

		{

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
		deferred = false;

		if (!descriptor || !sampler)
		{
			return {};
		}

		auto* desc = static_cast<sampled_image_descriptor*>(descriptor);


		if (!desc->image_handle)
		{

			if (desc->external_subresource_desc.op == rsx::deferred_request_command::nop)
			{
				return {};
			}

			// The descriptor is legal without an image when it carries a deferred
			// copy or gather (`sampled_image_descriptor::validate`, texture_cache.h:570).
			// Materialize it before the render pass opens; the copy encoder is a
			// queue-form producer, and the caller records its write below.
			texture* image = create_temporary_subresource(cmd, desc->external_subresource_desc);
			if (!image)
			{
				return {};
			}

			deferred = true;


			return { image->resource_id(), sampler->gpuResourceID() };
		}

		return { desc->image_handle->resource_id(), sampler->gpuResourceID() };
	}
}

#endif // RSX_ENABLE_METAL4
