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
	struct features
	{
		bool metal4_family = false; // GPUFamilyMetal4 probe result
		bool compiler_ok = false;   // MTL4::Compiler created (W11: one per device)
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

		// The 16-bool contract struct (RSXThread.h:80-98), Metal 4 plan
		// (docs/research/metal4/10 §2.2): 7 free, 4 work-to-land, 4 forced false,
		// 1 reshaped.  Work fields stay false until their sub-system lands.
		void fill_backend_config(rsx::backend_configuration& cfg) const;

	private:
		device() = default;
		bool init();

		MTL::Device* m_device = nullptr;
		MTL4::CommandQueue* m_queue = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		features m_features{};
	};

	[[noreturn]] void die(const std::string& err);
} // namespace mtl

#endif // RSX_ENABLE_METAL4
