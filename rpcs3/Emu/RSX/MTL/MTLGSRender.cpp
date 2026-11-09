// metal-cpp must be reached before GSRender.h: gcm_enums.h:1183 has
// `using namespace gcm`, which makes Objective-C's `Method` ambiguous in a TU
// that reaches metal-cpp afterwards.
#include "MTLDevice.h"

#include "MTLGSRender.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLCommandRecorder.h"
#include "MTLConstants.h"
#include "MTLDrawState.h"
#include "MTLEncoderState.h"
#include "MTLFill.h"
#include "MTLFormats.h"
#include "MTLFramebuffer.h"
#include "MTLInstrumentation.h"
#include "MTLPassPolicy.h"
#include "MTLPassScheduler.h"
#include "MTLPresent.h"
#include "MTLPresenter.h"
#include "MTLProgramPipeline.h"
#include "MTLResourceManager.h"
#include "MTLSamplerState.h"
#include "MTLShader.h"
#include "MTLShaderInterpreter.h"
#include "MTLSurfaceCache.h"
#include "MTLSurfaceState.h"
#include "MTLTextureCache.h"
#include "MTLZCULL.h"

#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/Core/RSXDrawCommands.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"
#include "Emu/RSX/NV47/HW/context_accessors.define.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog_native.h"
#include "Emu/RSX/Overlays/overlay_debug_overlay.h"
#include "Emu/RSX/Program/program_util.h"
#include "Emu/RSX/Program/SPIRVCommon.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "util/asm.hpp"
#include "util/video_provider.h"
#include "util/logs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// The two media-capture triggers are owned by the Qt side and the shared report
// path (RSXThread.cpp:43, VKPresent.cpp:16).
extern atomic_t<bool> g_user_asked_for_screenshot;
extern atomic_t<recording_mode> g_recording_mode;

namespace mtl
{
	// TODO(excise): level 5; defined with the pixel-probe budgets below and called
	// from the capture arming above, so the declaration sits at namespace scope
	// (the arming code lives in the unnamed namespace, the definition does not).
	void diagnostic_reset_window_state();

	// TODO(excise): level 5; the per-frame sampled-image dedupe of a capture
	// window, called from the capture arming above (same namespace split as the
	// window state).
	void diagnostic_reset_frame_state();

	// TODO(excise): level 3; the draw census is collected in the draw path and
	// printed once at teardown, so a run that is quit normally keeps the counts.
	void diagnostic_census_report();

	// TODO(excise): level 5; the rolling tape is frozen where a window is armed,
	// so an automatic trigger keeps the frames before it too.
	void diagnostic_tape_freeze();

	namespace
	{
		// TODO(excise): level 5; the index of the draw being encoded, so a
		// geometry dump can name it without reaching into the draw entry.
		u32 diagnostic_current_draw = 0;

		// EXCISE-BEGIN only-draw-probe
		// TODO(excise): level 5; MTL_PROBE_ONLY_DRAW=<n> or <lo>-<hi>.  Every other
		// draw is counted and dropped before it reaches the encoder, so the target
		// after the run holds exactly the named draws' contribution.  The draw id is
		// the '<n>' of 'draw <n>' in the log, which is also the halo the per-draw
		// records and the pixel probe carry, so a bisect is one binary search by
		// hand.  Reads the environment once; the run that sets it is not a shipping
		// frame.
		struct diagnostic_only_draw_range { bool set = false; u32 lo = 0; u32 hi = ~0u; };

		diagnostic_only_draw_range diagnostic_only_draw()
		{
			static const diagnostic_only_draw_range range = [] {
				diagnostic_only_draw_range result{};
				const char* value = std::getenv("MTL_PROBE_ONLY_DRAW");

				if (value && *value && *value != '0')
				{
					unsigned lo = 0, hi = ~0u;

					if (std::sscanf(value, "%u-%u", &lo, &hi) == 2)
					{
						result.set = true;
						result.lo = lo;
						result.hi = hi;
					}
					else if (std::sscanf(value, "%u", &lo) == 1)
					{
						result.set = true;
						result.lo = lo;
						result.hi = lo;
					}
				}

				return result;
			}();

			return range;
		}
		// EXCISE-END only-draw-probe

		// TODO(excise): level 5; the monotonic subdraw id the per-draw record and
		// the draw-parameter block share.  The ABI's draw_id was always zero, so a
		// dumped draw could not be joined to the census's global id.
		u32 diagnostic_global_draw_id = 0;

		// TODO(excise): whole-unit; level 5; a field-wise hash of the state the pipeline key
		// carries.  Hashing the struct bytes would read the padding the enums
		// leave behind, and the record exists to be compared across frames.
		u32 hash_blend_state(const blend_state& state)
		{
			u32 hash = 2166136261u;
			const auto mix = [&hash](u32 value) { hash = (hash ^ value) * 16777619u; };

			mix(state.enable_mask);
			mix(static_cast<u32>(state.source_rgb));
			mix(static_cast<u32>(state.source_a));
			mix(static_cast<u32>(state.destination_rgb));
			mix(static_cast<u32>(state.destination_a));
			mix(static_cast<u32>(state.equation_rgb));
			mix(static_cast<u32>(state.equation_a));
			mix(state.alpha_test_enabled ? 1u : 0u);
			mix(static_cast<u32>(state.alpha_func));
			mix(std::bit_cast<u32>(state.alpha_ref));
			mix(state.logic_op_enabled ? 1u : 0u);
			mix(static_cast<u32>(state.logic_operation));
			mix(static_cast<u32>(state.shade_mode));
			return hash;
		}

		// The guest bytes an RSX texture can sample.  The footprint is the texture's
		// own guest size, across its mip chain, because the guest memory is what a
		// hazard aliases.  The host image the cache binds may be a view, a temporary
		// copy or a differently-sized section; none of that changes which guest
		// bytes the draw reads.
		template <typename Texture>
		utils::address_range32 sampled_guest_range(const rsx::context* ctx, const Texture& texture)
		{
			if (!texture.enabled())
			{
				return {};
			}

			const u32 address = rsx::get_address(ctx, texture.offset(), texture.location());
			const usz size = rsx::get_texture_size(texture);

			if (!address || !size)
			{
				return {};
			}

			// The RSX address space is 32-bit.  A footprint that would run past the
			// end cannot be expressed; the clamped range is still conservative.
			const usz limit = 0x1'0000'0000ull - address;
			return utils::address_range32::start_length(address, static_cast<u32>(std::min(size, limit)));
		}

		u32 hash_raster_state(const raster_state& state)
		{
			u32 hash = 2166136261u;
			const auto mix = [&hash](u32 value) { hash = (hash ^ value) * 16777619u; };

			mix(static_cast<u32>(state.cull_face));
			mix(state.cull_enabled ? 1u : 0u);
			mix(static_cast<u32>(state.front_face));
			mix(static_cast<u32>(state.front_polygon_mode));
			mix(static_cast<u32>(state.back_polygon_mode));
			mix(state.poly_offset_fill ? 1u : 0u);
			mix(std::bit_cast<u32>(state.clip_min));
			mix(std::bit_cast<u32>(state.clip_max));
			mix(std::bit_cast<u32>(state.point_size));
			mix(std::bit_cast<u32>(state.line_width));
			mix(state.line_smooth ? 1u : 0u);
			mix(state.stipple_enabled ? 1u : 0u);
			mix(state.user_clip_control);

			for (u32 word : state.stipple_pattern)
			{
				mix(word);
			}

			return hash;
		}

#if MTL_DEBUG_LOGS
		// The frame counter the capture window uses.  It advances in open_frame(),
		// the one place every draw frame is opened, so a window covers complete
		// frames rather than an arbitrary run of draws.
		u64 diagnostic_frame_index = 0;
		u64 diagnostic_capture_last_frame = 0;
		bool diagnostic_capture_armed = false;

		// A window can be armed from the draw path (a content, cubemap or video
		// bind), not only from the screenshot key.  The dump itself has to run in
		// the flip, before the current frame's snapshot, so the frames before the
		// trigger are still the history the dump writes.  The flag carries the
		// arming across that gap; without it an automatic window records images
		// and per-draw state but no rings.
		bool diagnostic_capture_dump_pending = false;

		// TODO(excise): level 5; one counter for a presented frame, shared by the
		// image dumps and the ring snapshots.  Two counters would name the same
		// frame differently, and the analysis joins a screenshot to its rings by
		// this number alone.
		u32 diagnostic_present_index = 0;

		// TODO(excise): level 1; the present-gap watchdog.  A frame that never
		// presents is the one failure the RSX thread cannot report, because it is
		// blocked inside the present.  A side thread stamps the gap and the last
		// state, so the run leaves a timestamped record instead of a silent stall.
		std::thread diagnostic_watchdog;
		atomic_t<bool> diagnostic_watchdog_stop{false};
		atomic_t<bool> diagnostic_watchdog_gap{false};
		atomic_t<u64> diagnostic_watchdog_last_flip{0};
		atomic_t<u32> diagnostic_watchdog_present{0};
		atomic_t<u32> diagnostic_watchdog_frame{0};
		atomic_t<u32> diagnostic_watchdog_slot{0};
		atomic_t<u32> diagnostic_watchdog_draw{0};

		void diagnostic_watchdog_main()
		{
			while (!diagnostic_watchdog_stop)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(250));

				const u64 last = diagnostic_watchdog_last_flip.load();
				const u64 now = probe::now_ns();

				if (!last || now <= last)
				{
					continue;
				}

				const u64 gap = now - last;

				if (gap < 1'000'000'000ull)
				{
					continue;
				}

				// Report once per gap, at five seconds: a long shader link or a
				// slow load is not a hang, and repeating the line every quarter
				// second would bury the rest of the log.
				if (gap >= 5'000'000'000ull && !diagnostic_watchdog_gap.exchange(true))
				{
					rsx_log.error("Metal 4: watchdog, no present for %.3f s, present %u, frame %u, slot %u, draw %u",
						static_cast<f64>(gap) / 1e9,
						diagnostic_watchdog_present.load(),
						diagnostic_watchdog_frame.load(),
						diagnostic_watchdog_slot.load(),
						diagnostic_watchdog_draw.load());
				}
			}
		}

		void diagnostic_watchdog_start()
		{
			if (diagnostic_watchdog.joinable())
			{
				return;
			}

			diagnostic_watchdog_stop = false;
			diagnostic_watchdog_gap = false;
			diagnostic_watchdog_last_flip = probe::now_ns();
			diagnostic_watchdog = std::thread(diagnostic_watchdog_main);
		}

		void diagnostic_watchdog_finish()
		{
			if (!diagnostic_watchdog.joinable())
			{
				return;
			}

			diagnostic_watchdog_stop = true;
			diagnostic_watchdog.join();
		}

		// One window per scene class, not one window per run.  The car and its
		// reflection unit only exist once a scene has a car, so a wall-clock
		// window that fires on the title screen would spend the run's only window
		// before the car is drawn.  The gap stops a window from chaining inside
		// one screen while still allowing car select, the race start and a later
		// race frame to be captured separately.
		constexpr u32 diagnostic_capture_window_limit = 5;
		constexpr u64 diagnostic_capture_gap_frames = 1200;
		u32 diagnostic_capture_windows = 0;
		u64 diagnostic_capture_gap_until = 0;

		// TODO(excise): level 5; the captured subdraws whose index array and
		// vertex stream were dumped.  A window covers whole frames, so these
		// budgets stop one frame from spending the window on its background.
		u32 diagnostic_capture_index_dumps = 0;
		u32 diagnostic_capture_vertex_dumps = 0;


		// A manual window is armed from the flip, after the frame's draws are
		// encoded, so its first captured frame is the next one and the gap does
		// not apply: the user pressed the screenshot key on the frame that shows
		// the defect, and waiting out the gap would capture a later scene.  A
		// manual press also supersedes an automatic window that is still open: the
		// detectors can fire at a scene change, and the press is the stronger
		// signal, so refusing it is the one ordering that loses the frame the user
		// pointed at.
		void diagnostic_try_arm_capture(const char* reason, bool manual = false, bool forced = false)
		{
			if (!probe::at_least(5))
			{
				return;
			}

			// A manual capture is the user pointing at a live defect, and a
			// flickering defect needs several presses to catch.  The per-run window
			// budget therefore applies to automatic triggers only; the inter-window
			// gap is still bypassed for a manual press, so the next press can arm as
			// soon as the previous window ends.
			if ((!manual && diagnostic_capture_windows >= diagnostic_capture_window_limit) ||
				(!manual && diagnostic_frame_index <= diagnostic_capture_last_frame) ||
				(!manual && diagnostic_frame_index < diagnostic_capture_gap_until))
			{
				return;
			}

			const u64 first_frame = diagnostic_frame_index + (manual ? 1u : 0u);

			diagnostic_capture_last_frame = first_frame + probe::capture_frame_count() - 1;
			diagnostic_capture_gap_until = diagnostic_capture_last_frame + diagnostic_capture_gap_frames;
			diagnostic_capture_armed = true;
			diagnostic_capture_dump_pending = true;
			++diagnostic_capture_windows;
			diagnostic_reset_window_state();
			diagnostic_tape_freeze();

			MTL_NOTE(2, "Metal 4: probe capture %u armed by %s at frame %llu, window %llu..%llu, %.3f s",
				diagnostic_capture_windows, reason,
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<unsigned long long>(first_frame),
				static_cast<unsigned long long>(diagnostic_capture_last_frame),
				probe::elapsed_seconds());
		}

		bool diagnostic_capture_active()
		{
			return probe::at_least(5) && diagnostic_frame_index <= diagnostic_capture_last_frame;
		}

		void diagnostic_frame_opened()
		{
			++diagnostic_frame_index;

			// The census keys against the frame the draws of this frame land in.
			probe::census_note_frame(diagnostic_frame_index);

			// A finished window disarms.  The limit is per run and the gap is
			// between windows, so a window that stayed armed would spend the whole
			// run's five windows on the first scene it met (the title screen or the
			// menu), which is the one place the car is never drawn.
			if (diagnostic_capture_armed && diagnostic_frame_index > diagnostic_capture_last_frame)
			{
				diagnostic_capture_armed = false;

				// The RCA pack's report: the never-uploaded list, the BC zero-block
				// accounting and the unit-as-target hazard flags, printed once per
				// finished window (the arming frame and the F12 frame land in it).
				char reason[96];
				std::snprintf(reason, sizeof(reason), "capture window closed at frame %llu",
					static_cast<unsigned long long>(diagnostic_frame_index));
				probe::census_print(reason);
			}

			// Each captured frame dumps its own sampled images, so the address list
			// starts empty on every frame of the window (see the declaration).
			if (diagnostic_capture_active())
			{
				diagnostic_reset_frame_state();
			}
		}

		// TODO(excise): level 5; the bisection of the black-texture report.  With
		// MTL_PROBE_VARIANT=replace-textures=red every referenced fragment unit is
		// replaced by a one-texel red image of the unit's own type.  A surface that
		// turns red
		// proves the slot, the sampler and the shader's coordinate path are all
		// right and the image that was bound is wrong; a surface that stays black
		// proves the shader is not sampling what the binder writes.  One run decides
		// the side, and it is visible on screen without reading a log.
		//
		// A one-texel image removes UV and LOD together, so it cannot separate a
		// wrong coordinate from a wrong mip.  Two patterned modes add that split:
		// "uv" is a red/green coordinate gradient, and "checker" colours each
		// four-by-four block by its block index.  A splotch that becomes the wrong
		// pattern is a UV or LOD fault; one that stays black is after the sample.
		enum class replacement_mode
		{
			off,
			red,
			uv,
			checker
		};

		replacement_mode diagnostic_replace_mode()
		{
			static const replacement_mode mode = [] {
				const std::string value = probe::variant_arg("replace-textures");

				if (value == "red" || value == "1")
				{
					return replacement_mode::red;
				}

				if (value == "uv" || value == "2")
				{
					return replacement_mode::uv;
				}

				if (value == "checker" || value == "3")
				{
					return replacement_mode::checker;
				}

				return replacement_mode::off;
			}();

			return mode;
		}

		bool diagnostic_replace_textures()
		{
			return diagnostic_replace_mode() != replacement_mode::off;
		}

		// TODO(excise): level 5; MTL_PROBE_VARIANT=replace-units=0,3 narrows the
		// red replacement to the listed fragment units.  The whole-unit pass
		// proved the sampler path works but cannot say which unit a black surface
		// comes from; this one can, without a rebuild between attempts.
		bool diagnostic_replace_unit(u32 unit)
		{
			static const std::string list = probe::variant_arg("replace-units");

			if (list.empty())
			{
				return true;
			}

			const std::string token = std::to_string(unit);
			usz start = 0;

			while (start <= list.size())
			{
				const usz end = list.find(',', start);
				const usz length = (end == std::string::npos ? list.size() : end) - start;

				if (length == token.size() && list.compare(start, length, token) == 0)
				{
					return true;
				}

				if (end == std::string::npos)
				{
					break;
				}

				start = end + 1;
			}

			return false;
		}

		MTL::ResourceID diagnostic_replacement_texture(resource_manager& resources, rsx::texture_dimension_extended dimension)
		{
			// One cached image per replacement mode and texture kind.  The cache is
			// a diagnostic: a persistent image is deliberately never released, so
			// the cache outlives every bind of the run.
			constexpr u32 mode_count = 3;
			constexpr u32 kind_count = 3;
			static MTL::Texture* cache[mode_count][kind_count] = {};

			const replacement_mode mode = diagnostic_replace_mode();

			if (mode == replacement_mode::off)
			{
				return {};
			}

			u32 kind = 0;
			MTL::TextureType type = MTL::TextureType2D;

			switch (dimension)
			{
			case rsx::texture_dimension_extended::texture_dimension_cubemap:
				kind = 1;
				type = MTL::TextureTypeCube;
				break;
			case rsx::texture_dimension_extended::texture_dimension_3d:
				kind = 2;
				type = MTL::TextureType3D;
				break;
			default:
				break;
			}

			MTL::Texture*& slot = cache[static_cast<u32>(mode) - 1][kind];

			if (slot)
			{
				return slot->gpuResourceID();
			}

			const bool cube = (type == MTL::TextureTypeCube);
			// The gradient needs enough steps to read as a gradient; the checker
			// needs four-texel blocks and a block index small enough to colour.
			const u32 width = (mode == replacement_mode::red) ? 1u : (mode == replacement_mode::uv) ? 256u : 64u;
			const u32 height = width;

			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
			descriptor->setTextureType(type);
			descriptor->setPixelFormat(MTL::PixelFormat::PixelFormatRGBA8Unorm);
			descriptor->setWidth(width);
			descriptor->setHeight(height);
			descriptor->setDepth(1);
			descriptor->setMipmapLevelCount(1);
			descriptor->setSampleCount(1);
			descriptor->setArrayLength(1);
			descriptor->setStorageMode(MTL::StorageModeShared);
			descriptor->setUsage(MTL::TextureUsageShaderRead);

			std::vector<u8> pixels(static_cast<usz>(width) * height * 4);

			for (u32 y = 0; y < height; ++y)
			{
				for (u32 x = 0; x < width; ++x)
				{
					usz offset = (static_cast<usz>(y) * width + x) * 4;

					switch (mode)
					{
					case replacement_mode::red:
						pixels[offset + 0] = 255;
						pixels[offset + 1] = 0;
						pixels[offset + 2] = 0;
						break;
					case replacement_mode::uv:
						pixels[offset + 0] = static_cast<u8>(x * 255 / (width - 1));
						pixels[offset + 1] = static_cast<u8>(y * 255 / (height - 1));
						pixels[offset + 2] = ((x ^ y) & 0x10) ? 64 : 192;
						break;
					case replacement_mode::checker:
					{
						// A four-by-four block carries its own index in red and
						// green and a parity in blue.  A wrong coordinate lands on
						// the wrong block, so the colour changes with the error.
						const u32 block_x = x / 4;
						const u32 block_y = y / 4;
						const u32 index = block_x + block_y * (width / 4);
						pixels[offset + 0] = static_cast<u8>(index * 37);
						pixels[offset + 1] = static_cast<u8>(index * 101);
						pixels[offset + 2] = ((block_x + block_y) & 1) ? 255 : 32;
						break;
					}
					case replacement_mode::off:
						break;
					}

					pixels[offset + 3] = 255;
				}
			}

			slot = resources.new_texture(descriptor, MTL::StorageModeShared);
			descriptor->release();

			if (!slot)
			{
				return {};
			}

			resources.make_persistent(slot);

			const MTL::Region region = MTL::Region::Make2D(0, 0, width, height);
			const usz row_bytes = static_cast<usz>(width) * 4;

			if (cube)
			{
				for (u32 face = 0; face < 6; ++face)
				{
					slot->replaceRegion(region, 0, face, pixels.data(), row_bytes, row_bytes * height);
				}
			}
			else
			{
				slot->replaceRegion(region, 0, pixels.data(), row_bytes);
			}

			return slot->gpuResourceID();
		}

		// The sampled host images of one capture frame are dumped once each.  The
		// key is the host image itself, not the guest address: one guest address
		// names several host images at once (L172), and an address-only dedupe kept
		// the first of them and silently dropped the rest, which is why the
		// present-chain input image of 0x50110000 1280x720 was never dumped.  The
		// budget is per window and per frame, like the address key it replaces.
		// TODO(excise): level 5; the budget is per capture window.
		constexpr u32 diagnostic_capture_image_limit = 512;
		// TODO(excise): level 5; the disk budgets of the deep dumps.  One capture
		// window dumps at most this many frames of sampled images (the failing
		// frame is the first captured frame), and a run dumps at most this many
		// image files in total, so one capture window cannot fill a disk.  Compile
		// constants, not switches: a run that wants more opens a shorter window.
		constexpr u32 diagnostic_capture_frame_budget = 6;
		constexpr u32 diagnostic_capture_run_budget = 2048;
		const void* diagnostic_capture_images[diagnostic_capture_image_limit] = {};
		u32 diagnostic_capture_image_count = 0;
		u32 diagnostic_window_dump_frames = 0;
		u32 diagnostic_frame_dump_images = 0;
		u32 diagnostic_run_dump_images = 0;
		bool diagnostic_run_dump_budget_reported = false;

		bool diagnostic_capture_image_seen(const void* image)
		{
			for (u32 index = 0; index < diagnostic_capture_image_count; ++index)
			{
				if (diagnostic_capture_images[index] == image)
				{
					return true;
				}
			}

			return false;
		}

		// TODO(excise): level 5; the disk budget for the deep image dumps.  A
		// frame that starts dumping runs to its own end (the failing frame is
		// the evidence), but at most diagnostic_capture_frame_budget frames of a
		// window and diagnostic_capture_run_budget images in a run reach the
		// disk.  When the run budget runs out, the remaining samples are
		// counted, not dumped, and the one notice says why.
		bool diagnostic_image_dump_allowed()
		{
			if (diagnostic_run_dump_images >= diagnostic_capture_run_budget)
			{
				if (!diagnostic_run_dump_budget_reported)
				{
					diagnostic_run_dump_budget_reported = true;
					MTL_NOTE(2, "Metal 4: capture image budget %u exhausted; remaining samples are counted, not dumped", diagnostic_capture_run_budget);
				}

				return false;
			}

			return diagnostic_frame_dump_images || diagnostic_window_dump_frames < diagnostic_capture_frame_budget;
		}

		void diagnostic_image_dumped()
		{
			if (!diagnostic_frame_dump_images)
			{
				++diagnostic_window_dump_frames;
			}

			diagnostic_frame_dump_images++;
			diagnostic_run_dump_images++;
		}

		// TODO(excise): level 5; formats a byte block for the deep dumps.
		std::string dump_bytes(const void* data, usz size)
		{
			if (!data)
			{
				return "(null)";
			}

			static constexpr char digits[] = "0123456789abcdef";
			const u8* bytes = static_cast<const u8*>(data);
			std::string result;
			result.reserve(size * 3);

			for (usz index = 0; index < size; ++index)
			{
				if (index && (index % 4) == 0)
				{
					result += ' ';
				}

				result += digits[bytes[index] >> 4];
				result += digits[bytes[index] & 0x0f];
			}

			return result;
		}

		std::string dump_guest(u32 address, u32 words)
		{
			static constexpr char digits[] = "0123456789abcdef";
			std::string result;
			result.reserve(words * 9);

			// A diagnostic read must not fault: a fault on the RSX thread flushes
			// and invalidates the sections that overlap the page, so reading a live
			// render target here would change the frame it is measuring.  The sudo
			// mapping is the read that bypasses the protection the renderer owns.
			const auto* words_at = vm::get_super_ptr<u32>(address);

			for (u32 index = 0; index < words; ++index)
			{
				const u32 word = static_cast<u32>(words_at[index]);

				for (int shift = 28; shift >= 0; shift -= 4)
				{
					result += digits[(word >> shift) & 0x0f];
				}

				result += ((index % 4) == 3) ? ' ' : ',';
			}

			return result;
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the index array of a captured subdraw.  A menu
		// glyph that drops out is either the atlas or the quad that samples it, and
		// the index list is what separates the two: a wrong glyph index is visible
		// here, a missing glyph is visible in the atlas dump.
		void diagnostic_dump_indices(const char* label, const void* data, u32 count, bool wide)
		{
			if (!diagnostic_capture_active() || !data || !count ||
				diagnostic_capture_index_dumps >= 16)
			{
				return;
			}

			++diagnostic_capture_index_dumps;

			const u32 shown = std::min(count, 12u);
			char text[512];
			usz length = std::snprintf(text, sizeof(text), "Metal 4: capture indices %s, wide %u, count %u, first",
				label, wide ? 1u : 0u, count);

			for (u32 index = 0; index < shown && length < sizeof(text); ++index)
			{
				const u32 value = wide ? static_cast<const u32*>(data)[index]
					: static_cast<u32>(static_cast<const u16*>(data)[index]);
				length += std::snprintf(text + length, sizeof(text) - length, " %u", value);
			}

			if (count > shown && length < sizeof(text))
			{
				length += std::snprintf(text + length, sizeof(text) - length, ", last");

				for (u32 index = std::max(shown, count - 2u); index < count && length < sizeof(text); ++index)
				{
					const u32 value = wide ? static_cast<const u32*>(data)[index]
						: static_cast<u32>(static_cast<const u16*>(data)[index]);
					length += std::snprintf(text + length, sizeof(text) - length, " %u", value);
				}
			}

			MTL_NOTE(5, "%s", text);
		}
#endif // MTL_DEBUG_LOGS

		// The host index array and the vertex range of one subdraw
		// (GLVertexBuffers.cpp:66-150).  Metal has no fan, no quad list, no polygon
		// and no line loop (MTLDrawState.cpp:25), so those primitives expand into a
		// native index array in the shared writer (Common/BufferUtils.h:31).
		struct vertex_input_state
		{
			bool index_rebase = false;
			u32 min_index = 0;
			u32 max_index = 0;
			u32 vertex_draw_count = 0;
			u32 vertex_index_offset = 0;

			// Metal takes the index array as a GPU address argument, not an
			// argument-table entry, so it is a ring offset and a byte length.
			bool has_index_buffer = false;
			rsx::index_array_type index_type = rsx::index_array_type::u32;
			u32 index_offset = 0;
			u32 index_length = 0;
		};

		// The expansion predicate.  A native primitive reads the guest array
		// directly; the rest go through the shared writer.
		bool expands_primitive(rsx::primitive_type primitive)
		{
			return !::is_primitive_native(primitive);
		}

		// Metal restarts a strip at the largest index value and has no toggle, so a
		// guest strip with restart disabled cannot keep 0xFFFF as a vertex index in
		// a 16-bit array (MTLDrawState.cpp:25).  The strip array is widened instead.
		bool strip_topology(rsx::primitive_type primitive)
		{
			return primitive == rsx::primitive_type::line_strip ||
				primitive == rsx::primitive_type::triangle_strip ||
				primitive == rsx::primitive_type::quad_strip;
		}

		std::tuple<u32, u32, u32> widen_index_array(std::span<std::byte> dst, std::span<const std::byte> src)
		{
			const u32 count = static_cast<u32>(std::min(dst.size() / sizeof(u32), src.size() / sizeof(u16)));
			u32 min_index = 0;
			u32 max_index = 0;

			for (u32 i = 0; i < count; ++i)
			{
				be_t<u16> raw;
				std::memcpy(&raw, src.data() + i * sizeof(u16), sizeof(u16));

				const u32 index = raw.value();
				std::memcpy(dst.data() + i * sizeof(u32), &index, sizeof(u32));

				min_index = i ? std::min(min_index, index) : index;
				max_index = i ? std::max(max_index, index) : index;
			}

			return { min_index, max_index, count };
		}

		struct draw_command_visitor
		{
			const rsx::context& ctx;
			byte_ring& index_ring;
			const rsx::vertex_input_layout& layout;

			vertex_input_state operator()(const rsx::draw_array_command&) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const u32 vertex_count = clause.get_elements_count();
				const u32 min_index = clause.min_index();

				vertex_input_state result;
				result.min_index = min_index;
				result.max_index = (min_index + vertex_count) - 1;
				result.vertex_draw_count = vertex_count;

				if (expands_primitive(clause.primitive))
				{
					const u32 index_count = ::get_index_count(clause.primitive, vertex_count);
					u32 offset = 0;
					void* dst = index_ring.alloc(index_count * sizeof(u32), offset);
					if (!dst)
					{
						// TODO(excise): level 2; a draw the ring could not hold is
						// dropped silently, which is one way a quad disappears.
						MTL_PROBE_LATCH(2, "draw/skipped ring exhausted expansion");
						return {};
					}

					::write_index_array_for_non_indexed_non_native_primitive_to_buffer(
						static_cast<char*>(dst), clause.primitive, vertex_count);

#if MTL_DEBUG_LOGS
					diagnostic_dump_indices("expansion", dst, index_count, true);
#endif

#if MTL_DEBUG_LOGS
					// TODO(excise): level 3; the expanded index array of the first draws.
					{
						static u32 diagnostic_expansions = 0;

						if (diagnostic_expansions < 12)
						{
							const u32* expanded = static_cast<const u32*>(dst);
							const auto at = [&](u32 slot) -> u32 { return slot < index_count ? expanded[slot] : 0u; };

							MTL_NOTE(3, "Metal 4: expansion %u, primitive %u, vertices %u, indices %u, min %u, first %u %u %u %u %u %u",
								diagnostic_expansions, static_cast<u32>(clause.primitive), vertex_count, index_count, min_index,
								at(0), at(1), at(2), at(3), at(4), at(5));
						}

						diagnostic_expansions++;
					}
#endif // MTL_DEBUG_LOGS

					result.vertex_draw_count = index_count;
					result.has_index_buffer = true;
					result.index_type = rsx::index_array_type::u32;
					result.index_offset = offset;
					result.index_length = index_count * sizeof(u32);
				}

				return result;
			}

			vertex_input_state operator()(const rsx::draw_indexed_array_command& command) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const rsx::index_array_type type = clause.is_immediate_draw
					? rsx::index_array_type::u32
					: ctx.register_state->index_type();

				const u32 vertex_count = clause.get_elements_count();
				const u32 index_count = expands_primitive(clause.primitive)
					? ::get_index_count(clause.primitive, vertex_count)
					: vertex_count;

				const bool widen = type == rsx::index_array_type::u16 &&
					!ctx.register_state->restart_index_enabled() &&
					!expands_primitive(clause.primitive) &&
					strip_topology(clause.primitive);

				const u32 type_size = widen ? static_cast<u32>(sizeof(u32)) : ::get_index_type_size(type);
				const u32 length = index_count * type_size;
				u32 offset = 0;
				void* dst = index_ring.alloc(length, offset);
				if (!dst)
				{
					// TODO(excise): level 2; see the expansion site.
					MTL_PROBE_LATCH(2, "draw/skipped ring exhausted indexed");
					return {};
				}

				u32 min_index = 0;
				u32 max_index = 0;
				u32 written = index_count;

				if (widen)
				{
					std::tie(min_index, max_index, written) = widen_index_array(
						{ static_cast<std::byte*>(dst), length },
						command.raw_index_buffer);
				}
				else
				{
					std::tie(min_index, max_index, written) = ::write_index_array_data_to_buffer(
						{ static_cast<std::byte*>(dst), length },
						command.raw_index_buffer,
						type,
						clause.primitive,
						ctx.register_state->restart_index_enabled(),
						ctx.register_state->restart_index(),
						expands_primitive);
				}

#if MTL_DEBUG_LOGS
				diagnostic_dump_indices("indexed", dst, written, widen || (type == rsx::index_array_type::u32));

				// TODO(excise): level 3; the written index array of the first draws.
				{
					static u32 diagnostic_indices = 0;

					if (diagnostic_indices < 12)
					{
						const u16* short_indices = static_cast<const u16*>(dst);
						const u32* long_indices = static_cast<const u32*>(dst);
						const bool wide = widen || (type == rsx::index_array_type::u32);

						MTL_NOTE(3, "Metal 4: indices %u, primitive %u, wide %u, count %u, written %u, length %u, min %u max %u, first %u %u %u %u %u %u",
							diagnostic_indices, static_cast<u32>(clause.primitive), wide ? 1u : 0u, index_count, written, length,
							min_index, max_index,
							wide ? long_indices[0] : short_indices[0], wide ? long_indices[1] : short_indices[1],
							wide ? long_indices[2] : short_indices[2], wide ? long_indices[3] : short_indices[3],
							wide ? long_indices[4] : short_indices[4], wide ? long_indices[5] : short_indices[5]);
					}

					diagnostic_indices++;
				}
#endif // MTL_DEBUG_LOGS

				vertex_input_state result;
				result.min_index = min_index;
				result.max_index = max_index;
				result.vertex_draw_count = written;
				result.has_index_buffer = true;
				result.index_type = widen ? rsx::index_array_type::u32 : type;
				result.index_offset = offset;
				result.index_length = length;

				if (min_index > max_index || (min_index == max_index && clause.primitive != rsx::primitive_type::points))
				{
					// An empty set: nothing to draw.
					result.vertex_draw_count = 0;
					return result;
				}

				// The vertex range is offset by the base and the shader is told to
				// offset the vertex id, which the modulo op needs
				// (RSXVertexFetch.glsl:198).
				result.index_rebase = true;
				result.vertex_index_offset = ctx.register_state->vertex_data_base_index();
				return result;
			}

			vertex_input_state operator()(const rsx::draw_inlined_array&) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const u32 stream_length = static_cast<u32>(clause.inline_vertex_array.size());
				const u32 vertex_count = (stream_length * sizeof(u32)) / layout.interleaved_blocks[0]->attribute_stride;

				vertex_input_state result;
				result.max_index = vertex_count;
				result.vertex_draw_count = vertex_count;

				if (expands_primitive(clause.primitive))
				{
					const u32 index_count = ::get_index_count(clause.primitive, vertex_count);
					u32 offset = 0;
					void* dst = index_ring.alloc(index_count * sizeof(u32), offset);
					if (!dst)
					{
						// TODO(excise): level 2; see the expansion site.
						MTL_PROBE_LATCH(2, "draw/skipped ring exhausted inline");
						return {};
					}

					::write_index_array_for_non_indexed_non_native_primitive_to_buffer(
						static_cast<char*>(dst), clause.primitive, vertex_count);

#if MTL_DEBUG_LOGS
					diagnostic_dump_indices("inline", dst, index_count, true);
#endif

					result.vertex_draw_count = index_count;
					result.has_index_buffer = true;
					result.index_type = rsx::index_array_type::u32;
					result.index_offset = offset;
					result.index_length = index_count * sizeof(u32);
				}

				return result;
			}
		};
#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the per-draw state block, sampled for the first draws.
		void dump_draw_state(const rsx::context& ctx, u32 index, const surface_state& surface, const pipeline_properties& properties,
			const framebuffer& attachments, const rsx::framebuffer_layout& layout, const areau& scissor, bool supports_bc)
		{
			const blend_state encoded_blend = capture_blend_state(ctx);
			const raster_state raster = capture_raster_state(ctx);
			const depth_stencil_state depth = capture_depth_stencil_state(ctx.draw_state.depth_stencil);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, primitive %u, color target %u, color format %u, depth format %u, aa %u, raster type %u, clip %ux%u, depth enabled %u",
				index, static_cast<u32>(ctx.register_state->current_draw_clause.primitive), static_cast<u32>(surface.color_target),
				static_cast<u32>(surface.color_format), static_cast<u32>(surface.depth_format), static_cast<u32>(surface.antialias),
				static_cast<u32>(surface.raster_type), static_cast<u32>(surface.clip_width), static_cast<u32>(surface.clip_height),
				ctx.draw_state.depth_stencil.depth_test_enabled ? 1u : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, rsx color mask %u%u%u%u, blend enable mask 0x%x, factors src %u/%u dst %u/%u, equations %u/%u, alpha test %u, logic op %u/%u, shade %u",
				index, ctx.draw_state.blend.color_mask[0].r ? 1u : 0u, ctx.draw_state.blend.color_mask[0].g ? 1u : 0u,
				ctx.draw_state.blend.color_mask[0].b ? 1u : 0u, ctx.draw_state.blend.color_mask[0].a ? 1u : 0u,
				ctx.draw_state.blend.enable_mask,
				static_cast<u32>(encoded_blend.source_rgb), static_cast<u32>(encoded_blend.source_a),
				static_cast<u32>(encoded_blend.destination_rgb), static_cast<u32>(encoded_blend.destination_a),
				static_cast<u32>(encoded_blend.equation_rgb), static_cast<u32>(encoded_blend.equation_a),
				encoded_blend.alpha_test_enabled ? 1u : 0u, encoded_blend.logic_op_enabled ? 1u : 0u,
				static_cast<u32>(encoded_blend.logic_operation), static_cast<u32>(encoded_blend.shade_mode));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped write mask 0x%x 0x%x 0x%x 0x%x, attachment count %u, samples %u, alpha to coverage %u, alpha to one %u, raster %u, topology %u",
				index, static_cast<u32>(properties.write_mask[0]), static_cast<u32>(properties.write_mask[1]),
				static_cast<u32>(properties.write_mask[2]), static_cast<u32>(properties.write_mask[3]),
				properties.attachment_count, properties.sample_count, properties.alpha_to_coverage ? 1u : 0u,
				properties.alpha_to_one ? 1u : 0u, properties.raster_enabled ? 1u : 0u,
				static_cast<u32>(properties.input_primitive_topology));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped blend mask 0x%x src %u/%u dst %u/%u equations %u/%u alpha test %u logic %u/%u shade %u",
				index, properties.blend.enable_mask,
				static_cast<u32>(properties.blend.source_rgb), static_cast<u32>(properties.blend.source_a),
				static_cast<u32>(properties.blend.destination_rgb), static_cast<u32>(properties.blend.destination_a),
				static_cast<u32>(properties.blend.equation_rgb), static_cast<u32>(properties.blend.equation_a),
				properties.blend.alpha_test_enabled ? 1u : 0u, properties.blend.logic_op_enabled ? 1u : 0u,
				static_cast<u32>(properties.blend.logic_operation), static_cast<u32>(properties.blend.shade_mode));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, rsx depth test %u func %u write %u, stencil test %u, cull %u/%u, front face %u, polygon %u/%u, clip %.4f..%.4f, point %.3f",
				index, ctx.draw_state.depth_stencil.depth_test_enabled ? 1u : 0u,
				static_cast<u32>(ctx.draw_state.depth_stencil.depth_func), ctx.draw_state.depth_stencil.depth_write_enabled ? 1u : 0u,
				ctx.draw_state.depth_stencil.stencil_test_enabled ? 1u : 0u, raster.cull_enabled ? 1u : 0u,
				static_cast<u32>(raster.cull_face), static_cast<u32>(raster.front_face),
				static_cast<u32>(raster.front_polygon_mode), static_cast<u32>(raster.back_polygon_mode),
				static_cast<f64>(raster.clip_min), static_cast<f64>(raster.clip_max), static_cast<f64>(raster.point_size));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped cull %u winding %u fill %u, depth compare %u depth write %u",
				index, static_cast<u32>(capture_cull_mode(raster)), static_cast<u32>(capture_front_face(raster)),
				static_cast<u32>(capture_fill_mode(raster)), static_cast<u32>(depth.depth_compare), depth.depth_write ? 1u : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, scissor %d %d %ux%u, layout color 0x%x 0x%x 0x%x 0x%x pitch %u %u %u %u, zeta 0x%x pitch %u, target %u, aa %u",
				index, scissor.x1, scissor.y1, scissor.width(), scissor.height(),
				layout.color_addresses[0], layout.color_addresses[1], layout.color_addresses[2], layout.color_addresses[3],
				layout.color_pitch[0], layout.color_pitch[1], layout.color_pitch[2], layout.color_pitch[3],
				layout.zeta_address, layout.zeta_pitch, static_cast<u32>(layout.target), static_cast<u32>(layout.aa_mode));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, pass colors %u samples %u size %ux%u depth %u, color0 pixel format %u %ux%u samples %u",
				index, attachments.color_count(), attachments.sample_count(), attachments.width(), attachments.height(),
				attachments.has_depth() ? 1u : 0u,
				attachments.color(0) ? static_cast<u32>(attachments.color(0)->pixel_format()) : 0u,
				attachments.color(0) ? attachments.color(0)->width() : 0u,
				attachments.color(0) ? attachments.color(0)->height() : 0u,
				attachments.color(0) ? attachments.color(0)->samples() : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, clear color %.4f %.4f %.4f %.4f, clear depth %.4f stencil %u, clear covers %u",
				index, static_cast<f64>(surface.clear_color.red), static_cast<f64>(surface.clear_color.green),
				static_cast<f64>(surface.clear_color.blue), static_cast<f64>(surface.clear_color.alpha),
				static_cast<f64>(surface.clear_depth), static_cast<u32>(surface.clear_stencil),
				ctx.draw_state.clear.scissor_covers_surface ? 1u : 0u);

			for (u32 unit = 0; unit < 16; ++unit)
			{
				const auto& tex = ctx.register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 gcm_format = tex.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
				const auto swizzle = get_texture_component_layout(gcm_format);

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: draw %u, unit %u, format 0x%x, masked 0x%x, host pixel format %u, swizzle A%u R%u G%u B%u, %ux%u, mipmaps %u, dimension %u",
					index, unit, static_cast<u32>(tex.format()), gcm_format,
					static_cast<u32>(get_texture_format(gcm_format, supports_bc)),
					static_cast<u32>(swizzle[0]), static_cast<u32>(swizzle[1]), static_cast<u32>(swizzle[2]), static_cast<u32>(swizzle[3]),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), static_cast<u32>(tex.mipmap()),
					static_cast<u32>(tex.get_extended_texture_dimension()));
			}
		}

		// TODO(excise): level 3; the draw census.  The level-2 histograms count
		// ucode hashes, which are stable but opaque: a hash cannot be turned into
		// the `fp-<id>` artifact or the guest address a capture needs.  The census
		// keys one row by the two programs, the colour target and the sampled
		// addresses, prints the first draw of each row with its unit list, and
		// counts the rest.  It is the map from a scene ("the menu text draw", "the
		// car body") to the ids a capture arms on.  The row is bounded because a
		// scene has tens of distinct draws, not thousands.
		struct diagnostic_census_entry
		{
			u64 key = 0;
			u64 count = 0;
			u32 first_draw = 0;
		};

		constexpr u32 diagnostic_census_limit = 2048;
		diagnostic_census_entry diagnostic_census[diagnostic_census_limit];
		u32 diagnostic_census_count = 0;
		u64 diagnostic_census_draws = 0;
		u64 diagnostic_census_overflow = 0;

		u64 diagnostic_census_mix(u64 hash, u64 value)
		{
			return (hash ^ value) * 1099511628211ull;
		}

		void diagnostic_census_draw(const rsx::context& ctx, u32 draw, u32 vp_id, u64 vp_hash, u32 fp_id, u64 fp_hash,
			bool interpreter, const rsx::framebuffer_layout& layout, const surface_state& surface)
		{
			if (!probe::at_least(3))
			{
				return;
			}

			u64 key = 1469598103934665603ull;
			key = diagnostic_census_mix(key, vp_hash);
			key = diagnostic_census_mix(key, fp_hash);
			key = diagnostic_census_mix(key, layout.color_addresses[0]);
			key = diagnostic_census_mix(key, layout.zeta_address);
			key = diagnostic_census_mix(key, (static_cast<u64>(surface.color_format) << 32) | static_cast<u64>(surface.depth_format));

			for (u32 unit = 0; unit < rsx::limits::fragment_textures_count; ++unit)
			{
				const auto& tex = ctx.register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				key = diagnostic_census_mix(key, rsx::get_address(&ctx, tex.offset(), tex.location()));
				key = diagnostic_census_mix(key, static_cast<u32>(tex.format()));
			}

			++diagnostic_census_draws;

			for (u32 index = 0; index < diagnostic_census_count; ++index)
			{
				if (diagnostic_census[index].key == key)
				{
					++diagnostic_census[index].count;
					return;
				}
			}

			if (diagnostic_census_count >= diagnostic_census_limit)
			{
				++diagnostic_census_overflow;
				return;
			}

			const u32 index = diagnostic_census_count++;
			diagnostic_census[index] = { key, 1, draw };

			// The unit list is only built for a new row: the key above is the
			// whole signature, and formatting it on every draw would put a
			// snprintf in the hot path of a level-3 run.
			char units[512] = {};
			u32 units_len = 0;

			for (u32 unit = 0; unit < rsx::limits::fragment_textures_count; ++unit)
			{
				const auto& tex = ctx.register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 address = rsx::get_address(&ctx, tex.offset(), tex.location());
				const bool is_target =
					address == layout.color_addresses[0] || address == layout.color_addresses[1] ||
					address == layout.color_addresses[2] || address == layout.color_addresses[3] ||
					address == layout.zeta_address;

				const u32 remaining = (units_len < sizeof(units)) ? static_cast<u32>(sizeof(units) - units_len) : 0u;
				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(units + units_len, remaining, " u%u=0x%x/%x/%ux%u%s",
					unit, address, static_cast<u32>(tex.format()), static_cast<u32>(tex.width()),
					static_cast<u32>(tex.height()), is_target ? "/target" : "");

				if (written <= 0)
				{
					break;
				}

				units_len += static_cast<u32>(written);

				if (units_len >= sizeof(units) - 64)
				{
					break;
				}
			}

			// TODO(excise): level 3; one line per distinct draw signature.
			MTL_NOTE(3, "Metal 4: census %u, first draw %u, vp %u 0x%llx, fp %u 0x%llx, interpreter %u, target %u, color 0x%x fmt 0x%x %ux%u, depth 0x%x fmt 0x%x, units:%s",
				index, draw, vp_id, static_cast<unsigned long long>(vp_hash),
				fp_id, static_cast<unsigned long long>(fp_hash), interpreter ? 1u : 0u,
				static_cast<u32>(layout.target), layout.color_addresses[0],
				static_cast<u32>(surface.color_format), static_cast<u32>(layout.width), static_cast<u32>(layout.height),
				layout.zeta_address, static_cast<u32>(surface.depth_format), units_len ? units : " none");
		}
#endif // MTL_DEBUG_LOGS

		// The PSO key of one draw (HLD-05 §3.5).  Every field is a latch the encoder
		// state does not carry, so the key holds exactly the pipeline's inputs.
		pipeline_properties capture_pipeline_properties(const rsx::context& ctx, const surface_state& surface, const RSXVertexProgram& vertex)
		{
			blend_state blend = capture_blend_state(ctx);
			const raster_state raster = capture_raster_state(ctx);
			const anti_aliasing_state aa = capture_anti_aliasing_state(ctx.draw_state.multisample);

			// The guest drives the compose epilogue with the programmable-blend
			// descriptor bits (signed equations, forced blending); the epilogue blends
			// in the fragment shader through the framebuffer fetch.  The fixed-function
			// blend unit must not run on top of it — VK drops the FF blend here for the
			// same reason (RSX forces PB for any draw with a signed equation).  The
			// RSX bytes are otherwise unchanged; this only clears the FF enable mask.
			if (ctx.program.fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
			{
				blend.enable_mask = 0;
			}

			pipeline_properties properties;
			properties.color_format = surface.color_format;
			properties.attachment_count = static_cast<u8>(surface.color_attachment_count());
			properties.sample_count = surface.sample_count();
			properties.alpha_to_coverage = aa.alpha_to_coverage;
			properties.alpha_to_one = aa.alpha_to_one;
			properties.blend = blend;

			// The class names the draw's primitive family, which the encoder's draw
			// call restates.  A vertex program whose output mask declares the
			// point-size output writes [[point_size]] (MTLVertexProgram.cpp:408), and
			// the Metal compiler accepts that output with the point or the unspecified
			// class only: "Vertex shader writes point size but inputPrimitiveTopology
			// is MTLPrimitiveTopologyClassTriangle" (measured on the target device).
			// The RSX ignores the output of a non-point primitive, so such a draw
			// takes the class that constrains nothing; a point draw keeps the point
			// class, which is the class the output belongs to.
			const MTL::PrimitiveTopologyClass draw_class = get_primitive_topology_class(ctx.register_state->current_draw_clause.primitive);
			properties.input_primitive_topology = draw_class;

			if (draw_class != MTL::PrimitiveTopologyClassPoint &&
				(vertex.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE))
			{
				properties.input_primitive_topology = MTL::PrimitiveTopologyClassUnspecified;
			}

			// The RSX culls triangles only and Metal has no cull-both mode, so a
			// triangle-class cull-both draw drops the rasteriser while a point or
			// line draw keeps it (VK_CULL_MODE_FRONT_AND_BACK spares them too;
			// VKGSRender.cpp:212-218).  The draw's family decides this, not the class
			// the point-size rule above may have put in the key.
			properties.raster_enabled = !(culls_both_faces(raster) && draw_class == MTL::PrimitiveTopologyClassTriangle);

			for (u32 index = 0; index < properties.attachment_count; ++index)
			{
				properties.write_mask[index] = capture_color_write_mask(ctx.draw_state.blend.color_mask[index]);
			}

			return properties;
		}

		// The programmable-blend push constant (MTLFragmentProgram.cpp:183).  The
		// factor and equation fields pack the RSX enums, RGB in the low half and
		// alpha in the high half (RSXProgrammableBlendPrologue.glsl:7-9,106-108).
		u32 pack_blend_pair(u32 rgb, u32 a)
		{
			return (rgb & 0xffff) | (a << 16);
		}
	}

	// The backend objects of one renderer instance.  They are built in
	// on_init_thread and torn down in on_exit, in reverse order.
	struct MTLGSRender::frame_resources
	{
		frame_resources()
			: programs([this](const pipeline_properties& properties, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
			{
				// A pipeline the draw path built joins the on-disk set, so the next
				// boot starts warm (HLD-07 §4.1).  The preload does not notify, so a
				// load never rewrites what it read.
				if (shaders)
				{
					shaders->store(properties, vp, fp);
				}
			})
		{
		}

		resource_manager resources;
		command_recorder recorder;
		surface_cache cache;
		framebuffer attachments;
		fill_pass fill;
		presenter present;
		frame_output output;

		// The draw path (HLD-05 §3.5).
		constants_heap constants;
		depth_stencil_cache depth_cache;
		sampler_cache samplers;
		texture_cache textures;
		program_cache programs;

		// The asynchronous shader registry.  It is declared after the program
		// cache, so the cache is still alive when the registry destroys its jobs,
		// and before the disk set, which is only touched by the drain callback.
		pipeline_jobs jobs;

		// The fallback interpreter's shader and pipeline cache.  It lives beside
		// the real registry because its first-use link is serialized through the
		// same compiler gate (LLD-shader-interpreter.md §10).
		interpreter_cache interpreter;

		// The on-disk pipeline set of this title (HLD-07 §4.1).  It indexes
		// `programs`, so it shares this scope and dies before it.
		std::unique_ptr<shader_cache> shaders;
		rsx::vertex_input_layout vertex_layout;
		argument_binder binder;

		// The transform-constant file changes rarely, so it is uploaded only when
		// the shared state flag says so (Core/RSXDriverState.h:15) and the block
		// index is remembered between draws.
		u32 transform_constants_index = 0;
		bool transform_constants_valid = false;

		// The indexed-constant tables of the current instanced draw, by address.
		MTL::GPUAddress instancing_lookup = 0;
		MTL::GPUAddress instancing_constants = 0;

		// The compiled fragment program of the open clause.  emit_geometry needs its
		// literal relocation table.
		const fragment_program* compiled_fragment = nullptr;

		// The compiled vertex program of the open clause.  emit_geometry needs its
		// constant relocation table (GLGSRender.cpp:1159-1171).
		const vertex_program* compiled_vertex = nullptr;

		// The interpreter state of the open clause.  The mapping and output count
		// come from the linked interpreter pipeline; the instruction addresses are
		// frame-ring blocks uploaded once per draw, before the subdraw loop.
		bool interpreter_active = false;
		interpreter_texture_mapping interpreter_mapping;
		u32 interpreter_output_count = 1;
		// The compact bind range of the open fragment clause: the highest
		// referenced unit plus one.  A single-type shader indexes its array by RSX
		// unit, so this bounds the units it can read (LLD-shader-interpreter §6).
		u32 interpreter_texture_count = 0;
		MTL::GPUAddress interpreter_vertex_instructions = 0;
		MTL::GPUAddress interpreter_fragment_instructions = 0;
		u64 interpreter_vertex_key = 0;
		u64 interpreter_fragment_key = 0;
		MTL::GPUAddress interpreter_vertex_address = 0;
		MTL::GPUAddress interpreter_fragment_address = 0;
		argument_binder interpreter_binder;

		// The instruction blocks already uploaded in this frame, by program key.
		// The ring is per slot and reset at begin_frame, so the map is per frame.
		std::unordered_map<u64, MTL::GPUAddress> interpreter_vertex_blocks;
		std::unordered_map<u64, MTL::GPUAddress> interpreter_fragment_blocks;

		// The resolved texture units of the open clause.  The unit resolution runs
		// once per clause in end() and the subdraws only re-bind it, because a
		// subdraw never changes the texture registers (HLD-05 §3.2).
		std::array<texture_bind, fragment_texture_count> fragment_textures{};
		std::array<texture_bind, vertex_texture_count> vertex_textures{};

		// The depth-as-texture reads of the open clause.  A redirected depth unit
		// reads the stencil view of its surface at [30..45]; the emulated depth
		// compare reads the depth mirror at [50] (MTLArgumentBinds.h).
		std::array<texture_bind, fragment_texture_count> fragment_stencil{};
		texture_bind depth_input{};

		// The hazard record of the frame and its pass boundaries (HLD-03 §2.4).
		pass_scheduler scheduler;

		// The occlusion-query counters.  One shared visibility buffer serves every
		// pass of every slot (MTLZCULL.h).
		zcull zcull_control;

		// The open pass.  The header is metal-cpp-free, so the encoder lives here.
		MTL4::RenderCommandEncoder* encoder = nullptr;

		// TODO(excise): level 5; the state the draw path needs for the per-draw
		// record but that only end() computes.  The pipeline is set once per
		// clause, so emit_geometry cannot ask the program cache for it again
		// without a second lookup.
		MTL::RenderPipelineState* current_pipeline_state = nullptr;
		u32 current_pipeline_key = 0;
		u32 current_vp_id = 0;
		u32 current_fp_id = 0;
		f32 current_viewport[4] = {};
		u32 current_blend_hash = 0;
		u32 current_depth_hash = 0;
		u32 current_raster_hash = 0;
	};

	// The deferred flush queue.  A guest write into a range the texture cache owns
	// faults on the protected page, and the guest bytes are canonical, so the dirty
	// sections must reach guest memory before the faulting instruction is retried.
	// The fault runs on the guest thread and the flush needs an encoder, so the
	// faulting thread posts the request, raises the backend interrupt and waits
	// (GLGSRender.cpp:1298-1307, :1385-1396).
	//
	// The wait is not an optimisation: the page keeps its protection until the
	// flush completes, so a request that is never drained leaves the guest
	// faulting on the same instruction forever.
	struct MTLGSRender::flush_queue
	{
		struct request
		{
			texture_cache::thrashed_set sections;
			bool invalidate_samplers = false;

			atomic_t<bool> processed = false;
			volatile bool received = false;

			void producer_wait()
			{
				utils::spin_wait(processed, [](bool v) { return v; });
				received = true;
			}
		};

		std::list<request> pending;
		std::mutex guard;
	};

	MTLGSRender::MTLGSRender(rsx::context* ctx, rsx::display_output* display) noexcept
		: render_backend(ctx, display)
	{
		// The guest shaders are compiled from GLSL, so the glslang context belongs
		// to the renderer instance and comes up before the first one is compiled
		// (VKGSRender.cpp:704).  Without it every shader parses against zero limits.
		spirv::initialize_compiler_context();

		// bind_context returns early for the initial context, so the backend binds
		// it explicitly (VKGSRender.cpp:438, GLGSRender.cpp:39).
		on_bind_context(m_ctx);
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender()
	{
		spirv::finalize_compiler_context();
	}

	void MTLGSRender::on_init()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "init");
		m_device = device::create();
		if (!m_device)
		{
			die("no Metal 4 device is available");
		}

		// The 16-bool backend contract (RSXThread.h:80; 10 §2.2).
		m_device->fill_backend_config(m_config);

		// Which side writes the guest label decides whether an acquire can park
		// the FIFO reader on a GPU wait (DEVIATIONS-SYNC SY13, L138).
		rsx_log.notice("Metal 4: guest labels are written by the %s",
			m_config.supports_host_gpu_labels ? "GPU (host label synchronization)" : "CPU (host store)");

		// The resource layer and the recorder come up with the device, because the
		// guest window joins the persistent residency set and the ring is sized once
		// (HLD-02 I5, HLD-04 §3.2).
		m_frame_resources = std::make_unique<frame_resources>();
		m_flush_queue = std::make_unique<flush_queue>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->textures.set_bc_texture_support(m_frame_resources->resources.supports_bc_texture_compression());
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);
		m_frame_resources->fill.init(*m_device, m_frame_resources->resources);
		m_frame_resources->constants.init(m_frame_resources->resources, frame_slot_count);
		m_frame_resources->depth_cache.init(m_device->dev());
		m_frame_resources->samplers.init(m_device->dev());

		// The occlusion counters, and every pass carries their buffer
		// (MTLFramebuffer.cpp).  occlusion_queries() hands them to the thread.
		m_frame_resources->zcull_control.init(*m_device, m_frame_resources->resources, m_frame_resources->recorder);
		m_frame_resources->attachments.set_visibility_buffer(m_frame_resources->zcull_control.visibility_buffer(0));

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = m_frame)
		{
			m_frame_resources->present.init(*m_device, frame->handle());
		}

		// The output tier compiles its passes once, with the device (HLD-06 §2.6).
		if (!m_frame_resources->output.init(*m_device, m_frame_resources->resources))
		{
			die("Metal: output pass initialization failed");
		}

		// A warm start: the pipeline set of a previous run is rebuilt here, off the
		// draw path, so a draw never waits for a link (HLD-07 §4.1).  The load reports
		// its progress through the same overlay dialog as the other backends
		// (VKGSRender.cpp:1379-1391); a run with no overlay manager hides the frame
		// instead, because the load draws nothing of its own.
		m_frame_resources->shaders = std::make_unique<shader_cache>(m_frame_resources->programs, "metal", "v1");
		const shader_context context{ m_device->compiler(), m_device->pipeline_task_options(),
			&m_frame_resources->jobs, msl_entry_point, msl_entry_point, false, &m_device->lost_flag() };
		m_frame_resources->programs.set_preload_jobs(&m_frame_resources->jobs);
		m_frame_resources->interpreter.init(m_device->compiler(), m_device->pipeline_task_options(),
			&m_frame_resources->jobs, &m_device->lost_flag());

		if (!RSX(m_ctx)->overlay_manager())
		{
			GSFrameBase* frame = m_frame;

			if (frame)
			{
				frame->hide();
			}

			m_frame_resources->shaders->load(nullptr, context);

			if (frame)
			{
				frame->show();
			}
		}
		else
		{
			rsx::shader_loading_dialog_native dlg(RSX(m_ctx));
			m_frame_resources->shaders->load(&dlg, context);
		}

		create_frame_context();

		// The caches were built without a context; bind this one now.  bind_context
		// skips the initial context, so the explicit call is the only one.
		on_bind_context(m_ctx);

#if MTL_DEBUG_LOGS
		// TODO(excise): level 1; start the present-gap watchdog once the backend
		// is fully constructed, so it never observes a half-initialised frame.
		if (probe::at_least(1))
		{
			diagnostic_watchdog_start();
		}
#endif
	}

	void MTLGSRender::on_shutdown()
	{
		// The tables are printed one last time, because a bounded run ends in a
		// signal and its periodic report is the only other chance to see them.
		// TODO(excise): level 1; the final aggregate report.
		MTL_PERF_REPORT(1);
#if MTL_DEBUG_LOGS
		// TODO(excise): level 1; the watchdog stops before the frame resources it
		// samples are destroyed.
		diagnostic_watchdog_finish();
		// TODO(excise): level 3; the final draw census.
		diagnostic_census_report();
		// TODO(excise): level 5; the RCA pack's final table (uploads, samples,
		// never-uploaded list, BC zero-block accounting, hazards).
		probe::census_print("renderer teardown");
#endif
		// TODO(excise): level 2; the final coverage table.
		MTL_PROBE_DUMP(2);

		if (m_frame_resources)
		{
			// The compiler is the first owner that can outlive the renderer.  A
			// timeout here leaves the frame resources and the device alive; the
			// completion handler may still hold a program address.
			if (!m_frame_resources->jobs.shutdown())
			{
				// TODO(excise): level 1; the shader registry did not drain.
				MTL_PROBE_LATCH(1, "session/archive skipped after abort");
				rsx_log.error("Metal: shader jobs did not drain; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				destroy_frame_context();
				return;
			}

			if (m_device && m_device->lost_flag().load(std::memory_order_acquire))
			{
				// A removed device has no valid queue or compiler.  The archive flush
				// and the ring shutdown both touch it, so leave both objects alive.
				// TODO(excise): level 1; the device disappeared during the session.
				MTL_PROBE_LATCH(1, "device/removed at teardown");
				rsx_log.error("Metal: device removed; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				destroy_frame_context();
				return;
			}

			// The ring drains first: a frame that is still in flight refers to the
			// drawable's residency set, so dropping the layer's set before the drain
			// can fault the GPU and lose the very commit feedback the drain waits for.
			m_frame_resources->recorder.shutdown();
			// The GPU has drained at this point, so the interpreter's pipeline
			// states and libraries can go with the frame resources.
			m_frame_resources->interpreter.shutdown();
			m_frame_resources->present.shutdown();
			m_frame_resources->output.shutdown();
			m_frame_resources->zcull_control.shutdown();
			m_frame_resources->textures.destroy();
			m_frame_resources->samplers.shutdown();
			m_frame_resources->depth_cache.shutdown();
			m_frame_resources->constants.shutdown();
			m_frame_resources->fill.shutdown();
			m_frame_resources->resources.shutdown();
			m_frame_resources.reset();
		}

		if (m_device)
		{
			// The compiled pipelines are written last, when no new one can join
			// (HLD-07 §4.2).
			m_device->flush_pipeline_archive();
		}

		m_device.reset();
		destroy_frame_context();
	}

	void MTLGSRender::open_frame()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/open");
		if (m_frame_open || !m_frame_resources)
		{
			return;
		}

		// The ring's slot is only reusable after its previous frame has completed,
		// so begin_frame waits on the commit feedback before it resets the
		// allocator (HLD-04 §3.2).
		// A slot whose previous commit never reported is abandoned, and its command
		// buffer may still write.  The query gate must not treat that work as done
		// (Q3), so it receives the same result the recorder computed.
		const bool reported = m_frame_resources->recorder.begin_frame();
		const u32 slot = m_frame_resources->recorder.slot();
		m_frame_resources->output.on_slot_complete(slot);
		m_frame_resources->zcull_control.on_slot_complete(slot, reported);
		m_frame_resources->constants.begin_frame(m_frame_resources->recorder.slot());
		m_frame_resources->scheduler.begin_frame();
		m_frame_resources->binder.reset();
		m_frame_resources->interpreter_binder.reset();
		m_frame_resources->interpreter.begin_frame();
		m_frame_resources->interpreter_active = false;
		m_frame_resources->interpreter_vertex_key = 0;
		m_frame_resources->interpreter_fragment_key = 0;
		m_frame_resources->interpreter_vertex_address = 0;
		m_frame_resources->interpreter_fragment_address = 0;
		m_frame_resources->interpreter_vertex_blocks.clear();
		m_frame_resources->interpreter_fragment_blocks.clear();
		m_frame_resources->transform_constants_valid = false;
		m_frame_open = true;

#if MTL_DEBUG_LOGS
		diagnostic_frame_opened();

		// TODO(excise): level 5; the serialisation switch.  The backend overlaps
		// frames, so a defect can come from a later frame writing what an earlier
		// one still reads.  Waiting here means no other frame is in flight while
		// this one encodes: a defect that survives is in the data, not the overlap.
		if (probe::variant("serialise") && !m_frame_resources->recorder.wait_idle())
		{
			rsx_log.error("Metal 4: the serialising probe could not drain the GPU after opening a frame");
		}
#endif
	}

	void MTLGSRender::begin_draw()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/begin");
		if (!m_frame_resources)
		{
			return;
		}

		// Publish a link that completed since the previous clause.  This is the only
		// drain point inside the draw loop, and it does not wait.
		m_frame_resources->jobs.advance();
		open_frame();
	}

	bool MTLGSRender::on_framebuffer_bound(const rsx::framebuffer_layout& layout)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/bind");
		if (!m_frame_resources)
		{
			return false;
		}

		open_frame();

		// The core calls this hook even when the layout did not change, so the bound
		// set is still current and must not be rebuilt (RSXGraphEngine.cpp:827-839).
		if (layout.ignore_change && m_frame_resources->attachments.valid())
		{
			return true;
		}

		m_frame_resources->attachments.bind(m_frame_resources->recorder, m_frame_resources->cache,
			m_ctx->fb.layout, RSX(m_ctx)->resolution_scaling_config);
		reconcile_render_targets();

		// TODO(excise): level 2; the framebuffer bind stream and its formats.  A
		// bind runs near once per draw, so it is counted and classified here.
		MTL_PROBE_LATCH(2, "framebuffer/bind");
		MTL_PROBE_HISTOGRAM(2, "framebuffer color format", static_cast<u32>(m_ctx->fb.layout.color_format), "gcm color format");
		MTL_PROBE_HISTOGRAM(2, "framebuffer depth format", static_cast<u32>(m_ctx->fb.layout.depth_format), "gcm depth format");
		MTL_PROBE_HISTOGRAM(2, "framebuffer aa mode", static_cast<u32>(m_ctx->fb.layout.aa_mode), "aa mode");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first binds of the run only.
		static u32 diagnostic_binds = 0;
		const bool diagnostic_bind_trace = diagnostic_binds++ < 8;

		if (diagnostic_bind_trace)
		{
			MTL_NOTE(3, "Metal 4: bind context %u, color 0x%x/0x%x/0x%x/0x%x fmt 0x%x pitch %u, depth 0x%x fmt 0x%x pitch %u, %ux%u, aa_mode %u",
				static_cast<u32>(m_ctx->fb.current_context),
				m_ctx->fb.layout.color_addresses[0], m_ctx->fb.layout.color_addresses[1],
				m_ctx->fb.layout.color_addresses[2], m_ctx->fb.layout.color_addresses[3],
				static_cast<u32>(m_ctx->fb.layout.color_format), m_ctx->fb.layout.actual_color_pitch[0],
				m_ctx->fb.layout.zeta_address, static_cast<u32>(m_ctx->fb.layout.depth_format),
				m_ctx->fb.layout.actual_zeta_pitch,
				static_cast<u32>(m_ctx->fb.layout.width), static_cast<u32>(m_ctx->fb.layout.height),
				static_cast<u32>(m_ctx->fb.layout.aa_mode));
		}
#endif // MTL_DEBUG_LOGS

		return m_frame_resources->attachments.valid();
	}

	bool MTLGSRender::is_framebuffer_bound() const
	{
		return m_frame_resources && m_frame_resources->attachments.valid();
	}

	void MTLGSRender::reconcile_render_targets()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/reconcile");
		frame_resources& frame = *m_frame_resources;
		surface_cache& store = frame.cache;
		texture_cache& textures = frame.textures;
		command_recorder& cmd = frame.recorder;

		const u8 color_bpp = rsx::get_format_block_size_in_bytes(m_ctx->fb.layout.color_format);
		const u8 samples = rsx::get_format_sample_count(m_ctx->fb.layout.aa_mode);

		// A range that stops being a target is written back or dropped before the
		// record of it is replaced, because the cache no longer knows the address
		// afterwards (VKGSRender.cpp:2659-2692).
		for (u32 index = 0; index < rsx::limits::color_buffers_count; ++index)
		{
			if (m_ctx->fb.surface_info[index].pitch && g_cfg.video.write_color_buffers)
			{
				const auto stale_range = m_ctx->fb.surface_info[index].get_memory_range();
				textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
				textures.flush_if_cache_miss_likely(cmd, stale_range);
			}

			m_ctx->fb.surface_info[index].address = 0;
			m_ctx->fb.surface_info[index].pitch = 0;
			m_ctx->fb.surface_info[index].width = m_ctx->fb.layout.width;
			m_ctx->fb.surface_info[index].height = m_ctx->fb.layout.height;
			m_ctx->fb.surface_info[index].color_format = m_ctx->fb.layout.color_format;
			m_ctx->fb.surface_info[index].bpp = color_bpp;
			m_ctx->fb.surface_info[index].samples = samples;
		}

		if (m_ctx->fb.depth_surface_info.pitch && g_cfg.video.write_depth_buffer)
		{
			const auto stale_range = m_ctx->fb.depth_surface_info.get_memory_range();
			textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
			textures.flush_if_cache_miss_likely(cmd, stale_range);
		}

		m_ctx->fb.depth_surface_info.address = 0;
		m_ctx->fb.depth_surface_info.pitch = 0;
		m_ctx->fb.depth_surface_info.width = m_ctx->fb.layout.width;
		m_ctx->fb.depth_surface_info.height = m_ctx->fb.layout.height;
		m_ctx->fb.depth_surface_info.depth_format = m_ctx->fb.layout.depth_format;
		m_ctx->fb.depth_surface_info.bpp = rsx::get_format_block_size_in_bytes(m_ctx->fb.layout.depth_format);
		m_ctx->fb.depth_surface_info.samples = samples;

		// The surfaces the bind superseded or orphaned are released first, so a
		// release cannot drop a lock the bound-target pass below takes
		// (VKGSRender.cpp:2732-2776 then :2778-2810).
		for (surface* replaced : store.superseded_surfaces)
		{
			textures.discard_framebuffer_memory_region(cmd, replaced->get_memory_range());
		}

		store.superseded_surfaces.clear();

		for (auto& orphan : store.orphaned_surfaces)
		{
			surface* target = orphan.second;
			const auto range = target->get_memory_range();
			bool write_back = target->is_depth_surface()
				? !!g_cfg.video.write_depth_buffer
				: !!g_cfg.video.write_color_buffers;

			// An orphan that is already locked keeps its lock; locking it again
			// would protect a range its current owner does not own
			// (VKGSRender.cpp:2749-2761).
			if (write_back && !target->is_locked())
			{
				write_back = false;
			}

			if (write_back)
			{
				textures.lock_memory_region(cmd, target, range, false,
					target->get_surface_width<rsx::surface_metrics::pixels>(),
					target->get_surface_height<rsx::surface_metrics::pixels>(),
					target->get_rsx_pitch(), target);
			}
			else
			{
				textures.commit_framebuffer_memory_region(cmd, range);
			}
		}

		store.orphaned_surfaces.clear();

		for (u8 index : rsx::utility::get_rtt_indexes(m_ctx->fb.layout.target))
		{
			surface* target = store.m_bound_render_targets[index].second;
			if (!target)
			{
				continue;
			}

			m_ctx->fb.surface_info[index].address = m_ctx->fb.layout.color_addresses[index];
			m_ctx->fb.surface_info[index].pitch = m_ctx->fb.layout.actual_color_pitch[index];

			// A target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2780).
			if (!m_ctx->fb.surface_info[index].address || !m_ctx->fb.surface_info[index].pitch)
			{
				continue;
			}

			ensure(target->get_rsx_pitch() == m_ctx->fb.layout.actual_color_pitch[index]);

			const auto range = m_ctx->fb.surface_info[index].get_memory_range(m_ctx->fb.layout.aa_factors);

			// A cached view of this range, and every scaled or cropped view derived
			// from it, is stale the moment the range becomes an attachment.
			textures.notify_surface_changed(range);

			if (g_cfg.video.write_color_buffers)
			{
				// The section becomes the guest-side backing of the surface, so the
				// write-back at flip has somewhere to land (texture_cache.h:1410).
				textures.lock_memory_region(cmd, target, range, true,
					m_ctx->fb.surface_info[index].width, m_ctx->fb.surface_info[index].height,
					m_ctx->fb.surface_info[index].pitch, target);
			}
			else
			{
				// The guest copy is never updated, so drop the cached section: the
				// sample must resolve to the surface instead (texture_cache.h:2046).
				textures.commit_framebuffer_memory_region(cmd, range);
			}
		}

		// TODO(excise): level 2; the render-target store stream and its turnover.  A
		// store runs near once per draw, so it is counted and classified here.
		MTL_PROBE_LATCH(2, "framebuffer/store");
		MTL_PROBE_HISTOGRAM(2, "store orphans", static_cast<u32>(store.orphaned_surfaces.size()), "orphaned surfaces");
		MTL_PROBE_HISTOGRAM(2, "store superseded", static_cast<u32>(store.superseded_surfaces.size()), "superseded surfaces");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first stores of the run only.
		static u32 diagnostic_stores = 0;

		if (diagnostic_stores++ < 8)
		{
			char held[256] = {};
			u32 held_len = 0;

			for (u32 i = 0; i < 4; ++i)
			{
				const auto& bound = store.m_bound_render_targets[i];

				if (!bound.second)
				{
					continue;
				}

				const u32 remaining = (held_len < sizeof(held)) ? static_cast<u32>(sizeof(held) - held_len) : 0u;

				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(held + held_len, remaining, " c%u=0x%x/%ux%u/p%u", i,
					bound.first, static_cast<u32>(bound.second->width()), static_cast<u32>(bound.second->height()),
					bound.second->get_rsx_pitch());

				if (written <= 0)
				{
					break;
				}

				held_len += static_cast<u32>(written);

				if (held_len >= sizeof(held) - 48)
				{
					break;
				}
			}

			MTL_NOTE(3, "Metal 4: store holds%s, depth 0x%x, orphans %u, superseded %u",
				held_len ? held : " nothing", store.m_bound_depth_stencil.first,
				static_cast<u32>(store.orphaned_surfaces.size()), static_cast<u32>(store.superseded_surfaces.size()));
		}
#endif // MTL_DEBUG_LOGS

		if (surface* depth = store.m_bound_depth_stencil.second)
		{
			m_ctx->fb.depth_surface_info.address = m_ctx->fb.layout.zeta_address;
			m_ctx->fb.depth_surface_info.pitch = m_ctx->fb.layout.actual_zeta_pitch;

			// A depth target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2796).
			if (m_ctx->fb.depth_surface_info.address && m_ctx->fb.depth_surface_info.pitch)
			{
				ensure(depth->get_rsx_pitch() == m_ctx->fb.layout.actual_zeta_pitch);

				const auto range = m_ctx->fb.depth_surface_info.get_memory_range(m_ctx->fb.layout.aa_factors);
				textures.notify_surface_changed(range);

				if (g_cfg.video.write_depth_buffer)
				{
					textures.lock_memory_region(cmd, depth, range, true,
						m_ctx->fb.depth_surface_info.width, m_ctx->fb.depth_surface_info.height,
						m_ctx->fb.depth_surface_info.pitch, depth);
				}
				else
				{
					textures.commit_framebuffer_memory_region(cmd, range);
				}
			}
		}
	}

	void MTLGSRender::clear()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "clear");
		if (!m_frame_resources)
		{
			return;
		}

		// The core has already checked the frame skip, dropped the stencil bit for a
		// zero write mask, decoded draw_state.clear and bound the framebuffer.
		const rsx::clear_state& clear = m_ctx->draw_state.clear;
		const rsx::flags32_t mask = clear.mask;
		if (!mask)
		{
			return;
		}

		surface_state state = capture_surface_state(*m_ctx);

		// The colour fields are only decoded when a colour clear is possible; for a
		// float target the core deliberately zeroes the mask and leaves them stale.
		const bool write_color = clear.color_mask != 0;
		if (write_color)
		{
			state.clear_color = MTL::ClearColor::Make(
				clear.color_r / 255.f, clear.color_g / 255.f,
				clear.color_b / 255.f, clear.color_a / 255.f);
			state.clear_colormask = clear.color_mask;
			state.clear_color_fast = clear.color_mask_full;
		}

		const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
		const bool write_stencil = (mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0;
		if (write_depth)
		{
			// Format-decoded by the core; no host encoding is applied.
			state.clear_depth = clear.depth_value;
		}
		if (write_stencil)
		{
			state.clear_stencil = clear.stencil_value;
		}

		// A stencil write mask below 0xff makes the stencil clear a partial write,
		// which MTLLoadActionClear cannot express.  The fill then writes the
		// reference through the mask (S5; VKGSRender.cpp:1624-1638).
		const bool partial_stencil = write_stencil && clear.stencil_write_mask != 0xff;
		state.clear_stencil_fast = !partial_stencil;

		// A clear writes the attachments, so the store marks them newest for the same
		// reason a draw does: a later sample of the range must resolve to the surface
		// and not to the value the clear replaced (VKGSRender.cpp:1648-1651).
		m_frame_resources->cache.on_write({ write_color, write_color, write_color, write_color }, write_depth || write_stencil);

		framebuffer& attachments = m_frame_resources->attachments;

		if (clear.scissor_covers_surface && (!write_color || clear.color_mask_full) && !partial_stencil)
		{
			// TODO(excise): level 2; which clear path ran.  The whole-attachment
			// clear is a load action; the other path is a scissored fill.
			MTL_PROBE_LATCH(2, "clear/whole load action");

			// The clear is the whole attachment, so it is a load action and needs no
			// draw (LLD-surface invariant 4).  The pass is opened so the clear is
			// recorded, then closed.
			m_frame_resources->encoder = attachments.open_pass(m_frame_resources->recorder, mask, state);
			record_pass_hazards(false);
			m_frame_resources->encoder = nullptr;
			m_frame_resources->recorder.close_encoder();
			return;
		}

		// TODO(excise): level 2; a scissored fill, and whether it asked for a full
		// colour clear.  That combination used to open the pass with a load action
		// and wipe everything outside the scissor, so the count names the defect.
		MTL_PROBE_LATCH(2, "clear/scissored fill");
		if (clear.color_mask_full)
		{
			MTL_PROBE_LATCH(2, "clear/scissored fill full color");
		}

		// Metal has no scissored clear, so a partial clear is a full-screen fill
		// confined by the scissor (CONFLICTS.md C1).  The core's resolve_scissor
		// already clamped the region and cleared rtt_config_valid when it is invalid.
		const areau region = scaled_scissor(m_ctx->draw_state.scissor, m_ctx->fb.layout,
			RSX(m_ctx)->resolution_scaling_config);

		MTL4::RenderCommandEncoder* encoder = attachments.open_pass(m_frame_resources->recorder, 0, state);
		if (!encoder)
		{
			return;
		}

		m_frame_resources->encoder = encoder;
		record_pass_hazards(false);

		u16 x = static_cast<u16>(region.x1);
		u16 y = static_cast<u16>(region.y1);
		u16 width = static_cast<u16>(region.width());
		u16 height = static_cast<u16>(region.height());

		std::tie(x, y, width, height) = rsx::clip_region<u16>(attachments.width(), attachments.height(), x, y, width, height, true);

		const MTL::ScissorRect scissor = build_scissor_rect(x, y, static_cast<u32>(x) + width,
			static_cast<u32>(y) + height, static_cast<u16>(attachments.height()));
		encoder->setScissorRect(scissor);

		const std::array<f32, 4> color = {
			static_cast<f32>(state.clear_color.red),
			static_cast<f32>(state.clear_color.green),
			static_cast<f32>(state.clear_color.blue),
			static_cast<f32>(state.clear_color.alpha) };
		const MTL::PixelFormat color_format = attachments.color_count() != 0 ? attachments.color(0)->pixel_format() : MTL::PixelFormatInvalid;

		// Every fill takes its own value block, because a Metal 4 command buffer does
		// not snapshot buffer contents (MTLConstants.h fill_values).
		if (const MTL::GPUAddress values = m_frame_resources->constants.alloc_fill_values(color, state.clear_depth))
		{
			const fill_stencil stencil = write_stencil
				? fill_stencil{ clear.stencil_value, clear.stencil_write_mask }
				: fill_stencil{};
			m_frame_resources->fill.fill(m_frame_resources->recorder.fragment_table(), encoder, values, color_format,
				attachments.sample_count(), to_color_write_mask(clear.color_mask), write_depth, stencil);
		}

		m_frame_resources->encoder = nullptr;
		m_frame_resources->recorder.close_encoder();
	}

#if MTL_DEBUG_LOGS
	// TODO(excise): level 5; the sampled images of the present readback.
	MTL::Texture* diagnostic_sampled_image[8] = {};
	u32 diagnostic_sampled_address[8] = {};
	u32 diagnostic_sampled_context[8] = {};
	u32 diagnostic_sampled_count = 0;

	// TODO(excise): level 5; the bytes per pixel of one host format.
	u32 diagnostic_pixel_size(MTL::PixelFormat format)
	{
		switch (format)
		{
		case MTL::PixelFormat::PixelFormatRGBA8Unorm:
		case MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB:
		case MTL::PixelFormat::PixelFormatBGRA8Unorm:
		case MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB:
		case MTL::PixelFormat::PixelFormatRGB10A2Unorm:
		case MTL::PixelFormat::PixelFormatBGR10A2Unorm:
		case MTL::PixelFormat::PixelFormatRGBA32Uint:
		case MTL::PixelFormat::PixelFormatRGBA32Float:
			return 4;
		case MTL::PixelFormat::PixelFormatB5G6R5Unorm:
		case MTL::PixelFormat::PixelFormatA1BGR5Unorm:
		case MTL::PixelFormat::PixelFormatBGR5A1Unorm:
		case MTL::PixelFormat::PixelFormatR16Unorm:
		case MTL::PixelFormat::PixelFormatR16Float:
		case MTL::PixelFormat::PixelFormatRG8Unorm:
		case MTL::PixelFormat::PixelFormatRG16Unorm:
		case MTL::PixelFormat::PixelFormatRG16Float:
			return 2;
		case MTL::PixelFormat::PixelFormatR8Unorm:
			return 1;
		default:
			return 0;
		}
	}

	// TODO(excise): level 5; the block size of one host compressed format.  A
	// compressed readback is sized in blocks, not in pixels, so it cannot use the
	// scalar path above.
	u32 diagnostic_block_size(MTL::PixelFormat format, u32& edge)
	{
		switch (format)
		{
		case MTL::PixelFormat::PixelFormatBC1_RGBA:
		case MTL::PixelFormat::PixelFormatBC1_RGBA_sRGB:
			edge = 4;
			return 8;
		case MTL::PixelFormat::PixelFormatBC2_RGBA:
		case MTL::PixelFormat::PixelFormatBC2_RGBA_sRGB:
		case MTL::PixelFormat::PixelFormatBC3_RGBA:
		case MTL::PixelFormat::PixelFormatBC3_RGBA_sRGB:
			edge = 4;
			return 16;
		default:
			edge = 0;
			return 0;
		}
	}

	void dump_host_image(command_recorder& recorder, resource_manager& resources, MTL::Texture* image,
		const char* label, u32 serial, u32 address)
	{
		if (!image || !probe::at_least(5))
		{
			return;
		}

		const u32 width = static_cast<u32>(image->width());
		const u32 height = static_cast<u32>(image->height());
		const MTL::PixelFormat format = image->pixelFormat();
		const u32 pixel_size = diagnostic_pixel_size(format);
		u32 block_edge = 0;
		const u32 block_size = pixel_size ? 0u : diagnostic_block_size(format, block_edge);

		if (!width || !height || (!pixel_size && !block_size) || width > 4096 || height > 4096)
		{
			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: host image %s %u skipped, %ux%u format 0x%x",
				label, serial, width, height, static_cast<u32>(format));
			return;
		}

		// A blit needs an aligned row, so the rows are padded and the hash runs over
		// the row bytes only.  Padding is not initialised, and hashing it would make
		// an unchanged image look changed.  A compressed image is sized in blocks,
		// but the copy size stays in pixels because that is Metal's convention.
		const u32 row_units = pixel_size ? width : (width + block_edge - 1) / block_edge;
		const u32 row_bytes = row_units * (pixel_size ? pixel_size : block_size);
		const u32 row_count = pixel_size ? height : (height + block_edge - 1) / block_edge;
		const u32 row_pitch = utils::align(row_bytes, 0x100);
		const u64 length = static_cast<u64>(row_pitch) * row_count;

		// The user presses F12 several times in one run (menu, then two or three
		// times in the race), so the deep dumps are bounded by bytes, not only by
		// file count.  A little over a gigabyte of image files plus the tape's own
		// half-gigabyte keeps a four-window run well inside the free space on the
		// machine while still writing the frames that matter.
		constexpr u64 diagnostic_image_byte_budget = 1024ull * 1024ull * 1024ull;
		static u64 diagnostic_image_bytes = 0;
		static bool diagnostic_image_byte_budget_reported = false;

		if (diagnostic_image_bytes + length > diagnostic_image_byte_budget)
		{
			if (!diagnostic_image_byte_budget_reported)
			{
				diagnostic_image_byte_budget_reported = true;
				MTL_NOTE(2, "Metal 4: capture image byte budget %llu exhausted; later images are counted, not dumped",
					static_cast<unsigned long long>(diagnostic_image_byte_budget));
			}

			return;
		}

		MTL::Buffer* buffer = resources.new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

		if (!buffer)
		{
			return;
		}

		resources.make_persistent(buffer);

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			resources.defer_release(buffer);
			return;
		}

		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
		encoder->copyFromTexture(image, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width, height, 1),
			buffer, 0, row_pitch, row_pitch * row_count);
		recorder.close_encoder();

		// The copy must be observable, so the frame commits early and waits.
		recorder.flush_for_readback();

		const u8* bytes = static_cast<const u8*>(buffer->contents());
		u64 hash = 1469598103934665603ull;
		u32 nonzero = 0;
		u32 words = 0;
		u32 distinct = 0;
		u32 first[4] = {};

		if (bytes)
		{
			words = row_bytes / 4u;

			for (u32 y = 0; y < row_count; ++y)
			{
				const u8* row = bytes + static_cast<u64>(y) * row_pitch;
				const u32* data = reinterpret_cast<const u32*>(row);

				for (u32 i = 0; i < words; ++i)
				{
					nonzero += data[i] ? 1u : 0u;
					hash = (hash ^ data[i]) * 1099511628211ull;
				}

				if (!y)
				{
					for (u32 i = 0; i < 4 && i < words; ++i)
					{
						first[i] = data[i];
					}
				}
			}

			// The distinct byte count separates a flat image from one with content.
			// A flat image is what a stale, zeroed, or never-refreshed copy looks
			// like, and that is the question this readback exists to answer.
			u8 seen_bytes[256] = {};

			for (u32 y = 0; y < row_count; ++y)
			{
				const u8* row = bytes + static_cast<u64>(y) * row_pitch;

				for (u32 i = 0; i < row_bytes; ++i)
				{
					if (!seen_bytes[row[i]])
					{
						seen_bytes[row[i]] = 1;
						distinct++;
					}
				}
			}

			char path[512];
			std::snprintf(path, sizeof(path), "%s/host-%s-%u-addr%x-%ux%u-fmt%x.bin",
				probe::dump_directory(), label, serial, address, width, height, static_cast<u32>(format));

			std::error_code error;
			std::filesystem::create_directories(probe::dump_directory(), error);

			if (std::FILE* out = std::fopen(path, "wb"))
			{
				const usz written = std::fwrite(bytes, 1, length, out);
				std::fclose(out);
				diagnostic_image_bytes += written;

				if (written != length)
				{
					// The dump is evidence; a silent truncation is worse than none.
					// Say so per file so a corrupt capture is never mistaken for a good one.
					MTL_NOTE(2, "Metal 4: dump %s truncated at %llu of %u bytes (disk full?)", path,
						static_cast<unsigned long long>(written), length);
				}
			}
		}

		// TODO(excise): level 5; sampled byte dump.
		MTL_NOTE(5, "Metal 4: host image %s %u, address 0x%x, %ux%u format 0x%x, image %p, block %u/%u, rows %u, row bytes %u, hash 0x%llx, nonzero %u of %u words, distinct bytes %u, row 0 %08x %08x %08x %08x",
			label, serial, address, width, height, static_cast<u32>(format), static_cast<const void*>(image),
			block_edge, block_size, row_count, row_bytes,
			static_cast<unsigned long long>(hash), nonzero, words * row_count, distinct, first[0], first[1], first[2], first[3]);

		resources.defer_release(buffer);
	}

	// A fixed screen patch is read back after every draw of a capture window.  A
	// black panel raises three different questions: the draw wrote nothing, it
	// wrote opaque black, or a later draw replaced it.  The alpha byte separates
	// the first case (the clear leaves alpha zero) from the second (an opaque
	// write leaves 255), and a previous probe of the same patch separates the
	// third.  The patch list comes from MTL_PROBE_PIXELS ("x,y,w,h;x,y,w,h").
	struct diagnostic_pixel_rect
	{
		u32 x, y, w, h;
	};

	// One patch's post-draw readback.  `black` counts pure #000000 texels (the
	// defect signature), `opaque` counts texels whose alpha survived, and the
	// two together separate "a draw wrote black" from "nothing has written the
	// region yet".  The per-draw series is the attribution: the draw whose black
	// count jumps is the one that introduced the defect.
	struct diagnostic_patch_observation
	{
		u32 rect = 0;
		u32 x = 0;
		u32 y = 0;
		u32 width = 0;
		u32 height = 0;
		u32 black = 0;
		u32 opaque = 0;
		u32 pixels = 0;
	};

	// TODO(excise): level 5; the per-rect previous reading, so a sudden black
	// jump is the trigger and a steady dark region is not.  The target address
	// is part of the key: the same patch rect is read from different targets
	// inside one frame, and a pass change is not an increase.
	u32 diagnostic_patch_previous_black[8] = {};
	u32 diagnostic_patch_previous_address[8] = {};

	// TODO(excise): level 5; the automatic evidence dumps are bounded, because
	// a continuously corrupt frame would otherwise dump every draw.  Each hit
	// writes the target and the draw's sampled images once.
	u32 diagnostic_black_patch_dumps = 0;
	constexpr u32 diagnostic_black_patch_dump_limit = 32;
	constexpr u32 diagnostic_black_patch_image_limit = 16;

	diagnostic_pixel_rect diagnostic_pixel_rects[8] = {};
	u32 diagnostic_pixel_rect_count = 0;
	u32 diagnostic_pixel_probes = 0;
	bool diagnostic_pixel_rects_read = false;

	// TODO(excise): level 5; the per-frame black total.  A single frame can move
	// the defect between patches, so the tally (not one patch's count) is what a
	// frame is compared on.  It is logged with the feedback witness below.
	u32 diagnostic_black_count = 0;

	// TODO(excise): level 5; the feedback witness.  A unit that samples a bound
	// render target is the only place Metal can differ from GL and Vulkan: GL
	// makes it legal with glTextureBarrier and Vulkan with the feedback-loop
	// layout, and Metal has neither, so it copies.  The witness records, per
	// frame, how many such units were bound live and how many were copied, plus
	// the last few (unit, address, path) triples.  A run after the fix must show
	// live 0 for the post chain; a live count that is still nonzero names the
	// unit the copy did not reach.
	struct diagnostic_feedback_binding
	{
		MTL::Texture* image = nullptr;
		u32 unit = 0;
		u32 address = 0;
		u32 target = 0;
		u32 draw = 0;
		bool copy = false;
	};

	u32 diagnostic_feedback_live = 0;
	u32 diagnostic_feedback_copy = 0;
	diagnostic_feedback_binding diagnostic_feedback_bindings[8] = {};
	u32 diagnostic_feedback_binding_count = 0;

	// TODO(excise): level 5; the rolling pixel tape (MTL_PROBE_TAPE).  Each frame
	// owns one present stream and up to eight feedback streams.  A stream's
	// buffer is persistent shared memory; a frame copies into it with the GPU and
	// never reads it back, so the tape does not serialise the frame it watches.
	// F12 freezes the ring and the next present reads it once.
	struct diagnostic_tape_slot
	{
		MTL::Buffer* buffer = nullptr;
		u64 capacity = 0;
		u32 width = 0;
		u32 height = 0;
		u32 row_pitch = 0;
		u32 address = 0;
		u32 draw = 0;
		u32 unit = 0;
		u32 stream = 0;
		u64 frame = 0;
		bool present = false;
		bool copy = false;
		bool valid = false;
	};

	constexpr u32 diagnostic_tape_feedback_streams = 8;
	constexpr u32 diagnostic_tape_streams = 1 + diagnostic_tape_feedback_streams;
	constexpr u64 diagnostic_tape_byte_limit = 512ull * 1024ull * 1024ull;
	// The tape's own allocations are bounded too: a post-chain feedback image is a
	// full 1280x720 or 1408x768 target, and eight streams times the ring depth of
	// those is more than the run should take.  A slot above the bound is skipped
	// with one notice; the present stream and the smaller units still arrive.
	constexpr u64 diagnostic_tape_allocation_limit = 448ull * 1024ull * 1024ull;
	// A taped image larger than this is not part of the game's frame; skipping it
	// keeps a pathological guest extent from sizing a persistent buffer.
	constexpr u32 diagnostic_tape_max_dimension = 2048;
	std::vector<diagnostic_tape_slot> diagnostic_tape;
	bool diagnostic_tape_frozen = false;
	u64 diagnostic_tape_bytes = 0;
	u64 diagnostic_tape_allocated = 0;
	u32 diagnostic_tape_dumps = 0;
	bool diagnostic_tape_budget_reported = false;
	bool diagnostic_tape_allocation_reported = false;

	diagnostic_tape_slot* diagnostic_tape_slot_for(u64 frame, u32 stream)
	{
		const u32 depth = probe::tape_depth();

		if (!depth || stream >= diagnostic_tape_streams)
		{
			return nullptr;
		}

		if (diagnostic_tape.empty())
		{
			diagnostic_tape.resize(static_cast<usz>(depth) * diagnostic_tape_streams);
		}

		return &diagnostic_tape[static_cast<usz>(frame % depth) * diagnostic_tape_streams + stream];
	}

	bool diagnostic_tape_reserve(diagnostic_tape_slot& slot, u64 length, resource_manager& resources)
	{
		if (slot.buffer && slot.capacity >= length)
		{
			return true;
		}

		// A slot that grows replaces its buffer rather than failing: the present
		// stream starts at the boot resolution and later the game presents at its
		// own, and a feedback stream's first image can be a small scratch target
		// while the post chain's input is a full 1280x720 frame.  The old buffer is
		// deferred, not freed, so a copy already queued into it stays valid.
		const u64 capacity = utils::align(length, 0x1000);

		if (diagnostic_tape_allocated + capacity > diagnostic_tape_allocation_limit)
		{
			if (!diagnostic_tape_allocation_reported)
			{
				diagnostic_tape_allocation_reported = true;
				MTL_NOTE_WITNESS(5, "Metal 4: pixel tape allocation %llu bytes exhausted; later images are skipped",
					static_cast<unsigned long long>(diagnostic_tape_allocation_limit));
			}

			return false;
		}

		MTL::Buffer* buffer = resources.new_buffer(capacity, MTL::StorageModeShared);

		if (!buffer)
		{
			return false;
		}

		resources.make_persistent(buffer);

		if (slot.buffer)
		{
			diagnostic_tape_allocated -= slot.capacity;
			resources.defer_release(slot.buffer);
		}

		slot.buffer = buffer;
		slot.capacity = capacity;
		diagnostic_tape_allocated += capacity;
		return true;
	}

	void diagnostic_tape_copy(command_recorder& recorder, resource_manager& resources, MTL::Texture* image,
		u32 address, u32 draw, u32 unit, u64 frame, u32 stream, bool present, bool copy)
	{
		if (!probe::tape_depth() || !image)
		{
			return;
		}

		const u32 width = static_cast<u32>(image->width());
		const u32 height = static_cast<u32>(image->height());
		const MTL::PixelFormat format = image->pixelFormat();

		// The tape is a byte-for-byte record of the colour targets, so only the
		// four-byte layouts are taped; a packed or compressed image would need its
		// block geometry and is not part of the post chain.
		if (!width || !height || width > diagnostic_tape_max_dimension || height > diagnostic_tape_max_dimension ||
			image->sampleCount() != 1 ||
			(format != MTL::PixelFormat::PixelFormatBGRA8Unorm && format != MTL::PixelFormat::PixelFormatRGBA8Unorm))
		{
			return;
		}

		diagnostic_tape_slot* slot = diagnostic_tape_slot_for(frame, stream);

		if (!slot)
		{
			return;
		}

		const u32 row_pitch = utils::align(width * 4u, 0x100);
		const u64 length = static_cast<u64>(row_pitch) * height;

		if (!diagnostic_tape_reserve(*slot, length, resources))
		{
			return;
		}

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			return;
		}

		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
		encoder->copyFromTexture(image, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width, height, 1),
			slot->buffer, 0, row_pitch, row_pitch * height);
		recorder.close_encoder();

		slot->width = width;
		slot->height = height;
		slot->row_pitch = row_pitch;
		slot->address = address;
		slot->draw = draw;
		slot->unit = unit;
		slot->stream = stream;
		slot->frame = frame;
		slot->present = present;
		slot->copy = copy;
		slot->valid = true;
	}

	void diagnostic_tape_freeze()
	{
		if (!probe::tape_depth() || diagnostic_tape_frozen)
		{
			return;
		}

		diagnostic_tape_frozen = true;
		MTL_NOTE(2, "Metal 4: pixel tape frozen at frame %llu, depth %u",
			static_cast<unsigned long long>(diagnostic_frame_index), probe::tape_depth());
	}

	void diagnostic_tape_flush(command_recorder& recorder)
	{
		if (!diagnostic_tape_frozen)
		{
			return;
		}

		diagnostic_tape_frozen = false;

		// The copies are still queued, so one wait makes every slot readable.
		recorder.flush_for_readback();

		// The two readings below run on the bytes that were just made CPU-visible.
		// They cost nothing on the GPU and are what makes the tape a verdict rather
		// than a pile of files: the present timeline names the flicker frames, and
		// the sample tally says whether a feedback copy held black before its draw.
		const auto slot_bytes = [](const diagnostic_tape_slot& slot) -> const u8*
		{
			return slot.buffer ? static_cast<const u8*>(slot.buffer->contents()) : nullptr;
		};

		const auto count_pixels = [&](const diagnostic_tape_slot& slot, u32& black, u32& dark)
		{
			black = 0;
			dark = 0;

			const u8* bytes = slot_bytes(slot);

			if (!bytes)
			{
				return;
			}

			for (u32 y = 0; y < slot.height; ++y)
			{
				const u8* row = bytes + static_cast<u64>(y) * slot.row_pitch;

				for (u32 x = 0; x < slot.width; ++x)
				{
					const u8* pixel = row + x * 4u;

					if (!pixel[0] && !pixel[1] && !pixel[2])
					{
						++black;
						++dark;
					}
					else if (std::max({ pixel[0], pixel[1], pixel[2] }) <= 16)
					{
						++dark;
					}
				}
			}
		};

		for (diagnostic_tape_slot& slot : diagnostic_tape)
		{
			if (!slot.valid || !slot.buffer || slot.present)
			{
				continue;
			}

			u32 black = 0;
			u32 dark = 0;
			count_pixels(slot, black, dark);

			MTL_NOTE_WITNESS(5, "Metal 4: tape sample frame %llu draw %u unit %u addr 0x%x %ux%u path %s, black %u dark %u of %u",
				static_cast<unsigned long long>(slot.frame), slot.draw, slot.unit, slot.address,
				slot.width, slot.height, slot.copy ? "copy" : "live",
				black, dark, slot.width * slot.height);
		}

		// The present stream in frame order.  A newly-black pixel is the defect's
		// own signature: it is pure #000000 now and was not pure black in the
		// previous presented frame.  The count, the bounding box and the busiest
		// 16x16 cell locate the flicker without a configured rect, and the line is
		// printed for the frames before the key press, which no per-draw probe ever
		// reaches.
		{
			std::vector<const diagnostic_tape_slot*> presents;

			for (const diagnostic_tape_slot& slot : diagnostic_tape)
			{
				if (slot.valid && slot.buffer && slot.present)
				{
					presents.push_back(&slot);
				}
			}

			std::sort(presents.begin(), presents.end(), [](const diagnostic_tape_slot* a, const diagnostic_tape_slot* b)
			{
				return a->frame < b->frame;
			});

			for (usz index = 1; index < presents.size(); ++index)
			{
				const diagnostic_tape_slot& now = *presents[index];
				const diagnostic_tape_slot& before = *presents[index - 1];

				if (now.width != before.width || now.height != before.height || now.frame <= before.frame)
				{
					continue;
				}

				const u8* now_bytes = slot_bytes(now);
				const u8* before_bytes = slot_bytes(before);

				if (!now_bytes || !before_bytes)
				{
					continue;
				}

				u32 newly_black = 0;
				u32 newly_dark = 0;
				u32 x0 = now.width;
				u32 y0 = now.height;
				u32 x1 = 0;
				u32 y1 = 0;
				u32 cell_x = 0;
				u32 cell_y = 0;
				u32 cell_count = 0;
				// One counter per 16x16 cell of the whole taped image.  A 2048
				// square is the largest the tape accepts, so 128 by 128 cells covers
				// every slot; sizing this to the old 16 by 16 grid silently ignored
				// everything right of or below x=255.
				const u32 cell_columns = (now.width + 15u) / 16u;
				u32 cells[128 * 128] = {};

				for (u32 y = 0; y < now.height; ++y)
				{
					const u8* now_row = now_bytes + static_cast<u64>(y) * now.row_pitch;
					const u8* before_row = before_bytes + static_cast<u64>(y) * before.row_pitch;

					for (u32 x = 0; x < now.width; ++x)
					{
						const u8* now_pixel = now_row + x * 4u;
						const u8* before_pixel = before_row + x * 4u;

						const bool now_black = !now_pixel[0] && !now_pixel[1] && !now_pixel[2];
						const bool before_black = !before_pixel[0] && !before_pixel[1] && !before_pixel[2];

						const bool now_dark = now_black || std::max({ now_pixel[0], now_pixel[1], now_pixel[2] }) <= 16;
						const bool before_dark = before_black || std::max({ before_pixel[0], before_pixel[1], before_pixel[2] }) <= 16;

						if (!now_black || before_black)
						{
							if (!now_dark || before_dark)
							{
								continue;
							}

							++newly_dark;
							continue;
						}

						++newly_black;
						++newly_dark;

						if (x < x0) x0 = x;
						if (y < y0) y0 = y;
						if (x > x1) x1 = x;
						if (y > y1) y1 = y;

						const u32 cell = (y / 16u) * cell_columns + (x / 16u);
						if (cell < std::size(cells) && ++cells[cell] > cell_count)
						{
							cell_count = cells[cell];
							cell_x = x & ~15u;
							cell_y = y & ~15u;
						}
					}
				}

				if (newly_black || (newly_dark >= 64))
				{
					char bbox[64] = "-";

					if (newly_black)
					{
						std::snprintf(bbox, sizeof(bbox), "%u,%u %ux%u", x0, y0,
							x1 >= x0 ? x1 - x0 + 1 : 0u, y1 >= y0 ? y1 - y0 + 1 : 0u);
					}

					MTL_NOTE_WITNESS(5, "Metal 4: tape present frame %llu vs %llu %ux%u, newly-black %u bbox %s, newly-dark %u, worst cell %u,%u n=%u",
						static_cast<unsigned long long>(now.frame), static_cast<unsigned long long>(before.frame),
						now.width, now.height, newly_black, bbox, newly_dark, cell_x, cell_y, cell_count);
				}
			}
		}

		// Present frames are the flicker timeline and are written first.  A sample
		// stream can be a full-size target, and the disk budget is one pool: if the
		// samples filled it first, a later press would lose the very presents that
		// prove the frame.  The loop therefore makes two passes, present then sample.
		const auto write_slot = [&](diagnostic_tape_slot& slot)
		{
			if (!slot.valid || !slot.buffer)
			{
				return;
			}

			const u8* bytes = slot_bytes(slot);

			if (!bytes)
			{
				slot.valid = false;
				return;
			}

			const u32 row_bytes = slot.width * 4u;
			const u64 file_bytes = static_cast<u64>(row_bytes) * slot.height;

			// The measurement above always runs; only the file write is budgeted, so
			// a full budget still leaves the timeline in the log.
			if (diagnostic_tape_bytes + file_bytes > diagnostic_tape_byte_limit)
			{
				if (!diagnostic_tape_budget_reported)
				{
					diagnostic_tape_budget_reported = true;
					MTL_NOTE_WITNESS(5, "Metal 4: pixel tape budget %llu bytes exhausted; remaining slots are dropped",
						static_cast<unsigned long long>(diagnostic_tape_byte_limit));
				}

				slot.valid = false;
				return;
			}

			char path[1024];
			std::snprintf(path, sizeof(path), "%s/host-tape-f%llu-%s-s%u-d%u-u%u-addr%x-%ux%u.bin",
				probe::dump_directory(), static_cast<unsigned long long>(slot.frame),
				slot.present ? "present" : "sample", slot.stream, slot.draw, slot.unit, slot.address, slot.width, slot.height);

			if (std::FILE* out = std::fopen(path, "wb"))
			{
				for (u32 y = 0; y < slot.height; ++y)
				{
					std::fwrite(bytes + static_cast<u64>(y) * slot.row_pitch, 1, row_bytes, out);
				}

				std::fclose(out);
				diagnostic_tape_bytes += file_bytes;
				++diagnostic_tape_dumps;
			}

			slot.valid = false;
		};

		std::error_code directory_error;
		std::filesystem::create_directories(probe::dump_directory(), directory_error);

		for (diagnostic_tape_slot& slot : diagnostic_tape)
		{
			if (slot.present)
			{
				write_slot(slot);
			}
		}

		for (diagnostic_tape_slot& slot : diagnostic_tape)
		{
			if (!slot.present)
			{
				write_slot(slot);
			}
		}

		MTL_NOTE_WITNESS(5, "Metal 4: pixel tape dumped %u slot(s), %llu bytes total, %llu bytes allocated",
			diagnostic_tape_dumps, static_cast<unsigned long long>(diagnostic_tape_bytes),
			static_cast<unsigned long long>(diagnostic_tape_allocated));
	}

	// TODO(excise): level 5; a capture window may open on a frame with thousands of
	// draws (a race-start frame reached 670).  Every probe is a queue round trip,
	// so one window may spend at most this many draws: past it the run spends its
	// time waiting on a queue it has itself stalled (INVESTIGATION-2026-10-22 §10.6).
	// the per-window probe limit (512 draws) widens it for one run when the patch's draw is past
	// the default prefix; the stall is then the run's cost, not a bug.
	const u32 diagnostic_window_probe_limit = probe::pixel_probe_limit();
	u32 diagnostic_window_probes = 0;

	// TODO(excise): level 5; each capture window gets its own host-dump and
	// pixel-probe budget.  A single global budget spent it in the first window.
	void diagnostic_reset_window_state()
	{
		diagnostic_capture_image_count = 0;
		diagnostic_pixel_probes = 0;
		diagnostic_window_probes = 0;
		diagnostic_capture_index_dumps = 0;
		diagnostic_capture_vertex_dumps = 0;
		diagnostic_window_dump_frames = 0;
		diagnostic_frame_dump_images = 0;
		diagnostic_black_patch_dumps = 0;

		for (u32& previous : diagnostic_patch_previous_black)
		{
			previous = 0;
		}

		for (u32& address : diagnostic_patch_previous_address)
		{
			address = 0;
		}
	}

	// TODO(excise): level 5; the sampled-image dedupe is per frame.  The frame
	// that carries the defect is the one whose images have to be dumped, and a
	// per-window list kept the first frame's copy of every address instead.
	// The geometry budgets stay per window: the draw order repeats every frame,
	// so a per-frame reset would only dump the same background subdraws again.
	void diagnostic_reset_frame_state()
	{
		diagnostic_capture_image_count = 0;
		diagnostic_frame_dump_images = 0;

		// The black-jump baseline is per frame: the first probe of a frame
		// establishes it, so a pass from the previous frame cannot look like a
		// rise and spend an evidence dump.
		for (u32& previous : diagnostic_patch_previous_black)
		{
			previous = 0;
		}

		for (u32& address : diagnostic_patch_previous_address)
		{
			address = 0;
		}
	}

	// TODO(excise): level 3; the census counts at teardown, the busiest row first.
	void diagnostic_census_report()
	{
#if MTL_DEBUG_LOGS
		if (!probe::at_least(3) || !diagnostic_census_count)
		{
			return;
		}

		MTL_NOTE(3, "Metal 4: census total, %u rows, %llu draws, %llu rows over the limit",
			diagnostic_census_count, static_cast<unsigned long long>(diagnostic_census_draws),
			static_cast<unsigned long long>(diagnostic_census_overflow));

		bool reported[diagnostic_census_limit] = {};

		for (u32 rank = 0; rank < 16; ++rank)
		{
			u32 best = diagnostic_census_limit;

			for (u32 index = 0; index < diagnostic_census_count; ++index)
			{
				if (!reported[index] && (best == diagnostic_census_limit ||
					diagnostic_census[index].count > diagnostic_census[best].count))
				{
					best = index;
				}
			}

			if (best == diagnostic_census_limit || !diagnostic_census[best].count)
			{
				break;
			}

			reported[best] = true;

			MTL_NOTE(3, "Metal 4: census top %u, row %u, draws %llu, first draw %u",
				rank, best, static_cast<unsigned long long>(diagnostic_census[best].count),
				diagnostic_census[best].first_draw);
		}
#endif // MTL_DEBUG_LOGS
	}

	void diagnostic_read_pixel_rects()
	{
		diagnostic_pixel_rects_read = true;

		const char* text = std::getenv("MTL_PROBE_PIXELS");

		if (!text)
		{
			return;
		}

		for (const char* cursor = text; *cursor && diagnostic_pixel_rect_count < std::size(diagnostic_pixel_rects);)
		{
			unsigned x = 0, y = 0, w = 0, h = 0;

			if (std::sscanf(cursor, "%u,%u,%u,%u", &x, &y, &w, &h) != 4)
			{
				break;
			}

			diagnostic_pixel_rects[diagnostic_pixel_rect_count++] = { x, y, w, h };

			cursor = std::strchr(cursor, ';');

			if (!cursor)
			{
				break;
			}

			++cursor;
		}

		MTL_NOTE(2, "Metal 4: pixel probe, %u patch(es) configured", diagnostic_pixel_rect_count);
	}

	void probe_frame_pixels(command_recorder& recorder, resource_manager& resources, MTL::Texture* image, u32 serial, u64 fp_hash, u64 vp_hash,
		diagnostic_patch_observation* observations, u32* observation_count)
	{
		if (!image || !diagnostic_pixel_rect_count)
		{
			return;
		}

		const MTL::PixelFormat format = image->pixelFormat();

		// Only a four-byte colour layout puts one pixel in one word.  A packed or
		// compressed target would need its block size, and none occurs here.
		if (format != MTL::PixelFormat::PixelFormatBGRA8Unorm && format != MTL::PixelFormat::PixelFormatRGBA8Unorm)
		{
			return;
		}

		const u32 slice_width = static_cast<u32>(image->width());
		const u32 slice_height = static_cast<u32>(image->height());

		// One readback per draw, not one per patch.  Each flush is a queue wait, so
		// three patches would otherwise stall the frame three times at every draw;
		// all the copies are recorded on one encoder and one wait makes them all
		// CPU-visible.
		struct pending_patch
		{
			u32 index = 0;
			u32 x = 0;
			u32 y = 0;
			u32 width = 0;
			u32 height = 0;
			u32 row_pitch = 0;
			MTL::Buffer* buffer = nullptr;
		};

		pending_patch pending[8] = {};
		u32 pending_count = 0;

		for (u32 index = 0; index < diagnostic_pixel_rect_count; ++index)
		{
			const auto& rect = diagnostic_pixel_rects[index];

			if (!rect.w || !rect.h || rect.x >= slice_width || rect.y >= slice_height)
			{
				continue;
			}

			const u32 width = std::min(rect.w, slice_width - rect.x);
			const u32 height = std::min(rect.h, slice_height - rect.y);
			const u32 row_pitch = utils::align(width * 4u, 0x100);
			const u64 length = static_cast<u64>(row_pitch) * height;

			MTL::Buffer* buffer = resources.new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

			if (!buffer)
			{
				break;
			}

			resources.make_persistent(buffer);
			pending[pending_count++] = { index, rect.x, rect.y, width, height, row_pitch, buffer };
		}

		if (!pending_count)
		{
			return;
		}

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			for (u32 slot = 0; slot < pending_count; ++slot)
			{
				resources.defer_release(pending[slot].buffer);
			}

			return;
		}

		// The draw just closed its encoder, so the patch is the state that draw
		// left behind; one barrier orders that write before every copy below.
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });

		for (u32 slot = 0; slot < pending_count; ++slot)
		{
			const pending_patch& patch = pending[slot];
			encoder->copyFromTexture(image, 0, 0, MTL::Origin::Make(patch.x, patch.y, 0),
				MTL::Size::Make(patch.width, patch.height, 1),
				patch.buffer, 0, patch.row_pitch, patch.row_pitch * patch.height);
		}

		recorder.close_encoder();
		recorder.flush_for_readback();

		for (u32 slot = 0; slot < pending_count; ++slot)
		{
			const pending_patch& patch = pending[slot];
			const u32 index = patch.index;
			const u32 width = patch.width;
			const u32 height = patch.height;
			const u32 row_pitch = patch.row_pitch;
			const u8* bytes = static_cast<const u8*>(patch.buffer->contents());

			if (bytes)
			{
				u64 sums[4] = {};
				u32 nonzero[4] = {};
				u8 seen[256] = {};
				u32 distinct = 0;
				u32 first = 0;
				u32 black = 0;
				u32 opaque = 0;

				for (u32 y = 0; y < height; ++y)
				{
					const u8* row = bytes + static_cast<u64>(y) * row_pitch;

					for (u32 x = 0; x < width; ++x)
					{
						const u8* pixel = row + x * 4u;

						// BGRA/RGBA: the first three bytes are colour, the fourth
						// alpha.  The defect is pure #000000, so all three colour
						// bytes are zero; an opaque black pixel also carries alpha.
						if (!pixel[0] && !pixel[1] && !pixel[2])
						{
							++black;
						}

						if (pixel[3])
						{
							++opaque;
						}

						for (u32 channel = 0; channel < 4; ++channel)
						{
							sums[channel] += pixel[channel];
							nonzero[channel] += pixel[channel] ? 1u : 0u;

							if (!seen[pixel[channel]])
							{
								seen[pixel[channel]] = 1;
								++distinct;
							}
						}

						if (!first)
						{
							std::memcpy(&first, pixel, 4);
						}
					}
				}

				const u32 pixels = width * height;

				if (observations && observation_count && *observation_count < 8)
				{
					observations[*observation_count] = { index, patch.x, patch.y, width, height, black, opaque, pixels };
					++*observation_count;
				}

				MTL_NOTE(2, "Metal 4: pixel probe draw %u patch %u %u,%u %ux%u, fp 0x%llx vp 0x%llx, mean b %.1f g %.1f r %.1f a %.1f, nonzero b %u g %u r %u a %u of %u, black %u opaque %u, distinct %u, first 0x%08x",
					serial, index, patch.x, patch.y, width, height, static_cast<unsigned long long>(fp_hash), static_cast<unsigned long long>(vp_hash),
					static_cast<f64>(sums[0]) / pixels, static_cast<f64>(sums[1]) / pixels,
					static_cast<f64>(sums[2]) / pixels, static_cast<f64>(sums[3]) / pixels,
					nonzero[0], nonzero[1], nonzero[2], nonzero[3], pixels, black, opaque, distinct, first);
			}

			resources.defer_release(patch.buffer);
		}
	}

#endif // MTL_DEBUG_LOGS

	bool MTLGSRender::resolve_sampler_state(rsx::flags32_t& invalidated_programs)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "textures/resolve");
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;

		// The stencil half of a redirected depth unit samples through its own
		// sampler (VKDraw.cpp:707-717), shared by every unit of the clause.
		MTL::SamplerState* stencil_sampler = frame.samplers.get(stencil_mirror_sampler());
		frame.fragment_stencil = {};

#if MTL_DEBUG_LOGS
		// Each resolve is one draw's unit set.  The draw loop consumes the
		// feedback witness this resolve produced, not the previous draw's.
		diagnostic_feedback_binding_count = 0;

		// The per-unit trace runs near once per draw per unit, so a bounded prefix of
		// the run carries it and the counters carry the rest (anti-flood rule).
		static u32 diagnostic_unit_traces = 0;
#endif

		// A unit the program does not reference is never sampled, so it is skipped
		// (GLDraw.cpp:340-425).  The cache resolves the guest range to an image and
		// the sampler is a separate object, because MSL has no combined image
		// sampler (HLD-05 §3.2).
		for (u32 textures_ref = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!m_fs_sampler_descriptors[i])
			{
				m_fs_sampler_descriptors[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_fs_sampler_descriptors[i].get());
			const auto& tex = m_ctx->register_state->fragment_textures[i];

			// A disabled unit must not keep the image of the previous draw.  The
			// argument writer clears the table slot when the bind becomes null (B3).
			if (!tex.enabled())
			{
				// TODO(excise): level 2; a referenced unit was disabled.
				MTL_PROBE_LATCH(2, "texture/disabled");
				frame.fragment_textures[i] = {};
				frame.fragment_stencil[i] = {};
				continue;
			}

			const rsx::format_class previous_class = descriptor->format_class;
			// The core clears the referenced mask after this hook returns true; do
			// not consume the bit here.
			const bool dirty = m_ctx->textures.fragment_textures_dirty & (1u << i);
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;

				// TODO(excise): level 2; the format of an uploaded fragment texture.
				// An upload is far rarer than a draw, so the key is stable enough.
				MTL_PROBE_HISTOGRAM(2, "texture format",
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN), "gcm texture format");

				// TODO(excise): level 2; the host format the guest format resolved to,
				// and the context the upload came from.  A blit-engine upload is the
				// FMV/video path, which is one of the reported black screens.
				MTL_PROBE_HISTOGRAM(2, "texture host format",
					static_cast<u32>(mtl::get_texture_format(
						static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
						frame.resources.supports_bc_texture_compression())),
					"metal texture format");
				MTL_PROBE_HISTOGRAM(2, "texture upload context",
					static_cast<u32>(descriptor->upload_context), "upload context");

#if MTL_DEBUG_LOGS
				// TODO(excise): level 3; the first uploads of the run only.
				if (diagnostic_unit_traces++ < 16)
				{
					MTL_NOTE(3, "Metal 4: resolve fragment unit %u, address 0x%x, format 0x%x, %ux%u, dirty %u, class %u, ctx %u",
						i, rsx::get_address(m_ctx, tex.offset(), tex.location()), static_cast<u32>(tex.format()),
						static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), dirty ? 1u : 0u,
						static_cast<u32>(descriptor->format_class), static_cast<u32>(descriptor->upload_context));
				}
#endif // MTL_DEBUG_LOGS
			}

			if (!descriptor->validate())
			{
				// TODO(excise): level 2; a bound fragment texture that failed to
				// resolve, so the draw samples a null slot.
				MTL_PROBE_LATCH(2, "texture/bind null fragment");
				frame.fragment_textures[i] = {};
				continue;
			}

			descriptor->format_ex = tex.format_ex();

			if (descriptor->format_class != previous_class)
			{
				// The host image changed class under the program's feet.  The core
				// propagates the flag into graphics_state.
				invalidated_programs |= rsx::fragment_program_state_dirty;
			}

			bool deferred = false;
			frame.fragment_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_fragment_sampler(*m_ctx->register_state, static_cast<u8>(i))), deferred);

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the feedback witness.  The shared cache sets
			// `is_feedback_loop_read` when the unit samples the bound target, so this
			// line separates "the draw read a live attachment" from "the cache
			// copied it first" without guessing from the address.  A run after the
			// fix must show `copy` for the post chain; a surviving `live` names the
			// unit the copy did not reach.
			if (probe::at_least(5) && descriptor->is_feedback_loop_read)
			{
				// A copy descriptor carries no live view; the expired-feedback
				// rebuild above also clears it.  Deriving the path from the handle
				// therefore survives that rebuild, where the old
				// `is_cyclic_reference` test mislabelled the rebuilt copy as live.
				const bool copied = !descriptor->image_handle;

				if (copied)
				{
					++diagnostic_feedback_copy;
				}
				else
				{
					++diagnostic_feedback_live;
				}

				if (diagnostic_feedback_binding_count < std::size(diagnostic_feedback_bindings))
				{
					diagnostic_feedback_binding& binding = diagnostic_feedback_bindings[diagnostic_feedback_binding_count++];
					binding.image = descriptor->image_handle
						? descriptor->image_handle->value()
						: static_cast<MTL::Texture*>(probe::deferred_dump.image);
					binding.unit = i;
					binding.address = descriptor->ref_address;
					binding.target = 0;
					binding.draw = diagnostic_current_draw;
					binding.copy = copied;
				}

				MTL_NOTE_WITNESS(5, "Metal 4: feedback witness frame %llu draw %u unit %u addr 0x%x samples %u path %s",
					static_cast<unsigned long long>(diagnostic_frame_index), diagnostic_current_draw, i,
					descriptor->ref_address, descriptor->samples, copied ? "copy" : "live");
			}
#endif // MTL_DEBUG_LOGS

			// TODO(excise): level 5; the red-image bisection.  The whole branch is
			// the replacement experiment, so it goes with the probe.
			if (diagnostic_replace_textures() && diagnostic_replace_unit(i))
			{
				frame.fragment_textures[i].texture = diagnostic_replacement_texture(
					frame.resources, tex.get_extended_texture_dimension());
			}

			// TODO(excise): level 2; the sampled unit, counted rather than printed.
			if (resampled)
			{
				MTL_PROBE_LATCH(2, "texture/resolved");
			}
			else
			{
				MTL_PROBE_LATCH(2, "texture/retained");
			}

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; a cubemap unit exists only once the scene has a
			// car (or a reflection probe), so the bind itself is the capture
			// trigger.  A car-paint shader samples such a unit on every frame.
			if (tex.get_extended_texture_dimension() == rsx::texture_dimension_extended::texture_dimension_cubemap)
			{
				diagnostic_try_arm_capture("cubemap bind");
			}

			// TODO(excise): level 5; the video path binds its luma plane as a large
			// B8 texture (the two chroma planes follow at half extent), so this bind
			// is the one cue that reaches an intro video that has no cubemap.
			if ((static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN)) == CELL_GCM_TEXTURE_B8 &&
				tex.width() >= 512 && tex.height() >= 256)
			{
				diagnostic_try_arm_capture("video plane bind");
			}

			// TODO(excise): level 5; a deferred gather is materialised inside the
			// bind and released after the pass, so the host dump reads it here, in
			// the draw loop that owns the recorder.  One dump per address, like the
			// sampled units below.
			if (probe::deferred_dump.image)
			{
				if (diagnostic_capture_active() && probe::deferred_dump.address &&
					!diagnostic_capture_image_seen(probe::deferred_dump.image) &&
					diagnostic_capture_image_count < diagnostic_capture_image_limit &&
					diagnostic_image_dump_allowed())
				{
					diagnostic_capture_images[diagnostic_capture_image_count++] = probe::deferred_dump.image;
					diagnostic_image_dumped();

					MTL_NOTE(5, "Metal 4: deferred unit %u, op %u, address 0x%x, image %p",
						i, probe::deferred_dump.op, probe::deferred_dump.address, probe::deferred_dump.image);

					dump_host_image(frame.recorder, frame.resources,
						static_cast<MTL::Texture*>(probe::deferred_dump.image), "deferred",
						static_cast<u32>(diagnostic_frame_index * 100u + i), probe::deferred_dump.address);
				}

				probe::deferred_dump.image = nullptr;
			}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; one capture frame reaches the race units that
			// the bounded prefix of the whole run never reaches.
			if (diagnostic_capture_active())
			{
				const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());
				const auto& params = m_ctx->program.fragment_program.texture_params[i];
				const sampler_state state = capture_fragment_sampler(*m_ctx->register_state, static_cast<u8>(i));

				MTL_NOTE(5, "Metal 4: capture frame %llu unit %u, guest 0x%x ref 0x%x, raw 0x%x gcm 0x%x %ux%u pitch %u mips %u, ctx %u class %u, image %p %ux%u fmt 0x%x, sampler wrap %u/%u/%u filter %u/%u unnorm %u lod %.3f..%.3f bias %.3f aniso %u border %u compare %u/%u nearest %u, params scale %.4f %.4f %.4f bias %.4f %.4f %.4f clamp %.4f %.4f %.4f %.4f remap 0x%x control 0x%x",
					static_cast<unsigned long long>(diagnostic_frame_index), i, guest_address, descriptor->ref_address,
					static_cast<u32>(tex.format()),
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), tex.pitch(),
					static_cast<u32>(tex.get_exact_mipmap_count()),
					static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
					static_cast<const void*>(descriptor->image_handle),
					descriptor->image_handle ? descriptor->image_handle->width() : 0u,
					descriptor->image_handle ? descriptor->image_handle->height() : 0u,
					descriptor->image_handle ? static_cast<u32>(descriptor->image_handle->pixel_format()) : 0u,
					static_cast<u32>(state.wrap_s), static_cast<u32>(state.wrap_t), static_cast<u32>(state.wrap_r),
					static_cast<u32>(state.min_filter), static_cast<u32>(state.mag_filter),
					state.unnormalized_coords ? 1u : 0u,
					static_cast<f64>(state.min_lod), static_cast<f64>(state.max_lod), static_cast<f64>(state.lod_bias),
					state.max_aniso, static_cast<u32>(state.border_color),
					state.depth_compare ? 1u : 0u, static_cast<u32>(state.compare), state.force_nearest ? 1u : 0u,
					static_cast<f64>(params.scale[0]), static_cast<f64>(params.scale[1]), static_cast<f64>(params.scale[2]),
					static_cast<f64>(params.bias[0]), static_cast<f64>(params.bias[1]), static_cast<f64>(params.bias[2]),
					static_cast<f64>(params.clamp_min[0]), static_cast<f64>(params.clamp_min[1]),
					static_cast<f64>(params.clamp_max[0]), static_cast<f64>(params.clamp_max[1]),
					params.remap, params.control);

				const u32 capture_address = descriptor->ref_address ? descriptor->ref_address : guest_address;

				if (descriptor->image_handle && !diagnostic_capture_image_seen(static_cast<const void*>(descriptor->image_handle->value())) &&
					diagnostic_capture_image_count < diagnostic_capture_image_limit &&
					diagnostic_image_dump_allowed())
				{
					diagnostic_capture_images[diagnostic_capture_image_count++] = static_cast<const void*>(descriptor->image_handle->value());
					diagnostic_image_dumped();
					dump_host_image(frame.recorder, frame.resources, descriptor->image_handle->value(),
						"sample", static_cast<u32>(diagnostic_frame_index * 100u + i), capture_address);
				}

				// TODO(excise): level 5; the swizzle that is applied, not the one
				// the format declares.  The unit line above prints
				// get_texture_component_layout(), which is the guest format's channel
				// order; the shader samples the channels of the texture the table
				// binds, so the composed view map is the fact that decides it.
				if (descriptor->image_handle)
				{
					const rsx::texture_channel_remap_t remap = tex.decoded_remap();
					const auto applied = apply_swizzle_remap(
						descriptor->image_handle->get_native_component_layout(), remap);

					constexpr std::array<MTL::TextureSwizzle, 4> identity_layout =
					{
						MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed,
						MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue
					};

					MTL_NOTE(5, "Metal 4: capture frame %llu unit %u applied swizzle A%u R%u G%u B%u, %s, remap 0x%x",
						static_cast<unsigned long long>(diagnostic_frame_index), i,
						static_cast<u32>(applied[0]), static_cast<u32>(applied[1]),
						static_cast<u32>(applied[2]), static_cast<u32>(applied[3]),
						applied == identity_layout ? "identity, the base texture is bound" : "remapped, a swizzled view is bound",
						static_cast<u32>(remap.encoded));

					// TODO(excise): level 5; the swizzle that is actually bound.
					// image_handle is the view the table binds, and a view carries the
					// default identity layout, so the line above reads the view's map and
					// not the root's.  Print the root's map and the channels Metal holds,
					// which is what the sampler applies.
					const texture* root = descriptor->image_handle->image();
					const auto root_layout = root->get_native_component_layout();
					const MTL::TextureSwizzleChannels bound =
						descriptor->image_handle->value()->swizzle();

					MTL_NOTE(5, "Metal 4: capture frame %llu unit %u root layout A%u R%u G%u B%u, bound R%u G%u B%u A%u, view %u, fmt 0x%x",
						static_cast<unsigned long long>(diagnostic_frame_index), i,
						static_cast<u32>(root_layout[0]), static_cast<u32>(root_layout[1]),
						static_cast<u32>(root_layout[2]), static_cast<u32>(root_layout[3]),
						static_cast<u32>(bound.red), static_cast<u32>(bound.green),
						static_cast<u32>(bound.blue), static_cast<u32>(bound.alpha),
						(descriptor->image_handle == root) ? 0u : 1u,
						static_cast<u32>(descriptor->image_handle->pixel_format()));
				}
			}

			// TODO(excise): level 3; the first sampled units of the run only.
			if (diagnostic_unit_traces++ < 16)
			{
				MTL_NOTE(3, "Metal 4: fragment unit %u reads 0x%x, gcm 0x%x, %ux%u, pitch %u, ctx %u, class %u, ref 0x%x, %s, image %p %ux%u fmt 0x%x",
					i, rsx::get_address(m_ctx, tex.offset(), tex.location()),
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), tex.pitch(),
					static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
					descriptor->ref_address, resampled ? "resolved" : "retained",
					static_cast<const void*>(descriptor->image_handle),
					descriptor->image_handle ? descriptor->image_handle->width() : 0u,
					descriptor->image_handle ? descriptor->image_handle->height() : 0u,
					descriptor->image_handle ? static_cast<u32>(descriptor->image_handle->pixel_format()) : 0u);
			}
#endif // MTL_DEBUG_LOGS

			// The transfer that filled an uploaded image is a write, and the
			// scheduler orders only the edges it holds a write record for, so the
			// read this draw makes needs that record (HLD-03 §2.4).
			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.fragment_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.fragment_textures[i].texture,
					sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i]),
					access_stages(rsx::surface_access::transfer_write, false));
			}

			// A redirected depth unit also reads the stencil half of its surface,
			// through a castable view (VKDraw.cpp:702-721).  The view is the surface
			// itself when the surface carries no stencil, and the slot then stays
			// unbound.
			if ((m_ctx->program.fragment_program.texture_state.redirected_textures & (1u << i)) &&
				stencil_sampler && descriptor->image_handle)
			{
				if (texture* stencil = descriptor->image_handle->stencil_view())
				{
					frame.fragment_stencil[i] = { stencil->resource_id(), stencil_sampler->gpuResourceID() };
				}
			}
		}

		for (u32 textures_ref = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!m_vs_sampler_descriptors[i])
			{
				m_vs_sampler_descriptors[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_vs_sampler_descriptors[i].get());
			const auto& tex = m_ctx->register_state->vertex_textures[i];

			// A disabled unit binds null, not the image from the previous draw (B4).
			if (!tex.enabled())
			{
				// TODO(excise): level 2; a referenced unit was disabled.
				MTL_PROBE_LATCH(2, "texture/disabled");
				frame.vertex_textures[i] = {};
				continue;
			}

			const bool dirty = m_ctx->textures.vertex_textures_dirty & (1u << i);
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;

				// TODO(excise): level 2; the guest and host formats of an uploaded
				// vertex texture, and the upload context.
				MTL_PROBE_HISTOGRAM(2, "texture format",
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN), "gcm texture format");
				MTL_PROBE_HISTOGRAM(2, "texture host format",
					static_cast<u32>(mtl::get_texture_format(
						static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
						frame.resources.supports_bc_texture_compression())),
					"metal texture format");
				MTL_PROBE_HISTOGRAM(2, "texture upload context",
					static_cast<u32>(descriptor->upload_context), "upload context");

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: resolve vertex unit %u, address 0x%x, format 0x%x, %ux%u, dirty %u, ctx %u",
					i, rsx::get_address(m_ctx, tex.offset(), tex.location()), static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), dirty ? 1u : 0u,
					static_cast<u32>(descriptor->upload_context));
			}

			if (!descriptor->validate())
			{
				// TODO(excise): level 2; a bound vertex texture that failed to
				// resolve, so the draw samples a null slot.
				MTL_PROBE_LATCH(2, "texture/bind null vertex");
				frame.vertex_textures[i] = {};
				continue;
			}

			bool deferred = false;
			frame.vertex_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_vertex_sampler(*m_ctx->register_state, static_cast<u8>(i))), deferred);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: vertex unit %u reads 0x%x, gcm 0x%x, %ux%u, ctx %u, class %u, %s, image %p",
				i, rsx::get_address(m_ctx, tex.offset(), tex.location()),
				static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
				static_cast<u32>(tex.width()), static_cast<u32>(tex.height()),
				static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
				resampled ? "resolved" : "retained", static_cast<const void*>(descriptor->image_handle));

			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.vertex_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.vertex_textures[i].texture,
					sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i]),
					access_stages(rsx::surface_access::transfer_write, false));
			}
		}

		// The core adds textures_upload_time after this hook returns true.

		// A unit that cannot be resolved binds null; that is not a reason to skip
		// the draw or the program resolution that follows.
		return true;
	}

	void MTLGSRender::record_pass_hazards(bool sample_textures)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "pass/hazards");
		frame_resources& frame = *m_frame_resources;
		pass_scheduler& scheduler = frame.scheduler;

		scheduler.begin_pass();

		// The read set is the units the programs reference, not the whole resolved
		// array: an unreferenced unit still holds the previous clause's image, and a
		// barrier for it would consume a write record the frame still needs.
		if (sample_textures)
		{
			for (u32 referenced = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.fragment_textures[i].texture._impl)
				{
					scheduler.note_read(frame.fragment_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i]),
						access_stages(rsx::surface_access::shader_read, false));
				}
			}

			for (u32 referenced = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.vertex_textures[i].texture._impl)
				{
					scheduler.note_read(frame.vertex_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i]),
						access_stages(rsx::surface_access::shader_read, true));
				}
			}
		}

		// The attachment set is the pass's write set.  The store is a tile-stage
		// operation on Apple silicon (StoreActionStore), so the note names the
		// fragment and tile stages — a fragment-only producer never claims to
		// order the flush of the tile the pass wrote.
		for (u32 index = 0; index < frame.attachments.color_count(); ++index)
		{
			surface* color = frame.attachments.color(index);
			scheduler.note_write(color->resource_id(), color->get_memory_range(),
				access_stages(rsx::surface_access::shader_write, false) | MTL::StageTile);

			// A multisampled attachment resolves into its resolve target when the
			// pass ends, and the resolve target is the image a later draw samples
			// (HLD-02 §2.2).  The resolve is a tile-stage write of this pass, and
			// the scheduler orders only the writes it holds a record for, so the
			// read of the resolved image needs this one (HLD-03 §2.4).
			if (texture* resolve = color->resolve_surface.get())
			{
				// TODO(excise): level 2; the resolve-target write of a pass (area J).
				MTL_PROBE_LATCH(2, "surface/resolve target write");
				// The resolve target is a private image the cache does not key by
				// guest address, but it is the surface's resolved image, so a later
				// sample of the surface range must be ordered behind it.  The write
				// therefore carries the surface's footprint, not a range of its own.
				scheduler.note_write(resolve->resource_id(), color->get_memory_range(), MTL::StageTile);
			}
		}

		if (frame.attachments.has_depth())
		{
			scheduler.note_write(frame.attachments.depth()->resource_id(), frame.attachments.depth()->get_memory_range(),
				access_stages(rsx::surface_access::shader_write, false) | MTL::StageTile);

			// A multisampled depth attachment resolves into its resolve target in
			// the resolve pass, which is a tile-stage write of that pass — the same
			// note the colour branch carries for its resolve target (HLD-02 §2.2).
			if (texture* depth_resolve = frame.attachments.depth()->resolve_surface.get())
			{
				scheduler.note_write(depth_resolve->resource_id(), frame.attachments.depth()->get_memory_range(), MTL::StageTile);
			}

			// A redirected depth unit also reads the stencil half of its surface
			// through a castable view (MTLArgumentBinds.cpp:131-138).  The view is a
			// separate Metal texture with no neighbour the scheduler can see, so the
			// read is noted here against the depth surface's footprint — the stencil
			// bytes live in the same guest range.  Coverage duplicates the mirror
			// barrier for the mirror image, and extends it to the attachment write.
			for (u32 unit = 0; unit < frame.fragment_stencil.size(); ++unit)
			{
				if (frame.fragment_stencil[unit].texture._impl)
				{
					scheduler.note_read(frame.fragment_stencil[unit].texture, frame.attachments.depth()->get_memory_range(), MTL::StageFragment);
				}
			}
		}

		// A cyclic reference the cache met while it resolved this clause: the surface
		// is a render target and a sampled input at once, and only a pass boundary
		// orders that pair (REVIEW-FINDINGS.md, I4).
		if (frame.textures.consume_texture_barrier_request())
		{
			scheduler.request_texture_barrier();
		}

		// The carried edges of the pass that split, then this pass's own.
		scheduler.emit_split_barriers(frame.encoder);
		scheduler.flush(frame.encoder);
	}

	// The draw execution (HLD-05 §3.5).  One RSX draw clause is a sequence of
	// subdraws: the pipeline, the pass and the encoder state are per clause, and
	// the vertex streams and the ABI blocks are per subdraw.  end() runs before
	// rsx::thread::end(), which owns the post-execute cleanup (RSXThread.cpp:971).
	bool MTLGSRender::end_draw()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/end");

		// The core decoded the draw state, resolved the samplers and the programs,
		// and moved the pipeline bits into draw_state.dirty before this hook.  The
		// framebuffer is bound and a skipped draw never reaches here.
		if (!m_frame_resources)
		{
			return false;
		}

		frame_resources& frame = *m_frame_resources;
		auto& draw_call = m_ctx->register_state->current_draw_clause;

		// The recorder can have ended the encoder since the previous draw: an
		// upload, a resolve, a readback, a present, or a slot re-entry.  The cached
		// pointer is only valid while it still names the recorder's encoder.
		if (frame.encoder && static_cast<MTL4::CommandEncoder*>(frame.encoder) != frame.recorder.current_encoder())
		{
			frame.encoder = nullptr;
		}

		// The core's resolve_scissor already intersected the viewport, clamped to the
		// window and marked the invalid case.
		const areau scissor_region = scaled_scissor(m_ctx->draw_state.scissor, m_ctx->fb.layout,
			RSX(m_ctx)->resolution_scaling_config);

		// EXCISE-BEGIN draw-report-bools
		static bool diagnostic_program_reported = false;
		static bool diagnostic_pipeline_reported = false;
		static bool diagnostic_pass_reported = false;
		// EXCISE-END draw-report-bools

		// The programs of this draw.  The core resolved both, and the texture units
		// resolved before them.
		// EXCISE-BEGIN draw-counter
		static u32 diagnostic_draws = 0;
		// EXCISE-END draw-counter

		// TODO(excise): level 2; the draw stream is counted and classified here.
		MTL_PROBE_LATCH(2, "draw/record");
		MTL_PROBE_HISTOGRAM(2, "draw primitive", static_cast<u32>(draw_call.primitive), "rsx primitive");

#if MTL_DEBUG_LOGS
		// The per-draw trace is the largest trace source of the backend, so it prints
		// for a bounded prefix of the run and never again.  The counters and the
		// histograms above carry the rest of the draw stream (anti-flood rule).
		if (diagnostic_draws < 16)
		{
			char units[512] = {};
			u32 units_len = 0;

			for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
			{
				const auto& tex = m_ctx->register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());
				const bool is_target =
					guest_address == m_ctx->fb.layout.color_addresses[0] ||
					guest_address == m_ctx->fb.layout.color_addresses[1] ||
					guest_address == m_ctx->fb.layout.color_addresses[2] ||
					guest_address == m_ctx->fb.layout.color_addresses[3] ||
					guest_address == m_ctx->fb.layout.zeta_address;

				const u32 remaining = (units_len < sizeof(units)) ? static_cast<u32>(sizeof(units) - units_len) : 0u;

				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(units + units_len, remaining,
					" u%u=0x%x/%x/%ux%u%s", unit, guest_address, static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), is_target ? "/target" : "");

				if (written <= 0)
				{
					break;
				}

				units_len += static_cast<u32>(written);

				if (units_len >= sizeof(units) - 64)
				{
					break;
				}
			}

			// TODO(excise): level 3; the first draws of the run.
			MTL_NOTE(3, "Metal 4: draw %u, command %u, primitive %u, inline %u, color 0x%x/0x%x/0x%x/0x%x fmt 0x%x, depth 0x%x fmt 0x%x, %ux%u, aa_mode %u, scissor (%u,%u,%u,%u), samples:%s",
				diagnostic_draws, static_cast<u32>(draw_call.command), static_cast<u32>(draw_call.primitive),
				::size32(draw_call.inline_vertex_array),
				m_ctx->fb.layout.color_addresses[0], m_ctx->fb.layout.color_addresses[1],
				m_ctx->fb.layout.color_addresses[2], m_ctx->fb.layout.color_addresses[3],
				static_cast<u32>(m_ctx->fb.layout.color_format),
				m_ctx->fb.layout.zeta_address, static_cast<u32>(m_ctx->fb.layout.depth_format),
				static_cast<u32>(m_ctx->fb.layout.width), static_cast<u32>(m_ctx->fb.layout.height),
				static_cast<u32>(m_ctx->fb.layout.aa_mode),
				scissor_region.x1, scissor_region.y1, scissor_region.width(), scissor_region.height(),
				units_len ? units : " none");
		}
#endif // MTL_DEBUG_LOGS

		// TODO(excise): level 3; the texture barrier count of the first draw.
		if (diagnostic_draws == 0)
		{
			MTL_NOTE(3, "Metal 4: stats at draw %u, texture barriers %u",
				diagnostic_draws, frame.textures.diagnostic_texture_barriers);
		}

		// EXCISE-BEGIN draw-counter-advance
		diagnostic_draws++;
		diagnostic_current_draw = diagnostic_draws - 1;
		// EXCISE-END draw-counter-advance

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the RCA pack's per-draw census: every enabled
		// fragment unit of every draw is noted, so a failing frame always has its
		// upload/sample history.  The dump-tex arm writes the named guest address'
		// full region here as well, bypassing the dedupe keys and the budgets of the
		// sampled-image path (its own budget applies).
		if (probe::at_least(5))
		{
			const auto& fbl = m_ctx->fb.layout;

			for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
			{
				const auto& tex = m_ctx->register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());

				if (!guest_address)
				{
					continue;
				}

				if (guest_address == fbl.color_addresses[0] || guest_address == fbl.color_addresses[1] ||
					guest_address == fbl.color_addresses[2] || guest_address == fbl.color_addresses[3] ||
					guest_address == fbl.zeta_address)
				{
					// A unit sampling its own draw's target is the paint/reflection
					// identity-split class.
					probe::census_note_hazard(guest_address);
				}

				probe::census_note_sample(guest_address);

				if (probe::dump_tex_match(guest_address))
				{
					const u32 gcm_format = static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
					const u32 block = rsx::get_format_block_size_in_bytes(gcm_format);
					const u32 block_texels = rsx::get_format_block_size_in_texel(gcm_format);

					if (!block || !block_texels)
					{
						continue;
					}

					const u32 width_in_blocks = (static_cast<u32>(tex.width()) + block_texels - 1) / block_texels;
					const u32 height_in_blocks = (static_cast<u32>(tex.height()) + block_texels - 1) / block_texels;
					const u64 length = static_cast<u64>(width_in_blocks) * height_in_blocks * block;

					if (length && probe::dump_tex_take(length))
					{
						char path[512];
						std::snprintf(path, sizeof(path), "%s/dump-tex-draw%u-unit%u-addr%x-fmt%x-%ux%u.bin",
							probe::dump_directory(), diagnostic_current_draw, unit, guest_address, gcm_format,
							static_cast<u32>(tex.width()), static_cast<u32>(tex.height()));

						if (const auto* words = vm::get_super_ptr<u8>(guest_address))
						{
							if (std::FILE* out = std::fopen(path, "wb"))
							{
								std::fwrite(words, 1, length, out);
								std::fclose(out);
								MTL_NOTE(5, "Metal 4: dump-tex draw %u unit %u path %s, %llu bytes",
									diagnostic_current_draw, unit, path, static_cast<unsigned long long>(length));
							}
						}
					}
				}
			}
		}
#endif

		// TODO(excise): level 5; MTL_PROBE_ONLY_DRAW.  The named draws are encoded;
		// every other draw is counted and dropped, so the target after the run
		// holds exactly the named draws' contribution.  The draw id is the one the
		// 'draw <n>' lines and the per-draw records carry.
		if (const auto only_draw = diagnostic_only_draw(); only_draw.set &&
			(diagnostic_current_draw < only_draw.lo || diagnostic_current_draw > only_draw.hi))
		{
			return true;
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the texture units of the first draws, with a scan.
		if (diagnostic_draws < 24)
		{
			for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
			{
				const auto& tex = m_ctx->register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());
				u32 nonzero = 0;
				const auto* words_at = vm::get_super_ptr<u32>(guest_address);

				for (u32 word = 0; word < 1024; ++word)
				{
					nonzero += (static_cast<u32>(words_at[word]) != 0u);
				}

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: unit %u, address 0x%x, location %u, format 0x%x, %ux%u, nonzero %u/1024",
					unit, guest_address, static_cast<u32>(tex.location()), static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), nonzero);
			}
		}

		if (diagnostic_draws % 30 == 0)
		{
			static u32 diagnostic_samples[64] = {};
			static u32 diagnostic_sample_count = 0;

			for (u32 unit = 0; unit < frame.fragment_textures.size() && diagnostic_sample_count < 64; ++unit)
			{
				const auto& tex = m_ctx->register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());
				const u32 gcm_format = static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
				const u32 block = rsx::get_format_block_size_in_bytes(gcm_format);
				// A compressed format stores a 4x4 block in `block` bytes, so the
				// extent is in blocks and the byte span is block_count * block_size.
				// Multiplying the texel extent by the block size over-reads by 16x.
				const u32 block_texels = rsx::get_format_block_size_in_texel(gcm_format);
				const u32 width_in_blocks = (static_cast<u32>(tex.width()) + block_texels - 1) / block_texels;
				const u32 height_in_blocks = (static_cast<u32>(tex.height()) + block_texels - 1) / block_texels;
				const u32 length = width_in_blocks * height_in_blocks * block;

				if (!guest_address || !block || !length)
				{
					continue;
				}

				if (m_fs_sampler_descriptors[unit] && diagnostic_sampled_count <= 8)
				{
					auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_fs_sampler_descriptors[unit].get());

					if (descriptor && descriptor->image_handle)
					{
						u32 slot = diagnostic_sampled_count;

						for (u32 i = 0; i < diagnostic_sampled_count; ++i)
						{
							if (diagnostic_sampled_address[i] == guest_address)
							{
								slot = i;
								break;
							}
						}

						if (slot == diagnostic_sampled_count && diagnostic_sampled_count < 8)
						{
							diagnostic_sampled_count++;
						}

						if (slot < 8)
						{
							diagnostic_sampled_address[slot] = guest_address;
							diagnostic_sampled_image[slot] = descriptor->image_handle->value();
							diagnostic_sampled_context[slot] = static_cast<u32>(descriptor->upload_context);
						}
					}
				}

				bool seen = false;

				for (u32 i = 0; i < diagnostic_sample_count; ++i)
				{
					seen = seen || (diagnostic_samples[i] == guest_address);
				}

				if (seen)
				{
					continue;
				}

				diagnostic_samples[diagnostic_sample_count++] = guest_address;

				// The whole range is scanned, not just its head: a readable head says
				// nothing about the texels the shader actually reads.
				const u32 total_words = length / 4u;
				const u32 stride = std::max<u32>(1u, total_words / 8192u);
				u32 nonzero = 0;
				u32 scanned = 0;
				const auto* words_at = vm::get_super_ptr<u32>(guest_address);

				for (u32 word = 0; word < total_words; word += stride)
				{
					nonzero += (static_cast<u32>(words_at[word]) != 0u);
					scanned++;
				}

				char path[512];
				std::snprintf(path, sizeof(path), "%s/sample-%u-draw%u-addr%x-fmt%x-%ux%u.bin",
					probe::dump_directory(), diagnostic_sample_count, diagnostic_draws, guest_address, gcm_format,
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()));

				if (std::FILE* out = std::fopen(path, "wb"))
				{
					std::fwrite(vm::get_super_ptr<u8>(guest_address), 1, length, out);
					std::fclose(out);
				}

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: sample %u at draw %u, address 0x%x, format 0x%x, %ux%u, bytes %u, nonzero %u of %u scanned (stride %u)",
					diagnostic_sample_count, diagnostic_draws, guest_address, gcm_format,
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), length, nonzero, scanned, stride);

				// The region descriptor uses the effective pitch: a swizzled texture
				// has no guest pitch, and upload_texture substitutes the packed pitch
				// (texture_cache.h:3110-3127).  The width is expressed in block units
				// so the byte span is exact for a compressed format too.
				const bool swizzled = !(tex.format() & CELL_GCM_TEXTURE_LN);
				const u32 packed_pitch = rsx::get_format_packed_pitch(gcm_format,
					static_cast<u16>(tex.width()), !tex.border_type(), swizzled);
				const u32 effective_pitch = swizzled
					? packed_pitch
					: (tex.pitch() ? tex.pitch() : packed_pitch);

				const auto overlaps = frame.cache.get_merged_texture_memory_region(frame.recorder,
					guest_address, width_in_blocks, height_in_blocks,
					effective_pitch, static_cast<u8>(block), rsx::surface_access::shader_read);

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: store lookup for 0x%x, bound %u, overlaps %u",
					guest_address, frame.cache.address_is_bound(guest_address) ? 1u : 0u,
					static_cast<u32>(overlaps.size()));

				for (u32 slot = 0; slot < overlaps.size(); ++slot)
				{
					const auto& overlap = overlaps[slot];
					auto* surface = overlap.surface;

					if (!surface)
					{
						continue;
					}

					// The entry the search returned, with the state test that decides
					// whether the sampler is allowed to use it.
					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: overlap %u for 0x%x, base 0x%x, %ux%u pitch %u bpp %u, depth %u, clipped %u, reloaded %u, test %u, needs upload %u, write through %u, src %u,%u %ux%u, dst %u,%u %ux%u",
						slot, guest_address, overlap.base_address,
						static_cast<u32>(surface->width()), static_cast<u32>(surface->height()),
						surface->get_rsx_pitch(), surface->get_bpp(),
						overlap.is_depth ? 1u : 0u, overlap.is_clipped ? 1u : 0u, overlap.is_reloaded ? 1u : 0u,
						surface->test() ? 1u : 0u, surface->needs_cpu_upload() ? 1u : 0u, surface->write_through() ? 1u : 0u,
						static_cast<u32>(overlap.src_area.x), static_cast<u32>(overlap.src_area.y),
						static_cast<u32>(overlap.src_area.width), static_cast<u32>(overlap.src_area.height),
						static_cast<u32>(overlap.dst_area.x), static_cast<u32>(overlap.dst_area.y),
						static_cast<u32>(overlap.dst_area.width), static_cast<u32>(overlap.dst_area.height));
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		if (m_ctx->program.vertex_program.data.empty() || !m_ctx->program.fragment_program.ucode_length)
		{
			// TODO(excise): level 2; report the empty program once per run.
			if (!diagnostic_program_reported)
			{
				diagnostic_program_reported = true;
				rsx_log.error("Metal 4: the draw has no program (vertex bytes %u, fragment words %u)",
					static_cast<u32>(m_ctx->program.vertex_program.data.size()), m_ctx->program.fragment_program.ucode_length);
			}

			return false;
		}

		const surface_state surface = capture_surface_state(*m_ctx);
		pipeline_properties properties = capture_pipeline_properties(*m_ctx, surface, m_ctx->program.vertex_program);

		// TODO(excise): level 2; the classified draw state, one row per draw.
		MTL_PROBE_HISTOGRAM(2, "draw topology", static_cast<u32>(properties.input_primitive_topology), "topology");
		MTL_PROBE_HISTOGRAM(2, "draw sample count", properties.sample_count, "samples");
		MTL_PROBE_HISTOGRAM(2, "draw color format", static_cast<u32>(surface.color_format), "gcm color format");
		MTL_PROBE_HISTOGRAM(2, "draw depth format", static_cast<u32>(surface.depth_format), "gcm depth format");
		MTL_PROBE_HISTOGRAM(2, "draw write mask", static_cast<u32>(properties.write_mask[0]), "write mask 0");
		// TODO(excise): level 3; the programs of this draw.  A program changes more
		// often than a format does, so the lookup stays off the level-2 path, which
		// is the performance path.
		MTL_PROBE_HISTOGRAM(3, "draw vertex program", m_ctx->program.vp_metadata.ucode_hash, "vertex ucode hash");
		MTL_PROBE_HISTOGRAM(3, "draw fragment program", m_ctx->program.fp_metadata.ucode_hash, "fragment ucode hash");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; a content trigger opens the window before the
		// draw-state block below, so the matching draw itself is described.

		// TODO(excise): level 3; the draw-state block, for the first draws and the
		// capture frame.
		if (diagnostic_draws < 9 || diagnostic_capture_active())
		{
			dump_draw_state(*m_ctx, diagnostic_draws - 1, surface, properties, frame.attachments, m_ctx->fb.layout, scissor_region,
				frame.resources.supports_bc_texture_compression());
		}
#endif // MTL_DEBUG_LOGS

		// The shader mode chooses how a first-use miss is covered.  The interpreter
		// renders the clause through the shared body while the real pipeline links;
		// `interpreter_only` never builds a real program at all
		// (LLD-shader-interpreter.md §10).
		const shader_mode shadermode = g_cfg.video.shadermode.get();
		const bool interpreter_enabled = shadermode == shader_mode::async_with_interpreter ||
			shadermode == shader_mode::interpreter_only;
		const bool interpreter_only = shadermode == shader_mode::interpreter_only;

		interpreter_request interpreted_request;
		std::shared_ptr<const interpreter_pipeline> interpreted;

		if (interpreter_enabled &&
			interpreter_cache::build_request(*m_ctx, m_ctx->program.vertex_program, m_ctx->program.fragment_program,
				m_ctx->program.vp_metadata, m_ctx->program.fp_metadata, interpreted_request))
		{
			// The interpreter declares only the outputs it writes.  The real
			// pipeline still sees the unmodified mask, so the trim is a copy.
			pipeline_properties interpreter_properties = properties;

			for (u32 index = interpreted_request.output_count; index < interpreter_properties.write_mask.size(); ++index)
			{
				interpreter_properties.write_mask[index] = MTL::ColorWriteMaskNone;
			}

			interpreted = frame.interpreter.get(interpreted_request, interpreter_properties);

			// TODO(excise): level 2; the source is eligible but its pipeline is not
			// ready yet, so this draw takes the real-pipeline path or the skip.
			if (!interpreted)
			{
				MTL_PROBE_LATCH(2, "interpreter/pipeline not ready");
			}
		}

		const pipeline* program = nullptr;
		const vertex_program* compiled_vertex = nullptr;
		const fragment_program* compiled_fragment = nullptr;
		bool use_interpreter = false;

		if (interpreter_only && interpreted)
		{
			use_interpreter = true;
		}
		else
		{
			// A first-use miss yields once while the registry links.  The
			// interpreter covers that miss when it is available; a real build is
			// synchronous only for the two non-async modes and the interpreter-only
			// fallback (which means the interpreter rejected the program).
			const bool compile_async = !interpreter_only && shadermode != shader_mode::recompiler;

			const auto data = frame.programs.get_graphics_pipeline(
				&m_ctx->program.cache_hint,
				m_ctx->program.vertex_program,
				m_ctx->program.fragment_program,
				properties,
				compile_async,
				true,
				shader_context{ m_device->compiler(), m_device->pipeline_task_options(), &frame.jobs,
					msl_entry_point, msl_entry_point, true, &m_device->lost_flag() });

			program = std::get<0>(data);
			compiled_vertex = std::get<1>(data);
			compiled_fragment = std::get<2>(data);

			if (program && program->state)
			{
				// The real pipeline won.
			}
			else if (!interpreter_only && interpreted)
			{
				use_interpreter = true;
			}
			else
			{
				const bool failed = compiled_vertex && compiled_fragment &&
					frame.jobs.permanently_failed(compiled_vertex->id, compiled_fragment->id, properties);

				// TODO(excise): level 2; distinguish an in-flight link from a failed one.
				if (failed)
				{
					MTL_PROBE_LATCH(2, "draw/skipped pipeline failed");
				}
				else
				{
					MTL_PROBE_LATCH(2, "draw/skipped pipeline pending");
				}

				// TODO(excise): level 2; report the failed pipeline once per run.
				if (failed && !diagnostic_pipeline_reported)
				{
					diagnostic_pipeline_reported = true;
					rsx_log.error("Metal 4: the program cache returned no pipeline");
				}

				// The host miss contract yields once; the link is on Metal's compiler
				// threads.  Do not drain here: the next end()/begin() publishes.
				std::this_thread::yield();
				return false;
			}
		}

		// A transform-constant file is relocated against the compiled vertex
		// program.  Switching to the interpreter (or to a different compiled
		// program) invalidates that relocation, so the next upload is full.
		if (frame.interpreter_active != use_interpreter ||
			(!use_interpreter && frame.compiled_vertex != compiled_vertex))
		{
			frame.transform_constants_valid = false;
		}

		frame.interpreter_active = use_interpreter;

		// TODO(excise): level 2; which cover this draw used.  One latch per branch,
		// because a latch registers its name once per call site.
		if (use_interpreter)
		{
			MTL_PROBE_LATCH(2, "draw/interpreter");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/recompiler");
		}

		if (use_interpreter)
		{
			frame.compiled_vertex = nullptr;
			frame.compiled_fragment = nullptr;
			frame.interpreter_mapping = interpreted->mapping;
			frame.interpreter_output_count = interpreted->output_count;
			frame.interpreter_texture_count = 0;
			for (u32 unit = 0; unit < rsx::limits::fragment_textures_count; ++unit)
			{
				if (m_ctx->program.fp_metadata.referenced_textures_mask & (1u << unit))
				{
					frame.interpreter_texture_count = unit + 1;
				}
			}

			// The instruction blocks of this clause.  A block is 16 header bytes
			// followed by the guest ucode; the header carries the guest addresses
			// and the per-clause state the body switches on
			// (VKShaderInterpreter.cpp:2255-2293).
			byte_ring& ring = frame.constants.instruction_ring();

			const u64 vertex_key = static_cast<u64>(m_ctx->program.vp_metadata.ucode_hash) ^
				(static_cast<u64>(m_ctx->program.vertex_program.base_address) << 32) ^
				(static_cast<u64>(m_ctx->program.vertex_program.entry) << 16) ^
				static_cast<u64>(m_ctx->program.vertex_program.output_mask) ^
				(static_cast<u64>(m_ctx->program.vp_metadata.ucode_length) << 48);

			if (auto found = frame.interpreter_vertex_blocks.find(vertex_key); found != frame.interpreter_vertex_blocks.end())
			{
				frame.interpreter_vertex_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + m_ctx->program.vp_metadata.ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter vertex instruction ring exhausted");
					return false;
				}

				u32* header = static_cast<u32*>(block);
				header[0] = m_ctx->program.vertex_program.base_address;
				header[1] = m_ctx->program.vertex_program.entry;
				header[2] = m_ctx->program.vertex_program.output_mask;
				header[3] = m_ctx->register_state->two_side_light_en() ? 1u : 0u;
				std::memcpy(static_cast<std::byte*>(block) + 16, m_ctx->program.vertex_program.data.data(), m_ctx->program.vp_metadata.ucode_length);

				frame.interpreter_vertex_instructions = ring.address_of(offset);
				frame.interpreter_vertex_blocks.emplace(vertex_key, frame.interpreter_vertex_instructions);
			}

			const u64 fragment_key = static_cast<u64>(m_ctx->program.fp_metadata.ucode_hash) ^
				(static_cast<u64>(m_ctx->register_state->shader_control()) << 32) ^
				(static_cast<u64>(m_ctx->program.fragment_program.texture_state.texture_dimensions) << 48) ^
				(static_cast<u64>(m_ctx->program.fp_metadata.program_ucode_length) << 24);

			if (auto found = frame.interpreter_fragment_blocks.find(fragment_key); found != frame.interpreter_fragment_blocks.end())
			{
				frame.interpreter_fragment_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + m_ctx->program.fp_metadata.program_ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter fragment instruction ring exhausted");
					return false;
				}

				u32 resident = 0;

				for (u32 unit = 0; unit < 16; ++unit)
				{
					if ((m_ctx->program.fp_metadata.referenced_textures_mask & (1u << unit)) &&
						frame.fragment_textures[unit].texture._impl)
					{
						resident |= 1u << unit;
					}
				}

				u32* header = static_cast<u32*>(block);
				header[0] = m_ctx->register_state->shader_control();
				header[1] = m_ctx->program.fragment_program.texture_state.texture_dimensions;
				header[2] = resident;
				header[3] = 0;
				std::memcpy(static_cast<std::byte*>(block) + 16, m_ctx->program.fragment_program.get_data(), m_ctx->program.fp_metadata.program_ucode_length);

				frame.interpreter_fragment_instructions = ring.address_of(offset);
				frame.interpreter_fragment_blocks.emplace(fragment_key, frame.interpreter_fragment_instructions);
			}
		}
		else
		{
			frame.compiled_vertex = compiled_vertex;
			frame.compiled_fragment = compiled_fragment;
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the census row of this draw.  It needs the
		// compiled ids, which are only known after the pipeline resolves, and it
		// runs before the draw is encoded so its first draw is this one.
		diagnostic_census_draw(*m_ctx, diagnostic_current_draw,
			compiled_vertex ? compiled_vertex->id : ~0u, m_ctx->program.vp_metadata.ucode_hash,
			compiled_fragment ? compiled_fragment->id : ~0u, m_ctx->program.fp_metadata.ucode_hash,
			use_interpreter, m_ctx->fb.layout, surface);
#endif // MTL_DEBUG_LOGS

		// The emulated depth compare samples the depth surface, which this pass also
		// holds as its depth attachment.  A render pass cannot read its own
		// attachment, so the surface is copied to a mirror in its own encoder first
		// and the shader reads the mirror (REALISATION.md, "The depth/stencil-as-
		// texture cluster").
		frame.depth_input = {};
		if ((m_ctx->program.fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE) && frame.attachments.has_depth())
		{
			const MTL::ResourceID mirror = frame.attachments.depth()->mirror_depth(frame.recorder);
			MTL::SamplerState* sampler = frame.samplers.get(stencil_mirror_sampler());
			if (sampler && mirror._impl)
			{
				frame.depth_input = { mirror, sampler->gpuResourceID() };
			}
		}

		// C3: a draw cannot sample an attachment the open encoder is still rendering.
		// Metal has no feedback primitive and a stage barrier cannot make tile memory
		// samplable, so the encoder closes first.  The store plus the recorder's
		// boundary barrier then make the stored image legal input to the fresh pass
		// (H2).  The same-clause copy (L204) covers the case where the cache already
		// resolved the unit to a temporary: that copy closed the encoder itself.
		const auto samples_open_pass = [&]() -> bool
		{
			const auto conflicts = [&](MTL::ResourceID resource, const utils::address_range32& range)
			{
				for (u32 index = 0; index < frame.attachments.color_count(); ++index)
				{
					mtl::surface* color = frame.attachments.color(index);

					if (!color)
					{
						continue;
					}

					if (color->resource_id()._impl == resource._impl ||
						(range.valid() && range.overlaps(color->get_memory_range())))
					{
						return true;
					}
				}

				if (mtl::surface* depth = frame.attachments.depth())
				{
					if (depth->resource_id()._impl == resource._impl ||
						(range.valid() && range.overlaps(depth->get_memory_range())))
					{
						return true;
					}
				}

				return false;
			};

			for (u32 referenced = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.fragment_textures[i].texture._impl &&
					conflicts(frame.fragment_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i])))
				{
					return true;
				}
			}

			for (u32 referenced = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.vertex_textures[i].texture._impl &&
					conflicts(frame.vertex_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i])))
				{
					return true;
				}
			}

			return false;
		};

		// A copy or upload during texture resolution can have ended the encoder
		// since the top of this function; the pointer is only valid while the
		// recorder still owns it.  A copy, upload, readback, framebuffer change or a
		// re-entered slot closes the encoder behind this pointer, so the recorder is
		// the authority (C1).  The depth mirror above is one such close: it runs in
		// its own encoder.
		if (frame.encoder &&
			static_cast<MTL4::CommandEncoder*>(frame.encoder) != frame.recorder.current_encoder())
		{
			frame.encoder = nullptr;
		}

		draw_pass_policy policy;
		policy.encoder_open = frame.encoder != nullptr;
		policy.recorder_owns_encoder = policy.encoder_open;
		policy.pass_key_matches = policy.encoder_open && frame.attachments.can_absorb();
		policy.samples_attachment = policy.encoder_open && samples_open_pass();

		if (policy.samples_attachment)
		{
			MTL_PROBE_LATCH(1, "recorder/pass split feedback");
			frame.encoder = nullptr;
			policy.encoder_open = false;
			frame.recorder.close_encoder();
		}

		MTL4::RenderCommandEncoder* encoder = nullptr;

		if (policy.absorbs())
		{
			encoder = frame.encoder;
			MTL_PROBE_LATCH(1, "recorder/pass absorbed");
		}
		else
		{
			encoder = frame.attachments.open_pass(frame.recorder, 0, surface);

			if (!encoder)
			{
				// TODO(excise): level 2; report the failed pass once per run.
				if (!diagnostic_pass_reported)
				{
					diagnostic_pass_reported = true;
					rsx_log.error("Metal 4: the pass of the draw could not open");
				}

				// Returning false runs the NOP draw and the shared bookkeeping
				// (VKDraw.cpp:1283-1288).
				return false;
			}

			MTL_PROBE_LATCH(1, "recorder/pass opened");
			frame.encoder = encoder;

			// The hazard notes describe a pass, not a draw.  Within one encoder the
			// only writes are attachment writes, ordered by the hardware's defined
			// raster order, and a sampled attachment is copied before the pass opens.
			// So the notes are recorded once per pass, and the recorder's boundary
			// barrier orders the few pass transitions (C2/C6).
			record_pass_hazards(true);
		}

		// The mirror copy ran in the encoder that open_pass closed, so the pass reads
		// the mirror across a queue-scope edge.
		if (frame.depth_input.texture._impl)
		{
			emit_queue_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });
		}

		encoder->setRenderPipelineState(use_interpreter ? interpreted->state : program->state);

		const auto encoder_state = capture_encoder_state(*m_ctx, !!(m_ctx->program.fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE));
		MTL::ScissorRect scissor = build_scissor_rect(scissor_region.x1, scissor_region.y1, scissor_region.x1 + scissor_region.width(), scissor_region.y1 + scissor_region.height(), encoder_state.clip_height);
		apply_encoder_state(encoder, encoder_state, scissor, frame.depth_cache);

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the clause-level state the per-draw record needs.
		// The pipeline is set once here, and the raster/blend/depth latches are
		// cheap, so the draw path reads them from the frame rather than repeating
		// the lookups per subdraw.
		if (probe::at_least(5))
		{
			frame.current_pipeline_state = use_interpreter ? interpreted->state : program->state;
			frame.current_pipeline_key = static_cast<u32>(m_ctx->program.vp_metadata.ucode_hash ^ (m_ctx->program.fp_metadata.ucode_hash * 0x9e3779b1ull));
			frame.current_vp_id = frame.compiled_vertex ? frame.compiled_vertex->id : 0;
			frame.current_fp_id = frame.compiled_fragment ? frame.compiled_fragment->id : 0;

			const MTL::Viewport viewport = build_viewport(encoder_state.clip_width, encoder_state.clip_height,
				encoder_state.has_clip_range ? encoder_state.znear : 0.0,
				encoder_state.has_clip_range ? encoder_state.zfar : 1.0);

			frame.current_viewport[0] = static_cast<f32>(viewport.originX);
			frame.current_viewport[1] = static_cast<f32>(viewport.originY);
			frame.current_viewport[2] = static_cast<f32>(viewport.width);
			frame.current_viewport[3] = static_cast<f32>(viewport.height);
			frame.current_blend_hash = hash_blend_state(capture_blend_state(*m_ctx));
			frame.current_depth_hash = static_cast<u32>(hash_depth_stencil_state(capture_depth_stencil_state(m_ctx->draw_state.depth_stencil)));
			frame.current_raster_hash = hash_raster_state(capture_raster_state(*m_ctx));
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): diagnostic, MTL_PROBE_VARIANT=no-cull / MTL_PROBE_VARIANT=no-depth.  A
		// captured draw whose colour output is forced still writes nothing, and
		// the fragment program, the descriptor and the vertex stream are each
		// proven, so only a raster rejection is left.  Each switch removes one
		// candidate; neither touches a shader, so no archive name carries them.
		// They apply to the capture window only, because the probe is the reader.
		if (diagnostic_capture_active())
		{
			if (probe::variant("no-cull"))
			{
				encoder->setCullMode(MTL::CullModeNone);
			}

			if (probe::variant("no-depth"))
			{
				depth_stencil_state without_depth = encoder_state.depth_stencil;
				without_depth.depth_compare = rsx::comparison_function::always;
				without_depth.depth_write = false;
				apply_depth_stencil_state(encoder, frame.depth_cache, without_depth);
			}
		}
#endif // MTL_DEBUG_LOGS

		// TODO(excise): level 2; the coordinate audit.  The clip size against the
		// attachment, and the raster modes, decide whether geometry lands as the
		// title drew it (area A and area C of the design).
		MTL_PROBE_LATCH(2, encoder_state.clip_width == m_ctx->fb.layout.width &&
			encoder_state.clip_height == m_ctx->fb.layout.height
			? "audit/clip matches target" : "audit/clip mismatches target");
		MTL_PROBE_HISTOGRAM(2, "audit fill mode", static_cast<u32>(encoder_state.fill_mode), "triangle fill mode");
		MTL_PROBE_HISTOGRAM(2, "audit cull mode", static_cast<u32>(encoder_state.cull_mode), "cull mode");
		MTL_PROBE_HISTOGRAM(2, "audit winding", static_cast<u32>(encoder_state.winding), "winding");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the viewport audit of the first draws of the run
		// and of every draw of a capture window.  A captured draw that writes
		// nothing is judged by the viewport, the cull mode and the winding it
		// actually encoded, not by the register the state dump reports.
		static u32 diagnostic_viewports = 0;
		const bool diagnostic_viewport_due = diagnostic_viewports < 8 || diagnostic_capture_active();
		++diagnostic_viewports;

		if (diagnostic_viewport_due)
		{
			const MTL::Viewport viewport = build_viewport(encoder_state.clip_width, encoder_state.clip_height,
				encoder_state.has_clip_range ? encoder_state.znear : 0.0,
				encoder_state.has_clip_range ? encoder_state.zfar : 1.0);

			MTL_NOTE(3, "Metal 4: viewport %u, origin %.1f %.1f size %.1f %.1f z %.4f %.4f, scissor %u %u %ux%u, clip %ux%u, cull %u winding %u fill %u depthclip %u",
				diagnostic_viewports, viewport.originX, viewport.originY, viewport.width, viewport.height, viewport.znear, viewport.zfar,
				static_cast<u32>(scissor.x), static_cast<u32>(scissor.y), static_cast<u32>(scissor.width), static_cast<u32>(scissor.height),
				static_cast<u32>(encoder_state.clip_width), static_cast<u32>(encoder_state.clip_height),
				static_cast<u32>(encoder_state.cull_mode), static_cast<u32>(encoder_state.winding),
				static_cast<u32>(encoder_state.fill_mode), static_cast<u32>(encoder_state.depth_clip_mode));
		}
#endif // MTL_DEBUG_LOGS

		// The indexed-constant tables of a hardware-instanced draw.  The shared
		// helper replays the clause, so it runs before the subdraw loop
		// (RSXDrawCommands.cpp:743-751).
		if (m_ctx->program.vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS)
		{
			rsx::io_buffer lookup([&](usz size) -> std::tuple<void*, usz>
			{
				s32* block = frame.constants.alloc_instancing_lookup(static_cast<u32>(size / sizeof(s32)));
				frame.instancing_lookup = frame.constants.instancing_lookup_address();
				return { block, size };
			});

			rsx::io_buffer constants([&](usz size) -> std::tuple<void*, usz>
			{
				vec4_block* block = frame.constants.alloc_instancing_constants(static_cast<u32>(size / sizeof(vec4_block)));
				frame.instancing_constants = frame.constants.instancing_constants_address();
				return { block, size };
			});

			// A null program asks for a full re-upload, which is always correct
			// (RSXDrawCommands.cpp:752-754).  The compiled program supplies the
			// relocation table, so the instance tables stride like the shader
			// (RSXDrawCommands.cpp:755-756; VKGSRender.cpp:2167-2168).
			RSX(m_ctx)->GRAPH_frontend().fill_constants_instancing_buffer(lookup, constants, frame.compiled_vertex);
		}
		else
		{
			frame.instancing_lookup = 0;
			frame.instancing_constants = 0;
		}

		draw_call.begin();

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the overlay shortlist.  A draw of at most six
		// vertices whose scissor covers at least half the clip area is a full-screen
		// or panel-scale candidate, which is what an overlay/haze/composite looks
		// like.  The line prints the draw id, the programs, the blend mask, the
		// coverage and every referenced unit with its guest address and host image
		// id, which is the whole hunt for a screen-space draw in one grep.  Log
		// only; nothing about the frame changes.
		// This runs after draw_call.begin(): the clause is post-compile until
		// then (current_range_index == ~0u) and every range-relative query
		// ensures on that, which killed the run at the first draw whenever
		// MTL_PROBE>=3 (L198).
		if (probe::at_least(3))
		{
			const u32 elements = draw_call.get_elements_count();
			const u64 clip_width = std::max(1u, static_cast<u32>(m_ctx->fb.layout.width));
			const u64 clip_height = std::max(1u, static_cast<u32>(m_ctx->fb.layout.height));
			const u64 covered = static_cast<u64>(scissor_region.width()) * scissor_region.height() * 100u / (clip_width * clip_height);

			if (elements <= 6 && covered >= 50)
			{
				static u32 diagnostic_quads_logged = 0;

				if (diagnostic_quads_logged < 128)
				{
					++diagnostic_quads_logged;

					char units[512] = {};
					usz units_len = 0;

					for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
					{
						const auto& tex = m_ctx->register_state->fragment_textures[unit];

						if (!tex.enabled())
						{
							continue;
						}

						const u32 guest_address = rsx::get_address(m_ctx, tex.offset(), tex.location());
						const u64 image = frame.fragment_textures[unit].texture._impl;
						units_len += std::snprintf(units + units_len, units_len < std::size(units) ? std::size(units) - units_len : 0,
							" u%u=0x%x/%ux%u/id0x%llx", unit, guest_address,
							static_cast<u32>(tex.width()), static_cast<u32>(tex.height()),
							static_cast<unsigned long long>(image));

						if (units_len >= std::size(units))
						{
							units_len = std::size(units) - 1;
							break;
						}
					}

					MTL_NOTE(3, "Metal 4: quad %u, draw %u, fp 0x%llx, vp 0x%llx, primitive %u, count %u, blend 0x%02x, coverage %llu%%, scissor (%u,%u,%u,%u), units:%s",
						diagnostic_quads_logged, diagnostic_current_draw,
						static_cast<unsigned long long>(m_ctx->program.fp_metadata.ucode_hash),
						static_cast<unsigned long long>(m_ctx->program.vp_metadata.ucode_hash),
						static_cast<u32>(draw_call.primitive), elements, properties.blend.enable_mask, covered,
						static_cast<u32>(scissor_region.x1), static_cast<u32>(scissor_region.y1),
						static_cast<u32>(scissor_region.width()), static_cast<u32>(scissor_region.height()),
						units_len ? units : " none");
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		u32 sub_index = 0;
		do
		{
			emit_geometry(sub_index++);

			if (draw_call.is_trivial_instanced_draw && m_config.supports_hw_instanced_rendering)
			{
				draw_call.end();
			}
		}
		while (draw_call.next());

		// The post-draw instruments (tape, pixel probes, the first-draw dumps) read
		// the attachment after the draw, so they need the pass closed.  They are the
		// only post-draw reason to end a pass: every other draw leaves the encoder
		// open for the next one, which is where the pass count collapses (C1).
#if MTL_DEBUG_LOGS
		const bool close_for_diagnostics = probe::tape_depth()
			|| diagnostic_capture_active()
			|| diagnostic_draws < 24;
#else
		constexpr bool close_for_diagnostics = false;
#endif

		if (close_for_diagnostics)
		{
			frame.encoder = nullptr;
			frame.recorder.close_encoder();
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the tape of the images this draw sampled from the
		// bound target.  The bind recorded them and the draw's encoder is closed, so
		// the images are stable and the copy is queued without a readback.
		if (probe::tape_depth())
		{
			for (u32 binding = 0; binding < diagnostic_feedback_binding_count; ++binding)
			{
				const auto& entry = diagnostic_feedback_bindings[binding];
				diagnostic_tape_copy(frame.recorder, frame.resources, entry.image,
					entry.address, entry.draw, entry.unit, diagnostic_frame_index,
					binding + 1, false, entry.copy);
			}
		}

		// TODO(excise): level 5; the frame pixels a capture draw left behind.  The
		// patch is read after the draw, so the alpha byte says whether the draw
		// wrote the patch at all, and a patch that was already the same colour says
		// a later draw owns it.
		if (diagnostic_capture_active() && diagnostic_pixel_probes < 400000 && diagnostic_window_probes < diagnostic_window_probe_limit)
		{
			if (!diagnostic_pixel_rects_read)
			{
				diagnostic_read_pixel_rects();
			}

			if (diagnostic_pixel_rect_count)
			{
				if (auto* target = frame.attachments.color(0))
				{
					// TODO(excise): level 5; the identity of the texture the probe
					// reads.  A patch that stays at the clear value is only evidence
					// if the texture is the one the draw wrote and the one the frame
					// presents.
					if (diagnostic_window_probes == 0)
					{
						MTL_NOTE(5, "Metal 4: pixel probe target frame %llu, address 0x%x, %ux%u, format %u, pass colour attachments %u",
							static_cast<unsigned long long>(diagnostic_frame_index), target->base_addr,
							target->width(), target->height(),
							static_cast<u32>(target->pixel_format()),
							frame.attachments.color_count());
					}

					++diagnostic_pixel_probes;
					++diagnostic_window_probes;

					const u32 probe_serial = diagnostic_draws - 1;
					const u64 probe_fp = static_cast<unsigned long long>(m_ctx->program.fp_metadata.ucode_hash);
					const u64 probe_vp = static_cast<unsigned long long>(m_ctx->program.vp_metadata.ucode_hash);
					diagnostic_patch_observation observations[8] = {};
					u32 observation_count = 0;

					probe_frame_pixels(frame.recorder, frame.resources, target->value(), probe_serial, probe_fp, probe_vp,
						observations, &observation_count);

					// TODO(excise): level 5; the draw that introduces the defect.
					// A sudden rise in the patch's pure-black count is the
					// attribution, and the target plus every sampled image are
					// dumped at that draw, so the black is either in an input or
					// the shader produced it.  The previous reading is keyed by
					// the target address, because one frame reads the patch from
					// several targets and a pass change is not an increase.
					for (u32 observation_index = 0; observation_index < observation_count; ++observation_index)
					{
						const auto& observation = observations[observation_index];
						const u32 rect_index = observation.rect;

						if (rect_index >= std::size(diagnostic_patch_previous_black))
						{
							continue;
						}

						diagnostic_black_count += observation.black;

						const u32 previous = diagnostic_patch_previous_address[rect_index] == target->base_addr
							? diagnostic_patch_previous_black[rect_index] : observation.black;

						diagnostic_patch_previous_black[rect_index] = observation.black;
						diagnostic_patch_previous_address[rect_index] = target->base_addr;

						const bool jump = observation.black >= 64 && observation.black >= previous + 64;

						if (!jump || diagnostic_black_patch_dumps >= diagnostic_black_patch_dump_limit)
						{
							continue;
						}

						++diagnostic_black_patch_dumps;

						MTL_NOTE(2, "Metal 4: black patch draw %u rect %u %u,%u %ux%u, fp 0x%llx vp 0x%llx, black %u -> %u of %u, opaque %u, target 0x%x %ux%u fmt %u",
							probe_serial, rect_index, observation.x, observation.y,
							observation.width, observation.height, probe_fp, probe_vp,
							previous, observation.black, observation.pixels, observation.opaque,
							target->base_addr, target->width(), target->height(),
							static_cast<u32>(target->pixel_format()));

						// The target as the draw left it.
						dump_host_image(frame.recorder, frame.resources, target->value(), "black-target",
							probe_serial, target->base_addr);

						// Every image the draw sampled.  A black region in one of
						// these is the source; a clean set means the shader made it.
						u32 dumped = 0;

						for (u32 unit = 0; unit < m_fs_sampler_descriptors.size() && dumped < diagnostic_black_patch_image_limit; ++unit)
						{
							auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_fs_sampler_descriptors[unit].get());

							if (!descriptor || !descriptor->image_handle)
							{
								continue;
							}

							MTL::Texture* sampled = descriptor->image_handle->value();

							if (!sampled)
							{
								continue;
							}

							dump_host_image(frame.recorder, frame.resources, sampled, "black-sample",
								probe_serial * 16u + unit, static_cast<u32>(descriptor->ref_address));
							++dumped;
						}
					}
				}
			}
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the target and the resolve readback of the first
		// draws, and a few samples of the capture frame.
		if (diagnostic_draws < 24 || (diagnostic_capture_active() && (diagnostic_draws % 200 == 0)))
		{
			if (auto* target = frame.attachments.color(0))
			{
				dump_host_image(frame.recorder, frame.resources, target->value(), "target", diagnostic_draws, target->base_addr);

				const u32 guest_address = target->base_addr;
				u32 nonzero = 0;
				const auto* words_at = vm::get_super_ptr<u32>(guest_address);

				for (u32 word = 0; word < 1024; ++word)
				{
					nonzero += (static_cast<u32>(words_at[word]) != 0u);
				}

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: target %u guest bytes, address 0x%x, %ux%u, nonzero %u/1024",
					diagnostic_draws, guest_address, static_cast<u32>(target->value()->width()), static_cast<u32>(target->value()->height()), nonzero);

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: target %u, address 0x%x, resolve surface %s",
					diagnostic_draws, guest_address, target->resolve_surface ? "present" : "absent");

				if (target->resolve_surface)
				{
					dump_host_image(frame.recorder, frame.resources, target->resolve_surface->value(), "resolve", diagnostic_draws, guest_address);
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		// The draw wrote the attachments, so the surfaces now hold the newest pixels
		// for their guest ranges.  A later sample resolves to them only if the store
		// says so: its write tag is what wins the tie against a cached copy
		// (texture_cache.h:2680, VKDraw.cpp:1424).
		frame.cache.on_write(m_ctx->fb.layout.color_write_enabled, m_ctx->fb.layout.zeta_write_enabled);

		// A colour attachment resolves when its pass stores, so the resolved image
		// already holds this draw.  on_write() sets require_resolve for the next
		// consumer, but that consumer would resolve the same image a second time.
		// The depth attachment is different: its resolve is the explicit pass in
		// surface::resolve(), so it keeps the flag.
		for (u32 index = 0; index < frame.attachments.color_count(); ++index)
		{
			if (auto* color = frame.attachments.color(index); color && color->resolve_surface)
			{
				color->msaa_flags &= ~rsx::surface_state_flags::require_resolve;
			}
		}

		return true;
	}

	void MTLGSRender::emit_geometry(u32 sub_index)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "draw");
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "draw/subdraws", 1);
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;
		MTL4::RenderCommandEncoder* encoder = frame.encoder;
		auto& draw_call = m_ctx->register_state->current_draw_clause;

		// The vertex state of this subdraw (GLDraw.cpp:730-760).
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: (draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask);

#if MTL_DEBUG_LOGS
		// The geometry stream of the run.  Every trace below is a bounded prefix of
		// it and every deep dump is bounded to it (anti-flood rule).
		static u32 diagnostic_geometry = 0;
		static u32 diagnostic_traces = 0;
		const bool diagnostic_trace = (diagnostic_traces < 20) || ((diagnostic_traces % 200) == 0) || diagnostic_capture_active();
		diagnostic_traces++;

		if (diagnostic_geometry < 20)
		{
			MTL_NOTE(3, "Metal 4: geometry %u, subdraw %u, first %u, count %u",
				diagnostic_geometry, sub_index, draw_call.min_index(), draw_call.get_elements_count());
		}

		diagnostic_geometry++;
#endif // MTL_DEBUG_LOGS

		if (vertex_state & rsx::vertex_arrays_changed)
		{
			RSX(m_ctx)->GRAPH_frontend().analyse_inputs_interleaved(frame.vertex_layout, m_ctx->program.vp_metadata);
		}
		else if (vertex_state & rsx::vertex_base_changed)
		{
			for (auto& info : frame.vertex_layout.interleaved_blocks)
			{
				info->vertex_range.second = 0;
				const u32 vertex_base_offset = m_ctx->register_state->vertex_data_base_offset();
				info->real_offset_address = rsx::get_address(m_ctx, rsx::get_vertex_offset_from_base(vertex_base_offset, info->base_offset), info->memory_location);
			}
		}
		else
		{
			for (auto& info : frame.vertex_layout.interleaved_blocks)
			{
				info->vertex_range.second = 0;
			}
		}

		if (vertex_state && !frame.vertex_layout.validate())
		{
			// No vertex inputs are enabled.  Nothing reaches the GPU, but the
			// clause's remaining pipeline dependencies still have to run
			// (VKDraw.cpp:1026-1034).
			do
			{
				draw_call.execute_pipeline_dependencies(m_ctx);
			}
			while (draw_call.next());

			draw_call.end();
			return;
		}

		// The index array and the vertex range of this subdraw
		// (GLVertexBuffers.cpp:66-150).
		const vertex_input_state input = std::visit(
			draw_command_visitor{ *m_ctx, frame.constants.index_ring(), frame.vertex_layout },
			RSX(m_ctx)->GRAPH_frontend().get_draw_command(*m_ctx->register_state));

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "draw/vertices", input.vertex_draw_count);

		if (input.vertex_draw_count == 0)
		{
			return;
		}

		// The vertex upload.  The stream rings are bound once per frame, so only
		// the ring offsets travel per draw (HLD-05 §3.5).
		const u32 vertex_count = (input.max_index - input.min_index) + 1;
		u32 vertex_base = input.min_index;
		if (input.index_rebase)
		{
			vertex_base = rsx::get_index_from_base(vertex_base, m_ctx->register_state->vertex_data_base_index());
		}

		const auto required = RSX(m_ctx)->GRAPH_frontend().calculate_memory_requirements(frame.vertex_layout, vertex_base, vertex_count);
		u32 persistent_base = 0;
		u32 volatile_base = 0;
		void* persistent_data = nullptr;
		void* volatile_data = nullptr;

		if (required.first)
		{
			persistent_data = frame.constants.persistent_stream().alloc(required.first, persistent_base);
			if (!persistent_data)
			{
				// TODO(excise): level 2; a draw the vertex ring could not hold is
				// dropped silently, which is one way a quad disappears.
				MTL_PROBE_LATCH(2, "draw/skipped ring exhausted persistent");
				return;
			}
		}

		if (required.second)
		{
			volatile_data = frame.constants.volatile_stream().alloc(required.second, volatile_base);
			if (!volatile_data)
			{
				// TODO(excise): level 2; see the persistent site.
				MTL_PROBE_LATCH(2, "draw/skipped ring exhausted volatile");
				return;
			}
		}

		m_ctx->stats.setup_time += m_profiler.duration();

		if (persistent_data || volatile_data)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "draw/vertex upload");
			RSX(m_ctx)->GRAPH_frontend().write_vertex_data_to_memory(frame.vertex_layout, vertex_base, vertex_count, persistent_data, volatile_data);
		}

		m_ctx->stats.vertex_upload_time += m_profiler.duration();

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the vertex upload of the first subdraws.
		{
			static u32 diagnostic_uploads = 0;

			if (diagnostic_uploads < 12)
			{
				const f32* stream = (persistent_data && required.first >= 24u) ? static_cast<const f32*>(persistent_data) : nullptr;

				MTL_NOTE(5, "Metal 4: upload %u, persistent %llu bytes base %u, volatile %llu bytes base %u, first %.6f %.6f %.6f %.6f %.6f %.6f",
					diagnostic_uploads,
					static_cast<unsigned long long>(required.first), persistent_base,
					static_cast<unsigned long long>(required.second), volatile_base,
					stream ? static_cast<f64>(stream[0]) : 0.0, stream ? static_cast<f64>(stream[1]) : 0.0,
					stream ? static_cast<f64>(stream[2]) : 0.0, stream ? static_cast<f64>(stream[3]) : 0.0,
					stream ? static_cast<f64>(stream[4]) : 0.0, stream ? static_cast<f64>(stream[5]) : 0.0);
			}

			diagnostic_uploads++;
		}

		// TODO(excise): level 5; the vertex stream of a captured subdraw.  A menu
		// glyph is one quad, so the UVs and the colour of the first vertices are
		// what separate a missing glyph from a quad that samples the wrong one.
		if (diagnostic_capture_active() && diagnostic_capture_vertex_dumps < 16)
		{
			++diagnostic_capture_vertex_dumps;

			char strides[160] = {};
			u32 strides_len = 0;
			u32 block_index = 0;

			for (const auto& info : frame.vertex_layout.interleaved_blocks)
			{
				const u32 remaining = (strides_len < sizeof(strides)) ? static_cast<u32>(sizeof(strides) - strides_len) : 0u;
				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(strides + strides_len, remaining, " b%u=%u",
					block_index++, static_cast<u32>(info->attribute_stride));
				if (written <= 0)
				{
					break;
				}

				strides_len += static_cast<u32>(written);
			}

			const f32* persistent = (persistent_data && required.first >= sizeof(f32))
				? static_cast<const f32*>(persistent_data) : nullptr;
			const f32* volatile_stream = (volatile_data && required.second >= sizeof(f32))
				? static_cast<const f32*>(volatile_data) : nullptr;
			const u32 persistent_floats = persistent ? std::min<u32>(static_cast<u32>(required.first / sizeof(f32)), 16u) : 0u;
			const u32 volatile_floats = volatile_stream ? std::min<u32>(static_cast<u32>(required.second / sizeof(f32)), 8u) : 0u;

			char values[512];
			usz values_len = std::snprintf(values, sizeof(values), "Metal 4: capture vertices %u, draw %u, subdraw %u, base %u, count %u, persistent %llu, volatile %llu, strides:%s, first",
				diagnostic_capture_vertex_dumps - 1, diagnostic_current_draw, sub_index, vertex_base, vertex_count,
				static_cast<unsigned long long>(required.first), static_cast<unsigned long long>(required.second),
				strides_len ? strides : " none");

			for (u32 index = 0; index < persistent_floats && values_len < sizeof(values); ++index)
			{
				values_len += std::snprintf(values + values_len, sizeof(values) - values_len, " %.3f",
					static_cast<f64>(persistent[index]));
			}

			for (u32 index = 0; index < volatile_floats && values_len < sizeof(values); ++index)
			{
				values_len += std::snprintf(values + values_len, sizeof(values) - values_len, " | %.3f",
					static_cast<f64>(volatile_stream[index]));
			}

			MTL_NOTE(5, "%s", values);
		}

		// TODO(excise): level 5; MTL_PROBE_VARIANT=dump-draw=<id>.  The isolated draw's raw streams dump, bypassing the capture window and the vertex-ring budgets (its own 16 MB per run budget applies).
		if (probe::dump_draw_match(diagnostic_current_draw))
		{
			char path[512];

			if (persistent_data && required.first && probe::dump_draw_take(required.first))
			{
				std::snprintf(path, sizeof(path), "%s/dump-draw-%llu-persistent.bin",
					probe::dump_directory(), static_cast<unsigned long long>(diagnostic_current_draw));

				if (std::FILE* out = std::fopen(path, "wb"))
				{
					std::fwrite(persistent_data, 1, required.first, out);
					std::fclose(out);
					MTL_NOTE(5, "Metal 4: dump-draw persistent %s, %llu bytes", path,
						static_cast<unsigned long long>(required.first));
				}
			}

			if (volatile_data && required.second && probe::dump_draw_take(required.second))
			{
				std::snprintf(path, sizeof(path), "%s/dump-draw-%llu-volatile.bin",
					probe::dump_directory(), static_cast<unsigned long long>(diagnostic_current_draw));

				if (std::FILE* out = std::fopen(path, "wb"))
				{
					std::fwrite(volatile_data, 1, required.second, out);
					std::fclose(out);
					MTL_NOTE(5, "Metal 4: dump-draw volatile %s, %llu bytes", path,
						static_cast<unsigned long long>(required.second));
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		// The ABI blocks of this subdraw (HLD-05 §3.5).  The rings are per frame
		// slot, so a block is written, never read back.
		vertex_context* context = frame.constants.alloc_vertex_context();
		draw_parameters* params = frame.constants.alloc_draw_parameters();
		fragment_context* fragment = frame.constants.alloc_fragment_context();
		sampler_info* sampler = frame.constants.alloc_sampler_info();
		vec4_block* stipple = frame.constants.alloc_stipple();
		u32* vs_params = frame.constants.alloc_vertex_draw_params();

		if (!context || !params || !fragment || !sampler || !stipple || !vs_params)
		{
			return;
		}

		RSX(m_ctx)->GRAPH_frontend().fill_scale_offset_data(context->scale_offset_mat, false);
		RSX(m_ctx)->GRAPH_frontend().fill_user_clip_data(&context->user_clip_configuration_bits);
		context->transform_branch_bits = m_ctx->register_state->transform_branch_bits();
		context->point_size = m_ctx->register_state->point_size() * RSX(m_ctx)->resolution_scaling_config.scale_factor();
		context->z_near = m_ctx->register_state->clip_min();
		context->z_far = m_ctx->register_state->clip_max();

		RSX(m_ctx)->GRAPH_frontend().fill_fragment_state_buffer(fragment, m_ctx->program.fragment_program);
		m_ctx->program.fragment_program.texture_params.write_to(sampler, m_ctx->program.fp_metadata.referenced_textures_mask);
		std::memcpy(stipple, m_ctx->register_state->polygon_stipple_pattern(), stipple_block_count * sizeof(vec4_block));

		// The transform constants.  The full 468-vec4 file is re-uploaded only when
		// the shared state flag says the file changed
		// (Core/RSXDriverState.h:15).  The shader reads the compacted file, so the
		// upload applies the program's relocation table (GLGSRender.cpp:1159-1171).
		if (!frame.transform_constants_valid || m_ctx->draw_state.dirty.test(rsx::draw_state_t::group::transform_constants))
		{
			vec4_block* block = frame.constants.alloc_transform_constants();
			if (!block)
			{
				return;
			}

			const std::span<const u16> reloc_table = (frame.compiled_vertex && !frame.compiled_vertex->has_indexed_constants)
				? std::span<const u16>(frame.compiled_vertex->constant_ids)
				: std::span<const u16>{};

			RSX(m_ctx)->GRAPH_frontend().fill_vertex_program_constants_data(block, reloc_table);
			frame.transform_constants_index = frame.constants.transform_constants_index();
			frame.transform_constants_valid = true;
			m_ctx->draw_state.dirty.clear(rsx::draw_state_t::group::transform_constants);

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the transform constants of a bounded prefix.
			if (diagnostic_trace)
			{
				MTL_NOTE(5, "Metal 4: transform constants, index %u, reloc %u, vp base 0x%x, [0] %.4f %.4f %.4f %.4f [1] %.4f %.4f %.4f %.4f [2] %.4f %.4f %.4f %.4f [3] %.4f %.4f %.4f %.4f",
					frame.transform_constants_index, static_cast<u32>(reloc_table.size()), m_ctx->program.vertex_program.base_address,
					static_cast<f64>(block[0].v[0]), static_cast<f64>(block[0].v[1]), static_cast<f64>(block[0].v[2]), static_cast<f64>(block[0].v[3]),
					static_cast<f64>(block[1].v[0]), static_cast<f64>(block[1].v[1]), static_cast<f64>(block[1].v[2]), static_cast<f64>(block[1].v[3]),
					static_cast<f64>(block[2].v[0]), static_cast<f64>(block[2].v[1]), static_cast<f64>(block[2].v[2]), static_cast<f64>(block[2].v[3]),
					static_cast<f64>(block[3].v[0]), static_cast<f64>(block[3].v[1]), static_cast<f64>(block[3].v[2]), static_cast<f64>(block[3].v[3]));
			}
#endif // MTL_DEBUG_LOGS
		}

		params->vertex_base_index = input.index_rebase ? input.min_index : 0;
		params->vertex_index_offset = input.vertex_index_offset;
		params->draw_id = 0;
		params->xform_constants_offset = frame.transform_constants_index;
		params->vs_context_offset = frame.constants.vertex_context_index();
		params->fs_context_offset = frame.constants.fragment_context_index();
		params->fs_texture_base_index = frame.constants.sampler_info_index();
		params->fs_stipple_pattern_offset = frame.constants.stipple_index();
		params->reserved = 0;

		// The fragment literals, when the program has any
		// (ProgramStateCache.h:141).  The relocation table is the compiled
		// program's, not the RSX program's.
		params->fs_constants_offset = 0;
		const usz fragment_constant_count = frame.compiled_fragment ? frame.compiled_fragment->constant_offsets.size() : 0;
		if (fragment_constant_count)
		{
			vec4_block* constants = frame.constants.alloc_fragment_constants(static_cast<u32>(fragment_constant_count));
			if (!constants)
			{
				return;
			}

			rsx::write_fragment_constants_to_buffer(
				{ reinterpret_cast<f32*>(constants), fragment_constant_count * 4 },
				m_ctx->program.fragment_program,
				frame.compiled_fragment->constant_offsets,
				true);

			params->fs_constants_offset = frame.constants.fragment_constants_index();

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the fragment constants of a bounded prefix.
			if (diagnostic_trace)
			{
				const auto& offsets = frame.compiled_fragment->constant_offsets;
				const auto offset_at = [&](usz index) { return (index < offsets.size()) ? offsets[index] : 0u; };
				const auto value_at = [&](usz index, usz channel) -> f64 {
					return (index < fragment_constant_count) ? static_cast<f64>(constants[index].v[channel]) : 0.0;
				};

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: fragment constants, count %u, index %u, fp id %u, ucode offsets %u %u %u, values [0] %.4f %.4f %.4f %.4f [1] %.4f %.4f %.4f %.4f [2] %.4f %.4f %.4f %.4f",
					static_cast<u32>(fragment_constant_count), params->fs_constants_offset,
					frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
					offset_at(0), offset_at(1), offset_at(2),
					value_at(0, 0), value_at(0, 1), value_at(0, 2), value_at(0, 3),
					value_at(1, 0), value_at(1, 1), value_at(1, 2), value_at(1, 3),
					value_at(2, 0), value_at(2, 1), value_at(2, 2), value_at(2, 3));

				// TODO(excise): level 5; the car's lighting terms are not in [0]..[2]
				// (fp 53 has 59 constants), so a capture window writes the whole file.
				// One block per distinct constant file, because a 59-constant program
				// on every draw would spend the site's line cap before the race
				// window opens.
				if (diagnostic_capture_active())
				{
					static constexpr u32 block_capacity = 512;
					static u64 block_hashes[block_capacity] = {};
					static u32 block_count = 0;

					const u64 block_hash = probe::fnv1a(constants,
						static_cast<usz>(fragment_constant_count) * sizeof(vec4_block));
					bool seen = false;

					for (u32 i = 0; i < block_count; ++i)
					{
						if (block_hashes[i] == block_hash)
						{
							seen = true;
							break;
						}
					}

					if (!seen && block_count < block_capacity)
					{
						block_hashes[block_count++] = block_hash;

						for (usz row = 0; row < fragment_constant_count; row += 4)
						{
							char text[512];
							usz length = std::snprintf(text, sizeof(text), "Metal 4: fragment constants fp %u [%u]",
								frame.compiled_fragment ? frame.compiled_fragment->id : 0u, static_cast<u32>(row));

							for (usz index = row; index < std::min(row + 4, fragment_constant_count); ++index)
							{
								length += std::snprintf(text + length, sizeof(text) - length, " %.4f %.4f %.4f %.4f",
									value_at(index, 0), value_at(index, 1), value_at(index, 2), value_at(index, 3));
							}

							MTL_NOTE(5, "%s", text);
						}
					}
				}
			}
#endif // MTL_DEBUG_LOGS
		}

		RSX(m_ctx)->GRAPH_frontend().fill_vertex_layout_state(
			frame.vertex_layout,
			m_ctx->program.vp_metadata,
			vertex_base,
			vertex_count,
			&params->attrib_data[0][0],
			persistent_base,
			volatile_base);

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; one subdraw as the GPU received it.  The record is
		// written after the descriptor is filled and before the draw is encoded, so
		// it names the exact bytes and state the command buffer captures.  A
		// reconstruction at flip time cannot do this: the ring may grow and the
		// pipeline may be released before the flip.
		if (probe::at_least(5))
		{
			const areau scissor_region = scaled_scissor(m_ctx->draw_state.scissor, m_ctx->fb.layout,
				RSX(m_ctx)->resolution_scaling_config);
			draw_record record;
			record.global_id = ++diagnostic_global_draw_id;
			record.frame_id = static_cast<u32>(diagnostic_frame_index);

			// The ABI field is unread by every shader, so the record's id can
			// travel in the block and a dumped draw-parameter block joins its
			// record without a second index.
			params->draw_id = record.global_id;
			record.subdraw = sub_index;
			record.command = static_cast<u32>(draw_call.command);
			record.primitive = static_cast<u32>(draw_call.primitive);
			record.flags = (input.has_index_buffer ? 1u : 0u) | (input.index_rebase ? 2u : 0u) |
				(frame.interpreter_active ? 4u : 0u) | (required.second ? 8u : 0u);
			record.index_type = static_cast<u32>(input.index_type);
			record.index_count = input.vertex_draw_count;
			record.index_offset = input.index_offset;
			record.index_length = input.index_length;
			record.vertex_count = vertex_count;
			record.vertex_base = vertex_base;
			record.vertex_index_offset = input.vertex_index_offset;
			record.vertex_base_index = params->vertex_base_index;
			record.min_index = input.min_index;
			record.max_index = input.max_index;
			record.persistent_base = persistent_base;
			record.volatile_base = volatile_base;
			record.persistent_bytes = static_cast<u32>(required.first);
			record.volatile_bytes = static_cast<u32>(required.second);
			record.vp_id = frame.current_vp_id;
			record.fp_id = frame.current_fp_id;
			record.pipeline_key = frame.current_pipeline_key;
			record.pipeline_state = reinterpret_cast<u64>(frame.current_pipeline_state);
			record.vertex_desc_hash = static_cast<u32>(probe::fnv1a(params->attrib_data, sizeof(params->attrib_data)));
			record.xform_offset = params->xform_constants_offset;
			record.vs_context_offset = params->vs_context_offset;
			record.fs_context_offset = params->fs_context_offset;
			record.fs_constants_offset = params->fs_constants_offset;
			record.fs_texture_base_index = params->fs_texture_base_index;
			record.fs_stipple_offset = params->fs_stipple_pattern_offset;
			record.blend_values_index = (m_ctx->program.fragment_program.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
				? frame.constants.blend_values_index() : 0;

			for (u32 target = 0; target < 4; ++target)
			{
				record.color_target[target] = m_ctx->fb.layout.color_addresses[target];
			}

			record.color_format = static_cast<u32>(m_ctx->fb.layout.color_format);
			record.depth_target = m_ctx->fb.layout.zeta_address;
			record.depth_format = static_cast<u32>(m_ctx->fb.layout.depth_format);
			record.width = m_ctx->fb.layout.width;
			record.height = m_ctx->fb.layout.height;
			record.scissor[0] = scissor_region.x1;
			record.scissor[1] = scissor_region.y1;
			record.scissor[2] = scissor_region.width();
			record.scissor[3] = scissor_region.height();

			for (u32 axis = 0; axis < 4; ++axis)
			{
				record.viewport[axis] = frame.current_viewport[axis];
			}

			record.blend_hash = frame.current_blend_hash;
			record.depth_hash = frame.current_depth_hash;
			record.raster_hash = frame.current_raster_hash;

			for (u32 unit = 0; unit < 16; ++unit)
			{
				record.attrib_desc[unit] = params->attrib_data[unit][0];
				record.attrib_offset[unit] = params->attrib_data[unit][1];

				const auto& tex = m_ctx->register_state->fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				record.tex_addr[unit] = rsx::get_address(m_ctx, tex.offset(), tex.location());
				record.tex_format[unit] = static_cast<u32>(tex.format());
				record.tex_width[unit] = tex.width();
				record.tex_height[unit] = tex.height();
				record.tex_sampler_hash[unit] = static_cast<u32>(
					probe::fnv1a(&frame.fragment_textures[unit].sampler, sizeof(frame.fragment_textures[unit].sampler)));

				// The sampler registers, packed so a difference is visible in the
				// manifest: a smear is a filter or LOD defect and the identity of
				// the cached object cannot say which.
				record.tex_sampler_flags[unit] =
					(static_cast<u32>(tex.min_filter()) & 0x7) |
					((static_cast<u32>(tex.mag_filter()) & 0x3) << 3) |
					((static_cast<u32>(tex.wrap_s()) & 0x7) << 5) |
					((static_cast<u32>(tex.wrap_t()) & 0x7) << 8) |
					((static_cast<u32>(tex.wrap_r()) & 0x7) << 11) |
					((static_cast<u32>(tex.max_aniso()) & 0xf) << 14);
				record.tex_lod[unit][0] = tex.min_lod();
				record.tex_lod[unit][1] = tex.max_lod();
				record.tex_lod[unit][2] = tex.bias();
				record.tex_mip[unit] = tex.mipmap();
				record.tex_pitch[unit] = tex.pitch();
				record.tex_dimension[unit] = static_cast<u32>(tex.get_extended_texture_dimension());
				record.tex_format_ex[unit] = tex.remap();

				// The table entry the draw is about to write, not the guest's
				// request.  capture.py joins these to the host-sample dump so a
				// sampling fault can be read as "wrong object" or "right object".
				record.texture_id[unit] = frame.fragment_textures[unit].texture._impl;
				record.sampler_id[unit] = frame.fragment_textures[unit].sampler._impl;
			}

			// The hashes are the cheap cross-frame discriminator; the bytes are in
			// the dump, so a hash only has to answer "did this input change".
			record.hash_persistent = (persistent_data && required.first)
				? static_cast<u32>(probe::fnv1a(persistent_data, required.first)) : 0;
			record.hash_volatile = (volatile_data && required.second)
				? static_cast<u32>(probe::fnv1a(volatile_data, required.second)) : 0;

			const void* index_data = frame.constants.index_ring().data();
			record.hash_index = (index_data && input.index_length)
				? static_cast<u32>(probe::fnv1a(static_cast<const u8*>(index_data) + input.index_offset, input.index_length)) : 0;

			const vec4_block* xform = frame.constants.transform_constants_data();
			record.hash_xform = (xform && params->xform_constants_offset)
				? static_cast<u32>(probe::fnv1a(xform + params->xform_constants_offset, 4 * sizeof(vec4_block))) : 0;

			const usz fragment_constant_count = frame.compiled_fragment ? frame.compiled_fragment->constant_offsets.size() : 0;
			const vec4_block* fragment_constants = frame.constants.fragment_constants_data();
			record.hash_fragment_constants = (fragment_constants && params->fs_constants_offset && fragment_constant_count)
				? static_cast<u32>(probe::fnv1a(fragment_constants + params->fs_constants_offset, fragment_constant_count * sizeof(vec4_block))) : 0;

			record.hash_sampler = static_cast<u32>(probe::fnv1a(sampler, sampler_info_count * sizeof(sampler_info)));

			frame.constants.record_draw(record);
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the full geometry dump of the first subdraws.
		if (diagnostic_geometry < 9)
		{
			const u32 geometry = diagnostic_geometry - 1;

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, primitive %u, min %u, max %u, base %u, count %u, draw count %u, rebase %u, index buffer %u type %u offset %u length %u, required %u/%u",
				geometry, static_cast<u32>(draw_call.primitive), input.min_index, input.max_index, vertex_base, vertex_count,
				input.vertex_draw_count, input.index_rebase ? 1u : 0u, input.has_index_buffer ? 1u : 0u,
				static_cast<u32>(input.index_type), input.index_offset, input.index_length, required.first, required.second);

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, attribute mask 0x%x, blocks %u, volatile blocks %u, registers %u, branch bits 0x%x, z %.4f..%.4f, point %.3f",
				geometry, frame.vertex_layout.attribute_mask, static_cast<u32>(frame.vertex_layout.interleaved_blocks.size()),
				static_cast<u32>(frame.vertex_layout.volatile_blocks.size()), static_cast<u32>(frame.vertex_layout.referenced_registers.size()),
				context->transform_branch_bits, static_cast<f64>(context->z_near), static_cast<f64>(context->z_far),
				static_cast<f64>(context->point_size));

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, scale offset %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
				geometry,
				static_cast<f64>(context->scale_offset_mat[0]), static_cast<f64>(context->scale_offset_mat[1]),
				static_cast<f64>(context->scale_offset_mat[2]), static_cast<f64>(context->scale_offset_mat[3]),
				static_cast<f64>(context->scale_offset_mat[4]), static_cast<f64>(context->scale_offset_mat[5]),
				static_cast<f64>(context->scale_offset_mat[6]), static_cast<f64>(context->scale_offset_mat[7]),
				static_cast<f64>(context->scale_offset_mat[8]), static_cast<f64>(context->scale_offset_mat[9]),
				static_cast<f64>(context->scale_offset_mat[10]), static_cast<f64>(context->scale_offset_mat[11]),
				static_cast<f64>(context->scale_offset_mat[12]), static_cast<f64>(context->scale_offset_mat[13]),
				static_cast<f64>(context->scale_offset_mat[14]), static_cast<f64>(context->scale_offset_mat[15]));

			for (usz block_index = 0; block_index < frame.vertex_layout.interleaved_blocks.size(); ++block_index)
			{
				const auto& block = *frame.vertex_layout.interleaved_blocks[block_index];

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, block %u, stride %u, base offset %u, location %u, address 0x%x, interleaved %u, single vertex %u, range %u..%u, attributes %u",
					geometry, static_cast<u32>(block_index), static_cast<u32>(block.attribute_stride), block.base_offset,
					static_cast<u32>(block.memory_location), block.real_offset_address, block.interleaved ? 1u : 0u,
					block.single_vertex ? 1u : 0u, block.vertex_range.first, block.vertex_range.second,
					static_cast<u32>(block.locations.size()));

				for (const auto& attribute : block.locations)
				{
					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: geometry %u, block %u, attribute %u, modulo %u, frequency %u",
						geometry, static_cast<u32>(block_index), static_cast<u32>(attribute.index),
						attribute.modulo ? 1u : 0u, static_cast<u32>(attribute.frequency));
				}

				const u32 first = block.real_offset_address + (block.single_vertex ? 0 : vertex_base * block.attribute_stride);
				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, block %u, guest 0x%x: %s",
					geometry, static_cast<u32>(block_index), first, dump_guest(first, 16).c_str());
			}

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, persistent stream %u bytes at %u: %s",
				geometry, required.first, persistent_base, dump_bytes(persistent_data, std::min<usz>(required.first, 96)).c_str());
			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, volatile stream %u bytes at %u: %s",
				geometry, required.second, volatile_base, dump_bytes(volatile_data, std::min<usz>(required.second, 96)).c_str());

			for (u32 index = 0; index < 16; ++index)
			{
				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, attribute %u, placement %u, attrib_data %d %d",
					geometry, index, static_cast<u32>(frame.vertex_layout.attribute_placement[index]),
					params->attrib_data[index][0], params->attrib_data[index][1]);
			}

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, pass colors %u size %ux%u, base index %u, index offset %u, xform %u, vs context %u, fs context %u, fs texture base %u",
				geometry, frame.attachments.color_count(), frame.attachments.width(), frame.attachments.height(),
				params->vertex_base_index, params->vertex_index_offset, params->xform_constants_offset,
				params->vs_context_offset, params->fs_context_offset, params->fs_texture_base_index);

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, vertex program base 0x%x output mask 0x%x ctrl 0x%x, fragment ctrl 0x%x ucode %u, compiled vertex %u mask 0x%x, compiled fragment %u mask 0x%x, rop control 0x%x, alpha ref %.4f",
				geometry,
				m_ctx->program.vertex_program.base_address, m_ctx->program.vertex_program.output_mask, m_ctx->program.vertex_program.ctrl,
				m_ctx->program.fragment_program.ctrl, m_ctx->program.fragment_program.ucode_length,
				frame.compiled_vertex ? frame.compiled_vertex->id : 0u,
				frame.compiled_vertex ? frame.compiled_vertex->color_output_mask : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->color_output_mask : 0u,
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref));
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3 and level 5; the bound units and the vertex dump of a
		// bounded prefix of the geometry stream.
		if (diagnostic_trace)
		{
			for (u32 unit = 0; unit < 16; ++unit)
			{
				if (!(m_ctx->program.fp_metadata.referenced_textures_mask & (1u << unit)))
				{
					continue;
				}

				const sampler_info& info = sampler[unit];

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: unit %u, base %u, scale %.6f %.6f %.6f, bias %.6f %.6f %.6f, clamp %.4f %.4f %.4f %.4f, remap 0x%x, control 0x%x",
					unit, params->fs_texture_base_index,
					static_cast<f64>(info.scale[0]), static_cast<f64>(info.scale[1]), static_cast<f64>(info.scale[2]),
					static_cast<f64>(info.bias[0]), static_cast<f64>(info.bias[1]), static_cast<f64>(info.bias[2]),
					static_cast<f64>(info.clamp_min[0]), static_cast<f64>(info.clamp_min[1]),
					static_cast<f64>(info.clamp_max[0]), static_cast<f64>(info.clamp_max[1]),
					info.remap, info.control);
			}

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: fragment context, rop 0x%x, alpha ref %.4f, fog %.4f %.4f mode %u, wpos %.4f %.4f %.4f, fs context %u, vs context %u, xform %u",
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref),
				static_cast<f64>(fragment->fog_param0), static_cast<f64>(fragment->fog_param1), fragment->fog_mode,
				static_cast<f64>(fragment->wpos_scale), static_cast<f64>(fragment->wpos_bias[0]), static_cast<f64>(fragment->wpos_bias[1]),
				params->fs_context_offset, params->vs_context_offset, params->xform_constants_offset);

			const u32 stride = frame.vertex_layout.interleaved_blocks.empty()
				? 0u
				: static_cast<u32>(frame.vertex_layout.interleaved_blocks[0]->attribute_stride);

			if (persistent_data && stride)
			{
				const u8* stream = static_cast<const u8*>(persistent_data);

				for (u32 vertex = 0; vertex < 4; ++vertex)
				{
					const u8* source = stream + static_cast<usz>(vertex) * stride;
					f32 in_pos[4] = {};

					for (u32 channel = 0; channel < 3; ++channel)
					{
						const u32 word = (static_cast<u32>(source[channel * 4 + 0]) << 24)
							| (static_cast<u32>(source[channel * 4 + 1]) << 16)
							| (static_cast<u32>(source[channel * 4 + 2]) << 8)
							| static_cast<u32>(source[channel * 4 + 3]);
						std::memcpy(&in_pos[channel], &word, 4);
					}

					in_pos[3] = 1.0f;

					// TODO(excise): level 5; the components the trace above does not
					// use.  A position format of four components would take its w from
					// the fourth word, so the word is printed next to the forced 1.0.
					const u8* raw = stream + static_cast<usz>(vertex) * stride;
					const auto be_float = [](const u8* source, u32 index) {
						const u32 word = (static_cast<u32>(source[index * 4 + 0]) << 24)
							| (static_cast<u32>(source[index * 4 + 1]) << 16)
							| (static_cast<u32>(source[index * 4 + 2]) << 8)
							| static_cast<u32>(source[index * 4 + 3]);
						f32 value = 0.0f;
						std::memcpy(&value, &word, 4);
						return value;
					};
					const f32 raw3 = (stride >= 16) ? be_float(raw, 3) : 0.0f;
					const f32 raw4 = (stride >= 20) ? be_float(raw, 4) : 0.0f;

					// The generated vertex program scales and offsets the fetched
					// position by the context matrix and applies nothing else
					// (vs-0.metal:363-381), so the trace must do the same.
					f32 clip[4] = {};
					for (u32 column = 0; column < 4; ++column)
					{
						for (u32 row = 0; row < 4; ++row)
						{
							clip[column] += in_pos[row] * context->scale_offset_mat[row * 4 + column];
						}
					}

					const f64 w = static_cast<f64>(clip[3]);

					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: vertex %u, guest %.4f %.4f %.4f, clip %.4f %.4f %.4f %.4f, ndc %.4f %.4f %.4f, raw w %.4f tc %.4f",
						vertex,
						static_cast<f64>(in_pos[0]), static_cast<f64>(in_pos[1]), static_cast<f64>(in_pos[2]),
						static_cast<f64>(clip[0]), static_cast<f64>(clip[1]), static_cast<f64>(clip[2]), static_cast<f64>(clip[3]),
						static_cast<f64>(clip[0]) / w, static_cast<f64>(clip[1]) / w, static_cast<f64>(clip[2]) / w,
						static_cast<f64>(raw3), static_cast<f64>(raw4));
				}
			}
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the draw summary of one capture frame.  The
		// fragment-constant count is here because a program that reads no
		// constants cannot explain a lit surface, and that is a question a black
		// frame raises immediately.
		if (diagnostic_capture_active())
		{
			const usz fragment_constant_count = frame.compiled_fragment ? frame.compiled_fragment->constant_offsets.size() : 0;
			const u32 capture_stride = frame.vertex_layout.interleaved_blocks.empty()
				? 0u
				: static_cast<u32>(frame.vertex_layout.interleaved_blocks[0]->attribute_stride);

			MTL_NOTE(5, "Metal 4: capture geometry frame %llu, primitive %u count %u base %u, vp %u fp %u, fp constants %llu fs offset %u, textures dims 0x%x shadow 0x%x redirect 0x%x msaa 0x%x, rop 0x%x alpha %.4f, fog %.4f %.4f mode %u, wpos %.4f %.4f %.4f, attr mask 0x%x blocks %u stride %u",
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<u32>(draw_call.primitive), vertex_count, vertex_base,
				frame.compiled_vertex ? frame.compiled_vertex->id : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
				static_cast<unsigned long long>(fragment_constant_count), params->fs_constants_offset,
				m_ctx->program.fragment_program.texture_state.texture_dimensions,
				m_ctx->program.fragment_program.texture_state.shadow_textures,
				m_ctx->program.fragment_program.texture_state.redirected_textures,
				m_ctx->program.fragment_program.texture_state.multisampled_textures,
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref),
				static_cast<f64>(fragment->fog_param0), static_cast<f64>(fragment->fog_param1), fragment->fog_mode,
				static_cast<f64>(fragment->wpos_scale), static_cast<f64>(fragment->wpos_bias[0]), static_cast<f64>(fragment->wpos_bias[1]),
				frame.vertex_layout.attribute_mask,
				static_cast<u32>(frame.vertex_layout.interleaved_blocks.size()), capture_stride);

			// TODO(excise): level 5; the vertex description of the captured draw.
			// The vertex trace rebuilds the quad from the host stream and forces a
			// w of 1, so it proves the stream and the matrix; it does not prove the
			// descriptor the GPU decodes.  A descriptor that reads four components
			// where the stream holds three supplies a fetched w the trace hides, and
			// a zero w clips the whole quad.
			MTL_NOTE(5, "Metal 4: capture input frame %llu, indexed %u draw count %u index offset %u length %u rebase %u, vertex base %u count %u, persistent %u volatile %u, required %u/%u, attrib0 %d %d, attrib8 %d %d",
				static_cast<unsigned long long>(diagnostic_frame_index),
				input.has_index_buffer ? 1u : 0u, input.vertex_draw_count,
				input.index_offset, input.index_length, input.index_rebase ? 1u : 0u,
				vertex_base, vertex_count, persistent_base, volatile_base,
				static_cast<u32>(required.first), static_cast<u32>(required.second),
				params->attrib_data[0][0], params->attrib_data[0][1],
				params->attrib_data[8][0], params->attrib_data[8][1]);

			// TODO(excise): level 5; the matrix and clip range that scale the captured
			// draw.  The trace applies the matrix but never prints it, so a degenerate
			// z row cannot be told from a z of zero.
			MTL_NOTE(5, "Metal 4: capture context frame %llu, z %.4f..%.4f, scale offset %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<f64>(context->z_near), static_cast<f64>(context->z_far),
				static_cast<f64>(context->scale_offset_mat[0]), static_cast<f64>(context->scale_offset_mat[1]),
				static_cast<f64>(context->scale_offset_mat[2]), static_cast<f64>(context->scale_offset_mat[3]),
				static_cast<f64>(context->scale_offset_mat[4]), static_cast<f64>(context->scale_offset_mat[5]),
				static_cast<f64>(context->scale_offset_mat[6]), static_cast<f64>(context->scale_offset_mat[7]),
				static_cast<f64>(context->scale_offset_mat[8]), static_cast<f64>(context->scale_offset_mat[9]),
				static_cast<f64>(context->scale_offset_mat[10]), static_cast<f64>(context->scale_offset_mat[11]),
				static_cast<f64>(context->scale_offset_mat[12]), static_cast<f64>(context->scale_offset_mat[13]),
				static_cast<f64>(context->scale_offset_mat[14]), static_cast<f64>(context->scale_offset_mat[15]));
		}
#endif // MTL_DEBUG_LOGS

		// The substituted vertex push constant carries the draw-parameter block
		// index (HLD-05 §3.3).
		*vs_params = frame.constants.draw_parameters_index();

		// The binds of this subdraw.  A block the shader indexes by offset is bound
		// once at the ring base; a block it reads at offset zero is bound at its
		// own block (HLD-05 §3.5).
		argument_binds binds;
		binds.vertex_context = frame.constants.vertex_context_address();
		binds.vertex_constants = frame.constants.vertex_constants_address();
		binds.vertex_persistent_stream = frame.constants.persistent_stream().resource_id();
		binds.vertex_volatile_stream = frame.constants.volatile_stream().resource_id();
		binds.vertex_draw_parameters = frame.constants.draw_parameters_address();
		binds.vertex_draw_params = frame.constants.vertex_draw_params_block_address();
		binds.instancing_lookup = frame.instancing_lookup;
		binds.instancing_constants = frame.instancing_constants;
		binds.fragment_constants = frame.constants.fragment_constants_address();
		binds.fragment_state = frame.constants.fragment_context_address();
		binds.fragment_texture_params = frame.constants.sampler_info_address();
		binds.fragment_stipple = frame.constants.stipple_address();

		// The units resolved for this clause (HLD-05 §3.2).
		binds.vertex_textures = frame.vertex_textures;
		binds.fragment_textures = frame.fragment_textures;
		binds.fragment_stencil = frame.fragment_stencil;
		binds.fragment_depth_input = frame.depth_input;

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the addresses the table will carry.  A shader that
		// reads zero where the block holds a value is a binding question, and the
		// block values the trace prints above are the CPU's copy until these
		// addresses agree with where it wrote.
		if (diagnostic_capture_active())
		{
			const u64 vc = static_cast<u64>(binds.vertex_constants);
			const u64 fc = static_cast<u64>(binds.fragment_constants);
			const u64 vsc = static_cast<u64>(binds.vertex_context);
			const u64 fsc = static_cast<u64>(binds.fragment_state);

			MTL_NOTE(5, "Metal 4: capture binds frame %llu, vc 0x%llx index %u -> 0x%llx, fc 0x%llx index %u -> 0x%llx, fs ctx 0x%llx index %u -> 0x%llx, vs ctx 0x%llx index %u, draw params 0x%llx index %u",
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<unsigned long long>(vc), params->xform_constants_offset,
				static_cast<unsigned long long>(vc + static_cast<u64>(params->xform_constants_offset) * 16ull),
				static_cast<unsigned long long>(fc), params->fs_constants_offset,
				static_cast<unsigned long long>(fc + static_cast<u64>(params->fs_constants_offset) * 16ull),
				static_cast<unsigned long long>(fsc), params->fs_context_offset,
				static_cast<unsigned long long>(fsc + static_cast<u64>(params->fs_context_offset) * 32ull),
				static_cast<unsigned long long>(vsc), params->vs_context_offset,
				static_cast<unsigned long long>(binds.vertex_draw_parameters), frame.constants.draw_parameters_index());
		}
#endif // MTL_DEBUG_LOGS

		if (m_ctx->program.fragment_program.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
		{
			blend_values* blend = frame.constants.alloc_blend_values();
			if (!blend)
			{
				return;
			}

			// The block carries the blend configuration and the logic op, so it is
			// filled when either feature is on; the program reads only the part it
			// emulates (REVIEW-FINDINGS.md C1).
			blend->reserved = 0;
			blend->blend_eqn = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_equation_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_equation_a()));
			blend->blend_sfactors = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_func_sfactor_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_func_sfactor_a()));
			blend->blend_dfactors = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_func_dfactor_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_func_dfactor_a()));

			std::copy(m_ctx->draw_state.blend.constant_color.begin(), m_ctx->draw_state.blend.constant_color.end(), blend->blend_constants);

			blend->logic_op = static_cast<u32>(m_ctx->register_state->logic_operation());

			binds.fragment_blend = frame.constants.blend_values_block_address();
		}

		// Metal 4 captures the argument table once per draw, when the draw is
		// encoded, so the table is safe to mutate between draws but nothing is
		// captured at setArgumentTable (measured, 2026-10-22).  The binder writes
		// only the slots whose value changed, including a change to zero, and the
		// table is re-bound on every subdraw (REVIEW-FINDINGS.md F19).
		bool vertex_written [[maybe_unused]] = false;
		bool fragment_written [[maybe_unused]] = false;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "draw/argument binds");

			// The interpreter draws from its own tables, so its compact sampler
			// arrays never collide with a regular draw's unit bindings in the shared
			// tables (LLD-shader-interpreter.md §4.2).
			MTL4::ArgumentTable* const vertex_argument_table = frame.interpreter_active
				? frame.recorder.interpreter_vertex_table()
				: frame.recorder.vertex_table();
			MTL4::ArgumentTable* const fragment_argument_table = frame.interpreter_active
				? frame.recorder.interpreter_fragment_table()
				: frame.recorder.fragment_table();

			if (frame.interpreter_active)
			{
				interpreter_bindings interpreter_vertex;
				interpreter_vertex.instructions = frame.interpreter_vertex_instructions;

				interpreter_bindings interpreter_fragment;
				interpreter_fragment.instructions = frame.interpreter_fragment_instructions;

				const interpreter_texture_mapping& mapping = frame.interpreter_mapping;

				if (mapping.is_single())
				{
					// The single-type shader declares a 16-element array and indexes
					// it by the RSX unit.  The compact range is the highest referenced
					// unit plus one, so only the units the body can read are bound
					// (LLD-shader-interpreter.md §6).
					interpreter_fragment.texture_count = frame.interpreter_texture_count;

					for (u32 unit = 0; unit < frame.interpreter_texture_count; ++unit)
					{
						interpreter_fragment.textures[unit] = frame.fragment_textures[unit];
					}
				}
				else
				{
					// The mixed shader's arrays are packed by dimension.  The compact
					// index of a unit is its type's prefix plus its position in that
					// type, and the binder writes that index into the interpreter's
					// 12..27 texture range (LLD-shader-interpreter.md §6).
					u32 count = 0;

					for (u32 unit = 0; unit < 16; ++unit)
					{
						if (!(mapping.referenced_mask & (1u << unit)))
						{
							continue;
						}

						const u32 index = mapping.global_index(unit);
						interpreter_fragment.textures[index] = frame.fragment_textures[unit];
						count = std::max(count, index + 1);
					}

					interpreter_fragment.texture_count = count;
				}

				vertex_written = frame.interpreter_binder.bind_interpreter(
					vertex_argument_table, true, binds, interpreter_vertex);
				fragment_written = frame.interpreter_binder.bind_interpreter(
					fragment_argument_table, false, binds, interpreter_fragment);
			}
			else
			{
				vertex_written = frame.binder.bind(vertex_argument_table, true, binds);
				fragment_written = frame.binder.bind(fragment_argument_table, false, binds);
			}

			encoder->setArgumentTable(vertex_argument_table, MTL::RenderStageVertex);
			encoder->setArgumentTable(fragment_argument_table, MTL::RenderStageFragment);
		}

		// TODO(excise): level 2; the argument-table bind stream.  A bind runs once per
		// subdraw, so it is counted here and the trace stays a bounded prefix.
		MTL_PROBE_LATCH(2, "draw/bind");
		// TODO(excise): level 2; the binder writes only the slots whose value changed,
		// so a written and a cached table are different signatures (area N).
		if (vertex_written)
		{
			MTL_PROBE_LATCH(2, "bind/vertex table written");
		}
		else
		{
			MTL_PROBE_LATCH(2, "bind/vertex table cached");
		}

		if (fragment_written)
		{
			MTL_PROBE_LATCH(2, "bind/fragment table written");
		}
		else
		{
			MTL_PROBE_LATCH(2, "bind/fragment table cached");
		}

#if MTL_DEBUG_LOGS
		static u32 diagnostic_binds = 0;
		static u32 diagnostic_vertex_writes = 0;
		static u32 diagnostic_fragment_writes = 0;

		diagnostic_vertex_writes += vertex_written ? 1u : 0u;
		diagnostic_fragment_writes += fragment_written ? 1u : 0u;

		if (diagnostic_binds < 24)
		{
			// TODO(excise): level 3; the first subdraw binds of the run only.
			MTL_NOTE(3, "Metal 4: bind %u, vertex written %u, fragment written %u, vertex texture 0 (0x%llx, 0x%llx), fragment unit 0 (0x%llx, 0x%llx), unit 1 (0x%llx, 0x%llx)",
				diagnostic_binds, vertex_written ? 1u : 0u, fragment_written ? 1u : 0u,
				static_cast<unsigned long long>(binds.vertex_textures[0].texture._impl),
				static_cast<unsigned long long>(binds.vertex_textures[0].sampler._impl),
				static_cast<unsigned long long>(binds.fragment_textures[0].texture._impl),
				static_cast<unsigned long long>(binds.fragment_textures[0].sampler._impl),
				static_cast<unsigned long long>(binds.fragment_textures[1].texture._impl),
				static_cast<unsigned long long>(binds.fragment_textures[1].sampler._impl));

			MTL_NOTE(3, "Metal 4: bind %u, push constant index %u (of %u), draw parameters base 0x%llx, vertex base %u offset %u, xform %u, vs context %u, fs context %u, fs constants %u, fs texture base %u, attrib0 %d %d, attrib8 %d %d",
				diagnostic_binds, *vs_params, frame.constants.draw_parameters_index(),
				static_cast<unsigned long long>(frame.constants.draw_parameters_address()),
				params->vertex_base_index, params->vertex_index_offset, params->xform_constants_offset,
				params->vs_context_offset, params->fs_context_offset, params->fs_constants_offset,
				params->fs_texture_base_index,
				params->attrib_data[0][0], params->attrib_data[0][1],
				params->attrib_data[8][0], params->attrib_data[8][1]);

			for (u32 unit = 0; unit < 4; ++unit)
			{
				const sampler_info& info = sampler[unit];

				MTL_NOTE(3, "Metal 4: bind %u, texture parameters %u, scale %.4f %.4f %.4f, bias %.4f %.4f %.4f, clamp %.4f %.4f %.4f %.4f, remap 0x%x, control 0x%x",
					diagnostic_binds, unit,
					static_cast<f64>(info.scale[0]), static_cast<f64>(info.scale[1]), static_cast<f64>(info.scale[2]),
					static_cast<f64>(info.bias[0]), static_cast<f64>(info.bias[1]), static_cast<f64>(info.bias[2]),
					static_cast<f64>(info.clamp_min[0]), static_cast<f64>(info.clamp_min[1]),
					static_cast<f64>(info.clamp_max[0]), static_cast<f64>(info.clamp_max[1]),
					info.remap, info.control);
			}

			MTL_NOTE(3, "Metal 4: bind %u, vertex writes %u, fragment writes %u (of %u binds)",
				diagnostic_binds, diagnostic_vertex_writes, diagnostic_fragment_writes, diagnostic_binds + 1);
		}

		diagnostic_binds++;
#endif // MTL_DEBUG_LOGS

		m_ctx->stats.setup_time += m_profiler.duration();

		const MTL::PrimitiveType primitive = get_primitive_type(draw_call.primitive);

		// A ZCULL query that started or ended between draws takes effect on the
		// encoder that carries the following draws (MTLZCULL.h).
		frame.zcull_control.apply(encoder);

		// TODO(excise): level 2; the submitted geometry and its index form (area I,
		// area N).  A draw with no vertices and a draw with vertices are different
		// signatures, and so are an indexed and a non-indexed draw.
		MTL_PROBE_TOTAL(2, "draw/vertices submitted", input.vertex_draw_count);
		if (input.has_index_buffer)
		{
			MTL_PROBE_LATCH(2, "draw/indexed");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/not indexed");
		}

		if (input.vertex_draw_count)
		{
			MTL_PROBE_LATCH(2, "draw/nonempty");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/empty");
		}
		MTL_PROBE_HISTOGRAM(2, "draw metal primitive", static_cast<u32>(primitive), "metal primitive type");

		if (input.has_index_buffer)
		{
			const MTL::IndexType index_type = get_index_type(input.index_type);
			const MTL::GPUAddress index_address = frame.constants.index_ring().address() + input.index_offset;
			const u32 type_size = ::get_index_type_size(input.index_type);

			if (draw_call.is_trivial_instanced_draw)
			{
				encoder->drawIndexedPrimitives(primitive, input.vertex_draw_count, index_type,
					index_address, input.index_length, draw_call.pass_count(), 0, 0);
			}
			else if (draw_call.is_single_draw())
			{
				encoder->drawIndexedPrimitives(primitive, input.vertex_draw_count, index_type,
					index_address, input.index_length, 1, 0, 0);
			}
			else
			{
				// One draw per clause subrange: a primitive_restart_barrier inside
				// the clause ends a strip, so the ranges must not join into one
				// draw (VKDraw.cpp:1242-1252).
				u32 index_offset = 0;

				for (const auto& range : draw_call.get_subranges())
				{
					const u32 count = ::get_index_count(draw_call.primitive, range.count);
					const u32 byte_offset = index_offset * type_size;

					// Metal 4 has no separate index-buffer offset: the address is the
					// buffer base and the seventh argument is baseVertex.  Advance the
					// address and shrink the declared length instead (VKDraw.cpp:1242).
					if (byte_offset >= input.index_length)
					{
						break;
					}

					encoder->drawIndexedPrimitives(primitive, count, index_type, index_address + byte_offset,
						input.index_length - byte_offset, 1, 0, 0);
					index_offset += count;
				}
			}
		}
		else if (draw_call.is_trivial_instanced_draw)
		{
			encoder->drawPrimitives(primitive, 0, input.vertex_draw_count, draw_call.pass_count(), 0);
		}
		else if (draw_call.is_single_draw())
		{
			encoder->drawPrimitives(primitive, 0, input.vertex_draw_count, 1, 0);
		}
		else
		{
			// The non-indexed sibling of the subrange split above
			// (VKDraw.cpp:1194-1200).
			u32 vertex_offset = 0;

			for (const auto& range : draw_call.get_subranges())
			{
				encoder->drawPrimitives(primitive, vertex_offset, range.count, 1, 0);
				vertex_offset += range.count;
			}
		}

		m_ctx->stats.draw_exec_time += m_profiler.duration();
		m_ctx->stats.draw_calls++;
	}


	// Resolve the image that carries the RSX output.  A surface at the display
	// address wins, then a texture the cache already holds, then a copy of the guest
	// bytes.  `width` and `height` come back as the image's own extent.
	void MTLGSRender::get_present_source(u32 address, u32& width, u32& height, u32 pitch, u32 format, u8 eye,
		MTL::Texture*& image)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/source");
		auto& resources = *m_frame_resources;

		// The display format is a GCM texture format throughout: the AV configuration
		// reports it that way (rsx_utils.cpp:151), the image lookup and the pixel
		// format below take it that way, and the Vulkan present path hands it to this
		// same overload (VKPresent.cpp:314).  The surface-colour overload rejects it
		// ("Invalid color format 0x85" for CELL_GCM_TEXTURE_A8R8G8B8) and the throw
		// ends the RSX thread.
		const u32 format_bpp = rsx::get_format_block_size_in_bytes(format);

		const auto overlap_info = resources.cache.get_merged_texture_memory_region(
			resources.recorder, address, width, height, pitch, format_bpp, rsx::surface_access::transfer_read);

		if (!overlap_info.empty())
		{
			const auto& section = overlap_info.back();
			surface* target = section.surface;

			if (section.base_address >= address)
			{
				const u32 surface_width = target->get_surface_width<rsx::surface_metrics::samples>();
				const u32 surface_height = target->get_surface_height<rsx::surface_metrics::samples>();
				bool viable = false;

				if (section.base_address == address)
				{
					// The surface either fills the buffer or crops into it.
					viable = (surface_width >= width && surface_height >= height);
				}
				else
				{
					// The surface sits inside the buffer, so the buffer carries a
					// border and the insets must be symmetric (GLPresent.cpp:82).
					const u32 inset_offset = section.base_address - address;
					const u32 inset_y = inset_offset / pitch;
					const u32 inset_x = (inset_offset % pitch) / format_bpp;

					viable = ((surface_width + inset_x + inset_x) == width &&
						(surface_height + inset_y + inset_y) == height);
				}

				if (viable)
				{
					// A transfer access resolves a multisampled surface, so the
					// present always reads one sample per pixel
					// (VKRenderTargets.cpp:839).
					target->memory_barrier(resources.recorder, rsx::surface_access::transfer_read);

					if (texture* output = target->get_surface(rsx::surface_access::transfer_read))
					{
						image = output->value();

						// TODO(excise): level 3; per-event trace line.
						MTL_NOTE(3, "Metal 4: present source from the surface, address 0x%x, %ux%u", address, width, height);

						std::tie(width, height) = rsx::apply_resolution_scale<true>(
							RSX(m_ctx)->resolution_scaling_config, std::min(surface_width, width), std::min(surface_height, height));
						return;
					}
				}
			}
		}
		else if (auto* cached = resources.textures.find_texture_from_dimensions<true>(address, format);
			cached && cached->get_raw_texture() && cached->get_raw_texture()->width() >= width &&
			cached->get_raw_texture()->height() >= height)
		{
			// The frame was drawn offscreen or in software and blitted to display.
			image = cached->get_raw_texture()->value();

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: present source from the texture cache, address 0x%x", address);
			return;
		}

		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: flip image was not in the cache, uploading the surface from the CPU (address 0x%x, format 0x%x)", address, format);

		const MTL::PixelFormat expected_format = mtl::get_texture_format(format, resources.resources.supports_bc_texture_compression());
		const u32 pixel_size = get_pixel_size(expected_format);

		if (!pixel_size || !address || !pitch)
		{
			return;
		}

		const u32 row_bytes = width * pixel_size;
		const u64 length = static_cast<u64>(row_bytes) * height;

		MTL::Texture*& scratch = m_flip_scratch[eye];

		if (!scratch || scratch->width() != width || scratch->height() != height || scratch->pixelFormat() != expected_format)
		{
			if (scratch)
			{
				m_frame_resources->resources.defer_release(scratch);
				scratch = nullptr;
			}

			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
			descriptor->setTextureType(MTL::TextureType2D);
			descriptor->setPixelFormat(expected_format);
			descriptor->setWidth(width);
			descriptor->setHeight(height);
			descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
			scratch = m_frame_resources->resources.new_texture(descriptor, MTL::StorageModePrivate);
			descriptor->release();

			if (!scratch)
			{
				return;
			}

			m_frame_resources->resources.make_persistent(scratch);
		}

		// The guest pitch may not be the host row length, so the staging copy
		// repacks each row (GLPresent.cpp:127).
		MTL::Buffer* staging = m_frame_resources->resources.new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

		if (!staging)
		{
			return;
		}

		// Read through the unprotected mirror, the same view the guest window
		// import exposes to the GPU (vm::get_super_ptr).
		const u8* guest = vm::get_super_ptr<const u8>(address);
		u8* host = static_cast<u8*>(staging->contents());

		if (!guest || !host)
		{
			m_frame_resources->resources.defer_release(staging);
			return;
		}

		const u32 copy_bytes = std::min(row_bytes, pitch);

		for (u32 row = 0; row < height; ++row)
		{
			std::memcpy(host + (static_cast<u64>(row) * row_bytes), guest + (static_cast<u64>(row) * pitch), copy_bytes);
		}

		MTL4::ComputeCommandEncoder* encoder = m_frame_resources->recorder.open_compute();

		if (!encoder)
		{
			m_frame_resources->resources.defer_release(staging);
			return;
		}

		encoder->copyFromBuffer(staging, 0, row_bytes, row_bytes * height, MTL::Size::Make(width, height, 1),
			scratch, 0, 0, MTL::Origin::Make(0, 0, 0));

		m_frame_resources->recorder.close_encoder();
		m_frame_resources->resources.defer_release(staging);

		image = scratch;
	}

	// C7.  The RSX output is the surface at the flip address.  It is composed into
	// the drawable by the output tier and presented through the one submit path the
	// recorder owns (HLD-03 §4.9, HLD-06 §2.4).
	bool MTLGSRender::present_frame(const rsx::display_flip_info_t& info)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/flip");

		// A flip always owns a frame.  The guest can flip with no draw since the
		// last flip, and the output pass still needs a command buffer, so the frame
		// opens here when it is not already open (VKPresent.cpp:471-493).  A flip
		// never returns with no present, because that is a frozen image
		// (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if (!m_frame_resources || !m_device || m_device->lost_flag())
		{
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/no frame resources", 1);
			return false;
		}

		open_frame();

		m_profiler.start();

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; one index for this presented frame, shared by
		// the image dumps and the ring snapshot, so the analysis joins them.
		++diagnostic_present_index;
#endif

		// The resolution scale is a live setting.  A change clones the resident
		// surfaces at the next flip, before the output pass reads them
		// (VKPresent.cpp:1000-1020).
		const rsx::surface_scaling_config_t active_scaling_config =
		{
			.scale_percent = static_cast<u16>(g_cfg.video.resolution_scale_percent),
			.min_scalable_dimension = static_cast<u16>(g_cfg.video.min_scalable_dimension),
		};

		if (active_scaling_config != RSX(m_ctx)->resolution_scaling_config)
		{
			m_frame_resources->cache.sync_scaling_config(m_frame_resources->recorder, active_scaling_config);
			RSX(m_ctx)->resolution_scaling_config = active_scaling_config;
		}

		auto& resources = *m_frame_resources;
		const u32 slot = resources.recorder.slot();

#if MTL_DEBUG_LOGS
		// TODO(excise): level 1; the state the watchdog stamps while the RSX
		// thread is blocked inside this flip.  A hang is otherwise a silent log.
		diagnostic_watchdog_present = diagnostic_present_index;
		diagnostic_watchdog_frame = static_cast<u32>(diagnostic_frame_index);
		diagnostic_watchdog_slot = slot;
		diagnostic_watchdog_draw = diagnostic_global_draw_id;

		// A gap that the watchdog reported is over.  The frames before it are
		// still the newest history entries, because the blocked flip never took
		// its snapshot, so the hang's inputs are the ones the dump writes.
		if (diagnostic_watchdog_gap.exchange(false))
		{
			rsx_log.notice("Metal 4: watchdog recovery, present %u, frame %u", diagnostic_present_index, diagnostic_watchdog_frame.load());
			diagnostic_try_arm_capture("watchdog", true);
		}
#endif

		// TODO(excise): level 2; the flip stream and its correlation key.  The flip
		// index below is the frame correlation id: it joins the flip, the acquire and
		// the display stamp of one frame.
		if (info.skip_frame)
		{
			MTL_PROBE_LATCH(2, "present/flip skipped");
		}
		else
		{
			MTL_PROBE_LATCH(2, "present/flip delivered");
		}
		MTL_PROBE_HISTOGRAM(2, "present flip buffer", static_cast<u32>(info.buffer), "display buffer index");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first flips of the run.
		static u32 diagnostic_flips = 0;

		if (diagnostic_flips < 16)
		{
			MTL_NOTE(3, "Metal 4: flip %u, buffer %u, skip %u",
				diagnostic_flips, static_cast<u32>(info.buffer), info.skip_frame ? 1u : 0u);
		}

		diagnostic_flips++;
#endif // MTL_DEBUG_LOGS

		// The recorder waited for this slot when the frame opened, so a capture that
		// slot recorded its previous time round is readable now (HLD-06 §2.7).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/deliver");
			resources.output.deliver(slot, *m_frame);
		}

		const auto& avconfig = g_fxo->get<rsx::avconf>();

		u32 buffer_width = 0;
		u32 buffer_height = 0;
		u32 buffer_pitch = 0;
		u32 av_format = CELL_GCM_TEXTURE_A8R8G8B8;
		MTL::Texture* image = nullptr;
		MTL::Texture* image2 = nullptr;

		// A skipped frame is dropped, not presented late, and the display buffer's
		// declared extent decides whether a present is attempted
		// (GLPresent.cpp:164, :173-201).
		if (!info.skip_frame && info.buffer < m_ctx->display_buffers_count)
		{
			buffer_width = m_ctx->display_buffers[info.buffer].width;
			buffer_height = m_ctx->display_buffers[info.buffer].height;
			buffer_pitch = m_ctx->display_buffers[info.buffer].pitch;

			if (!buffer_width)
			{
				buffer_width = avconfig.resolution_x;
				buffer_height = avconfig.resolution_y;
			}

			if (avconfig.state)
			{
				av_format = avconfig.get_compatible_gcm_format();

				if (!buffer_pitch)
				{
					buffer_pitch = buffer_width * avconfig.get_bpp();
				}

				const size2u video_frame_size = avconfig.video_frame_size();
				buffer_width = std::min(buffer_width, video_frame_size.width);
				buffer_height = std::min(buffer_height, video_frame_size.height);
			}
			else if (!buffer_pitch)
			{
				buffer_pitch = buffer_width * 4;
			}

			if (buffer_width && buffer_height)
			{
				// The source is the surface at the display buffer's guest address,
				// not the display-buffer index (VKPresent.cpp:551).
				const u32 address = rsx::get_address(m_ctx, m_ctx->display_buffers[info.buffer].offset, CELL_GCM_LOCATION_LOCAL);
				get_present_source(address, buffer_width, buffer_height, buffer_pitch, av_format, 0, image);

				// EXCISE-BEGIN present-trace
				static u32 diagnostic_presents = 0;
				// TODO(excise): level 5; the present-source dump budget, per run.
				// A bare run reaches 5-6 presents at this cap; the sparse and
				// capture-window branches would otherwise dump every present.
				constexpr u32 diagnostic_present_dump_budget = 64;
				static u32 diagnostic_present_dumps = 0;
				// EXCISE-END present-trace

				// TODO(excise): level 2; the present branch.  The source resolves here
				// or it does not, and the two outcomes are different defects.
				if (image)
				{
					MTL_PROBE_LATCH(2, "present/source resolved");
				}
				else
				{
					MTL_PROBE_LATCH(2, "present/source missing");
				}
				// TODO(excise): level 2; the output format of the present.
				MTL_PROBE_HISTOGRAM(2, "present format", static_cast<u32>(av_format), "rsx av format");

				// TODO(excise): level 3; the first presents of the run.
				if (diagnostic_presents < 12)
				{
					MTL_NOTE(3, "Metal 4: present %u, display index %u, address 0x%x, %ux%u, pitch %u, image %s",
						diagnostic_presents, info.buffer, address, buffer_width, buffer_height, buffer_pitch,
						image ? "resolved" : "none");
				}

				// TODO(excise): level 3; the present counter.
				diagnostic_presents++;

#if MTL_DEBUG_LOGS
				// TODO(excise): level 5; the rolling tape's presented frame.  The
				// copy is recorded, not read, so it does not stall the present; F12
				// freezes the ring and one readback at that present writes it.
				if (probe::tape_depth())
				{
					diagnostic_tape_copy(resources.recorder, resources.resources, image,
						address, diagnostic_current_draw, 0, diagnostic_frame_index,
						0, true, false);
				}

				// TODO(excise): level 5; the per-frame witness, present in the log
				// before the key press.  A `live` count for a bound-target sample is
				// the unsafe Metal binding; `copy` is the fixed path.  The per-draw
				// `feedback witness` line above names each binding; this line is the
				// frame's tally and is not bound by the generic site cap, so it still
				// prints when the race is reached.
				if (probe::at_least(5) && (diagnostic_feedback_live || diagnostic_feedback_copy))
				{
					MTL_NOTE_WITNESS(5, "Metal 4: frame witness %llu present %u draw %u, feedback live %u copy %u, black %u",
						static_cast<unsigned long long>(diagnostic_frame_index), diagnostic_present_index,
						diagnostic_current_draw, diagnostic_feedback_live, diagnostic_feedback_copy,
						diagnostic_black_count);
				}

				// The witness and the black tally are per frame, so the count starts
				// over at the present that ends this frame.
				diagnostic_black_count = 0;
				diagnostic_feedback_live = 0;
				diagnostic_feedback_copy = 0;
				diagnostic_feedback_binding_count = 0;
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
				// TODO(excise): level 5; the present readback of the first presents,
				// a sparse sample of the whole run, and every eighth present of a
				// capture window.  The user presses F12 three or four times, so a
				// dense window dump would be hundreds of megabytes per press; the tape
				// already keeps the dense pre-trigger frames and the per-draw probe
				// keeps the patch timeline, so eight-frame sampling is enough here.
				const bool periodic_present = diagnostic_present_index < 20000 && (diagnostic_present_index % 600) == 0;
				const bool window_present = diagnostic_capture_active() && (diagnostic_present_index % 8) == 0;

				if (image && diagnostic_present_dumps < diagnostic_present_dump_budget &&
					(diagnostic_present_index < 6 || window_present || periodic_present))
				{
					dump_host_image(resources.recorder, resources.resources, image, "present", diagnostic_present_index, address);
					++diagnostic_present_dumps;
				}

				if (diagnostic_present_index < 4 && diagnostic_present_dumps < diagnostic_present_dump_budget)
				{
					for (u32 sample = 0; sample < diagnostic_sampled_count && diagnostic_present_dumps < diagnostic_present_dump_budget; ++sample)
					{
						// TODO(excise): level 5; sampled byte dump.
						MTL_NOTE(5, "Metal 4: readback at present %u, sample %u, address 0x%x, context %u",
							diagnostic_present_index, sample, diagnostic_sampled_address[sample],
							diagnostic_sampled_context[sample]);

						dump_host_image(resources.recorder, resources.resources, diagnostic_sampled_image[sample],
							"sample", diagnostic_present_index * 16u + sample, diagnostic_sampled_address[sample]);
						++diagnostic_present_dumps;
					}
				}
#endif // MTL_DEBUG_LOGS

				if (!image)
				{
					buffer_width = 0;
					buffer_height = 0;
				}
			}

			if (image && avconfig.stereo_enabled) [[unlikely]]
			{
				// The second eye sits below the first, separated by the RSX blanking
				// interval (VKPresent.cpp:565).
				const auto [unused, min_expected_height] = rsx::apply_resolution_scale<true>(
					RSX(m_ctx)->resolution_scaling_config, RSX_SURFACE_DIMENSION_IGNORED, static_cast<u32>(m_ctx->display_buffers[info.buffer].height) + 30);

				if (image->height() < min_expected_height)
				{
					const u32 image_offset = (m_ctx->display_buffers[info.buffer].height + 30) * buffer_pitch + m_ctx->display_buffers[info.buffer].offset;
					get_present_source(rsx::get_address(m_ctx, image_offset, CELL_GCM_LOCATION_LOCAL), buffer_width, buffer_height, buffer_pitch, av_format, 1, image2);
				}
			}
		}

		present_source source{ image, image2, buffer_width, buffer_height };

		// The capture reads the source before the output path scales it
		// (VKPresent.cpp:684).  Only a capture the user asked for happens in a
		// run that is not tracing; the periodic one is instrumentation.
#if MTL_DEBUG_LOGS
		// A bounded periodic capture runs beside the user's request, so a session
		// with no key press still yields frames to compare (anti-flood rule).
		static u32 diagnostic_captures = 0;
		const bool screenshot_requested = g_user_asked_for_screenshot.exchange(false);
		const bool user_asked_for_screenshot = screenshot_requested ||
			(++diagnostic_captures % 6000 == 0 && diagnostic_captures < 30000);

		// TODO(excise): level 5; the user's key press is the detector for a
		// transient defect.  The frame they photographed is already encoded, so
		// the window starts at the next frame; a defect that lasts longer than one
		// frame is then captured with its own unit state and sampled images, which
		// a clock-armed window would only reach by luck.
		if (screenshot_requested)
		{
			diagnostic_try_arm_capture("screenshot", true);
			diagnostic_tape_freeze();
		}
#else
		const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false);
#endif
		const bool user_is_recording = (g_recording_mode != recording_mode::stopped && m_frame->can_consume_frame());

		if (source.image && (user_asked_for_screenshot || user_is_recording))
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/capture");
			resources.output.request_capture(resources.recorder, resources.constants, source, user_asked_for_screenshot,
				g_cfg.video.record_with_overlays.get(), RSX(m_ctx)->overlay_manager());
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the surface the user's capture read.  The
		// periodic present dump does not reach the photographed frame, so without
		// this the screenshot can only be compared with a later, clean frame.
		// The dump runs after the capture is encoded, so it cannot perturb it.
		if (screenshot_requested && source.image)
		{
			const u32 shot = diagnostic_present_index;
			MTL_NOTE(5, "Metal 4: screenshot surface %u, %ux%u", shot,
				static_cast<u32>(source.image->width()), static_cast<u32>(source.image->height()));
			dump_host_image(resources.recorder, resources.resources, source.image, "shot", shot, 0);

			// The ring dump itself runs once, in the common pending path below,
			// so a screenshot and an automatic trigger share the same ordering:
			// the history before the current snapshot, then the forward burst.
		}

		// TODO(excise): level 5; the frozen tape.  The readback runs after the
		// screenshot dump so both see the same submitted stream; the slots before
		// the press are the frames the user was actually looking at.
		diagnostic_tape_flush(resources.recorder);
#endif

		// The CPU-reduction limits are re-evaluated on an emu flip only
		// (VKPresent.cpp:586-589, GLPresent.cpp:253-257).
		if (info.emu_flip)
		{
			RSX(m_ctx)->display_ctrl().evaluate_cpu_usage_reduction_limits();
			resources.present.update_vsync();
		}

		const u32 target_width = static_cast<u32>(std::max(0, m_frame->client_width()));
		const u32 target_height = static_cast<u32>(std::max(0, m_frame->client_height()));

		MTL::Texture* drawable = nullptr;

		// The drawable is acquired even when the present source is missing, so the
		// window shows this flip's black clear and not the previous image
		// (GLPresent.cpp:276-281, VKPresent.cpp:781-802).
		if (!info.skip_frame && target_width && target_height &&
			resources.present.acquire(resources.recorder, target_width, target_height, drawable))
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/compose");
			const areau display_area{0, 0, target_width, target_height};
			const areau aspect = (g_cfg.video.stretch_to_display_area.get() || !source.width || !source.height)
				? display_area
				: avconfig.aspect_convert_region({ source.width, source.height }, { target_width, target_height });

			MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
			MTL::RenderPassColorAttachmentDescriptor* attachment = pass->colorAttachments()->object(0);
			attachment->setTexture(drawable);
			// The area outside the aspect rectangle is the letterbox, and it is black
			// (GLPresent.cpp:279).
			attachment->setLoadAction(MTL::LoadActionClear);
			attachment->setStoreAction(MTL::StoreActionStore);
			attachment->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));

			if (g_cfg.video.debug_overlay)
			{
				// The overlay is a guest-facing view; the counters are the shared
				// frame statistics Vulkan and OpenGL already print
				// (VKPresent.cpp:922-985).  Metal has no cache counters of its own,
				// so the lines it cannot answer are left out rather than invented.
				rsx::overlays::set_debug_overlay_text(fmt::format(
					"Internal Resolution:      %s\n"
					"RSX Load:                 %3u%%\n"
					"draw calls: %17u\n"
					"submits: %20u\n"
					"draw call setup: %12lldus\n"
					"vertex upload time: %9lldus\n"
					"texture upload time: %8lldus\n"
					"draw call execution: %8lldus\n"
					"submit and flip: %12lldus\n"
					"Program cache lookup ellision: %u/%u",
					info.stats.framebuffer_stats.to_string(RSX(m_ctx)->resolution_scaling_config, !m_config.supports_hw_msaa),
					RSX(m_ctx)->get_load(), info.stats.draw_calls, info.stats.submit_count,
					static_cast<long long>(info.stats.setup_time),
					static_cast<long long>(info.stats.vertex_upload_time),
					static_cast<long long>(info.stats.textures_upload_time),
					static_cast<long long>(info.stats.draw_exec_time),
					static_cast<long long>(info.stats.flip_time),
					info.stats.program_cache_lookups_ellided, info.stats.program_cache_lookups_total));
			}

			if (MTL4::RenderCommandEncoder* encoder = resources.recorder.open_pass(pass))
			{
				// The output tier samples images that the frame's earlier encoders
				// wrote, and it is not an RSX pass, so the scheduler does not hold
				// this edge.  The read is a later encoder, so the queue form
				// applies (HLD-03 §2.1 item 6).
				emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });

				resources.output.compose(encoder, resources.constants, drawable, source, aspect, display_area,
					avconfig.gamma, !g_cfg.video.full_rgb_range_output.get(), avconfig.stereo_enabled, RSX(m_ctx)->overlay_manager());
			}
			else
			{
				// The output pass could not open, so the drawable stays empty.  It is
				// still presented, because a drawable the queue signalled must reach
				// the display or the layer keeps it.
				rsx_log.error("Metal 4: the output pass could not open, so the frame presents empty");
			}

			pass->release();
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/composed", 1);
		}
		else
		{
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/no acquire", 1);
		}

		// Present submits the frame and presents the drawable it holds, so a frame
		// that acquired one always presents it and the ring advances.
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/present");
			resources.present.present(resources.recorder);
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 1; the watchdog's clock.  It advances after the
		// present, so a stall inside the acquire or the submit is the gap it sees.
		diagnostic_watchdog_last_flip = probe::now_ns();
#endif

		// The overlays the frame retired lose their temporary images here, and the
		// manager then frees them (GLPresent.cpp:283).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/retire");
			resources.output.retire_overlays(RSX(m_ctx)->overlay_manager());

			// The cache retires the temporary subresources and the predictor state of
			// the frame that just ended (GLPresent.cpp:535).
			resources.textures.on_frame_end();
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the rings of the frame that just presented.  The
		// recorder has submitted, but the heap still holds this frame's slot until
		// the next open_frame, so the bytes the GPU read are still here.  The ring
		// keeps twelve frames, which is what lets the next key press reach back to
		// the frame the user was looking at; the forward burst then covers the
		// frames that follow it.
		//
		// The dump starts before this frame's snapshot: the twelve entries it
		// writes as m12..m1 are then the frames before the trigger, and
		// `continue_dump` writes the trigger frame itself as p1.  A trigger that
		// armed in the draw path reaches here through the pending flag.
		if (diagnostic_capture_dump_pending)
		{
			diagnostic_capture_dump_pending = false;
			resources.constants.begin_dump(diagnostic_present_index);
		}

		resources.constants.snapshot_frame(diagnostic_present_index);
		resources.constants.continue_dump();
#endif

		m_frame_open = false;

		m_ctx->stats.flip_time = m_profiler.duration();
		// TODO(excise): level 1; frame boundary.
		MTL_PERF_FRAME(1);

		// Window bookkeeping only; the present itself happened above.  A skipped
		// frame is dropped, not presented late (GLPresent.cpp:174-178).
		m_frame->flip(m_context, info.skip_frame);

		return drawable != nullptr;
	}

	void MTLGSRender::post_present()
	{
	}

	rsx::device_status MTLGSRender::check_device_status() const
	{
		return (m_device && m_device->lost_flag().load(std::memory_order_acquire))
			? rsx::device_status::lost
			: rsx::device_status::ok;
	}

	std::unique_ptr<rsx::context_resources> MTLGSRender::create_context_resources(rsx::context*)
	{
		// The Metal caches are device-scoped and owned by the backend, so a context
		// carries no resources of its own.
		return std::make_unique<rsx::context_resources>();
	}

	void MTLGSRender::on_release_context_resources(rsx::context*)
	{
	}

	void MTLGSRender::on_bind_context(rsx::context* ctx)
	{
		if (!m_frame_resources)
		{
			return;
		}

		// The caches are shared by every context, so the bound one moves with the
		// switch.  The attachments belong to the previous context's layout.
		m_frame_resources->vertex_layout.set_context(ctx);
		m_frame_resources->cache.set_context(ctx);
		m_frame_resources->textures.set_context(ctx);
		m_frame_resources->attachments.reset();
	}

	void MTLGSRender::on_unbind_context(rsx::context*)
	{
		if (m_frame_resources)
		{
			m_frame_resources->attachments.reset();
		}
	}

	rsx::reports::occlusion_query_provider* MTLGSRender::occlusion_queries()
	{
		return m_frame_resources ? &m_frame_resources->zcull_control : nullptr;
	}

	void MTLGSRender::patch_transform_constants(rsx::context* ctx, u32, u32)
	{
		if (ctx != m_ctx)
		{
			return;
		}

		// The group is the upload trigger; emit_geometry reads it before each draw.
		m_ctx->draw_state.dirty.set(rsx::draw_state_t::group::transform_constants);
	}

	bool MTLGSRender::is_current_program_interpreted() const
	{
		return m_frame_resources && m_frame_resources->interpreter_active;
	}

	std::string MTLGSRender::logical_id() const
	{
		if (!m_device || !m_device->dev() || !m_device->dev()->name())
		{
			return "Metal | NULL";
		}

		return "Metal | " + std::string(m_device->dev()->name()->utf8String());
	}

	std::string MTLGSRender::configured_logical_id() const
	{
		if (rsx::resolve_render_backend_type(g_cfg.video.renderer) != video_renderer::metal)
		{
			return "";
		}

		return logical_id();
	}

	// A mid-frame wait must submit: the guest can wait on a label that only the
	// GPU can release, and the GPU cannot progress on a frame that has not been
	// committed (HLD-04 §4.3).
	void MTLGSRender::on_semaphore_acquire_wait()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/semaphore wait");

		// This path is a guest spin, so it fires millions of times a session.  It is
		// counted and classified and it never prints a line: a trace line here
		// produced three quarters of one measured log (anti-flood rule).
#if MTL_PERF_ENABLED
		// TODO(excise): level 2; the wait count and the flush depth at the wait.
		{
			const u32 pending_flushes = m_flush_queue ? static_cast<u32>(m_flush_queue->pending.size()) : 0u;

			MTL_PROBE_TOTAL(2, "sync/semaphore waits", 1);
			// The key is the flush depth, capped at four: key 4 means four or more.
			MTL_PROBE_HISTOGRAM(2, "sync/semaphore wait flush depth", pending_flushes < 4 ? pending_flushes : 4, "pending flushes");
			if (m_frame_open)
			{
				MTL_PROBE_LATCH(2, "sync/semaphore wait/frame open");
			}
			else
			{
				MTL_PROBE_LATCH(2, "sync/semaphore wait/frame closed");
			}
		}
#endif // MTL_PERF_ENABLED

		// A guest waiting on a label waits on a write that is still being recorded, so
		// the frame commits here.  The commit does not wait: the guest's own wait is
		// the wait, and the slot comes back on the next encoder (HLD-04 §4.3).
		if (m_frame_open && m_frame_resources->recorder.label_writes_pending())
		{
			// TODO(excise): level 2; a guest wait that made the frame commit.
			MTL_PROBE_LATCH(2, "sync/semaphore wait/label commit");
			m_frame_resources->recorder.commit_pending_labels();
		}

		// A pending flush request, or an emu flip the guest is waiting on, runs the
		// local task here (GLGSRender.cpp:1328-1335, VKGSRender.cpp:1136-1144).  The
		// wait does not submit or close the frame: the frame stays open until its
		// flip presents it, because a flip that finds the frame closed is a frozen
		// image (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if ((m_flush_queue && !m_flush_queue->pending.empty()) ||
			RSX(m_ctx)->display_ctrl().has_flip_request(rsx::thread::flip_request::emu_requested))
		{
			RSX(m_ctx)->do_local_task(rsx::FIFO::state::lock_wait);
		}
	}

	// A back-end semaphore release publishes to guest memory.  Metal has no host
	// GPU label path, so the CPU write must follow the GPU work it releases.  The
	// current slot commits and returns; its later commit proves every earlier
	// commit complete because the queue is serialized (MTLCommandRecorder Q4).
	// Without this wait the guest can recycle the FIFO while the GPU still reads
	// it, and the RSX thread then dies with a dead FIFO queue (LOG-FINDINGS L11).
	// The FIFO desync capture (RR7-FREEZE-FORENSICS.md §8.1).  The shared FIFO
	// gathers the data; the backend prints it, through the probe facility so the
	// bulk text stays out of the shared log and the run's cost is nil at level 0.
	// TODO(excise): whole-unit; level 2; the hook and the print go with the probe work.
	void MTLGSRender::on_fifo_desync(const rsx::fifo_desync_info& info)
	{
		if (!mtl::probe::at_least(2))
		{
			return;
		}

		// The method index the offending word would name, and whether the method
		// table knows it.  A word that names an undefined method is a data word
		// that the host took for a command (L52), and the index says which
		// register range the host was walking when it went wrong.
		const u32 reg = (info.last_command & 0xffff) >> 2;

		std::string words;
		if (info.words_valid)
		{
			for (u32 index = 0; index < 8; ++index)
			{
				words += fmt::format(" %08x", info.words[index]);
			}
		}
		else
		{
			words = " <unmapped>";
		}

		// The commands the host dispatched before the error.  The one that precedes
		// the corruption is the command whose argument block was mis-counted, and
		// a bulk write is the only command that carries a variable-length data
		// block.  The list is not filtered by the jump target: the target can be a
		// wild address, and the interesting range is the restore point onwards.
		std::string history;
		u32 printed = 0;

		for (u32 index = 0; index < info.history_count; ++index)
		{
			const rsx::dispatch_entry& entry = info.history[index];

			history += fmt::format(" [reg 0x%x value 0x%x cmd 0x%x at 0x%x]", entry.reg, entry.value, entry.cmd, entry.position);
			printed++;
		}

		MTL_NOTE(2, "Metal 4: FIFO desync capture: get=0x%x put=0x%x get_guest=0x%x restore_point=0x%x code_start=0x%x ret_addr=0x%x last_label=0x%x/0x%x label_in_ring=%u label_offset=0x%x cmd=0x%x method_index=0x%x method_known=%u words:%s",
			info.get, info.put, info.get_guest, info.restore_point, info.code_start, info.ret_addr,
			info.last_label_address, info.last_label_value, info.label_in_ring ? 1u : 0u, info.label_offset,
			info.last_command, reg, info.last_command_method_known ? 1u : 0u, words.c_str());

		MTL_NOTE(2, "Metal 4: FIFO desync capture: %u dispatched commands:%s",
			printed, history.c_str());

		// TODO(excise): level 2; the cache snapshot against the ring at the same
		// offsets.  The host parses out of the cache copy, so a cached word that
		// differs from the ring word at the same position is a stale read: the
		// guest rewrote that part of the ring after the snapshot was taken.
		{
			std::string cache;
			std::string live;

			for (u32 index = 0; index < 16; ++index)
			{
				cache += info.cache_words_valid[index] ? fmt::format(" %08x", info.cache_words[index]) : std::string(" --------");

				// The ring dump starts at GET - 0x3fc and ends at GET, so only the
				// positions up to GET have a live word to compare with.
				if (index <= 8)
				{
					live += fmt::format(" %08x", info.words_before[8 - index]);
				}
				else
				{
					live += " --------";
				}
			}

			MTL_NOTE(2, "Metal 4: FIFO desync cache: addr=0x%x size=0x%x next=0x%x %08x:%s",
				info.cache_addr, info.cache_size, info.next_cmd_entry, info.get - 32, cache.c_str());
			MTL_NOTE(2, "Metal 4: FIFO desync ring  %08x:%s", info.get - 32, live.c_str());
		}

		// TODO(excise): level 2; the ring words that lead up to the failing one,
		// oldest line first.  A command whose argument count ran long leaves its
		// header in this window, so the header can be decoded against the stream
		// the guest actually wrote.
		if (info.words_before_valid)
		{
			for (u32 line = 0; line < 8; ++line)
			{
				const u32 first = 255 - line * 32;
				std::string row = fmt::format("Metal 4: FIFO desync ring 0x%x:", info.get - first * 4);

				for (u32 step = 0; step < 32; ++step)
				{
					row += fmt::format(" %08x", info.words_before[first - step]);
				}

				MTL_NOTE(2, "%s", row.c_str());
			}
		}
	}

	bool MTLGSRender::sync_label(u32 address)
	{
		// The recorder owns the label addresses and the commits that carry them.
		// Without frame resources there is no GPU label write to wait for.
		if (!m_frame_resources)
		{
			return false;
		}

		return m_frame_resources->recorder.sync_label(address);
	}

	bool MTLGSRender::release_GCM_label(u32 type, u32 address, u32 data)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/label release");

		if (!m_frame_resources)
		{
			return false;
		}

		// A label is the only signal the GPU gives the guest, so the GPU performs the
		// write, in stream order.  A CPU write cannot be ordered against the work the
		// release hands back, and making the RSX thread wait for the GPU at every
		// release keeps the frame rate at the GPU's rate (L38, L129).
		open_frame();

		// The guest word is big-endian, so the copy moves the bytes a CPU store would
		// (VKGSRender.cpp:1726).
		const u32 value = std::bit_cast<u32, be_t<u32>>(data);

		// TODO(excise): level 2; the boot producer waits on this word.
		MTL_NOTE(2, "sync/label release type=0x%x addr=0x%x value=0x%x open=%d\n",
			type, address, value, m_frame_open ? 1 : 0);

		if (m_frame_open && m_frame_resources->recorder.write_label(address, value))
		{
			// A back-end release is waited on by the guest without an RSX
			// acquire, and the next encoder boundary may never arrive while the
			// FIFO is parked.  Submit the batch now, without waiting, as VK does
			// with its label command buffer (VKGSRender.cpp:1744-1759); the slot
			// comes back at the next encoder.
			if (type == NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE)
			{
				m_frame_resources->recorder.commit_pending_labels();
			}

			return true;
		}

		// The GPU cannot reach the address, so the caller stores it after the GPU has
		// stopped reading the range.
		if (m_frame_open)
		{
			m_frame_resources->recorder.flush_for_readback();
		}
		else if (!m_frame_resources->recorder.wait_idle())
		{
			rsx_log.error("Metal 4: a label release found GPU work in flight after the feedback timeout");
		}

		// TODO(excise): level 2; the label releases that still drain the GPU.
		MTL_PROBE_LATCH(2, "sync/label release drained");

		return false;
	}

	// The flush runs here, on the RSX thread, because it needs an encoder and the
	// fault that asked for it runs on a guest thread (GLGSRender.cpp:1329-1350).
	// A request is released by the faulting thread once it has consumed its result,
	// so the list keeps the requests that are still in flight.
	bool MTLGSRender::on_local_task(rsx::FIFO::state)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "local task");
		if (m_flush_queue && m_frame_resources)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "local task/drain queue");

			std::lock_guard lock(m_flush_queue->guard);

			// A request leaves the queue once its owner has stopped waiting and the
			// RSX thread has serviced it.  A request with no flush work is never
			// waited on, so its `received` is set at the post; the `processed` term
			// keeps it until the loop below has applied its sampler flags.
			m_flush_queue->pending.remove_if([](const flush_queue::request& q) { return q.received && q.processed; });
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "local task/pending requests", m_flush_queue->pending.size());

			for (flush_queue::request& request : m_flush_queue->pending)
			{
				if (request.processed)
				{
					continue;
				}

				// The request may have arrived between two frames, and the recorder
				// has no command buffer open while the ring slot is idle.
				{
					// TODO(excise): level 1; block timer.
					MTL_PERF_SCOPE(1, "local task/drain/open frame");
					open_frame();
				}

				{
					// TODO(excise): level 1; block timer.
					MTL_PERF_SCOPE(1, "local task/drain/flush all");
					m_frame_resources->textures.flush_all(m_frame_resources->recorder, request.sections);
				}

				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "local task/requests serviced", 1);

				if (request.invalidate_samplers)
				{
					// A cached image became invalid, so every unit re-resolves on its
					// next draw (VKGSRender.cpp:1028).  The flags are the RSX thread's
					// to write, because the faulting thread cannot touch them.
					invalidate_samplers();
					// TODO(excise): level 1; running total.
					MTL_PERF_COUNT(1, "local task/sampler invalidations", 1);
				}

				request.processed = true;
			}
		}

		// The thread owns the flips, the invalidations and the native-UI path; the
		// backend only drains its deferred work.
		return true;
	}

	// A guest write to a range a surface owns invalidates that surface, because
	// the guest bytes are canonical and the host copy is now stale
	// (VKGSRender.cpp:1483).
	bool MTLGSRender::scaled_image_from_memory([[maybe_unused]] const rsx::blit_src_info& src, [[maybe_unused]] const rsx::blit_dst_info& dst, [[maybe_unused]] bool interpolate)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "blit/scaled image");

		// TODO(excise): level 2; the transfer stream and its formats (area H).
		if (interpolate)
		{
			MTL_PROBE_LATCH(2, "transfer/interpolated");
		}
		else
		{
			MTL_PROBE_LATCH(2, "transfer/direct");
		}
		MTL_PROBE_HISTOGRAM(2, "transfer source format", static_cast<u32>(src.format), "gcm source format");
		MTL_PROBE_HISTOGRAM(2, "transfer destination format", static_cast<u32>(dst.format), "gcm destination format");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first transfers of the run.
		static u32 diagnostic_transfers = 0;

		if (diagnostic_transfers < 16)
		{
			MTL_NOTE(3, "Metal 4: blit %u, src 0x%x %ux%u pitch %u bpp %u fmt %u dma 0x%x, dst 0x%x %ux%u pitch %u bpp %u fmt %u dma 0x%x, clip %ux%u, scale %.2f/%.2f, inter %u",
				diagnostic_transfers, src.rsx_address, src.width, src.height, src.pitch, src.bpp,
				static_cast<u32>(src.format), src.dma,
				dst.rsx_address, dst.width, dst.height, dst.pitch, dst.bpp,
				static_cast<u32>(dst.format), dst.dma,
				dst.clip_width, dst.clip_height, static_cast<f64>(dst.scale_x), static_cast<f64>(dst.scale_y),
				interpolate ? 1u : 0u);
		}

		diagnostic_transfers++;
#endif // MTL_DEBUG_LOGS

		// Metal has no 2D-engine path for this transfer, so the shared code falls
		// back to a CPU copy (nv3089.cpp:599-603).  That copy reads and writes guest
		// memory directly, bypassing the cache's protection.  Materialize the source
		// sections first, then drop the destination sections: otherwise the next
		// sampler reads the stale host copy of the destination (the black Xevious
		// panel was this path).  A GPU blit is still the cheaper route (SF8/SF11).
		if (m_frame_resources && src.rsx_address && src.pitch && src.height)
		{
			const u32 src_length = src.pitch * static_cast<u32>(src.height);
			m_frame_resources->textures.invalidate_range(m_frame_resources->recorder,
				utils::address_range32::start_length(src.rsx_address, src_length),
				rsx::invalidation_cause::read);
		}

		if (m_frame_resources && dst.rsx_address && dst.pitch && dst.clip_height)
		{
			const u32 dst_length = dst.pitch * static_cast<u32>(dst.clip_height);
			m_frame_resources->textures.invalidate_range(m_frame_resources->recorder,
				utils::address_range32::start_length(dst.rsx_address, dst_length),
				rsx::invalidation_cause::write);
		}

		return false;
	}

	void MTLGSRender::write_barrier(u32 memory_address, u32 memory_range)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/write barrier");
		if (!m_frame_resources)
		{
			return;
		}

		// TODO(excise): level 2; the write-barrier stream and a coarse size bucket
		// (area J).  The bucket is 0 for a range up to 256 bytes, 1 to 4 KiB, 2 to
		// 64 KiB and 3 above.
		MTL_PROBE_LATCH(2, "sync/write barrier");
		MTL_PROBE_HISTOGRAM(2, "sync/write barrier size",
			memory_range <= 256 ? 0 : memory_range <= 4096 ? 1 : memory_range <= 65536 ? 2 : 3, "barrier range bucket");

#if MTL_DEBUG_LOGS
		static u32 diagnostic_barriers = 0;

		// TODO(excise): level 3; the first write barriers of the run.
		if (diagnostic_barriers++ < 16)
		{
			MTL_NOTE(3, "Metal 4: write_barrier 0x%x + 0x%x", memory_address, memory_range);
		}
#endif // MTL_DEBUG_LOGS

		m_frame_resources->cache.invalidate_range(utils::address_range32::start_length(memory_address, memory_range));
	}

	// A guest access to a locked page invalidates the sections that overlap it
	// (VKGSRender.cpp:1277).  A fault on a guest thread is recorded and queued for
	// the RSX thread, which owns the encoders.  A fault on the RSX thread itself
	// cannot be queued for that thread, so it flushes in place
	// (GLGSRender.cpp:1281).
	bool MTLGSRender::on_access_violation(rsx::context* ctx, u32 address, bool is_writing)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "fault");
		if (ctx != m_ctx || !m_frame_resources)
		{
			return false;
		}

		// TODO: the RSX thread faults on this page while it owns it, every frame,
		// and the access that reaches a protected page is still to be found.  A
		// wait for this thread stalls the whole run ("Thread [rsx::thread] is too
		// sleepy"), so the flush runs in place until the access is known.
		const bool can_flush = RSX(m_ctx)->is_current_thread();

		if (can_flush)
		{
			// The fault of the RSX thread is the one that stalls the run, so it is
			// always reported; a fault of a guest thread is routine and handled by
			// the deferred path below.
			rsx_log.notice("Metal 4: access violation 0x%x, writing %u, on the RSX thread", address, is_writing ? 1u : 0u);
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/on RSX thread", 1);
		}
		else
		{
			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: access violation 0x%x, writing %u", address, is_writing ? 1u : 0u);
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/on guest thread", 1);
		}

		const rsx::invalidation_cause cause = is_writing
			? (can_flush ? rsx::invalidation_cause::write : rsx::invalidation_cause::deferred_write)
			: (can_flush ? rsx::invalidation_cause::read : rsx::invalidation_cause::deferred_read);

		if (can_flush)
		{
			if (is_writing)
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/direct write", 1);
			}
			else
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/direct read", 1);
			}
		}
		else
		{
			if (is_writing)
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/deferred write", 1);
			}
			else
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/deferred read", 1);
			}
		}

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/mm flush");
			rsx::mm_flush(address);
		}

		auto result = [&] {
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/invalidate address");
			return m_frame_resources->textures.invalidate_address(m_frame_resources->recorder, address, cause);
		}();

		if (!result.violation_handled)
		{
			// The fault is not the texture cache's; the thread then asks ZCULL.
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/unhandled", 1);
			return false;
		}

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/handled", 1);
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/flushable sections", result.num_flushable);
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/sections to flush", result.sections_to_flush.size());
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/sections to unprotect", result.sections_to_unprotect.size());

		if (can_flush)
		{
			// The flush ran on this call, so nothing is left to wait for, and the
			// flags below are this thread's to write in any case.
			if (result.invalidate_samplers)
			{
				invalidate_samplers();
			}

			return true;
		}

		const bool should_invalidate_samplers = result.invalidate_samplers;

		// The deferred path keeps sections_to_flush only when the RSX thread has to
		// write them back; every other handled fault unprotects in place, so an empty
		// set means the page is already writable and the faulting instruction can be
		// retried at once (texture_cache.h:1329-1351).
		const bool flush_pending = !result.sections_to_flush.empty();

		flush_queue::request* request = nullptr;
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/enqueue");
			std::lock_guard lock(m_flush_queue->guard);

			auto& queued = m_flush_queue->pending.emplace_back();
			queued.sections = std::move(result);
			queued.invalidate_samplers = should_invalidate_samplers;
			queued.received = !flush_pending;
			request = &queued;

			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "fault/queue depth", m_flush_queue->pending.size());
		}

		RSX(m_ctx)->raise_backend_interrupt();
		vm::temporary_unlock();

		if (flush_pending)
		{
			// The page keeps its protection until the flush reaches guest memory, so
			// the faulting instruction must not be retried before the request
			// completes (GLGSRender.cpp:1298-1307).
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/producer wait");
			request->producer_wait();
		}
		else
		{
			// Nothing is written back, so the request carries only the sampler flags,
			// which the RSX thread reads on its next pass.  This thread does not wait:
			// the wait would be a whole frame of the guest thread at a frame rate the
			// RSX thread already paces.
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/not waited", 1);
		}

		return true;
	}

	// The guest unmapped a range, so every host copy of it is invalid
	// (VKGSRender.cpp:1312).
	void MTLGSRender::on_invalidate_memory_range(rsx::context* ctx, const utils::address_range32& range, rsx::invalidation_cause cause)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "invalidate range");
		if (ctx != m_ctx || !m_frame_resources)
		{
			return;
		}

		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: invalidate memory range");

		const auto data = m_frame_resources->textures.invalidate_range(m_frame_resources->recorder, range, cause);

		if (cause == rsx::invalidation_cause::unmap && data.violation_handled)
		{
			m_frame_resources->textures.purge_unreleased_sections();
		}
	}

	// A tile is unbound when the guest re-tiles memory, so every cached section
	// may have been re-laid-out underneath it.  The units re-resolve on their
	// next draw (VKGSRender.cpp:1407).
	void MTLGSRender::notify_tile_unbound(u32 /*tile*/)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "tile unbound");
		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: tile unbound, every unit re-resolves");

		invalidate_samplers();
	}

	void MTLGSRender::invalidate_samplers()
	{
		m_ctx->textures.fragment_textures_dirty = (1u << rsx::limits::fragment_textures_count) - 1;
		m_ctx->textures.vertex_textures_dirty = (1u << rsx::limits::vertex_textures_count) - 1;
	}

	void MTLGSRender::on_unmap_memory_range(const utils::address_range32&)
	{
		// The purge lives in on_invalidate_memory_range, where the thread delivers
		// an unmap cause.  Metal has no DMA-side unmap.
	}
}

#endif // RSX_ENABLE_METAL4
