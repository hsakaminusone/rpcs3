#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLInstrumentation.h"

#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/cache_utils.hpp"
#include "Emu/system_config.h"
#include "util/logs.hpp"

#include <cstdlib>
#include <filesystem>
#include <sys/sysctl.h>

namespace mtl
{
	// The text of a framework error, with the domain and code, because a failure
	// can carry an error object whose description is empty.
	static std::string archive_error_text(NS::Error* error)
	{
		if (!error)
		{
			return "no error object";
		}

		const char* domain = error->domain() ? error->domain()->utf8String() : "?";
		const char* text = error->localizedDescription() ? error->localizedDescription()->utf8String() : "?";
		return fmt::format("%s (%s, code %lld)", text, domain, static_cast<long long>(error->code()));
	}

	u32 macos_major()
	{
		char version[16]{};
		size_t length = sizeof(version);
		if (sysctlbyname("kern.osproductversion", version, &length, nullptr, 0) != 0)
		{
			return 0;
		}

		const auto dot = std::strchr(version, '.');
		const std::string major = dot ? std::string(version, dot - version) : std::string(version);
		const int value = std::atoi(major.c_str());
		return value > 0 ? static_cast<u32>(value) : 0u;
	}

	bool metal4_supported()
	{
		if (__builtin_available(macOS 26.0, *))
		{
			return true;
		}

		rsx_log.error("Metal 4 requires macOS %u or newer on Apple silicon. Host reported: %u", metal4_api_floor, macos_major());
		return false;
	}

	[[noreturn]] void die(const std::string& err)
	{
		rsx_log.fatal("%s", err);
		fmt::throw_exception("Metal 4: %s", err);
	}

	std::unique_ptr<device> device::create()
	{
		auto dev = std::unique_ptr<device>(new device());
		if (!dev->init())
		{
			return nullptr;
		}

		return dev;
	}

	device::~device()
	{
		if (m_removal_observer)
		{
			NS::NotificationCenter::defaultCenter()->removeObserver(m_removal_observer);
			m_removal_observer = nullptr;
		}
	}

	// TODO(excise): level 1; one warning for each user setting this backend does not
	// read, and one coverage key for each.  A setting that is on and ignored is a
	// silent difference from the reference backends, so it must be visible.
	void report_ignored_settings()
	{
		struct ignored
		{
			const char* name;
			bool set;
		};

		const ignored settings[] =
		{
			// These name a facility the backend does not have, or a workaround that
			// is already the permanent behaviour.  The rest of the old list is read
			// by shared RSX code (readback, strict rendering, ZCULL sync) or is now
			// wired into the Metal path (disable_msl_fast_math), so warning about it
			// would be false.
			{ "disable_hardware_texel_remapping", g_cfg.video.disable_hardware_texel_remapping.get() },
			{ "force_high_precision_z_buffer", g_cfg.video.force_high_precision_z_buffer.get() },
			{ "disable_vertex_cache", g_cfg.video.disable_vertex_cache.get() },
		};

		for (u32 index = 0; index < ::size32(settings); ++index)
		{
			if (!settings[index].set)
			{
				continue;
			}

			// TODO(excise): level 1; a setting the backend ignores is a silent
			// behaviour difference, and level 2 classifies which one.
			MTL_WARN(1, "Metal 4: setting %s is ignored by this backend", settings[index].name);
			MTL_PROBE_HISTOGRAM(2, "setting ignored", index, settings[index].name);
		}
	}

	bool device::init()
	{
		if (!metal4_supported())
		{
			return false;
		}

#if MTL_DIAGNOSTICS
		// TODO(excise): level 1; the boot banner and the channel raise.
		probe::apply_log_level();

		// TODO(revert): while a probe level is set, drop the HLE channels the titles
		// under validation flood.  Ridge Racer 7 alone emits ~318k sys_spu/sys_event
		// lines and, at the FMV handoff, a PAMF demuxer spin of ~97k cellDmuxPamf
		// lines in a few seconds, which buries the probe output; an error still
		// prints.  This is a probe-run convenience and not part of the backend, so it
		// goes when the probe work ends.
		if (probe::at_least(1))
		{
			logs::set_level("sys_spu", logs::level::error);
			logs::set_level("sys_event", logs::level::error);
			logs::set_level("cellDmux", logs::level::error);
			logs::set_level("cellDmuxPamf", logs::level::error);
			logs::set_level("cellVdec", logs::level::error);
		}
#endif

		m_device = MTL::CreateSystemDefaultDevice();
		if (!m_device)
		{
			rsx_log.error("No usable Metal device was found.");
			return false;
		}

		if (!m_device->supportsFamily(MTL::GPUFamily::GPUFamilyMetal4))
		{
			const std::string gpu_name = m_device->name() ? m_device->name()->utf8String() : "unknown";
			rsx_log.error("Metal 4 is not supported on this GPU: %s", gpu_name);
			return false;
		}

		// A device removal cannot be recovered by this backend.  The observer only
		// stops new shader submissions; the existing RSX fatal path stops the frame
		// loop.  The handler must not touch any cache or release a compiler object.
		NS::ObserverFunction removal_handler = [this](NS::Notification*)
		{
			m_lost.store(true, std::memory_order_release);
		};
		m_removal_observer = NS::NotificationCenter::defaultCenter()->addObserver(
			MTL::DeviceWasRemovedNotification, nullptr, nullptr, removal_handler);

		m_queue = m_device->newMTL4CommandQueue();
		if (!m_queue)
		{
			die("MTL4 command queue creation failed");
		}

		// The L3 driver pipeline cache (HLD-07 §4.2).  The archive path carries the
		// GPU and the OS version, because an archive is a device slice (06 §2.4).
		if (const std::string cache_root = rpcs3::cache::get_ppu_cache();
			!g_cfg.video.disable_on_disk_shader_cache && !cache_root.empty())
		{
			m_archive_path = fmt::format("%sshaders_cache/metal/archive-%llx-%u.bin",
				cache_root, static_cast<unsigned long long>(m_device->registryID()), macos_major());
		}

		MTL4::CompilerDescriptor* descriptor = MTL4::CompilerDescriptor::alloc()->init();

		// The serializer is what records the compiled pipelines.  It is created on
		// a cold start too, or nothing would ever be written.
		if (!m_archive_path.empty())
		{
			MTL4::PipelineDataSetSerializerDescriptor* serializer_descriptor = MTL4::PipelineDataSetSerializerDescriptor::alloc()->init();
			// CaptureBinaries alone.  The archive holds binaries, and asking for
			// the descriptors as well makes the flush fail with no error at all
			// (HLD-07 §4.2).
			serializer_descriptor->setConfiguration(MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
			m_serializer = m_device->newPipelineDataSetSerializer(serializer_descriptor);

			if (m_serializer)
			{
				descriptor->setPipelineDataSetSerializer(m_serializer);
			}
			else
			{
				rsx_log.error("Metal: the pipeline data set serializer was not created. The driver pipeline cache is off.");
			}
		}

		// One compiler per device (docs/research/metal4/06 §2.5; workaround W11:
		// concurrent MTL4Compiler instances corrupt the heap on macOS 26).
		m_compiler = m_device->newCompiler(descriptor, nullptr);
		if (!m_compiler)
		{
			die("MTL4 compiler creation failed");
		}

		open_pipeline_archive();

		m_features.metal4_family = true;
		m_features.compiler_ok = true;

		import_guest_window();

#if MTL_DIAGNOSTICS
		// TODO(excise): level 2; the session classifier.  The device and the archive
		// state decide whether a run is a cold start or a warm start.
		MTL_PROBE_LATCH(2, "session/device created");
		report_ignored_settings();
#endif

		rsx_log.success("Metal 4 device initialized: %s", m_device->name() ? m_device->name()->utf8String() : "unknown");
		return true;
	}

	bool device::open_pipeline_archive()
	{
		if (m_archive_path.empty())
		{
			// TODO(excise): level 2; the on-disk shader cache is off.
			MTL_PROBE_LATCH(2, "session/archive off");
			return false;
		}

		std::error_code error_code;
		if (!std::filesystem::exists(m_archive_path, error_code))
		{
			// TODO(excise): level 2; a cold start, so the archive has no shaders.
			MTL_PROBE_LATCH(2, "session/archive absent");
			return false;
		}

		NS::Error* error = nullptr;
		m_archive = m_device->newArchive(NS::URL::fileURLWithPath(NS::String::string(m_archive_path.c_str(), NS::UTF8StringEncoding)), &error);
		if (!m_archive)
		{
			// A rejected archive is a cold start, not a failure (HLD-07 §7).
			// TODO(excise): level 2; the archive was rejected.
			MTL_PROBE_LATCH(2, "session/archive rejected");
			rsx_log.error("Metal: the pipeline archive %s was rejected: %s", m_archive_path, archive_error_text(error));
			return false;
		}

		m_task_options = MTL4::CompilerTaskOptions::alloc()->init();
		m_task_options->setLookupArchives(NS::Array::array(m_archive));
		// TODO(excise): level 2; a warm start, so the archive supplies shaders.
		MTL_PROBE_LATCH(2, "session/archive loaded");
		rsx_log.notice("Metal: pipeline archive loaded from %s", m_archive_path);
		return true;
	}

	void device::flush_pipeline_archive()
	{
		if (!m_serializer || m_archive_path.empty())
		{
			return;
		}

		// TODO(excise): level 4; block timer.
		MTL_PERF_SCOPE(4, "archive/flush");

		std::error_code error_code;
		std::filesystem::create_directories(std::filesystem::path(m_archive_path).parent_path(), error_code);

		NS::Error* error = nullptr;
		if (!m_serializer->serializeAsArchiveAndFlushToURL(NS::URL::fileURLWithPath(NS::String::string(m_archive_path.c_str(), NS::UTF8StringEncoding)), &error))
		{
			// The flush is destructive, so a second call reports "nothing to
			// serialize"; this path must stay the only one (HLD-07 §4.2).
			rsx_log.error("Metal: the pipeline archive %s was not written: %s", m_archive_path, archive_error_text(error));
			return;
		}

		// TODO(excise): level 2; the archive was written at exit.
		MTL_PROBE_LATCH(2, "session/archive written");
		rsx_log.notice("Metal: pipeline archive written to %s", m_archive_path);
	}

	void device::import_guest_window()
	{
		// HLD-02 §2.1.  Import the unprotected mirror, not the guest view: the no-copy
		// import requires every page of the range writable (Metal returns nil otherwise)
		// and the guest view carries protection::no pages.  The mirror aliases the same
		// guest RAM (vm.cpp:846) and is how the renderers reach guest memory
		// (vm::get_super_ptr).  The emulator owns the region, so the deallocator must
		// not free it.
		m_window.buffer = m_device->newBuffer(vm::g_sudo_addr, guest_address_space, MTL::ResourceStorageModeShared, nullptr);
		if (!m_window.buffer)
		{
			die(fmt::format("guest memory import failed (%llu bytes)", guest_address_space));
		}

		m_window.gpu_address = m_window.buffer->gpuAddress();
	}

	void device::fill_backend_config(rsx::backend_configuration& cfg) const
	{
		// Docs: no native multi-draw command (docs/research/metal4/09 §4); a
		// host-built indirect batcher lands later, until then draw clauses loop.
		cfg.supports_multidraw = false;
		cfg.supports_hw_msaa = true;
		cfg.supports_hw_a2c = true;
		cfg.supports_hw_a2c_1spp = false; // unverified at 1 spp; GL disables it too (GLGSRender.cpp:61)
		cfg.supports_hw_a2one = true;
		cfg.supports_hw_instanced_rendering = true;
		cfg.supports_programmable_blending = true; // framebuffer fetch [[color(n)]] (HLD-01 §4.3)
		cfg.supports_normalized_barycentrics = true;
		// Metal validates `MTLViewport.znear/zfar` to [0,1] and has no unrestricted
		// depth range, so the general clip range is the shader zclip/depth-range
		// transform (LLD-raster §3.2, invariant 4; VKDraw.cpp:1305-1320).
		cfg.supports_extended_depth_range = false;
		cfg.supports_framebufferless_rendering = true;
		cfg.supports_asynchronous_compute = true;  // independent command buffers (HLD-04 §3.2)

		// Metal has no logic-op state, so the ROP emulates it by reading the
		// destination colour through the framebuffer fetch the programmable-blend
		// path already owns (REVIEW-FINDINGS.md C1).
		cfg.supports_native_logic_op = false;

		// Forced-off on Metal 4 bring-up (docs/research/metal4/10 §2.2):
		cfg.supports_hw_renormalization = false;   // Apple silicon is not NV silicon
		cfg.supports_passthrough_dma = false;      // no host-pointer device DMA (HLD-02 §2.1)
		cfg.supports_last_provoking_vertex = false;// no public provoking-vertex mode; smooth fallback
		cfg.supports_hw_conditional_render = false;// bring-up: CPU skip (HLD-04 §3.6)
		// The flag routes every label release through MTLGSRender::release_GCM_label.
		// Metal writes the label on the CPU, so that hook drains the GPU before the
		// write; without it the guest can recycle the FIFO under the GPU (L11).
		cfg.supports_host_gpu_labels = true;
	}
} // namespace mtl

#endif // RSX_ENABLE_METAL4
