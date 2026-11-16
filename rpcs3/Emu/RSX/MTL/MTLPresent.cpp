#include "MTLDevice.h"
#include "MTLInstrumentation.h"

#include "MTLPresent.h"

#ifdef RSX_ENABLE_METAL4

#include "../GSFrameBase.h"
#include "../Overlays/overlay_manager.h"

#include "Emu/system_config.h"

#include "MTLBarrier.h"
#include "MTLCommandRecorder.h"
#include "MTLConstants.h"
#include "MTLFormats.h"
#include "MTLResourceManager.h"
#include "MTLUpscaleFSR.h"

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
		m_device = &dev;
		m_fsr_init_attempted = false;

		// Named, not folded: a silent `false` out of a four-way chain cannot be
		// attributed from a crash dump, and the pass that failed is the whole
		// content of the report.
		if (!m_scale.init(dev, resources, true))
		{
			rsx_log.error("Metal: output scale pass initialization failed");
			return false;
		}

		if (!m_nearest.init(dev, resources, false))
		{
			rsx_log.error("Metal: output nearest pass initialization failed");
			return false;
		}

		if (!m_calibration.init(dev, resources))
		{
			rsx_log.error("Metal: output calibration pass initialization failed");
			return false;
		}

		if (!m_ui.init(dev, resources))
		{
			rsx_log.error("Metal: output UI pass initialization failed");
			return false;
		}

		// FSR is optional and mode-gated: VK/GL create their upscaler on first use,
		// only when output_scaling selects it.  Initializing it here made a failure
		// in that optional pass fatal to all presentation.

		return true;
	}

	void frame_output::shutdown()
	{
		m_fsr.shutdown();
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

	present_source frame_output::upscale_source(command_recorder& recorder, constants_heap& constants,
		const present_source& source, const areau& aspect)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/upscale");

		// FSR is a pass of its own only in this mode, and it needs a first eye, a
		// source rectangle and a destination to enlarge into.  A stereo frame keeps
		// the plain filter: the two eyes share one pass in the other backends too
		// (VKPresent.cpp:855).
		if (g_cfg.video.output_scaling != output_scaling_mode::fsr ||
			!source.image || source.image2 || !source.width || !source.height ||
			!aspect.width() || !aspect.height())
		{
			// TODO(excise): level 2; the upscale that could not run (area B).
			MTL_PROBE_LATCH(2, "present/upscale refused");
			return source;
		}

		// FSR 1 is an enlargement filter.  When the frame is already at or above the
		// output size the pass declines and the compose's own filter runs, which is
		// the fallback the Vulkan pass takes for the same input (fsr_pass.cpp:304).
		if (!fsr::is_enlargement(source.width, source.height, static_cast<u32>(aspect.width()), static_cast<u32>(aspect.height())))
		{
			// TODO(excise): level 2; the upscale that could not run (area B).
			MTL_PROBE_LATCH(2, "present/upscale refused");
			return source;
		}

		// Create on first use, as VK/GL do.  If compilation fails, run() returns
		// null and the compose keeps its own filter; presentation is unaffected.
		if (!m_fsr_init_attempted)
		{
			m_fsr_init_attempted = true;

			if (m_device && m_resources && !m_fsr.init(*m_device, *m_resources))
			{
				rsx_log.error("Metal: output FSR pass initialization failed; using the compose filter");
			}
		}

		MTL::Texture* image = m_fsr.run(recorder, constants, source.image, source.width, source.height,
			static_cast<u32>(aspect.width()), static_cast<u32>(aspect.height()));

		if (!image)
		{
			return source;
		}

		// TODO(excise): level 2; the upscale ran (area A).
		MTL_PROBE_LATCH(2, "present/upscale ran");

		present_source upscaled = source;
		upscaled.image = image;
		upscaled.width = static_cast<u32>(aspect.width());
		upscaled.height = static_cast<u32>(aspect.height());
		return upscaled;
	}

	void frame_output::compose(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, MTL::Texture* target,
		const present_source& source, const areau& aspect, const areau& display_area,
		f32 gamma, bool limited_range, bool stereo, rsx::overlays::display_manager* overlays)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/compose");
		if (!encoder || !target || !source.image || !aspect.width() || !aspect.height())
		{
			// TODO(excise): level 2; the compose that could not run (area A, area B).
			MTL_PROBE_LATCH(2, "present/compose refused");
			return;
		}

		// TODO(excise): level 2; the compose ran (area A).
		MTL_PROBE_LATCH(2, "present/compose ran");

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
			// The output scaling mode is the filter of the final sample.  Nearest
			// and bilinear differ in the filter only, and the calibration path
			// above samples with the calibration pass in both backends, so the
			// selection lives here (VKPresent.cpp:810-821, :885).
			present_scale_pass& scaler =
				(g_cfg.video.output_scaling == output_scaling_mode::nearest) ? m_nearest : m_scale;

			scaler.run(encoder, constants, aspect, target, source.image);
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

	// An overlay leaves the active list when the UI replaces or removes it, and the
	// manager then keeps it in its dirty list until the renderer has dropped the
	// images it owned (overlay_manager.h:110).  The temporary images belong to this
	// pass, so the frame that ended retires them here (VKPresent.cpp:256).
	void frame_output::retire_overlays(rsx::overlays::display_manager* overlays)
	{
		if (!overlays || !overlays->has_dirty())
		{
			return;
		}

		overlays->lock_shared();

		std::vector<u32> uids;
		uids.reserve(overlays->get_dirty().size());

		for (const auto& view : overlays->get_dirty())
		{
			m_ui.remove_temp_resources(view->uid);
			uids.push_back(view->uid);
		}

		overlays->unlock_shared();
		overlays->dispose(uids);
	}

	void frame_output::request_capture(command_recorder& recorder, constants_heap& constants,
		const present_source& source, bool screenshot, bool with_overlays,
		rsx::overlays::display_manager* overlays)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/capture");
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

			// The capture samples the image the frame's earlier encoders wrote, and
			// the heaps do not track hazards, so the read needs the queue form of
			// the edge, exactly as the drawable compose does (MTLGSRender.cpp:5050).
			// Without it the capture can observe the frame's tiles before they are
			// flushed, and a photographed frame shows content the final image does
			// not.
			emit_queue_barrier(pass_encoder, barrier_spec{ MTL::StageAll, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });

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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/deliver");
		if (slot >= frame_slot_count)
		{
			// TODO(excise): level 2; a slot outside the ring (area B).
			MTL_PROBE_LATCH(2, "present/deliver refused");
			return false;
		}

		// TODO(excise): level 2; the capture reached the ring slot (area B).
		MTL_PROBE_LATCH(2, "present/deliver accepted");

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
