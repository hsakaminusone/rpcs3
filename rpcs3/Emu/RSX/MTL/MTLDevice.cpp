#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/RSXThread.h"
#include "Emu/Memory/vm.h"
#include "util/logs.hpp"

#include <cstdlib>
#include <sys/sysctl.h>

namespace mtl
{
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

	bool device::init()
	{
		if (!metal4_supported())
		{
			return false;
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

		m_queue = m_device->newMTL4CommandQueue();
		if (!m_queue)
		{
			die("MTL4 command queue creation failed");
		}

		// One compiler per device (docs/research/metal4/06 §2.5; workaround W11:
		// concurrent MTL4Compiler instances corrupt the heap on macOS 26).
		m_compiler = m_device->newCompiler(MTL4::CompilerDescriptor::alloc()->init(), nullptr);
		if (!m_compiler)
		{
			die("MTL4 compiler creation failed");
		}

		m_features.metal4_family = true;
		m_features.compiler_ok = true;

		import_guest_window();

		rsx_log.success("Metal 4 device initialized: %s", m_device->name() ? m_device->name()->utf8String() : "unknown");
		return true;
	}

	void device::import_guest_window()
	{
		// HLD-02 §2.1.  The guest block is anonymous mmap memory owned by the
		// emulator, so the deallocator must not free it.
		m_window.buffer = m_device->newBuffer(vm::g_base_addr, guest_address_space, MTL::ResourceStorageModeShared, nullptr);
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
		cfg.supports_host_gpu_labels = false;      // reshaped; guest writes stay CPU-side for now
	}
} // namespace mtl

#endif // RSX_ENABLE_METAL4
