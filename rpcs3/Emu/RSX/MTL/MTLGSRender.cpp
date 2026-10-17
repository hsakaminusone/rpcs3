// metal-cpp must be reached before GSRender.h: gcm_enums.h:1183 has
// `using namespace gcm`, which makes Objective-C's `Method` ambiguous in a TU
// that reaches metal-cpp afterwards.
#include "MTLDevice.h"

#include "MTLGSRender.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	MTLGSRender::MTLGSRender(utils::serial* ar) noexcept
		: GSRender(ar)
	{
	}

	// Out of line so the unique_ptr<device> teardown sees the complete type
	// (MTLGSRender.h only forward-declares it).
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

		GSRender::on_init_thread();
	}

	void MTLGSRender::on_exit()
	{
		m_device.reset();
		GSRender::on_exit();
	}
}

#endif // RSX_ENABLE_METAL4
