#pragma once

#include "util/types.hpp"
#include "MTLAvailability.h"
#include <atomic>
#include <memory>

// Metal 4 backend port surface (docs/research/metal4/08 §2; HLD-01 §3.1).
#ifdef RSX_ENABLE_METAL4

#include "Metal/Metal.hpp"

namespace rsx
{
	struct backend_configuration;
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
		const std::atomic<bool>& lost_flag() const { return m_lost; }

		// Write the captured pipelines to the archive of this device and OS
		// (HLD-07 §4.2).  Safe to call once, at teardown; a no-op when the
		// on-disk shader cache is off or nothing was captured.
		void flush_pipeline_archive();

		// The 16-bool contract struct (RSXThread.h:80-98), Metal 4 plan
		// (docs/research/metal4/10 §2.2): 7 free, 4 work-to-land, 4 forced false,
		// 1 reshaped.  Work fields stay false until their sub-system lands.
		void fill_backend_config(rsx::backend_configuration& cfg) const;

	private:
		device() = default;
		bool init();
		bool open_pipeline_archive();
		void import_guest_window();

		// The L3 archive path, empty when the on-disk shader cache is off or the
		// PPU cache has no root.  Keyed by GPU and OS, because an archive is a
		// device slice (06 §2.4).
		std::string m_archive_path;

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
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
