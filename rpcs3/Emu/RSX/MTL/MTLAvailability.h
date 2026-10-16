#pragma once

#include "util/types.hpp"

// Metal 4 availability (docs/research/metal4/06 §3).  The backend builds only on
// Apple silicon; the OS version is a runtime gate, not a build flag, because
// metal-cpp ships no availability annotations (06 §3.4).  The MTL4 API appears in
// macOS 26, but the macOS 26 MTL4 compiler is broken (workaround W11), so the
// floor is 27.
#if defined(HAVE_METAL) && defined(__APPLE__) && defined(__aarch64__)
#define RSX_ENABLE_METAL4 1
#endif

namespace mtl
{
	// Metal 4 API availability (docs/research/metal4/06 §3.1): the MTL4 types are
	// macOS 26.0+.
	constexpr u32 metal4_api_floor = 26;

	// Product floor: the UI offers Metal from 27, because the macOS 26 MTL4 compiler
	// is broken (workaround W11).
	constexpr u32 metal4_offer_floor = 27;

	constexpr bool metal4_compiled()
	{
#ifdef RSX_ENABLE_METAL4
		return true;
#else
		return false;
#endif
	}

	constexpr bool metal4_available(u32 macos_major) noexcept
	{
		return metal4_compiled() && macos_major >= metal4_offer_floor;
	}

#ifdef RSX_ENABLE_METAL4
	// OS-major part of the gate; arm64 is enforced by not building the backend
	// elsewhere (docs/research/metal4/06 §3.5).  Declared here so the UI can gate
	// the renderer without reaching metal-cpp (MTLGSRender.h).
	u32 macos_major();
	bool metal4_supported();
#endif
}
