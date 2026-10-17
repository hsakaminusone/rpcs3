#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLDepthStencil.h"

#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/Utils/color_utils.hpp"

#include <unordered_map>

namespace mtl
{
	// The latched sampler state (LLD-texture §3.2).  This is the key of the
	// MTL::SamplerState, so it holds only the fields that build the object.  The
	// remaining bits of the address word (unsigned_remap, gamma, signed_remap) are
	// format-decode state, not sampler state (RSXTexture.cpp:68-126).
	struct sampler_state
	{
		rsx::texture_wrap_mode wrap_s = rsx::texture_wrap_mode::wrap;
		rsx::texture_wrap_mode wrap_t = rsx::texture_wrap_mode::wrap;
		rsx::texture_wrap_mode wrap_r = rsx::texture_wrap_mode::wrap;

		rsx::texture_minify_filter min_filter = rsx::texture_minify_filter::nearest;
		rsx::texture_magnify_filter mag_filter = rsx::texture_magnify_filter::nearest;

		f32 min_lod = 0.0f;
		f32 max_lod = 0.0f;
		u32 max_aniso = 1;

		MTL::SamplerBorderColor border_color = MTL::SamplerBorderColorTransparentBlack;

		// A shadow sampler compares instead of filtering.  The caller sets the flag
		// from current_fp_texture_state.shadow_textures (RSXThread.cpp:2599-2605); the
		// compare value is SET_TEXTURE_ADDRESS.zfunc (LLD-texture §3.2).
		bool depth_compare = false;
		rsx::comparison_function compare = rsx::comparison_function::always;
	};

	// Read one fragment texture unit (rsx::method_registers.fragment_textures[unit]).
	sampler_state capture_fragment_sampler(u8 unit);

	// Read one vertex texture unit (rsx::method_registers.vertex_textures[unit]).
	// A vertex texture has no anisotropy and no comparison.
	sampler_state capture_vertex_sampler(u8 unit);

	MTL::SamplerAddressMode get_address_mode(rsx::texture_wrap_mode mode);
	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_minify_filter filter);
	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_magnify_filter filter);

	// The minify filter carries the mip mode as well as the min filter
	// (gcm_enums.h:1524-1533; CONFLICTS.md C4).
	MTL::SamplerMipFilter get_mip_filter(rsx::texture_minify_filter filter);

	// Metal's maxAnisotropy is 1..16 (04 §2.1 #15).
	u32 get_max_anisotropy(rsx::texture_max_anisotropy aniso);

	// The custom border colour falls back to the closest of the three Metal values
	// (04 R18).  The SNORM re-expansion runs first (RSXTexture.cpp:368-436).
	MTL::SamplerBorderColor get_closest_border_color(const color4f& color);

	// Build the immutable object.  The caller owns the result.
	MTL::SamplerState* create_sampler_state(MTL::Device* device, const sampler_state& state);

	// The value semantics of the latch, so it can key the cache.  A memcmp would
	// read the padding the members leave behind.
	bool operator==(const sampler_state& a, const sampler_state& b);
	u64 hash_sampler_state(const sampler_state& state);

	// The sampler object cache (LLD-texture §3.2).  The latch has few bits and the
	// RSX cycles through a handful of sampler configurations, so an object is built
	// once and reused.  The object is never released while the cache lives, because
	// a Metal 4 command buffer holds no strong reference to what it binds
	// (HLD-02 I7).
	class sampler_cache
	{
	public:
		void init(MTL::Device* device) { m_device = device; }
		void shutdown();

		MTL::SamplerState* get(const sampler_state& state);

	private:
		struct hasher
		{
			usz operator()(const sampler_state& state) const { return hash_sampler_state(state); }
		};

		MTL::Device* m_device = nullptr;
		std::unordered_map<sampler_state, MTL::SamplerState*, hasher> m_cache;
	};
}

#endif // RSX_ENABLE_METAL4
