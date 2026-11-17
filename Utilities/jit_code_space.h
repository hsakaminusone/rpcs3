#pragma once

#include <vector>

#include "util/types.hpp"

// Code-space reuse (§8.5.1).
//
// With per-page invalidation a recompiled block needs a home, and unmapping one
// per recompile is both slow and hostile to the instruction cache. Compiled code
// is therefore allocated in fixed chunks and a chunk is reused in place, never
// unmapped, until the module is unloaded.
//
// A chunk may only be reused when nothing can still be executing in it. That is
// what the epoch from the page index is for: a chunk whose last writer is older
// than the caller's epoch has no live code pointing at it.
namespace jit_code_space
{
	constexpr usz chunk_size = 64 * 1024;

	// Register a chunk as belonging to this space. Its initial epoch is 0, so it
	// is immediately reusable by any caller.
	void add(u8* base);

	// Take a free chunk whose last writer is older than `epoch`, oldest first.
	// Returns nullptr when every chunk is live or too new; the caller then
	// allocates a new chunk and registers it.
	u8* acquire(u64 epoch);

	// Return a chunk, stamped with the epoch the caller observed when it stopped
	// using it.
	void release(u8* base, u64 epoch);

	usz live_count();
	usz free_count();

	// Forget every chunk. Used by the module unload path and by tests.
	void reset();
}
