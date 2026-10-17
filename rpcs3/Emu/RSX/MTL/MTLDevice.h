#pragma once

#include "util/types.hpp"
#include "MTLAvailability.h"
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
	// guest address space, so a guest address is a buffer offset.
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

		MTL::Device* dev() const { return m_device; }
		MTL4::CommandQueue* queue() const { return m_queue; }
		MTL4::Compiler* compiler() const { return m_compiler; }
		const features& f() const { return m_features; }
		const guest_window& window() const { return m_window; }

		// The 16-bool contract struct (RSXThread.h:80-98), Metal 4 plan
		// (docs/research/metal4/10 §2.2): 7 free, 4 work-to-land, 4 forced false,
		// 1 reshaped.  Work fields stay false until their sub-system lands.
		void fill_backend_config(rsx::backend_configuration& cfg) const;

	private:
		device() = default;
		bool init();
		void import_guest_window();

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		features m_features{};
		guest_window m_window{};
	};

	[[noreturn]] void die(const std::string& err);
} // namespace mtl

#endif // RSX_ENABLE_METAL4
