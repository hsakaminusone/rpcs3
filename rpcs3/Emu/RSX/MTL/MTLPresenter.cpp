#include "MTLDevice.h"

#include "MTLPresenter.h"

#ifdef RSX_ENABLE_METAL4

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
		if (m_layer)
		{
			detach_metal_layer(m_view, m_layer);
			m_layer = nullptr;
		}

		m_view = nullptr;
		m_device = nullptr;
		m_queue = nullptr;
	}

	bool presenter::present(MTL::Texture* source, command_recorder& recorder)
	{
		if (!m_layer || !m_queue || !source)
		{
			return false;
		}

		// A copy needs one sample per pixel on both sides, and the drawable carries
		// the source format.  A multisampled or unsupported source is dropped rather
		// than copied into a mismatched destination.
		if (source->sampleCount() != 1 || !layer_supports_format(source->pixelFormat()))
		{
			return false;
		}

		// The drawable carries the source resolution and the layer scales it to the
		// view, so the present is one copy and no scale pass.
		const u32 width = static_cast<u32>(source->width());
		const u32 height = static_cast<u32>(source->height());
		const MTL::PixelFormat format = source->pixelFormat();

		if (width != m_width || height != m_height || format != m_format)
		{
			set_metal_layer_drawable(m_layer, width, height, static_cast<u32>(format));
			m_width = width;
			m_height = height;
			m_format = format;
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

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();
		if (!encoder)
		{
			recorder.set_present_drawable(nullptr);
			return false;
		}

		encoder->copyFromTexture(source, drawable->texture());
		recorder.close_encoder();
		recorder.submit();

		drawable->present();
		return true;
	}
}

#endif // RSX_ENABLE_METAL4
