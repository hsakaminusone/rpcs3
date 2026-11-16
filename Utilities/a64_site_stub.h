#pragma once

#include "util/types.hpp"

#include <cstring>

// Per-site stub for a patched fault site (design §8.1.2).
//
// Layout, 16 words / 64 bytes:
//
//   0  str  x30, [sp, #-16]!     ; the `b` that enters here left the region's
//                                ; return address in x30; `blr` would overwrite it
//   1  ldr  x0, <site>           ; literal, the site's table entry
//   2  ldr  x1, [x0, #8]         ; guest address recorded by the handler
//   3  mov  w2, #size_log2
//   4  mov  w3, #is_write
//   5  ldr  x16, <helper>
//   6  blr  x16
//   7  ldr  x30, [sp], #16
//   8  ldr  x16, <resume>        ; the instruction after the patched access
//   9  br   x16
//  10  .quad site
//  12  .quad helper
//  14  .quad resume
//
// The stub must sit within the branch range of its site, so the caller places it
// in the module's near stub island. Every offset here is a link-time constant, so
// no runtime state is needed beyond the site entry.
namespace a64_stub
{
	inline constexpr u32 word_count = 16;
	inline constexpr u32 size_bytes = word_count * sizeof(u32);

	struct site_stub
	{
		u32 data[word_count]{};
	};

	// `site` points at an entry whose guest address is at offset 8.
	inline site_stub make(uptr site, uptr helper, uptr resume, u8 size_log2, bool is_write) noexcept
	{
		site_stub stub;

		stub.data[0] = 0xF81F'0FFE;                                            // str x30, [sp, #-16]!
		stub.data[1] = 0x5800'0000u | (9u << 5) | 0u;                          // ldr x0, site
		stub.data[2] = 0xF940'0401;                                            // ldr x1, [x0, #8]
		stub.data[3] = 0x5280'0000u | (u32(size_log2) << 5) | 2u;               // mov w2, #size_log2
		stub.data[4] = 0x5280'0000u | (is_write ? (1u << 5) : 0u) | 3u;        // mov w3, #is_write
		stub.data[5] = 0x5800'0000u | (7u << 5) | 16u;                         // ldr x16, helper
		stub.data[6] = 0xD63F'0200;                                            // blr x16
		stub.data[7] = 0xF841'07FE;                                            // ldr x30, [sp], #16
		stub.data[8] = 0x5800'0000u | (6u << 5) | 16u;                         // ldr x16, resume
		stub.data[9] = 0xD61F'0200;                                            // br x16

		std::memcpy(&stub.data[10], &site, sizeof(site));
		std::memcpy(&stub.data[12], &helper, sizeof(helper));
		std::memcpy(&stub.data[14], &resume, sizeof(resume));

		return stub;
	}
}
