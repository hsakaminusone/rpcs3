#pragma once

#include "MTLCommandRecorder.h"
#include "MTLSurface.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/surface_store.h"

namespace mtl
{
	// The trait set of the generic surface store (HLD-01 §3.3).  Every member is a
	// decision about the Metal projection of the RSX surface model, and nothing else.
	// The traits hold no state: a member that allocates gets the resource layer from
	// its command list.
	struct surface_cache_traits
	{
		using surface_storage_type = std::unique_ptr<surface>;
		using surface_type = surface*;
		using buffer_object_storage_type = std::unique_ptr<buffer>;
		using buffer_object_type = buffer*;
		using command_list_type = command_recorder&;
		using barrier_descriptor_t = rsx::deferred_clipped_region<surface_type>;

		static surface_storage_type create_new_surface(command_list_type cmd, u32 address, rsx::surface_color_format format, u16 width, u16 height, u32 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling);
		static surface_storage_type create_new_surface(command_list_type cmd, u32 address, rsx::surface_depth_format2 format, u16 width, u16 height, u32 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling);

		static surface_type get(const surface_storage_type& storage) { return storage.get(); }
		static buffer_object_type get(const buffer_object_storage_type& storage) { return storage.get(); }

		static bool is_compatible_surface(const surface_type& surface, const surface_type& ref, u16 width, u16 height, u8 sample_count);
		static bool is_reusable_surface(const surface_type& surface, const surface_type& ref);
		static bool surface_is_pitch_compatible(const surface_storage_type& surface, u32 pitch);
		static bool surface_matches_properties(const surface_storage_type& surface, rsx::surface_color_format format, u16 width, u16 height, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, bool check_refs = false);
		static bool surface_matches_properties(const surface_storage_type& surface, rsx::surface_depth_format2 format, u16 width, u16 height, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, bool check_refs = false);

		static void invalidate_surface_contents(command_list_type cmd, surface_type surface, rsx::surface_color_format format, u32 address, u32 pitch);
		static void invalidate_surface_contents(command_list_type cmd, surface_type surface, rsx::surface_depth_format2 format, u32 address, u32 pitch);

		static void prepare_surface_for_drawing(command_list_type cmd, surface_type surface);
		static void prepare_surface_for_sampling(command_list_type cmd, surface_type surface);

		static void notify_surface_invalidated(const surface_storage_type& surface);
		static void notify_surface_reused(const surface_storage_type& surface);
		static void notify_surface_persist(const surface_storage_type& surface);

		static void clone_surface(command_list_type cmd, surface_storage_type& sink, surface_type ref, u32 address, barrier_descriptor_t& prev, const rsx::surface_scaling_config_t& scaling);
		static surface_storage_type convert_pitch(command_list_type cmd, surface_storage_type& src, u32 out_pitch);

		static void write_render_target_to_memory(command_list_type cmd, buffer_object_type bo, surface_type surface, u32 dst_offset, u32 src_offset, u32 write_length);

		template <u32 BlockSize>
		static buffer_object_storage_type merge_bo_list(command_list_type cmd, const std::vector<buffer_object_type>& bo_list);
	};

	// The generic store is not forked (HLD-02 I10); only the traits above are Metal.
	using surface_cache = rsx::surface_store<surface_cache_traits>;
}

#endif // RSX_ENABLE_METAL4
