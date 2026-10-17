#include "MTLSurfaceCache.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	namespace
	{
		// The host sample count follows the video configuration, so the guest AA mode
		// stays metadata when the override is off (VKRenderTargets.h:241-250).
		u8 host_sample_count(rsx::surface_antialiasing antialias)
		{
			return (g_cfg.video.antialiasing_level == msaa_level::_auto) ? rsx::get_format_sample_count(antialias) : 1;
		}

		void retag(surface* object, u32 address, u32 pitch)
		{
			object->rsx_pitch = pitch;
			object->queue_tag(address);
			object->last_use_tag = 0;
			object->stencil_init_flags = 0;
			object->memory_usage_flags = rsx::surface_usage_flags::unknown;
			object->raster_type = rsx::surface_raster_type::linear;
		}
	}

	surface_cache_traits::surface_storage_type surface_cache_traits::create_new_surface(command_list_type cmd, u32 address, rsx::surface_color_format format, u16 width, u16 height, u32 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling)
	{
		auto result = std::make_unique<surface>(cmd.resources());
		result->create_color(address, format, width, height, pitch, antialias, scaling);
		result->add_ref();
		return result;
	}

	surface_cache_traits::surface_storage_type surface_cache_traits::create_new_surface(command_list_type cmd, u32 address, rsx::surface_depth_format2 format, u16 width, u16 height, u32 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling)
	{
		auto result = std::make_unique<surface>(cmd.resources());
		result->create_depth(address, format, width, height, pitch, antialias, scaling);
		result->add_ref();
		return result;
	}

	bool surface_cache_traits::is_compatible_surface(const surface_type& surface, const surface_type& ref, u16 width, u16 height, u8 sample_count)
	{
		return surface->get_gcm_format() == ref->get_gcm_format() &&
			surface->get_spp() == sample_count &&
			surface->get_surface_width<rsx::surface_metrics::pixels>() == width &&
			surface->get_surface_height<rsx::surface_metrics::pixels>() == height;
	}

	bool surface_cache_traits::is_reusable_surface(const surface_type& surface, const surface_type& ref)
	{
		// The image shape must match, because the allocation is reused as it is
		// (surface_store.h:108).
		return surface->value() && surface->pixel_format() == ref->pixel_format() && surface->samples() == ref->samples();
	}

	bool surface_cache_traits::surface_is_pitch_compatible(const surface_storage_type& surface, u32 pitch)
	{
		return surface->get_rsx_pitch() == pitch;
	}

	bool surface_cache_traits::surface_matches_properties(const surface_storage_type& surface, rsx::surface_color_format format, u16 width, u16 height, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, bool check_refs)
	{
		if (check_refs && surface->has_refs())
		{
			return false;
		}

		return surface->get_surface_color_format() == format &&
			surface->get_spp() == host_sample_count(antialias) &&
			surface->matches_dimensions(width, height) &&
			surface->get_resolution_scaling_config() == scaling;
	}

	bool surface_cache_traits::surface_matches_properties(const surface_storage_type& surface, rsx::surface_depth_format2 format, u16 width, u16 height, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, bool check_refs)
	{
		if (check_refs && surface->has_refs())
		{
			return false;
		}

		return surface->get_surface_depth_format() == format &&
			surface->get_spp() == host_sample_count(antialias) &&
			surface->matches_dimensions(width, height) &&
			surface->get_resolution_scaling_config() == scaling;
	}

	void surface_cache_traits::invalidate_surface_contents(command_list_type, surface_type surface, rsx::surface_color_format format, u32 address, u32 pitch)
	{
		surface->set_format(format);
		retag(surface, address, pitch);
	}

	void surface_cache_traits::invalidate_surface_contents(command_list_type, surface_type surface, rsx::surface_depth_format2 format, u32 address, u32 pitch)
	{
		surface->set_format(format);
		retag(surface, address, pitch);
	}

	void surface_cache_traits::prepare_surface_for_drawing(command_list_type cmd, surface_type surface)
	{
		// A surface that is about to become an attachment must hold the guest
		// contents, because a partial write leaves the rest of the surface in place
		// (HLD-02 §2.1).
		surface->memory_barrier(cmd, rsx::surface_access::gpu_reference);
		surface->memory_usage_flags |= rsx::surface_usage_flags::attachment;
	}

	void surface_cache_traits::prepare_surface_for_sampling(command_list_type, surface_type)
	{
		// No Metal state tracks the binding: the pass scheduler owns attachment
		// selection (HLD-03 §2.4).
	}

	void surface_cache_traits::notify_surface_invalidated(const surface_storage_type& surface)
	{
		if (!surface->old_contents.empty())
		{
			// The deferred regions were never transferred, so drop their references.
			surface->clear_rw_barrier();
		}

		surface->release();
	}

	void surface_cache_traits::notify_surface_reused(const surface_storage_type& surface)
	{
		surface->state_flags |= rsx::surface_state_flags::erase_bkgnd;
		surface->add_ref();
	}

	void surface_cache_traits::notify_surface_persist(const surface_storage_type&)
	{
	}

	void surface_cache_traits::clone_surface(command_list_type cmd, surface_storage_type& sink, surface_type ref, u32 address, barrier_descriptor_t& prev, const rsx::surface_scaling_config_t& scaling)
	{
		const bool initialize = !sink || !sink->has_refs();

		if (!sink)
		{
			auto result = std::make_unique<surface>(cmd.resources());

			// The clone keeps the reference format and sample count, and takes its
			// extent from the region (surface_store.h:181).
			if (ref->is_depth_surface())
			{
				result->create_depth(address, ref->get_surface_depth_format(), prev.width, prev.height, ref->get_rsx_pitch(), ref->get_aa_mode(), scaling);
			}
			else
			{
				result->create_color(address, ref->get_surface_color_format(), prev.width, prev.height, ref->get_rsx_pitch(), ref->get_aa_mode(), scaling);
			}

			sink = std::move(result);
		}

		if (initialize)
		{
			sink->reset();
			sink->msaa_flags = rsx::surface_state_flags::ready;
			sink->add_ref();

			sink->set_aa_mode(ref->get_aa_mode());
			sink->format_info = ref->format_info;
			sink->memory_usage_flags = rsx::surface_usage_flags::storage;
			sink->state_flags = rsx::surface_state_flags::erase_bkgnd;
			sink->sample_layout = ref->sample_layout;
			sink->stencil_init_flags = ref->stencil_init_flags;
			sink->rsx_pitch = ref->get_rsx_pitch();
			sink->surface_width = prev.width;
			sink->surface_height = prev.height;
			sink->native_pitch = static_cast<u32>(prev.width) * ref->get_bpp() * ref->samples_x;
			sink->queue_tag(address);
		}

		if (!sink->old_contents.empty())
		{
			if (sink->get_surface_width<rsx::surface_metrics::pixels>() > prev.width ||
				sink->get_surface_height<rsx::surface_metrics::pixels>() > prev.height)
			{
				sink->write_barrier(cmd);
			}
			else
			{
				sink->clear_rw_barrier();
			}
		}

		prev.target = sink.get();
		sink->set_old_contents_region(prev, false);
	}

	surface_cache_traits::surface_storage_type surface_cache_traits::convert_pitch(command_list_type, surface_storage_type& src, u32)
	{
		// No Metal surface is pitch-converted.  The store discards the source and
		// rebuilds the target from the guest contents (surface_store.h:486).
		src->state_flags = rsx::surface_state_flags::erase_bkgnd;
		return {};
	}

	void surface_cache_traits::write_render_target_to_memory(command_list_type cmd, buffer_object_type bo, surface_type surface, u32 dst_offset, u32 src_offset, u32 write_length)
	{
		// The read makes the contents valid first: a pending resolve or an
		// uninitialised background would otherwise be copied out stale
		// (VKRenderTargets.h:643).
		surface->read_barrier(cmd);

		// The buffer object is a range of the guest window, so the guest address is
		// the offset into it (HLD-02 §3.5).
		surface->write_to_memory(cmd, bo->value(), bo->base_address + dst_offset, src_offset, write_length);
	}

	template <u32 BlockSize>
	surface_cache_traits::buffer_object_storage_type surface_cache_traits::merge_bo_list(command_list_type cmd, const std::vector<buffer_object_type>& bo_list)
	{
		u32 base = 0;
		u32 length = 0;
		bool resolved = false;

		for (buffer_object_type entry : bo_list)
		{
			if (entry)
			{
				if (!resolved)
				{
					base = entry->base_address;
					resolved = true;
				}

				length += entry->range_length;
				continue;
			}

			if (!resolved)
			{
				base += BlockSize;
			}

			length += BlockSize;
		}

		// The guest window covers the whole range, so the merge records the range and
		// copies nothing (HLD-02 §3.5).
		auto result = std::make_unique<buffer>(&cmd.resources());
		result->borrow(cmd.resources().guest_buffer(), base, length);
		return result;
	}

	template surface_cache_traits::buffer_object_storage_type surface_cache_traits::merge_bo_list<0x400000>(command_list_type, const std::vector<buffer_object_type>&);
}

#endif // RSX_ENABLE_METAL4
