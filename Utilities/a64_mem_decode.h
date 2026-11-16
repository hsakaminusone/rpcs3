#pragma once

#include "util/types.hpp"

// Decode the single-register load/store at a fault site (design §8.1.2).
//
// The patcher needs two facts about the access it is about to replace: the
// direction and the width. The design records both at translation time; this
// reads them out of the instruction instead, which needs no metadata to travel
// from the translator through MCJIT to the handler.
//
// Anything that is not a plain single-register access with no writeback is
// reported as invalid, and a site that decodes as invalid is never patched. That
// fallback is the reason this can be conservative: an unknown instruction keeps
// faulting, which is the behaviour that already exists.
namespace a64_mem
{
	struct access
	{
		// False when the instruction is not a patchable single-register access.
		bool valid = false;

		bool is_write = false;

		// log2 of the number of bytes moved: 0..4 (1, 2, 4, 8, 16).
		u8 size_log2 = 0;
	};

	// Load/store register (unsigned immediate, unscaled, or register offset),
	// general-purpose and SIMD&FP. Everything else is invalid.
	//
	// Atomics share the encoding space with the register-offset form, so the
	// option bits are checked explicitly rather than assumed.
	inline access decode(u32 insn) noexcept
	{
		// 31:30 size, 29:27 111
		if (((insn >> 27) & 7) != 0b111)
		{
			return {};
		}

		// 26 V, 25:24 opc2. 10 and 11 are the unprivileged and writeback-ordered
		// forms, which this does not claim to understand.
		const u32 size = insn >> 30;
		const u32 v = (insn >> 26) & 1;
		const u32 opc2 = (insn >> 24) & 3;

		if (opc2 > 0b01)
		{
			return {};
		}

		// 23:22 opc, but the two families index it differently.
		const u32 opc = (insn >> 22) & 3;

		u32 width;
		bool is_write;

		if (v)
		{
			// SIMD&FP: bit 22 is the direction, and size 00 is the 1-byte or the
			// 16-byte form, chosen by bit 23.
			is_write = (opc & 1) == 0;
			width = size == 0 ? ((opc & 2) ? 16u : 1u) : (1u << size);
		}
		else
		{
			// General: only 00 is a store. 01 zero-extends, 10 extends to 64 bits
			// and 11 to 32 bits, and all three move 1 << size bytes.
			is_write = opc == 0;
			width = 1u << size;
		}

		if (opc2 == 0b00)
		{
			// 21 distinguishes register offset from the unscaled/dev forms.
			if ((insn >> 21) & 1)
			{
				// Register offset is only itself when the option bits are present;
				// the atomics that share this space do not have them.
				if (((insn >> 10) & 3) != 0b10)
				{
					return {};
				}
			}
			else if (((insn >> 10) & 3) != 0)
			{
				// Post-index and pre-index write back the base register. A stub
				// would have to reproduce that, so they are not patchable here.
				return {};
			}
		}

		access result;
		result.valid = true;
		result.is_write = is_write;
		result.size_log2 = static_cast<u8>(width == 16 ? 4 : (width == 8 ? 3 : (width == 4 ? 2 : (width == 2 ? 1 : 0))));
		return result;
	}
}
