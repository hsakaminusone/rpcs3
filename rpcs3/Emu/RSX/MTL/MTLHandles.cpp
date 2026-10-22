#include "MTLHandles.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	std::array<MTL::TextureSwizzle, 4> apply_swizzle_remap(const std::array<MTL::TextureSwizzle, 4>& layout, const rsx::texture_channel_remap_t& decoded_remap)
	{
		return decoded_remap.remap<MTL::TextureSwizzle>(layout, MTL::TextureSwizzleZero, MTL::TextureSwizzleOne);
	}

	texture* texture::get_view(const rsx::texture_channel_remap_t& remap)
	{
		if (m_image != this)
		{
			return m_image->get_view(remap);
		}

		const MTL::TextureType source_type = value()->textureType();

		if (source_type != MTL::TextureType2D && source_type != MTL::TextureTypeCube &&
			source_type != MTL::TextureType2DArray && source_type != MTL::TextureType3D)
		{
			return this;
		}

		constexpr std::array<MTL::TextureSwizzle, 4> identity_layout = {
			MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };

		const auto layout = apply_swizzle_remap(m_native_layout, remap);

		// Metal keeps no swizzle on the base texture, so a view is wanted whenever the
		// composed map is not the identity.  An identity remap alone does not settle it:
		// a format whose host channel order differs from the guest's still needs the view
		// (glutils/image.cpp:481-490 applies the same map to the GL texture itself).
		if (layout == identity_layout)
		{
			return this;
		}

		if (const auto found = m_views.find(remap.encoded); found != m_views.end())
		{
			return found->second.get();
		}

		const MTL::TextureSwizzleChannels channels = MTL::TextureSwizzleChannels::Make(layout[1], layout[2], layout[3], layout[0]);

		// A multisampled source keeps its type.  A cube view stays a cube: the
		// six faces are its slice range, not six 2D array elements.  A 3D view
		// uses depth for the slice range; a 2D array uses its array length.
		// Forcing a cube to 2D makes a texturecube sampling declaration read only
		// one face (B6, REVIEW-FINDINGS.md B14/TX7).
		MTL::TextureType type = source_type;
		if (sample_count() > 1)
		{
			type = MTL::TextureType2DMultisample;
		}

		const NS::UInteger slices =
			source_type == MTL::TextureTypeCube ? 6u :
			source_type == MTL::TextureType3D ? value()->depth() :
			value()->arrayLength();

		auto view = std::make_unique<texture>(m_resources);
		view->m_image = this;
		view->assign(value()->newTextureView(pixel_format(), type,
			NS::Range::Make(0, value()->mipmapLevelCount()),
			NS::Range::Make(0, slices),
			channels));

		texture* result = view.get();
		m_views.emplace(remap.encoded, std::move(view));
		return result;
	}

	texture* texture::stencil_view()
	{
		return (m_image != this) ? m_image->stencil_view() : nullptr;
	}
}

#endif // RSX_ENABLE_METAL4
