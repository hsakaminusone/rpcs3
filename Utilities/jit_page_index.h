#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"

#include <vector>

// Per-page invalidation index (§8.1.4) and invalidation epoch (§8.5.1).
//
// Guest code is compiled in modules, but a guest code change is per page. Today
// any change drops a whole module (jit_module_manager); this index answers the
// two questions that let a change be narrower and let cached links be trusted:
//
//   * which compiled modules cover the page that just changed;
//   * has anything changed since an epoch was taken.
//
// The epoch is a plain counter read without a lock. A reader that observes the
// same epoch twice knows that nothing it cached in between was invalidated,
// which is what a link cell or an IBTC entry needs before it can be trusted.
//
// The index holds no host addresses, so it survives a code-space move and can be
// persisted next to the module object (the sidecar of PR 19).
namespace jit_page
{
	// A guest page number: addr >> 12.
	using page_t = u32;

	// Record that [addr, addr + size) is covered by the compiled `module`.
	// Repeated calls for the same module and page are idempotent.
	void note_compiled(u32 addr, u32 size, u64 module);

	// Record that [addr, addr + size) changed. Bumps the epoch once, then marks
	// every page in the range. Returns the epoch after the bump.
	u64 note_invalidated(u32 addr, u32 size);

	// Current epoch. Every invalidation bumps it exactly once.
	u64 epoch() noexcept;

	// Modules covering `page`. Returned by value so the caller can sweep them
	// without holding the index lock.
	std::vector<u64> modules_over(page_t page);

	// Drop all coverage recorded for `module`. Called when the module is
	// released, so a later page change does not resurrect it.
	void forget_module(u64 module);

	// Forget everything. Used by the emulator reset path and by tests.
	void reset();
}
