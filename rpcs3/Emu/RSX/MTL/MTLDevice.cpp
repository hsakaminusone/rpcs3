#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4


#include "Emu/Memory/vm.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/cache_utils.hpp"
#include "Emu/system_config.h"
#include "rpcs3_version.h"
#include "util/logs.hpp"
#include "util/vm.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <sys/sysctl.h>
#include <vector>

namespace mtl
{
	std::atomic<bool> g_abandoned_worker{false};

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
		// init creates many autoreleased framework objects on the RSX thread, which
		// has no pool of its own (L6).
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
		auto dev = std::unique_ptr<device>(new device());
		const bool ok = dev->init();
		pool->release();

		if (!ok)
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

		// Release in dependency order: the task options name the archive, the
		// compiler names the serializer, and the guest window is a view of the
		// device's address space (L6).
		auto release = [](auto*& object)
		{
			if (object)
			{
				object->release();
				object = nullptr;
			}
		};

		release(m_task_options);
		release(m_archive);

		// The aliases name the current generation, so only the generations are
		// released (C29).
		m_compiler = nullptr;
		m_serializer = nullptr;

		for (archive_generation& generation : m_generations)
		{
			release(generation.serializer);
			release(generation.compiler);
		}

		m_generations.clear();

		release(m_window.buffer);
		m_window.gpu_address = 0;
		release(m_queue);
		release(m_device);
	}


	bool device::init()
	{
		if (!metal4_supported())
		{
			return false;
		}


		// The debug-output setting is the validation switch on this backend, as it is
		// for OpenGL's debug context (gl_gs_frame.cpp:22-25) and Vulkan's validation
		// layers.  Metal reads its debug layer from the environment, and only at the
		// first device, so it is set here and never later.  `overwrite` is 0, so a
		// value the user set in their own environment wins.
		if (g_cfg.video.debug_output)
		{
			setenv("MTL_DEBUG_LAYER", "1", 0);
			setenv("MTL_DEBUG_LAYER_ERROR_MODE", "nslog", 0);
		}

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

		// Let the compiler keep more than one pipeline in flight.  The default is
		// NO, which serialises library and link work behind the RSX thread's
		// submission rate (C30).
		m_device->setShouldMaximizeConcurrentCompilation(true);

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

		// The L3 driver pipeline cache (HLD-07 §4.2).  The prefix carries the GPU,
		// the OS version and every input that changes the generated code, because
		// an archive is a device slice and a stale one silently runs old binaries
		// (06 §2.4; C14).  Each session and each flush writes its own file, so a
		// crash loses at most the pipelines compiled since the last flush (C29).
		if (const std::string cache_root = rpcs3::cache::get_ppu_cache();
			!g_cfg.video.disable_on_disk_shader_cache && !cache_root.empty())
		{
			m_archive_prefix = fmt::format("%sshaders_cache/metal/archive-%llx-%u-g%u-m%u-%s-",
				cache_root, static_cast<unsigned long long>(m_device->registryID()), macos_major(),
				MTL_GENERATOR_VERSION, g_cfg.video.disable_msl_fast_math.get() ? 1u : 0u,
				rpcs3::get_commit_and_hash().second);


			m_session_stamp = fmt::format("%llu", static_cast<unsigned long long>(std::time(nullptr)));

			// Two compilers may coexist only where W11 is fixed (macOS 27).
			m_rotation_enabled = macos_major() >= 27;
		}

		MTL4::CompilerDescriptor* descriptor = MTL4::CompilerDescriptor::alloc()->init();

		// The serializer is what records the compiled pipelines.  It is created on
		// a cold start too, or nothing would ever be written.
		archive_generation generation;

		if (!m_archive_prefix.empty())
		{
			MTL4::PipelineDataSetSerializerDescriptor* serializer_descriptor = MTL4::PipelineDataSetSerializerDescriptor::alloc()->init();
			// CaptureBinaries alone.  The archive holds binaries, and asking for
			// the descriptors as well makes the flush fail with no error at all
			// (HLD-07 §4.2).
			serializer_descriptor->setConfiguration(MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
			generation.serializer = m_device->newPipelineDataSetSerializer(serializer_descriptor);

			if (generation.serializer)
			{
				descriptor->setPipelineDataSetSerializer(generation.serializer);
			}
			else
			{
				rsx_log.error("Metal: the pipeline data set serializer was not created. The driver pipeline cache is off.");
			}
		}

		// One compiler per device (docs/research/metal4/06 §2.5; workaround W11:
		// concurrent MTL4Compiler instances corrupt the heap on macOS 26).
		generation.compiler = m_device->newCompiler(descriptor, nullptr);
		if (!generation.compiler)
		{
			die("MTL4 compiler creation failed");
		}

		m_generations.push_back(generation);
		m_compiler = generation.compiler;
		m_serializer = generation.serializer;
		m_next_rotation = std::chrono::steady_clock::now() + std::chrono::minutes(5);

		open_pipeline_archive();

		m_features.metal4_family = true;
		m_features.compiler_ok = true;

		import_guest_window();


		rsx_log.success("Metal 4 device initialized: %s", m_device->name() ? m_device->name()->utf8String() : "unknown");
		return true;
	}

	bool device::open_pipeline_archive()
	{
		if (m_archive_prefix.empty())
		{
			return false;
		}

		// Load every archive of this device, OS and code generation, newest first,
		// so a crash only loses the last flush (C29).  The session that wrote an
		// archive may have been killed, so nothing beyond the file name is assumed.
		const std::filesystem::path directory = std::filesystem::path(m_archive_prefix).parent_path();
		const std::string file_prefix = std::filesystem::path(m_archive_prefix).filename().string();

		struct archive_file
		{
			std::string path;
			std::filesystem::file_time_type time;
		};

		std::vector<archive_file> files;
		std::error_code error_code;

		for (const auto& entry : std::filesystem::directory_iterator(directory, error_code))
		{
			if (!entry.is_regular_file(error_code))
			{
				continue;
			}

			const std::string name = entry.path().filename().string();

			if (name.size() <= file_prefix.size() || name.compare(0, file_prefix.size(), file_prefix) != 0 ||
				entry.path().extension() != ".bin")
			{
				continue;
			}

			files.push_back({ entry.path().string(), entry.last_write_time(error_code) });
		}

		if (files.empty())
		{
			return false;
		}

		std::sort(files.begin(), files.end(), [](const archive_file& a, const archive_file& b)
		{
			return a.time > b.time;
		});

		// Keep the newest eight; older ones are compacted away.  Every archive that
		// is kept is loaded, so compaction never loses a pipeline the run needs.
		constexpr usz keep = 8;
		std::vector<MTL4::Archive*> archives;

		// The URL and array objects are autoreleased; the RSX thread has no pool.
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		for (usz index = 0; index < files.size(); ++index)
		{
			if (index >= keep)
			{
				std::filesystem::remove(files[index].path, error_code);
				continue;
			}

			NS::Error* error = nullptr;
			MTL4::Archive* archive = m_device->newArchive(
				NS::URL::fileURLWithPath(NS::String::string(files[index].path.c_str(), NS::UTF8StringEncoding)), &error);

			if (!archive)
			{
				// A rejected archive is a cold start, not a failure (HLD-07 §7).
				rsx_log.error("Metal: the pipeline archive %s was rejected: %s", files[index].path, archive_error_text(error));
				continue;
			}

			archives.push_back(archive);
		}

		if (archives.empty())
		{
			pool->release();
			return false;
		}

		m_task_options = MTL4::CompilerTaskOptions::alloc()->init();
		m_task_options->setLookupArchives(NS::Array::array(
			reinterpret_cast<const NS::Object* const*>(archives.data()), archives.size()));

		// The array retains each archive; drop the +1 that newArchive returned.
		for (MTL4::Archive* archive : archives)
		{
			archive->release();
		}

		rsx_log.notice("Metal: %zu pipeline archive(s) loaded, newest %s", archives.size(), files.front().path);
		pool->release();
		return true;
	}

	bool device::flush_generation(archive_generation& generation)
	{
		if (!generation.serializer || generation.flushed || m_archive_prefix.empty())
		{
			return false;
		}


		// The flush builds URLs, strings and error objects on the RSX thread, which
		// has no autorelease pool (L6).
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		const std::string path = fmt::format("%s%s-%u.bin", m_archive_prefix, m_session_stamp, m_flush_index);

		std::error_code error_code;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error_code);

		NS::Error* error = nullptr;
		if (!generation.serializer->serializeAsArchiveAndFlushToURL(
			NS::URL::fileURLWithPath(NS::String::string(path.c_str(), NS::UTF8StringEncoding)), &error))
		{
			// The flush is destructive, so a second call reports "nothing to
			// serialize"; each generation is therefore written once (HLD-07 §4.2).
			rsx_log.error("Metal: the pipeline archive %s was not written: %s", path, archive_error_text(error));
			pool->release();
			return false;
		}

		pool->release();

		generation.flushed = true;
		++m_flush_index;

		rsx_log.notice("Metal: pipeline archive written to %s", path);
		return true;
	}

	void device::flush_pipeline_archive()
	{
		// Every generation that has not been retired yet is written, including
		// generation zero, which only reaches the disk here (C29).
		for (archive_generation& generation : m_generations)
		{
			flush_generation(generation);
		}
	}

	void device::maybe_rotate_archive(u64 compiled_pipelines, bool jobs_idle)
	{
		if (!m_rotation_enabled || !jobs_idle || m_generations.empty())
		{
			return;
		}

		const bool count_reached = compiled_pipelines >= m_pipelines_at_rotation + 256;
		const bool time_reached = std::chrono::steady_clock::now() >= m_next_rotation;

		if (!count_reached && !time_reached)
		{
			return;
		}

		// A new serializer and compiler record the next window.  Generation zero is
		// never retired: the interpreter and the fixed pipelines hold it for the
		// whole session, so flushing it here would race their compiles.
		archive_generation next;

		if (!m_archive_prefix.empty())
		{
			MTL4::PipelineDataSetSerializerDescriptor* serializer_descriptor = MTL4::PipelineDataSetSerializerDescriptor::alloc()->init();
			serializer_descriptor->setConfiguration(MTL4::PipelineDataSetSerializerConfigurationCaptureBinaries);
			next.serializer = m_device->newPipelineDataSetSerializer(serializer_descriptor);
		}

		MTL4::CompilerDescriptor* descriptor = MTL4::CompilerDescriptor::alloc()->init();

		if (next.serializer)
		{
			descriptor->setPipelineDataSetSerializer(next.serializer);
		}

		next.compiler = m_device->newCompiler(descriptor, nullptr);

		if (!next.compiler)
		{
			rsx_log.error("Metal: archive rotation could not create a compiler; keeping the current one");
			return;
		}

		// Retire the current generation.  It is idle, so no link can still be using
		// its compiler; its serializer is written now, which is the crash-safe
		// window (C29).  The index is captured before the push, because the vector
		// may reallocate and a reference would dangle.
		const u32 retired_index = m_current_generation;
		m_generations.push_back(next);
		m_current_generation = static_cast<u32>(m_generations.size()) - 1;
		m_compiler = next.compiler;
		m_serializer = next.serializer;
		m_pipelines_at_rotation = compiled_pipelines;
		m_next_rotation = std::chrono::steady_clock::now() + std::chrono::minutes(5);

		// Generation zero is the interpreter's; only later generations are
		// retired, flushed and released.  The release bounds the memory a long
		// session would otherwise keep in dead compilers.
		if (retired_index != 0)
		{
			archive_generation& retired = m_generations[retired_index];
			flush_generation(retired);

			if (retired.serializer)
			{
				retired.serializer->release();
				retired.serializer = nullptr;
			}

			if (retired.compiler)
			{
				retired.compiler->release();
				retired.compiler = nullptr;
			}
		}
	}

	void device::import_guest_window()
	{
		// HLD-02 §2.1.  Import the unprotected mirror, not the guest view: the no-copy
		// import requires every page of the range writable (Metal returns nil otherwise),
		// and the guest view carries protection::no pages — macOS/ARM reserves guest
		// memory PROT_NONE (upstream 8d6b42e78).  The mirror aliases the same guest RAM
		// (vm.cpp:846) and is how the renderers reach guest memory (vm::get_super_ptr).
		// The emulator owns the region, so the deallocator must not free it.  Make the
		// mirror read-write first; memory_protect is the mprotect alone, where
		// memory_commit would also prefault all 4 GiB with MADV_WILLNEED.
		utils::memory_protect(vm::g_sudo_addr, guest_address_space, utils::protection::rw);
		m_window.buffer = m_device->newBuffer(vm::g_sudo_addr, guest_address_space, MTL::ResourceStorageModeShared, nullptr);
		if (!m_window.buffer)
		{
			die(fmt::format("guest memory import failed (%llu bytes)", guest_address_space));
		}

		m_window.gpu_address = m_window.buffer->gpuAddress();
	}

	rsx::problem_severity device::memory_load_severity() const
	{
		const u64 budget = m_device->recommendedMaxWorkingSetSize();
		const u64 allocated = m_device->currentAllocatedSize();

		if (!budget || !allocated)
		{
			return rsx::problem_severity::low;
		}

		const u64 percentage = (allocated * 100) / budget;

		if (percentage <= 75)
		{
			return rsx::problem_severity::low;
		}

		rsx::problem_severity severity = rsx::problem_severity::moderate;

		if (percentage > 95)
		{
			severity = rsx::problem_severity::fatal;
		}
		else if (percentage > 90)
		{
			severity = rsx::problem_severity::severe;
		}

		// The ratio alone is not the whole answer: enough headroom for the next
		// scale's surfaces means there is no pressure, and a little headroom keeps
		// the basic resources (VKResourceManager.cpp:237-253).
		const f32 res_scale = rsx::get_current_renderer()->resolution_scaling_config.scale_factor();
		constexpr u64 _1M = 0x100000;
		const u64 threshold_1 = static_cast<u64>(256 * res_scale * res_scale) * _1M;
		const u64 threshold_2 = static_cast<u64>(64 * res_scale * res_scale) * _1M;
		const u64 remaining = allocated < budget ? budget - allocated : 0;

		if (allocated < budget / 2 || remaining > threshold_1)
		{
			severity = rsx::problem_severity::low;
		}
		else if (remaining > threshold_2)
		{
			severity = rsx::problem_severity::moderate;
		}

		return severity;
	}

	void device::fill_backend_config(rsx::backend_configuration& cfg) const
	{
		// Metal 4 has no multi-draw command, and nothing in the tree reads this flag
		// today (the VK and GL draw paths consult their own device query), so it
		// records the hardware fact rather than selecting a path.
		cfg.supports_multidraw = false;

		// The host sample count follows the video MSAA setting (surface::create,
		// MTLSurface.cpp:63-65), so a 1-sample target must not claim hardware MSAA
		// or hardware A2C: the core would then set ROP_MULTISAMPLED and declare a
		// multisample texture against a single-sample surface (RSXGraphEngine.cpp:
		// 933-970), and hardware A2C would be a silent no-op.  VK and GL gate the
		// same two flags (VKGSRender.cpp:746-752, GLGSRender.cpp:50-56).
		const bool host_multisampled = g_cfg.video.antialiasing_level != msaa_level::none;
		cfg.supports_hw_msaa = host_multisampled;
		cfg.supports_hw_a2c = host_multisampled;
		cfg.supports_hw_a2c_1spp = false; // unverified at 1 spp; GL disables it too (GLGSRender.cpp:54)
		cfg.supports_hw_a2one = true;
		cfg.supports_hw_instanced_rendering = true;
		cfg.supports_programmable_blending = true; // framebuffer fetch [[color(n)]] (HLD-01 §4.3)
		cfg.supports_normalized_barycentrics = true;
		// Metal validates `MTLViewport.znear/zfar` to [0,1] and has no unrestricted
		// depth range, so the general clip range is the shader zclip/depth-range
		// transform (LLD-raster §3.2, invariant 4; VKDraw.cpp:1305-1320).
		cfg.supports_extended_depth_range = false;
		cfg.supports_framebufferless_rendering = true;
		// Every encoder in this backend is encoded on the slot's one command buffer
		// and its one queue, so there is no async path to advertise.  VK sets this
		// only when it really owns a second queue (VKGSRender.cpp:788-793).
		cfg.supports_asynchronous_compute = false;

		// Metal has no logic-op state, so the ROP emulates it by reading the
		// destination colour through the framebuffer fetch the programmable-blend
		// path already owns (REVIEW-FINDINGS.md C1).
		cfg.supports_native_logic_op = false;

		// Forced-off on Metal 4 bring-up (docs/research/metal4/10 §2.2):
		cfg.supports_hw_renormalization = false;   // Apple silicon is not NV silicon
		cfg.supports_passthrough_dma = false;      // no host-pointer device DMA (HLD-02 §2.1)
		// Metal's [[flat]] interpolant comes from the first vertex, while RSX takes
		// the last; the draw path rotates every primitive group so the two agree
		// (MTL/MTLGSRender.cpp, Common/BufferUtils.cpp expand_indexed_flat).
		cfg.supports_last_provoking_vertex = true;
		cfg.supports_hw_conditional_render = false;// bring-up: CPU skip (HLD-04 §3.6)
		// The guest label is memory the CPU parser reads at the acquire.  A host
		// store at the release is visible to that read immediately, which is what
		// VK and GL do by default (VK: nv47_sync.hpp:83 with the setting off;
		// GL: its host DMA writer).  The GPU write is the opt-in path: it is
		// ordered against the work it releases, but the acquire must then wait
		// for the GPU to land the word, and that wait holds the FIFO reader
		// (L135, L138).  Honor the setting; do not force the cost on every run.
		cfg.supports_host_gpu_labels = !!g_cfg.video.host_label_synchronization;
	}
} // namespace mtl

#endif // RSX_ENABLE_METAL4
