#include "MTLSamplerState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"

#include "util/fnv_hash.hpp"

#include <algorithm>
#include <bit>

namespace mtl
{
	MTL::SamplerAddressMode get_address_mode(rsx::texture_wrap_mode mode)
	{
		switch (mode)
		{
		case rsx::texture_wrap_mode::wrap:
			return MTL::SamplerAddressModeRepeat;
		case rsx::texture_wrap_mode::mirror:
			return MTL::SamplerAddressModeMirrorRepeat;
		case rsx::texture_wrap_mode::clamp_to_edge:
		case rsx::texture_wrap_mode::clamp:
			return MTL::SamplerAddressModeClampToEdge;
		case rsx::texture_wrap_mode::border:
			return MTL::SamplerAddressModeClampToBorderColor;
		case rsx::texture_wrap_mode::mirror_once_clamp_to_edge:
		case rsx::texture_wrap_mode::mirror_once_border:
		case rsx::texture_wrap_mode::mirror_once_clamp:
			// Metal has no mirror-once variant.  MirrorClampToEdge keeps the mirror
			// half and drops the border (RSX has no per-unit border fetch).
			return MTL::SamplerAddressModeMirrorClampToEdge;
		}

		return MTL::SamplerAddressModeClampToEdge;
	}

	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_minify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_minify_filter::nearest:
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::nearest_linear:
			return MTL::SamplerMinMagFilterNearest;
		case rsx::texture_minify_filter::linear:
		case rsx::texture_minify_filter::linear_nearest:
		case rsx::texture_minify_filter::linear_linear:
		case rsx::texture_minify_filter::convolution_min:
			return MTL::SamplerMinMagFilterLinear;
		}

		return MTL::SamplerMinMagFilterNearest;
	}

	MTL::SamplerMinMagFilter get_min_mag_filter(rsx::texture_magnify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_magnify_filter::nearest:
			return MTL::SamplerMinMagFilterNearest;
		case rsx::texture_magnify_filter::linear:
		case rsx::texture_magnify_filter::convolution_mag:
			return MTL::SamplerMinMagFilterLinear;
		}

		return MTL::SamplerMinMagFilterNearest;
	}

	MTL::SamplerMipFilter get_mip_filter(rsx::texture_minify_filter filter)
	{
		switch (filter)
		{
		case rsx::texture_minify_filter::nearest:
		case rsx::texture_minify_filter::linear:
			// The minify filter names the mipmap base level (gcm_enums.h:1526-1527).
			return MTL::SamplerMipFilterNotMipmapped;
		case rsx::texture_minify_filter::nearest_nearest:
		case rsx::texture_minify_filter::linear_nearest:
			return MTL::SamplerMipFilterNearest;
		case rsx::texture_minify_filter::nearest_linear:
		case rsx::texture_minify_filter::linear_linear:
		case rsx::texture_minify_filter::convolution_min:
			// convolution_min looks close to linear_linear (gcm_enums.h:1532).
			return MTL::SamplerMipFilterLinear;
		}

		return MTL::SamplerMipFilterNotMipmapped;
	}

	u32 get_max_anisotropy(rsx::texture_max_anisotropy aniso)
	{
		switch (aniso)
		{
		case rsx::texture_max_anisotropy::x1: return 1;
		case rsx::texture_max_anisotropy::x2: return 2;
		case rsx::texture_max_anisotropy::x4: return 4;
		case rsx::texture_max_anisotropy::x6: return 6;
		case rsx::texture_max_anisotropy::x8: return 8;
		case rsx::texture_max_anisotropy::x10: return 10;
		case rsx::texture_max_anisotropy::x12: return 12;
		case rsx::texture_max_anisotropy::x16: return 16;
		}

		return 1;
	}

	MTL::SamplerBorderColor get_closest_border_color(const color4f& color)
	{
		// Metal has three border colours (04 R18).  A bright colour becomes white, then
		// an opaque colour becomes black, then transparent black.
		if ((color.r + color.g + color.b) > 1.35f)
		{
			return MTL::SamplerBorderColorOpaqueWhite;
		}

		if (color.a > 0.5f)
		{
			return MTL::SamplerBorderColorOpaqueBlack;
		}

		return MTL::SamplerBorderColorTransparentBlack;
	}

	namespace
	{
		// The fields shared by the two texture classes.  The comparison sampler is
		// selected by the sampled format: the RSX compares a fetched depth against
		// the reference instead of filtering it (VKDraw.cpp:404-410).
		template <typename Texture>
		sampler_state capture_sampler(const Texture& texture)
		{
			sampler_state state;

			state.wrap_s = texture.wrap_s();
			state.wrap_t = texture.wrap_t();
			state.wrap_r = texture.wrap_r();
			state.min_filter = texture.min_filter();
			state.mag_filter = texture.mag_filter();

			// The LOD range is clamped to the resident mip chain, because the guest
			// value can exceed it (VKDraw.cpp:467-518).  Metal has no sampler LOD
			// bias, so the guest bias is a recorded limitation (04 R19).
			const u32 mipmaps = texture.get_exact_mipmap_count();
			if (get_mip_filter(state.min_filter) != MTL::SamplerMipFilterNotMipmapped && mipmaps > 1)
			{
				state.min_lod = std::min(texture.min_lod(), static_cast<f32>(mipmaps - 1));
				state.max_lod = std::min(texture.max_lod(), static_cast<f32>(mipmaps - 1));
			}

			const u32 format = texture.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
			state.depth_compare = (format >= CELL_GCM_TEXTURE_DEPTH24_D8 && format <= CELL_GCM_TEXTURE_DEPTH16_FLOAT);

			return state;
		}
	}

	sampler_state capture_fragment_sampler(u8 unit)
	{
		const auto& texture = rsx::method_registers.fragment_textures[unit];
		sampler_state state = capture_sampler(texture);

		// The SNORM expansion is part of the RSX border colour
		// (RSXTexture.cpp:376-378; VKDraw.cpp:412-413).
		state.border_color = get_closest_border_color(texture.remapped_border_color(texture.argb_signed() != 0));

		// The comparison reference is SET_TEXTURE_ADDRESS.zfunc.
		state.compare = texture.zfunc();
		state.max_aniso = get_max_anisotropy(texture.max_aniso());

		return state;
	}

	sampler_state capture_vertex_sampler(u8 unit)
	{
		// A vertex texture has no zfunc or signed-channel register, so it is never
		// a comparison sampler and its border colour carries no expansion.
		const auto& texture = rsx::method_registers.vertex_textures[unit];
		sampler_state state = capture_sampler(texture);

		state.border_color = get_closest_border_color(texture.remapped_border_color());
		state.depth_compare = false;

		return state;
	}

	sampler_state stencil_mirror_sampler()
	{
		// Clamp-to-border with an opaque-black border, nearest, no mip
		// (VKDraw.cpp:707-717).
		sampler_state state;
		state.wrap_s = state.wrap_t = state.wrap_r = rsx::texture_wrap_mode::border;
		state.border_color = MTL::SamplerBorderColorOpaqueBlack;

		return state;
	}

	MTL::SamplerState* create_sampler_state(MTL::Device* device, const sampler_state& state)
	{
		MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();

		descriptor->setSAddressMode(get_address_mode(state.wrap_s));
		descriptor->setTAddressMode(get_address_mode(state.wrap_t));
		descriptor->setRAddressMode(get_address_mode(state.wrap_r));
		descriptor->setMinFilter(get_min_mag_filter(state.min_filter));
		descriptor->setMagFilter(get_min_mag_filter(state.mag_filter));
		descriptor->setMipFilter(get_mip_filter(state.min_filter));
		descriptor->setLodMinClamp(state.min_lod);
		descriptor->setLodMaxClamp(state.max_lod);
		descriptor->setMaxAnisotropy(state.max_aniso);
		descriptor->setBorderColor(state.border_color);

		if (state.depth_compare)
		{
			descriptor->setCompareFunction(get_compare_function(state.compare));
		}

		MTL::SamplerState* result = device->newSamplerState(descriptor);

		descriptor->release();
		return result;
	}

	bool operator==(const sampler_state& a, const sampler_state& b)
	{
		return a.wrap_s == b.wrap_s
			&& a.wrap_t == b.wrap_t
			&& a.wrap_r == b.wrap_r
			&& a.min_filter == b.min_filter
			&& a.mag_filter == b.mag_filter
			&& a.min_lod == b.min_lod
			&& a.max_lod == b.max_lod
			&& a.max_aniso == b.max_aniso
			&& a.border_color == b.border_color
			&& a.depth_compare == b.depth_compare
			&& a.compare == b.compare;
	}

	u64 hash_sampler_state(const sampler_state& state)
	{
		// Pack the enums into one word and the floats by bit pattern, so the hash
		// stays consistent with operator== without touching padding.
		struct packed
		{
			u32 enums;
			u32 max_aniso;
			u32 min_lod;
			u32 max_lod;
		};

		const packed key = {
			static_cast<u32>(state.wrap_s)
				| (static_cast<u32>(state.wrap_t) << 4)
				| (static_cast<u32>(state.wrap_r) << 8)
				| (static_cast<u32>(state.min_filter) << 12)
				| (static_cast<u32>(state.mag_filter) << 16)
				| (static_cast<u32>(state.border_color) << 20)
				| (static_cast<u32>(state.compare) << 24)
				| (static_cast<u32>(state.depth_compare) << 28),
			state.max_aniso,
			std::bit_cast<u32>(state.min_lod),
			std::bit_cast<u32>(state.max_lod),
		};

		return rpcs3::hash_struct<packed>(key);
	}

	void sampler_cache::shutdown()
	{
		for (const auto& [key, object] : m_cache)
		{
			object->release();
		}

		m_cache.clear();
		m_device = nullptr;
	}

	MTL::SamplerState* sampler_cache::get(const sampler_state& state)
	{
		const auto found = m_cache.find(state);
		if (found != m_cache.end())
		{
			return found->second;
		}

		MTL::SamplerState* object = create_sampler_state(m_device, state);
		if (object)
		{
			m_cache.emplace(state, object);
		}

		return object;
	}
}

#endif // RSX_ENABLE_METAL4
