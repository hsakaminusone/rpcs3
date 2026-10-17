#pragma once

#include "MTLArgumentBinds.h"
#include "MTLCommandRecorder.h"
#include "MTLHandles.h"
#include "MTLSurface.h"

#ifdef RSX_ENABLE_METAL4

#include "../Common/texture_cache.h"

#include <memory>
#include <vector>

namespace mtl
{
	class cached_texture_section;
	class texture_cache;

	struct texture_cache_traits
	{
		using commandbuffer_type      = mtl::command_recorder&;
		using section_storage_type    = mtl::cached_texture_section;
		using texture_cache_type      = mtl::texture_cache;
		using texture_cache_base_type = rsx::texture_cache<mtl::texture_cache, mtl::texture_cache_traits>;
		using image_resource_type     = mtl::texture*;
		using image_view_type         = mtl::texture*;
		using image_storage_type      = mtl::texture;
		using texture_format          = MTL::PixelFormat;
		using viewable_image_type     = mtl::texture*;
	};

	class cached_texture_section : public rsx::cached_texture_section<mtl::cached_texture_section, mtl::texture_cache_traits>
	{
		using baseclass = rsx::cached_texture_section<mtl::cached_texture_section, mtl::texture_cache_traits>;
		friend baseclass;

		texture* vram_texture = nullptr;
		std::unique_ptr<texture> managed_texture;

		// The flush readback target.  A Metal 4 command buffer has no blit encoder, so
		// the GPU-to-CPU copy is a compute pass over a shared buffer.
		std::unique_ptr<buffer> m_staging;

		// The recorder that encoded the staging copy.  The readback waits on it, so
		// the CPU never reads a copy the GPU has not written (REVIEW-FINDINGS.md, B4).
		command_recorder* m_readback_cmd = nullptr;

		MTL::PixelFormat format = MTL::PixelFormat::PixelFormatBGRA8Unorm;

	public:
		using baseclass::cached_texture_section;

		void create(u16 w, u16 h, u16 depth, u16 mipmaps, texture* image, u32 rsx_pitch, bool managed)
		{
			ensure(!exists() || !is_managed() || vram_texture == image);

			vram_texture = image;

			if (managed)
			{
				managed_texture.reset(vram_texture);
			}
			else
			{
				ensure(!managed_texture);
			}

			flushed = false;
			synchronized = false;
			sync_timestamp = 0ull;

			ensure(rsx_pitch);

			this->rsx_pitch = rsx_pitch;
			this->width = w;
			this->height = h;
			this->real_pitch = 0;
			this->depth = depth;
			this->mipmaps = mipmaps;

			format = image->pixel_format();

			baseclass::on_section_resources_created();
		}

		// The framebuffer path.  lock_memory_region_impl forwards the surface as the
		// extra argument (texture_cache.h:1410), because the surface holds the depth
		// or colour format the section cannot recover from the image alone.
		void create(u16 w, u16 h, u16 depth, u16 mipmaps, texture* image, u32 rsx_pitch, bool managed, const surface* surf)
		{
			set_gcm_format(surf->get_gcm_format());
			create(w, h, depth, mipmaps, image, rsx_pitch, managed);
			format = surf->pixel_format();
		}

		void set_dimensions(u32 width, u32 height, u32 /*depth*/, u32 pitch)
		{
			this->width = static_cast<u16>(width);
			this->height = static_cast<u16>(height);
			rsx_pitch = pitch;
		}

		void set_format(MTL::PixelFormat new_format) { format = new_format; }
		MTL::PixelFormat get_format() const { return format; }

		void copy_texture(command_recorder& cmd, bool miss);

		void* map_synchronized(u32 offset, u32 size);

		void finish_flush() {}

		void destroy()
		{
			if (!is_locked() && !m_staging && vram_texture == nullptr && !managed_texture)
			{
				return;
			}

			m_staging.reset();
			managed_texture.reset();
			vram_texture = nullptr;

			baseclass::on_section_resources_destroyed();
		}

		void sync_surface_memory(const rsx::simple_array<cached_texture_section*>& surfaces)
		{
			auto* rtt = static_cast<surface*>(vram_texture);
			rtt->sync_tag();

			for (auto& s : surfaces)
			{
				rtt->inherit_surface_contents(static_cast<surface*>(s->vram_texture));
			}
		}

		bool exists() const { return vram_texture != nullptr; }
		bool is_managed() const { return !exists() || managed_texture != nullptr; }

		texture* get_view(const rsx::texture_channel_remap_t& remap) { return vram_texture->get_view(remap); }
		texture* get_raw_texture() const { return managed_texture.get(); }

		// Only called for framebuffer_storage sections, so vram_texture is a surface.
		surface* get_render_target() const { return static_cast<surface*>(vram_texture); }

		bool is_depth_texture() const
		{
			return format == MTL::PixelFormat::PixelFormatDepth16Unorm ||
				format == MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 ||
				format == MTL::PixelFormat::PixelFormatDepth32Float;
		}
	};

	class texture_cache : public rsx::texture_cache<mtl::texture_cache, mtl::texture_cache_traits>
	{
	private:
		using baseclass = rsx::texture_cache<mtl::texture_cache, mtl::texture_cache_traits>;
		friend baseclass;

		struct temporary_texture : public texture, public rsx::ref_counted
		{
			u64 properties_encoding = 0;

			explicit temporary_texture(resource_manager* resources) : texture(resources) {}
		};

		std::vector<std::unique_ptr<temporary_texture>> m_temporary_surfaces;
		const u32 max_cached_image_pool_size = 256;

	private:
		void clear()
		{
			baseclass::clear();
			m_temporary_surfaces.clear();
		}

		static MTL::TextureType get_texture_type(rsx::texture_dimension_extended type)
		{
			switch (type)
			{
			case rsx::texture_dimension_extended::texture_dimension_cubemap:
				return MTL::TextureTypeCube;
			case rsx::texture_dimension_extended::texture_dimension_3d:
				return MTL::TextureType3D;
			default:
				return MTL::TextureType2D;
			}
		}

		static u64 encode_properties(MTL::PixelFormat format, MTL::TextureType type, u16 width, u16 height, u16 depth, u8 mipmaps)
		{
			ensure(((width | height) & ~0x3fff) == 0, "Image dimensions are too large - lower your resolution scale.");
			ensure(depth >= 1 && depth <= 512);
			ensure(mipmaps <= 13);

			return (static_cast<u64>(width) << 0) |
				(static_cast<u64>(height) << 14) |
				(static_cast<u64>(depth - 1) << 28) |
				(static_cast<u64>(mipmaps) << 37) |
				(static_cast<u64>(type) << 41) |
				(static_cast<u64>(format) << 45);
		}

		texture* create_image(command_recorder& cmd, MTL::PixelFormat format, MTL::TextureType type, u16 width, u16 height, u16 depth, u8 mipmaps) const
		{
			MTL::TextureDescriptor* descriptor = make_descriptor(format, type, width, height, depth, mipmaps);

			auto* result = new texture(&cmd.resources());
			result->assign(cmd.resources().new_texture(descriptor, MTL::StorageModePrivate));
			descriptor->release();
			return result;
		}

		static MTL::TextureDescriptor* make_descriptor(MTL::PixelFormat format, MTL::TextureType type, u16 width, u16 height, u16 depth, u8 mipmaps)
		{
			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
			descriptor->setTextureType(type);
			descriptor->setPixelFormat(format);
			descriptor->setWidth(width);
			descriptor->setHeight(height);
			descriptor->setDepth(type == MTL::TextureType3D ? depth : 1);
			descriptor->setArrayLength(type == MTL::TextureTypeCube ? 6 : 1);
			descriptor->setMipmapLevelCount(mipmaps);
			descriptor->setSampleCount(1);
			descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView);
			return descriptor;
		}

		std::array<MTL::TextureSwizzle, 4> get_component_mapping(u32 gcm_format, rsx::component_order flags) const
		{
			switch (gcm_format)
			{
			case CELL_GCM_TEXTURE_DEPTH24_D8:
			case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
			case CELL_GCM_TEXTURE_DEPTH16:
			case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
				return { MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed };
			default:
				break;
			}

			switch (flags)
			{
			case rsx::component_order::native:
				return { MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
			case rsx::component_order::swapped_native:
				return { MTL::TextureSwizzleBlue, MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen };
			default:
				return get_texture_component_layout(gcm_format);
			}
		}

		// The upload helper.  It is not the base's sampled-image resolver
		// (texture_cache.h:3070), which this class must keep reachable by name.
		void upload_texture_data(command_recorder& cmd, texture* dst, u32 gcm_format, bool is_swizzled, const std::vector<rsx::subresource_layout>& layouts) const;

		void initialize_subresource_from_memory(command_recorder& cmd, texture* dst, const deferred_subresource& desc, rsx::texture_dimension_extended type) const
		{
			upload_texture_data(cmd, dst, desc.gcm_format, desc.swizzled, rsx::get_subresources_layout(desc, type));
		}

		texture* create_temporary_subresource_impl(command_recorder& cmd, texture* src, MTL::PixelFormat format, MTL::TextureType dst_type, u32 gcm_format,
			u16 width, u16 height, u16 depth, u8 mipmaps, const rsx::texture_channel_remap_t& remap, const copy_region_descriptor* copy = nullptr);

		void copy_transfer_regions_impl(command_recorder& cmd, texture* dst_image, const rsx::simple_array<copy_region_descriptor>& sources) const;

		texture* get_template_from_collection_impl(const rsx::simple_array<copy_region_descriptor>& sections_to_transfer) const
		{
			if (sections_to_transfer.size() == 1) [[likely]]
			{
				return sections_to_transfer.front().src;
			}

			texture* result = nullptr;
			for (const auto& section : sections_to_transfer)
			{
				if (!section.src)
				{
					continue;
				}

				if (!result)
				{
					result = section.src;
					continue;
				}

				if (result->get_native_component_layout() != section.src->get_native_component_layout())
				{
					// Mixing native layouts needs a compute pass that does not exist yet.
					return nullptr;
				}
			}

			return result;
		}

	protected:
		texture* create_temporary_subresource_view(command_recorder& cmd, const deferred_subresource& desc) override
		{
			ensure(desc.sections_to_copy.size() == 1);
			const auto& section = desc.sections_to_copy.front();
			return create_temporary_subresource_impl(
				cmd, section.src,
				MTL::PixelFormat::PixelFormatInvalid,
				MTL::TextureType2D, desc.gcm_format,
				desc.width, desc.height, 1, 1,
				desc.remap,
				&section);
		}

		texture* generate_cubemap_from_images(command_recorder& cmd, const deferred_subresource& desc) override
		{
			auto* _template = get_template_from_collection_impl(desc.sections_to_copy);
			auto* result = create_temporary_subresource_impl(cmd, _template, MTL::PixelFormat::PixelFormatInvalid, MTL::TextureTypeCube, desc.gcm_format,
				desc.width, desc.height, 1, desc.exact_mip_count(), desc.remap);

			if (desc.force_bg_load)
			{
				initialize_subresource_from_memory(cmd, result->image(), desc, rsx::texture_dimension_extended::texture_dimension_cubemap);
			}

			copy_transfer_regions_impl(cmd, result->image(), desc.sections_to_copy);
			return result;
		}

		texture* generate_3d_from_2d_images(command_recorder& cmd, const deferred_subresource& desc) override
		{
			auto* _template = get_template_from_collection_impl(desc.sections_to_copy);
			auto* result = create_temporary_subresource_impl(cmd, _template, MTL::PixelFormat::PixelFormatInvalid, MTL::TextureType3D, desc.gcm_format,
				desc.width, desc.height, desc.depth, desc.exact_mip_count(), desc.remap);

			if (desc.force_bg_load)
			{
				initialize_subresource_from_memory(cmd, result->image(), desc, rsx::texture_dimension_extended::texture_dimension_3d);
			}

			copy_transfer_regions_impl(cmd, result->image(), desc.sections_to_copy);
			return result;
		}

		texture* generate_atlas_from_images(command_recorder& cmd, const deferred_subresource& desc) override
		{
			auto* _template = get_template_from_collection_impl(desc.sections_to_copy);
			auto* result = create_temporary_subresource_impl(cmd, _template, MTL::PixelFormat::PixelFormatInvalid, MTL::TextureType2D, desc.gcm_format,
				desc.width, desc.height, 1, 1, desc.remap);

			if (desc.force_bg_load)
			{
				initialize_subresource_from_memory(cmd, result->image(), desc, rsx::texture_dimension_extended::texture_dimension_2d);
			}

			copy_transfer_regions_impl(cmd, result->image(), desc.sections_to_copy);
			return result;
		}

		texture* generate_2d_mipmaps_from_images(command_recorder& cmd, const deferred_subresource& desc) override
		{
			const auto mipmaps = ::narrow<u8>(desc.sections_to_copy.size());
			auto* _template = get_template_from_collection_impl(desc.sections_to_copy);
			auto* result = create_temporary_subresource_impl(cmd, _template, MTL::PixelFormat::PixelFormatInvalid, MTL::TextureType2D, desc.gcm_format,
				desc.width, desc.height, 1, mipmaps, desc.remap);

			if (desc.force_bg_load)
			{
				initialize_subresource_from_memory(cmd, result->image(), desc, rsx::texture_dimension_extended::texture_dimension_2d);
			}

			copy_transfer_regions_impl(cmd, result->image(), desc.sections_to_copy);
			return result;
		}

		void release_temporary_subresource(texture* view) override
		{
			for (auto& e : m_temporary_surfaces)
			{
				if (e.get() == view->image())
				{
					e->release();
					return;
				}
			}
		}

		void update_image_contents(command_recorder& cmd, texture* dst, const deferred_subresource& desc) override
		{
			copy_transfer_regions_impl(cmd, dst, desc.sections_to_copy);
		}

		cached_texture_section* create_new_texture(command_recorder& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch,
			u32 gcm_format, rsx::texture_upload_context context, rsx::texture_dimension_extended type, bool swizzled, rsx::component_order swizzle_flags, rsx::flags32_t /*flags*/) override
		{
			const rsx::image_section_attributes_t search_desc = { .gcm_format = gcm_format, .width = width, .height = height, .depth = depth, .mipmaps = mipmaps };
			const bool allow_dirty = (context != rsx::texture_upload_context::framebuffer_storage);
			auto& cached = *find_cached_texture(rsx_range, search_desc, true, true, allow_dirty);
			ensure(!cached.is_locked());

			texture* image = nullptr;
			if (cached.exists())
			{
				// Try and reuse this image data. It is very likely to match our needs.
				image = cached.get_raw_texture();
				if (!image || cached.get_image_type() != type)
				{
					// Type mismatch, discard.
					cached.destroy();
					image = nullptr;
				}
				else
				{
					ensure(cached.is_managed());

					cached.set_dimensions(width, height, depth, pitch);
					cached.set_format(image->pixel_format());
				}
			}

			if (!image)
			{
				ensure(!cached.exists());
				image = create_image(cmd, get_texture_format(gcm_format), get_texture_type(type), width, height, depth, static_cast<u8>(mipmaps));
				image->set_format_class(rsx::classify_format(gcm_format));

				cached.reset(rsx_range);
				cached.set_image_type(type);
				cached.set_gcm_format(gcm_format);
				cached.create(width, height, depth, mipmaps, image, pitch, true);
			}

			cached.set_view_flags(swizzle_flags);
			cached.set_context(context);
			cached.set_swizzled(swizzled);
			cached.set_dirty(false);

			image->set_native_component_layout(get_component_mapping(gcm_format, swizzle_flags));

			if (context != rsx::texture_upload_context::blit_engine_dst)
			{
				AUDIT(cached.get_memory_read_flags() != rsx::memory_read_flags::flush_always);
				read_only_range = cached.get_min_max(read_only_range, rsx::section_bounds::locked_range);
				cached.protect(utils::protection::ro);
			}
			else
			{
				// Protection is handled by the caller.
				no_access_range = cached.get_min_max(no_access_range, rsx::section_bounds::locked_range);
			}

			update_cache_tag();
			return &cached;
		}

		cached_texture_section* create_nul_section(command_recorder& /*cmd*/, const utils::address_range32& rsx_range, const rsx::image_section_attributes_t& attrs,
			const rsx::GCM_tile_reference& /*tile*/, bool /*memory_load*/) override
		{
			auto& cached = *find_cached_texture(rsx_range, { .gcm_format = RSX_GCM_FORMAT_IGNORED }, true, false, false);
			ensure(!cached.is_locked());

			cached.reset(rsx_range);
			cached.create_dma_only(attrs.width, attrs.height, attrs.pitch);
			cached.set_dirty(false);

			no_access_range = cached.get_min_max(no_access_range, rsx::section_bounds::locked_range);
			update_cache_tag();
			return &cached;
		}

		cached_texture_section* upload_image_from_cpu(command_recorder& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch, u32 gcm_format,
			rsx::texture_upload_context context, const std::vector<rsx::subresource_layout>& subresource_layout, rsx::texture_dimension_extended type, bool input_swizzled) override
		{
			auto* section = create_new_texture(cmd, rsx_range, width, height, depth, mipmaps, pitch, gcm_format, context, type, input_swizzled,
				rsx::component_order::default_, 0);

			upload_texture_data(cmd, section->get_raw_texture(), gcm_format, input_swizzled, subresource_layout);
			section->last_write_tag = rsx::get_shared_tag();
			return section;
		}

		void set_component_order(cached_texture_section& section, u32 gcm_format, rsx::component_order flags) override
		{
			if (flags == section.get_view_flags())
			{
				return;
			}

			auto* image = section.get_raw_texture();
			ensure(image);
			image->set_native_component_layout(get_component_mapping(gcm_format, flags));
			section.set_view_flags(flags);
		}

		void insert_texture_barrier(command_recorder&, texture*, bool) override
		{}

		bool render_target_format_is_compatible(texture* tex, u32 gcm_format) override
		{
			return tex->pixel_format() == get_texture_format(gcm_format);
		}

		void prepare_for_dma_transfers(command_recorder&) override
		{}

		void cleanup_after_dma_transfers(command_recorder&) override
		{}

	public:
		using baseclass::texture_cache;

		void destroy() override
		{
			clear();
		}

		bool is_depth_texture(u32 rsx_address, u32 rsx_size) override
		{
			reader_lock lock(m_cache_mutex);

			auto& block = m_storage.block_for(rsx_address);

			if (block.get_locked_count() == 0)
			{
				return false;
			}

			for (auto& tex : block)
			{
				if (tex.is_dirty())
				{
					continue;
				}

				if (!tex.overlaps(rsx_address, rsx::section_bounds::full_range))
				{
					continue;
				}

				if ((rsx_address + rsx_size - tex.get_section_base()) <= tex.get_section_size())
				{
					return tex.is_depth_texture();
				}
			}

			return false;
		}

		void on_frame_end() override
		{
			trim_sections();

			if (m_storage.m_unreleased_texture_objects >= m_max_zombie_objects)
			{
				purge_unreleased_sections();
			}

			if (m_temporary_surfaces.size() > max_cached_image_pool_size)
			{
				m_temporary_surfaces.resize(max_cached_image_pool_size / 2);
			}

			baseclass::on_frame_end();
		}

		// Resolve one RSX texture unit to the Metal image and sampler of this draw.
		// `descriptor` is the unit's latch (RSXThread.h:261) and `sampler` is the
		// object built by the renderer's sampler cache (MTLGSRender.cpp:249).
		texture_bind resolve_texture_bind(const rsx::sampled_image_descriptor_base* descriptor, MTL::SamplerState* sampler);
	};
}

#endif // RSX_ENABLE_METAL4
