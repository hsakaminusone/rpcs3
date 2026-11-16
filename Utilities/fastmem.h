#pragma once

#include "util/types.hpp"

// Fault-site table for the native memory tiers (design §8.1.2).
//
// A site is identified by the host PC of the faulting access, so the table
// needs no translation-time metadata: AArch64 data faults are precise and the
// instruction is retried after the page is fixed.
//
// This is the observational half of PR 8, not the fastmem tier. Nothing is
// patched and no access is re-executed from here yet: it records where faults
// happen so the tier decision has data. It sees every data fault that reaches
// the generic decoder, which is a superset of the eventual fastmem sites (RSX,
// mmapper and MMIO faults are included), so its counters describe the fault
// distribution, not fastmem coverage.
//
// The table is a fixed-size open-addressing hash. Faults are rare enough that
// lookups are not on a hot path, but several emulated threads can fault at
// once, so insertion is lock-free. A full table never affects correctness:
// new sites are counted and reported as recurring without being recorded.
//
// The key is published in a single CAS, because that is what makes a site
// findable and what keeps two threads from recording it twice. The history
// fields behind it are filled immediately afterwards and are advisory: a reader
// that matches a key can see zero or lagging values for one fault, which the
// caller must treat as "no history yet" rather than act on. Nothing in the
// handler depends on those fields yet; the tier that does will re-read them
// after a second fault at the site.
//
// Host PCs only mean anything while the JIT object containing them is alive, so
// a site record must be revoked when its object is released: see invalidate_range.
namespace fastmem
{
	enum class site_state : u8
	{
		first,     // no earlier fault at this instruction
		recurring, // this instruction has faulted before
	};

	// Record one fault. `host_pc` is the address of the faulting instruction,
	// `guest_addr` the faulting guest address, `is_write` the direction.
	// Safe to call from a signal handler.
	site_state note_fault(uptr host_pc, u32 guest_addr, bool is_write) noexcept;

	// Number of recorded sites, for diagnostics and the session summary.
	u64 site_count() noexcept;

	// Forget every site whose host PC falls in [host_begin, host_end). The caller
	// uses this when a JIT object is released, before its address space is
	// reused: a stale key would otherwise match an unrelated instruction and
	// attribute its history to it. Safe to call while other threads fault.
	void invalidate_range(uptr host_begin, uptr host_end) noexcept;

	// Drop all state. Host PCs are only meaningful while their JIT objects live,
	// so the caller must guarantee that no CPU thread can fault while this runs;
	// it is called when the last one has exited.
	void reset() noexcept;
}
