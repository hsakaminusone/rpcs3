#include "MTLDevice.h"

#include "MTLPresenter.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLBarrier.h"
#include "MTLCommandRecorder.h"
#include "MTLMetalLayer.h"

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

	void presenter::present(command_recorder& recorder)
	{
		MTL::Drawable* drawable = m_drawable;
		m_drawable = nullptr;

		recorder.submit();

		if (drawable)
		{
			drawable->present();
		}
	}
}

#endif // RSX_ENABLE_METAL4
