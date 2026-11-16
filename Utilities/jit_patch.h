#pragma once

#include "JIT.h"

#include <cstring>

// In-place redirection of one generated memory instruction (design §8.1.2).
//
// A patched site becomes a 4-byte `b` to a per-site stub in a near island. The
// store is a single aligned 32-bit write, which is atomic at instruction
// granularity on AArch64: a thread that is not executing this instruction sees
// either the original access or the branch, and both are valid code.
//
// The caller owns the site for the duration: no thread may be inside the
// instruction being replaced (the quiesce epoch of §7.2 A7). This helper does not
// establish that, it only performs the write and publishes it.
namespace jit_patch
{
#if defined(ARCH_ARM64)
	// `b` takes a signed 26-bit instruction offset: +/-128 MiB, and the site and
	// target must share 4-byte alignment.
	inline constexpr s64 branch_reach = 0x8000000;

	// Encode `b <target>` at `site`, or 0 when the displacement does not fit and
	// the far form (load + br, 16 bytes) is required instead.
	inline u32 encode_branch(const void* site, const void* target) noexcept
	{
		const s64 delta = static_cast<const u8*>(target) - static_cast<const u8*>(site);

		if (delta % 4 || delta < -branch_reach || delta > branch_reach - 4)
		{
			return 0;
		}

		return 0x1400'0000u | (static_cast<u32>(delta / 4) & 0x03FF'FFFFu);
	}

	// Replace the instruction at `site` with a branch to `target`, under one write
	// window, and publish it. Returns false if the target is out of reach.
	inline bool patch_branch(void* site, const void* target) noexcept
	{
		const u32 insn = encode_branch(site, target);

		if (!insn)
		{
			return false;
		}

		jit_write_guard write_guard;
		std::memcpy(site, &insn, sizeof(insn));
		jit_flush_icache(site, sizeof(insn));

		return true;
	}

	// True when the word at `site` is the branch this helper would write. Used to
	// keep a patch idempotent: a site that is already redirected is not rewritten.
	inline bool branch_matches(const void* site, const void* target) noexcept
	{
		const u32 expected = encode_branch(site, target);

		if (!expected)
		{
			return false;
		}

		u32 current;
		std::memcpy(&current, site, sizeof(current));

		return current == expected;
	}
#else
	// The patch form is architecture-specific. Other hosts keep the fallback path
	// and never call these, so they report that no patch was applied.
	inline bool patch_branch(void*, const void*) noexcept
	{
		return false;
	}

	inline bool branch_matches(const void*, const void*) noexcept
	{
		return false;
	}
#endif
}
