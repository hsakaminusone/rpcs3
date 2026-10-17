#include "MTLDevice.h"

#include "MTLPresenter.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"
#include "MTLCommandRecorder.h"
#include "MTLMetalLayer.h"

#include "Emu/system_config.h"

#include <QuartzCore/QuartzCore.hpp>

namespace mtl
{
	presenter::~presenter()
	{
		shutdown();
	}

	bool presenter::init(const device& dev, void* view)
	{
		if (!view)
		{
			return false;
		}

		m_device = dev.dev();
		m_queue = dev.queue();
		m_view = view;

		// The first present sets the drawable size from the source, so the layer
		// needs no size here.
		m_layer = attach_metal_layer(view, m_device);
		return m_layer != nullptr;
	}

	void presenter::shutdown()
	{
		m_drawable = nullptr;

		if (m_layer)
		{
			detach_metal_layer(m_view, m_layer);
			m_layer = nullptr;
		}

		m_view = nullptr;
		m_device = nullptr;
		m_queue = nullptr;
	}

	bool presenter::acquire(command_recorder& recorder, u32 width, u32 height, MTL::Texture*& target)
	{
		target = nullptr;

		if (!m_layer || !m_queue || !width || !height)
		{
			return false;
		}

		// The drawable is the client area, so a resize sizes the drawable and the
		// output path scales the RSX image into it (HLD-06 §2.3).
		if (width != m_width || height != m_height)
		{
			set_metal_layer_drawable(m_layer, width, height, static_cast<u32>(m_format));
			m_width = width;
			m_height = height;
		}

		update_vsync();

		CA::MetalDrawable* drawable = static_cast<CA::MetalLayer*>(m_layer)->nextDrawable();

		if (!drawable)
		{
			// The drawable pool is empty.  The frame is dropped, not presented late.
			return false;
		}

		// The recorder waits for the drawable before it encodes and signals it after
		// the commit, so the present never leaves the submit path (HLD-03 §4.9).
		recorder.set_present_drawable(drawable);

		target = drawable->texture();

		if (!target)
		{
			recorder.set_present_drawable(nullptr);
			return false;
		}

		m_drawable = drawable;
		return true;
	}

	void presenter::update_vsync()
	{
		const vsync_mode mode = g_cfg.video.vsync;
		const bool sync = (mode == vsync_mode::full);

		// Metal has one display-sync state, so only a full sync is a true vsync.
		// The other two modes present as soon as the drawable is ready, and adaptive
		// paces the present to one refresh interval, which is the closest available
		// behaviour to a sync that yields when a frame is late (HLD-06 §2.8).
		m_present_interval = (mode == vsync_mode::adaptive) ? metal_layer_refresh_interval(m_view) : 0.0;

		if (m_sync_applied && sync == m_sync_enabled)
		{
			return;
		}

		set_metal_layer_sync(m_layer, sync);
		m_sync_enabled = sync;
		m_sync_applied = true;
	}

	void presenter::present(command_recorder& recorder)
	{
		MTL::Drawable* drawable = m_drawable;
		m_drawable = nullptr;

		recorder.submit();

		if (drawable)
		{
			if (m_present_interval > 0.0)
			{
				// The paced present of the adaptive mode.  The layer carries no
				// display sync in that mode, so this is what bounds the rate.
				drawable->presentAfterMinimumDuration(m_present_interval);
			}
			else
			{
				drawable->present();
			}
		}
	}
}

#endif // RSX_ENABLE_METAL4
