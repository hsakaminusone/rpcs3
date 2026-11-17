#pragma once

#include <span>
#include <vector>

#include "util/types.hpp"

// Loop-first region formation (§8.2.1).
//
// The unit of translation is a region: one entry and every block reachable from
// it without crossing a block that could be reached from somewhere else. The
// frontend already has the control-flow information for this — it is the same
// decoded instruction stream the analyser produced — so nothing here decodes.
//
// A block may only be absorbed when it has exactly one predecessor, and that
// predecessor is inside the region being built. Anything else is an entry: it
// needs its own host implementation, and absorbing it would create a second
// copy of one guest address.
namespace ppu_region
{
	// One basic block, as the analyser sees it.
	struct block
	{
		u32 addr = 0;        // guest address of its first instruction
		u32 size = 0;        // instructions in the block
		u32 target = 0;      // direct successor of a terminating branch
		bool has_target = false; // ends in a direct branch
		bool is_exit = false;    // indirect branch, syscall, or unknown control flow
	};

	struct region
	{
		u32 entry = 0;           // guest address the exec table targets
		u32 instructions = 0;    // total instructions inside the region
		bool is_loop = false;    // the entry is a loop header
		std::vector<u32> blocks; // guest addresses, entry first
	};

	// Loop headers, longest back edge first. An empty result means the stream
	// contains no loop, and formation degenerates to straight-line entries.
	std::vector<u32> loop_headers(std::span<const block> blocks);

	// Form regions for the given stream. Loop headers are formed first, in
	// descending span order; every other entry follows in address order. A walk
	// stops at `budget` instructions, at a block with more than one predecessor,
	// at an exit, or where two edges would join.
	//
	// Regions shorter than `min_instructions` are dropped unless they are loops:
	// below that length the fixed entry cost is cheaper than the region
	// machinery, so the caller emits a plain block instead.
	std::vector<region> form(std::span<const block> blocks, u32 budget, u32 min_instructions);
}
