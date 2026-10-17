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

		if (remap.encoded == RSX_TEXTURE_REMAP_IDENTITY ||
			sample_count() > 1 ||
			(value()->textureType() != MTL::TextureType2D && value()->textureType() != MTL::TextureTypeCube))
		{
			return this;
		}

		if (const auto found = m_views.find(remap.encoded); found != m_views.end())
		{
			return found->second.get();
		}

		const auto layout = apply_swizzle_remap(m_native_layout, remap);
		const MTL::TextureSwizzleChannels channels = MTL::TextureSwizzleChannels::Make(layout[1], layout[2], layout[3], layout[0]);

		auto view = std::make_unique<texture>(m_resources);
		view->m_image = this;
		view->assign(value()->newTextureView(pixel_format(), MTL::TextureType2D,
			NS::Range::Make(0, value()->mipmapLevelCount()),
			NS::Range::Make(0, value()->arrayLength()),
			channels));

		texture* result = view.get();
		m_views.emplace(remap.encoded, std::move(view));
		return result;
	}
}

#endif // RSX_ENABLE_METAL4
