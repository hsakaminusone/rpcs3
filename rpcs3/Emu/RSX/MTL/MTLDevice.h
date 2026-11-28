#pragma once

#include "util/types.hpp"
#include "MTLAvailability.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// Metal 4 backend port surface (docs/research/metal4/08 §2; HLD-01 §3.1).
#ifdef RSX_ENABLE_METAL4

#include "Metal/Metal.hpp"

namespace rsx
{
	struct backend_configuration;
	enum class problem_severity : u8;
}

namespace mtl
{
	// The drawable ring bounds the frame slots (HLD-03 §4.9; HLD-04 §3.2).  The
	// recorder, the resource residency and the per-draw ABI rings all size to it.
	constexpr u32 frame_slot_count = 3;

	struct features
	{
		bool metal4_family = false; // GPUFamilyMetal4 probe result
		bool compiler_ok = false;   // MTL4::Compiler created (W11: one per device)
	};

	// The RSX address space is 32-bit, so the guest projection is 4 GiB.
	constexpr u64 guest_address_space = 0x1'0000'0000;

	// The guest XDR projection (HLD-02 §3.1).  One no-copy buffer covers the whole
	// guest address space, so a guest address is a buffer offset.  The buffer is the
	// unprotected mirror (vm::g_sudo_addr): the no-copy import requires every page
	// writable, and the protected guest view is not.
	struct guest_window
	{
		MTL::Buffer* buffer = nullptr;
		MTL::GPUAddress gpu_address = 0;

		bool valid() const { return buffer != nullptr; }
	};

	// Set when a shader-job shutdown timed out and the link worker could not be
	// stopped.  The worker still holds a compiler and program addresses, so a
	// later init must not create a second device beside it (L6).
	extern std::atomic<bool> g_abandoned_worker;

	// Bump on any change to the MSL/GLSL the backend emits, so a stale archive is
	// never loaded for a build that generates different code (C14).
	constexpr u32 MTL_GENERATOR_VERSION = 2;

	// The part of a cache key that changes the generated code: the generator
	// version, the math mode, and whether vertex position invariance is on.  A
	// development build also folds in its commit, so an emitter change between
	// commits never reuses the previous build's binaries (C14).
	std::string code_generation_key();

	// One MTL::Device + one MTL4::CommandQueue + one MTL4::Compiler
	// (HLD-01 §3.4; one compiler per device, KosmicKrisp workaround W11).
	class device
	{
	public:
		// Null on gate or probe failure; callers must treat that as "backend absent".
		static std::unique_ptr<device> create();
		~device();

		MTL::Device* dev() const { return m_device; }
		MTL4::CommandQueue* queue() const { return m_queue; }
		MTL4::Compiler* compiler() const { return m_compiler; }
		MTL4::CompilerTaskOptions* pipeline_task_options() const { return m_task_options; }
		const features& f() const { return m_features; }
		const guest_window& window() const { return m_window; }
		std::atomic<bool>& lost_flag()
		{
			return m_lost;
		}
		const std::atomic<bool>& lost_flag() const { return m_lost; }

		// Write the captured pipelines to the archive of this device and OS
		// (HLD-07 §4.2).  Safe to call once, at teardown; a no-op when the
		// on-disk shader cache is off or nothing was captured.
		void flush_pipeline_archive();

		// Crash-safe archive rotation (C29).  A crash loses at most the pipelines
		// compiled since the last rotation, because each retired generation is
		// flushed to its own session file.  Called at a frame boundary with the
		// shader-job count and whether the registry is idle; a compiler is never
		// swapped under an in-flight link.  Generation zero is never retired: the
		// interpreter holds it for the whole session.
		void maybe_rotate_archive(u64 compiled_pipelines, bool jobs_idle);

		// The 16-bool contract struct (RSXThread.h:80-98), Metal 4 plan
		// (docs/research/metal4/10 §2.2): 7 free, 4 work-to-land, 4 forced false,
		// 1 reshaped.  Work fields stay false until their sub-system lands.
		void fill_backend_config(rsx::backend_configuration& cfg) const;

		// Device-allocator pressure, in the same severity scale Vulkan derives
		// from its VMM (VKResourceManager.cpp:204).  The resolution-scale change
		// reclaims the caches at these thresholds (VKPresent.cpp:1013-1041), and
		// Metal has no allocator report of its own, so it uses what the device
		// reports: allocated bytes against the recommended working set.
		rsx::problem_severity memory_load_severity() const;

	private:
		device() = default;
		bool init();
		bool open_pipeline_archive();
		void import_guest_window();

		// One compiler and the serializer it records into.  A rotation retires a
		// generation only when no job can still use it (C29).
		struct archive_generation
		{
			MTL4::Compiler* compiler = nullptr;
			MTL4::PipelineDataSetSerializer* serializer = nullptr;
			bool flushed = false;
		};

		// Write one generation's captured pipelines to a new session file.  A
		// generation is written once; the flush is destructive.
		bool flush_generation(archive_generation& generation);

		// The archive directory entry prefix, empty when the on-disk shader cache
		// is off or the PPU cache has no root.  Keyed by GPU, OS and generator
		// inputs, because an archive is a device slice (06 §2.4; C14).
		std::string m_archive_prefix;
		// This session's stamp, so a flush lands in a new file rather than
		// overwriting an earlier one.
		std::string m_session_stamp;
		u32 m_flush_index = 0;

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		// The generation new work uses.  Generation zero stays current until the
		// first rotation and is never retired, because the interpreter holds it.
		std::vector<archive_generation> m_generations;
		u32 m_current_generation = 0;
		std::chrono::steady_clock::time_point m_next_rotation{};
		u64 m_pipelines_at_rotation = 0;
		bool m_rotation_enabled = false;

		// Aliases of the current generation, kept so the hot accessors stay one
		// load.  They are reassigned only on the RSX thread.
		MTL4::Compiler* m_compiler = nullptr;
		MTL4::PipelineDataSetSerializer* m_serializer = nullptr;

		MTL4::Archive* m_archive = nullptr;
		MTL4::CompilerTaskOptions* m_task_options = nullptr;
		features m_features{};
		guest_window m_window{};
		std::atomic<bool> m_lost{false};
		NS::Object* m_removal_observer = nullptr;
	};

	[[noreturn]] void die(const std::string& err);
} // namespace mtl

#endif // RSX_ENABLE_METAL4
