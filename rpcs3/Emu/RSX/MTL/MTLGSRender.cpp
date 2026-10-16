// metal-cpp must be reached before GSRender.h: gcm_enums.h:1183 has
// `using namespace gcm`, which makes Objective-C's `Method` ambiguous in a TU
// that reaches metal-cpp afterwards.
#include "MTLDevice.h"

#include "MTLGSRender.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLCommandRecorder.h"
#include "MTLPresenter.h"
#include "MTLResourceManager.h"
#include "MTLSurfaceCache.h"

namespace mtl
{
	// The four backend objects of one renderer instance.  They are built in
	// on_init_thread and torn down in on_exit, in reverse order.
	struct MTLGSRender::frame_resources
	{
		resource_manager resources;
		command_recorder recorder;
		surface_cache cache;
		presenter present;
	};

	MTLGSRender::MTLGSRender(utils::serial* ar) noexcept
		: GSRender(ar)
	{
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender() = default;

	u64 MTLGSRender::get_cycles()
	{
		return thread_ctrl::get_cycles(static_cast<named_thread<MTLGSRender>&>(*this));
	}

	void MTLGSRender::on_init_thread()
	{
		m_device = device::create();
		if (!m_device)
		{
			die("no Metal 4 device is available");
		}

		// The 16-bool backend contract (RSXThread.h:80; 10 §2.2).
		m_device->fill_backend_config(backend_config);

		// The resource layer and the recorder come up with the device, because the
		// guest window joins the persistent residency set and the ring is sized once
		// (HLD-02 I5, HLD-04 §3.2).
		m_frame_resources = std::make_unique<frame_resources>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = get_frame())
		{
			m_frame_resources->present.init(*m_device, frame->handle());
		}

		GSRender::on_init_thread();
	}

	void MTLGSRender::on_exit()
	{
		if (m_frame_resources)
		{
			// The layer goes first, then the ring drains, then the resources go.
			m_frame_resources->present.shutdown();
			m_frame_resources->recorder.shutdown();
			m_frame_resources->resources.shutdown();
			m_frame_resources.reset();
		}

		m_device.reset();
		GSRender::on_exit();
	}

	// C7.  The RSX output is the surface at the flip address, and it is presented
	// through the one submit path the recorder owns (HLD-03 §4.9).
	void MTLGSRender::flip(const rsx::display_flip_info_t& info)
	{
		if (m_frame_resources && m_frame_resources->present.valid())
		{
			if (surface* source = m_frame_resources->cache.get_surface_at(info.buffer))
			{
				if (texture* output = source->get_surface(rsx::surface_access::memory_read))
				{
					m_frame_resources->recorder.begin_frame();

					// The presenter submits the frame it fills.  A dropped frame is
					// submitted here instead, so the ring advances either way.
					if (!m_frame_resources->present.present(output->value(), m_frame_resources->recorder))
					{
						m_frame_resources->recorder.submit();
					}
				}
			}
		}

		GSRender::flip(info);
	}
}

#endif // RSX_ENABLE_METAL4
