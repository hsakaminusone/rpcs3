#include "MTLDevice.h"

#include "MTLPresent.h"

#ifdef RSX_ENABLE_METAL4

#include "../GSFrameBase.h"
#include "../Overlays/overlay_manager.h"

#include "MTLBarrier.h"
#include "MTLCommandRecorder.h"
#include "MTLConstants.h"
#include "MTLFormats.h"
#include "MTLResourceManager.h"

#include "util/asm.hpp"
#include "util/logs.hpp"
#include "../Utils/algorithm.hpp"

#include <algorithm>
#include <mutex>
#include <vector>

namespace mtl
{
	bool frame_output::init(const device& dev, resource_manager& resources)
	{
		m_resources = &resources;

		if (!m_scale.init(dev, resources, true) || !m_nearest.init(dev, resources, false) ||
			!m_calibration.init(dev, resources) || !m_ui.init(dev, resources))
		{
			rsx_log.error("Metal: output pass initialization failed");
			return false;
		}

		return true;
	}

	void frame_output::shutdown()
	{
		m_ui.destroy();

		if (m_capture_target)
		{
			m_resources->defer_release(m_capture_target);
			m_capture_target = nullptr;
		}

		for (capture& pending : m_captures)
		{
			if (pending.buffer)
			{
				m_resources->defer_release(pending.buffer);
			}
		}

		m_captures = {};
	}

	void frame_output::compose(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, MTL::Texture* target,
		const present_source& source, const areau& aspect, const areau& display_area,
		f32 gamma, bool limited_range, bool stereo, rsx::overlays::display_manager* overlays)
	{
		if (!encoder || !target || !source.image || !aspect.width() || !aspect.height())
		{
			return;
		}

		// The calibration pass is the only one that applies gamma, the range
		// conversion and the stereo merge, so the plain scale serves the rest
		// (VKPresent.cpp:828).
		if (limited_range || !rsx::fcmp(gamma, 1.f) || stereo)
		{
			rsx::simple_array<MTL::Texture*> images;
			images.push_back(source.image);

			if (source.image2)
			{
				images.push_back(source.image2);
			}

			m_calibration.run(encoder, constants, aspect, target, images, gamma, limited_range, stereo);
		}
		else
		{
			m_scale.run(encoder, constants, aspect, target, source.image);
		}

		if (!overlays || !overlays->has_visible())
		{
			return;
		}

		// The UI is laid out and updated by the RSX thread, and the run-update chain
		// must not see a list that the thread is changing (VKPresent.cpp:672).
		std::lock_guard lock(*overlays);

		for (const auto& view : overlays->get_views())
		{
			const areau area = view->use_window_space ? display_area : aspect;
			m_ui.run(encoder, constants, area, target, *view);
		}
	}

	void frame_output::request_capture(command_recorder& recorder, constants_heap& constants,
		const present_source& source, bool screenshot, bool with_overlays,
		rsx::overlays::display_manager* overlays)
	{
		const u32 slot = recorder.slot();

		// The slot's own capture is consumed before this frame records one, so a
		// pending capture means the request is a duplicate.
		if (m_captures[slot].buffer || !source.image)
		{
			return;
		}

		const u32 pixel_size = get_pixel_size(source.image->pixelFormat());

		if (!pixel_size)
		{
			rsx_log.error("Metal: unsupported capture format 0x%x", static_cast<u32>(source.image->pixelFormat()));
			return;
		}

		const u32 width = source.width ? std::min<u32>(source.width, static_cast<u32>(source.image->width())) : static_cast<u32>(source.image->width());
		const u32 height = source.height ? std::min<u32>(source.height, static_cast<u32>(source.image->height())) : static_cast<u32>(source.image->height());

		if (!width || !height)
		{
			return;
		}

		const u32 row_pitch = width * pixel_size;
		const u64 length = static_cast<u64>(row_pitch) * height;

		MTL::Buffer* buffer = m_resources->new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

		if (!buffer)
		{
			return;
		}

		m_resources->make_persistent(buffer);

		MTL::Texture* image = source.image;

		if (with_overlays && overlays && overlays->has_visible())
		{
			// The composited capture needs its own image: the overlays must not
			// touch the RSX output that the rest of the frame still reads.
			if (!m_capture_target || m_capture_target->width() != width || m_capture_target->height() != height ||
				m_capture_target->pixelFormat() != image->pixelFormat())
			{
				if (m_capture_target)
				{
					m_resources->defer_release(m_capture_target);
					m_capture_target = nullptr;
				}

				MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
				descriptor->setTextureType(MTL::TextureType2D);
				descriptor->setPixelFormat(image->pixelFormat());
				descriptor->setWidth(width);
				descriptor->setHeight(height);
				descriptor->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
				m_capture_target = m_resources->new_texture(descriptor, MTL::StorageModePrivate);
				descriptor->release();

				if (!m_capture_target)
				{
					m_resources->defer_release(buffer);
					return;
				}

				m_resources->make_persistent(m_capture_target);
			}

			const areau full{0, 0, width, height};

			MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
			MTL::RenderPassColorAttachmentDescriptor* attachment = pass->colorAttachments()->object(0);
			attachment->setTexture(m_capture_target);
			attachment->setLoadAction(MTL::LoadActionClear);
			attachment->setStoreAction(MTL::StoreActionStore);
			attachment->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));

			MTL4::RenderCommandEncoder* pass_encoder = recorder.open_pass(pass);
			pass->release();

			if (!pass_encoder)
			{
				m_resources->defer_release(buffer);
				return;
			}

			m_scale.run(pass_encoder, constants, full, m_capture_target, image);

			std::lock_guard lock(*overlays);

			for (const auto& view : overlays->get_views())
			{
				m_ui.run(pass_encoder, constants, full, m_capture_target, *view);
			}

			recorder.close_encoder();
			image = m_capture_target;
		}

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			m_resources->defer_release(buffer);
			return;
		}

		// The source is written by the frame's own encoders, so the read needs the
		// queue form of the edge (HLD-03 §2.1 item 6).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });

		encoder->copyFromTexture(image, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width, height, 1),
			buffer, 0, row_pitch, row_pitch * height);

		recorder.close_encoder();

		capture& pending = m_captures[slot];
		pending.buffer = buffer;
		pending.width = width;
		pending.height = height;
		pending.is_bgra = is_bgra_format(image->pixelFormat());
		pending.screenshot = screenshot;
		pending.ready = false;
	}

	void frame_output::on_slot_complete(u32 slot)
	{
		if (slot < frame_slot_count)
		{
			m_captures[slot].ready = true;
		}
	}

	bool frame_output::deliver(u32 slot, GSFrameBase& frame)
	{
		if (slot >= frame_slot_count)
		{
			return false;
		}

		capture& pending = m_captures[slot];

		if (!pending.buffer || !pending.ready)
		{
			return false;
		}

		const u8* pixels = static_cast<const u8*>(pending.buffer->contents());
		std::vector<u8> data;

		if (pixels)
		{
			data.assign(pixels, pixels + (static_cast<usz>(pending.width) * pending.height * 4));
		}

		const u32 width = pending.width;
		const u32 height = pending.height;
		const bool is_bgra = pending.is_bgra;
		const bool screenshot = pending.screenshot;

		m_resources->defer_release(pending.buffer);
		pending = {};

		if (data.empty())
		{
			return false;
		}

		if (screenshot)
		{
			frame.take_screenshot(std::move(data), width, height, is_bgra);
		}
		else
		{
			frame.present_frame(std::move(data), width * 4, width, height, is_bgra);
		}

		return true;
	}
}

#endif // RSX_ENABLE_METAL4
