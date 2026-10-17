#pragma once

#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Utils/color_utils.hpp"

#include <array>
#include <memory>
#include <unordered_map>

namespace mtl
{
	// A Metal allocation held by the application.  A Metal 4 command buffer holds no
	// strong reference to the resources it uses, so an object must survive until the
	// frame that used it completes.  The handle therefore returns the allocation to
	// the resource manager, which releases it after the frame ring is idle
	// (HLD-02 I7).  A handle is never copied, because it owns the allocation.
	class handle
	{
	public:
		handle(const handle&) = delete;
		handle& operator=(const handle&) = delete;

	protected:
		handle() = default;
		explicit handle(resource_manager* resources) : m_resources(resources) {}

		~handle()
		{
			if (!m_allocation || !m_owned)
			{
				return;
			}

			if (m_resources)
			{
				m_resources->defer_release(m_allocation);
			}
			else
			{
				m_allocation->release();
			}
		}

		// A borrowed allocation is owned elsewhere, so this handle does not release
		// it.
		void adopt(MTL::Allocation* allocation, bool owned = true)
		{
			m_allocation = allocation;
			m_owned = owned;
		}

		MTL::Allocation* allocation() const { return m_allocation; }

		resource_manager* m_resources = nullptr;

	private:
		MTL::Allocation* m_allocation = nullptr;
		bool m_owned = true;
	};

	// The Metal channel order of a guest format, A-R-G-B, composed with the RSX
	// remap vector (GLTexture.cpp:894).
	std::array<MTL::TextureSwizzle, 4> apply_swizzle_remap(const std::array<MTL::TextureSwizzle, 4>& layout, const rsx::texture_channel_remap_t& decoded_remap);

	// One Metal texture.  It is the surface store's image_storage_type, so it is
	// always owned by a std::unique_ptr (HLD-01 §3.3; HLD-02 §3.2).  It is
	// polymorphic because the generic store cross-casts an image to rsx::ref_counted
	// (surface_utils.h:504), and only the most derived object holds that base.
	class texture : public handle
	{
	public:
		explicit texture(resource_manager* resources) : handle(resources) { m_image = this; }
		virtual ~texture() = default;

		void assign(MTL::Texture* value) { adopt(value); }
		MTL::Texture* value() const { return static_cast<MTL::Texture*>(allocation()); }

		MTL::ResourceID resource_id() const { return value()->gpuResourceID(); }
		u32 width() const { return static_cast<u32>(value()->width()); }
		u32 height() const { return static_cast<u32>(value()->height()); }
		u32 sample_count() const { return static_cast<u32>(value()->sampleCount()); }
		MTL::PixelFormat pixel_format() const { return value()->pixelFormat(); }
		// The shared descriptor holds the sample count as a u8 (TextureUtils.h:223).
		u8 samples() const { return static_cast<u8>(sample_count()); }

		// The root image of a view, so the generic cache recovers the section's
		// allocation from a swizzled view (texture_cache.h:589).
		texture* image() const { return m_image; }

		// OGL and Metal both swizzle at the view, so no packed map is needed
		// (glutils/image.h:472).
		u32 encoded_component_map() const { return 0u; }

		virtual rsx::format_class format_class() const { return m_format_class; }
		void set_format_class(rsx::format_class format_class) { m_format_class = format_class; }

		void set_native_component_layout(const std::array<MTL::TextureSwizzle, 4>& layout)
		{
			if (m_native_layout == layout)
			{
				return;
			}

			m_native_layout = layout;
			m_views.clear();
		}

		std::array<MTL::TextureSwizzle, 4> get_native_component_layout() const { return m_native_layout; }

		// The sampled view for one unit's remap (GLTexture.cpp:450).  The view is
		// cached on the root and released with it.
		texture* get_view(const rsx::texture_channel_remap_t& remap);

		// The stencil read of a redirected depth unit (REALISATION.md, "The
		// depth/stencil-as-texture cluster").  Only a depth surface has a castable
		// stencil view, so the base returns null and a sampled view delegates to
		// its root, which is the surface.
		virtual texture* stencil_view();

	private:
		texture* m_image = nullptr;
		std::unordered_map<u32, std::unique_ptr<texture>> m_views;
		std::array<MTL::TextureSwizzle, 4> m_native_layout =
		{
			MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue
		};

	protected:
		rsx::format_class m_format_class = rsx::RSX_FORMAT_CLASS_UNDEFINED;
	};

	// One Metal buffer.  It is the surface store's buffer_object_type.  The guest
	// window covers every guest range, so a DMA buffer object borrows it and records
	// the range instead of owning an allocation (HLD-02 §3.5).
	class buffer : public handle
	{
	public:
		explicit buffer(resource_manager* resources) : handle(resources) {}

		void assign(MTL::Buffer* value) { adopt(value); }

		void borrow(MTL::Buffer* value, u32 address, u32 length)
		{
			adopt(value, false);
			base_address = address;
			range_length = length;
		}

		MTL::Buffer* value() const { return static_cast<MTL::Buffer*>(allocation()); }

		// The guest range this object covers (surface_cache_dma.hpp:60,104).
		u32 base_address = 0;
		u32 range_length = 0;

		u32 operator*() const { return range_length; }
	};
}

#endif // RSX_ENABLE_METAL4
