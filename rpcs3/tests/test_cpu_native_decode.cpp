#include <gtest/gtest.h>

#include "Utilities/a64_mem_decode.h"

// Every word below came out of assembling the mnemonic next to it with the
// platform toolchain, so this is a check against the assembler and not against
// the decoder's own idea of the encoding.
namespace
{
	struct case_t
	{
		const char* mnemonic;
		u32 insn;
		bool valid;
		bool is_write;
		u8 size_log2;
	};
}

#if defined(ARCH_ARM64)
TEST(CpuNativeMemDecode, MatchesTheAssembler)
{
	const case_t cases[]
	{
		{"ldr w0, [x1]",              0xB9400020u, true, false, 2},
		{"ldr w0, [x1, #16]",         0xB9401020u, true, false, 2},
		{"str w0, [x1, #16]",         0xB9001020u, true, true,  2},
		{"ldrb w0, [x1, #3]",         0x39400C20u, true, false, 0},
		{"ldrh w0, [x1, #6]",         0x79400C20u, true, false, 1},
		{"ldrsb w0, [x1]",            0x39C00020u, true, false, 0},
		{"ldrsh w0, [x1]",            0x79C00020u, true, false, 1},
		{"ldrsw x0, [x1]",            0xB9800020u, true, false, 2},
		{"ldr x0, [x1, #8]",          0xF9400420u, true, false, 3},
		{"str x0, [x1, #8]",          0xF9000420u, true, true,  3},
		{"ldur w0, [x1, #-4]",        0xB85FC020u, true, false, 2},
		{"stur x0, [x1, #-8]",        0xF81F8020u, true, true,  3},
		{"ldr w0, [x1, x2]",          0xB8626820u, true, false, 2},
		{"strh w2, [x3, x4]",         0x78246862u, true, true,  1},
		{"ldr x0, [x1, w2, sxtw]",    0xF862C820u, true, false, 3},
		{"ldrb w0, [x1, #4095]",      0x397FFC20u, true, false, 0},
		{"ldr x0, [x1, #32760]",      0xF97FFC20u, true, false, 3},

		// SIMD&FP: size 00 with opc bit 1 is the 16-byte form, without it 1 byte.
		{"ldr q0, [x1, #16]",         0x3DC00420u, true, false, 4},
		{"str q0, [x1, #16]",         0x3D800420u, true, true,  4},
		{"ldr d0, [x1]",              0xFD400020u, true, false, 3},
		{"str d0, [x1]",              0xFD000020u, true, true,  3},
		{"ldr s0, [x1]",              0xBD400020u, true, false, 2},
		{"str s0, [x1]",              0xBD000020u, true, true,  2},
		{"ldr h0, [x1]",              0x7D400020u, true, false, 1},
		{"str b0, [x1]",              0x3D000020u, true, true,  0},
		{"ldur q0, [x1]",             0x3CC00020u, true, false, 4},
		{"str q0, [x1, x2]",          0x3CA26820u, true, true,  4},

		// Not patchable: writeback, pairs, atomics, and the reservation set.
		{"ldrb w0, [x1], #1",         0x38401420u, false, false, 0},
		{"str x0, [x1, #8]!",         0xF8008C20u, false, false, 0},
		{"ldp w0, w1, [x2]",          0x29400440u, false, false, 0},
		{"stp w0, w1, [x2, #8]",      0x29010440u, false, false, 0},
		{"ldp x0, x1, [x2]",          0xA9400440u, false, false, 0},
		{"stp q0, q1, [x2, #32]",     0xAD010440u, false, false, 0},
		{"ldxr w0, [x1]",             0x885F7C20u, false, false, 0},
		{"stxr w0, w1, [x2]",         0x88007C41u, false, false, 0},
		{"ldar w0, [x1]",             0x88DFFC20u, false, false, 0},
		{"ldadd w0, w1, [x2]",        0xB8200041u, false, false, 0},
		{"ret",                       0xD65F03C0u, false, false, 0},
		{"movz w0, #1",               0x52800020u, false, false, 0},
	};

	for (const case_t& c : cases)
	{
		const a64_mem::access a = a64_mem::decode(c.insn);

		EXPECT_EQ(a.valid, c.valid) << c.mnemonic;
		EXPECT_EQ(a.is_write, c.is_write) << c.mnemonic;
		EXPECT_EQ(a.size_log2, c.size_log2) << c.mnemonic;
	}
}

// A site that decodes as unknown must never be patched: this is the fallback that
// keeps an unexpected instruction on the fault path instead of degrading it.
TEST(CpuNativeMemDecode, UnknownNeverClaimsAWidth)
{
	for (u32 word = 0; word < 0x10000; ++word)
	{
		const a64_mem::access a = a64_mem::decode(word);
		ASSERT_FALSE(a.valid) << word;
	}

	// Words that do decode always report a width the helper can handle.
	for (u32 insn = 0x39400000u; insn < 0x39410000u; ++insn)
	{
		const a64_mem::access a = a64_mem::decode(insn);

		if (a.valid)
		{
			ASSERT_LE(a.size_log2, 4u) << insn;
		}
	}
}
#else
TEST(CpuNativeMemDecode, DecoderIsAArch64Only)
{
	// The encoding space is meaningless on other hosts; the patcher never runs
	// there, so there is nothing to decode.
	SUCCEED();
}
#endif
