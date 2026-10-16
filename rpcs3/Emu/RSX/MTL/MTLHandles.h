#pragma once

#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

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
			if (!m_allocation)
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

		void adopt(MTL::Allocation* allocation) { m_allocation = allocation; }
		MTL::Allocation* allocation() const { return m_allocation; }

	private:
		resource_manager* m_resources = nullptr;
		MTL::Allocation* m_allocation = nullptr;
	};

	// One Metal texture.  It is the surface store's image_storage_type, so it is
	// always owned by a std::unique_ptr (HLD-01 §3.3; HLD-02 §3.2).
	class texture : public handle
	{
	public:
		explicit texture(resource_manager* resources) : handle(resources) {}

		void reset(MTL::Texture* value) { adopt(value); }
		MTL::Texture* value() const { return static_cast<MTL::Texture*>(allocation()); }

		MTL::ResourceID resource_id() const { return value()->gpuResourceID(); }
		u32 width() const { return static_cast<u32>(value()->width()); }
		u32 height() const { return static_cast<u32>(value()->height()); }
		u32 sample_count() const { return static_cast<u32>(value()->sampleCount()); }
		MTL::PixelFormat pixel_format() const { return value()->pixelFormat(); }
	};

	// One Metal buffer.  It is the surface store's buffer_object_type.
	class buffer : public handle
	{
	public:
		explicit buffer(resource_manager* resources) : handle(resources) {}

		void reset(MTL::Buffer* value) { adopt(value); }
		MTL::Buffer* value() const { return static_cast<MTL::Buffer*>(allocation()); }

		MTL::GPUAddress gpu_address() const { return value()->gpuAddress(); }
		u64 length() const { return value()->length(); }
	};
}

#endif // RSX_ENABLE_METAL4
