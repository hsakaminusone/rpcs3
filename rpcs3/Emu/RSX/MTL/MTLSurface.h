#pragma once

#include "MTLCommandRecorder.h"
#include "MTLFormats.h"
#include "MTLHandles.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/surface_utils.h"

namespace mtl
{
	// One RSX surface (HLD-02 §3.2).  The surface *is* the image handle, because the
	// generic store recovers a surface from an image pointer (surface_store.h:391).
	// The guest bytes stay canonical, so this object is the Metal projection of a
	// guest range (HLD-02 §2.1).
	class surface : public texture, public rsx::render_target_descriptor<texture*>
	{
	public:
		explicit surface(resource_manager& resources) : texture(&resources) {}

		texture* get_surface(rsx::surface_access access_type) override;
		bool is_depth_surface() const override { return m_depth; }

		u8 samples() const { return static_cast<u8>(sample_count()); }
		rsx::format_class format_class() const override { return rsx::classify_format(get_gcm_format()); }

		// The scaled extent test the store uses to match an existing surface
		// (surface_store.h:498).
		bool matches_dimensions(u16 width_, u16 height_) const
		{
			const auto [scaled_w, scaled_h] = rsx::apply_resolution_scale<true>(resolution_scaling_config, width_, height_);
			return scaled_w == width() && scaled_h == height();
		}

		// Make the contents valid for the access that follows (surface_store.h:1114).
		// Encoder boundaries and barriers belong to the pass scheduler (HLD-03 §2.4),
		// so this function moves the data only.
		void memory_barrier(command_recorder& cmd, rsx::surface_access access_type);
		void read_barrier(command_recorder& cmd) { memory_barrier(cmd, rsx::surface_access::shader_read); }
		void write_barrier(command_recorder& cmd) { memory_barrier(cmd, rsx::surface_access::shader_write); }

		void create_color(u32 address, rsx::surface_color_format format, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling);
		void create_depth(u32 address, rsx::surface_depth_format2 format, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling);

		// Write a byte range of the surface into the guest window, for the DMA path
		// (surface_store.h:1001).
		void write_to_memory(command_recorder& cmd, MTL::Buffer* target, u32 dst_offset, u32 src_offset, u32 write_length);

	private:
		void create(u32 address, u16 width, u16 height, u16 pitch, rsx::surface_antialiasing antialias, const rsx::surface_scaling_config_t& scaling, MTL::PixelFormat format, u8 bpp, bool depth);

		void flush_old_contents(command_recorder& cmd);
		void load_background(command_recorder& cmd);
		void resolve(command_recorder& cmd);
		void unresolve(command_recorder& cmd);

		bool m_depth = false;
	};
}

#endif // RSX_ENABLE_METAL4
