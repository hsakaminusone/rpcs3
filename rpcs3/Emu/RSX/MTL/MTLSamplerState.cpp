#include "MTLSamplerState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"

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

	sampler_state capture_fragment_sampler(u8 unit)
	{
		const rsx::fragment_texture& texture = rsx::method_registers.fragment_textures[unit];

		sampler_state state;

		state.wrap_s = texture.wrap_s();
		state.wrap_t = texture.wrap_t();
		state.wrap_r = texture.wrap_r();
		state.min_filter = texture.min_filter();
		state.mag_filter = texture.mag_filter();
		state.min_lod = texture.min_lod();
		state.max_lod = texture.max_lod();
		state.max_aniso = get_max_anisotropy(texture.max_aniso());
		state.border_color = get_closest_border_color(texture.remapped_border_color());
		state.compare = texture.zfunc();

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
}

#endif // RSX_ENABLE_METAL4
