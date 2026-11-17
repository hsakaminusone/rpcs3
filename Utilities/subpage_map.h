#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"

// Sub-page notification map (§8.1.3).
//
// The host page is 16 KB and the guest page is 4 KB, so four guest pages share
// one host page. A notification region must fault when the guest touches an
// unmapped guest page even though a sibling guest page has mapped the host page,
// and that is invisible to the hardware. The map below carries the state the
// generated check needs, one byte per host page.
//
// This file only maintains the state. The check that consumes it is emitted by
// the translator, and the slow path lives in the VM.

namespace vm::subpage
{
	enum state : u8
	{
		full = 0,      // all four guest pages mapped: T0 access, no check
		partial = 1,   // some mapped, some not: T1 access with the sub-page check
		empty = 2,     // none mapped: hardware fault, no check needed
		not_notify = 3 // outside every notification region: no check at all
	};

	// Host page size and the number of host pages covering the guest reservation.
	constexpr u32 host_page_shift = 14;
	constexpr u32 host_page_count = 1u << (32 - host_page_shift);
	constexpr u32 guest_pages_per_host_page = 1u << (host_page_shift - 12);

	// State per host page. Exposed because the generated check loads it directly:
	// one load and a compare against `partial`.
	extern atomic_t<u8> g_host_page_state[host_page_count];

	// Non-zero while at least one notification region exists. The generated check
	// tests this first, so a title that never creates one pays a single load and a
	// predicted-not-taken branch per guarded access.
	extern atomic_t<u32> g_mem_subpage_active;

	// Four bits per host page, one per guest page, set while that guest page is
	// mapped. This is the source of truth for the state above.
	extern atomic_t<u8> g_subpage_mapped[host_page_count];

	// A notification region was created over [addr, addr + size), or removed.
	// Recomputed from the mapped bits, so the order against note_mapped() and
	// note_unmapped() does not matter.
	void note_notification(u32 addr, u32 size, bool enabled);

	// Guest pages became mapped, or stopped being mapped.
	void note_mapped(u32 addr, u32 size);
	void note_unmapped(u32 addr, u32 size);

	// State of the host page containing `addr`.
	state query(u32 addr);

	// Forget everything. Used by the VM reset path and by tests.
	void reset();
}

namespace vm
{
	// The slow path of the check above: a guest page inside a mapped host page is
	// not mapped. Called once from generated code, never in a loop.
	void handle_guest_subpage_fault(u32 addr, u32 size, bool is_write);
}
