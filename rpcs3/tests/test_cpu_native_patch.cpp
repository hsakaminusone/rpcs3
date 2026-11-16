#include <gtest/gtest.h>

#include "Utilities/jit_patch.h"

#include <cstring>

#if defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// CPU-FASTMEM-008: the branch the patch writes has to be the branch the system
// assembler would have written for the same displacement. The reference words
// below come from assembling `b .+8`, `b .-4`, `b .+0x7FFFFFC` and `b .-0x8000000`
// with the platform toolchain, so this test fails if the encoding drifts, not
// just if it disagrees with itself.
#if defined(ARCH_ARM64)
TEST(CpuNativePatch, EncoderMatchesTheAssembler)
{
	alignas(4) u8 site[4]{};

	EXPECT_EQ(jit_patch::encode_branch(site, site + 8), 0x1400'0002u);
	EXPECT_EQ(jit_patch::encode_branch(site, site - 4), 0x17FF'FFFFu);
	EXPECT_EQ(jit_patch::encode_branch(site, site + 0x7FFF'FFCu), 0x15FF'FFFFu);

	// The far end of the negative range is representable; one instruction beyond
	// it is not, and that is what the far form exists for.
	EXPECT_EQ(jit_patch::encode_branch(site, site - jit_patch::branch_reach), 0x1600'0000u);
	EXPECT_EQ(jit_patch::encode_branch(site, site - jit_patch::branch_reach - 4), 0u);
	EXPECT_EQ(jit_patch::encode_branch(site, site + jit_patch::branch_reach), 0u);

	// A branch can only be placed at an instruction boundary.
	EXPECT_EQ(jit_patch::encode_branch(site, site + 2), 0u);
	EXPECT_EQ(jit_patch::encode_branch(site, site + 6), 0u);
}

// CPU-FASTMEM-009: the whole primitive on real code. One function is patched to
// jump into another, and the call has to return the second function's value.
TEST(CpuNativePatch, PatchedSiteRedirectsExecution)
{
#if defined(__APPLE__)
	const long page_size = ::sysconf(_SC_PAGESIZE);
	ASSERT_GT(page_size, 0);

	u8* const code = static_cast<u8*>(::mmap(nullptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
	ASSERT_NE(code, MAP_FAILED);

	// movz w0, #1; ret   and   movz w0, #2; ret
	constexpr u8 return_one[]{0x20, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
	constexpr u8 return_two[]{0x40, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};

	{
		jit_write_guard write_guard;
		std::memcpy(code, return_one, sizeof(return_one));
		std::memcpy(code + 16, return_two, sizeof(return_two));
		jit_flush_icache(code, 32);
	}

	const auto entry = reinterpret_cast<u32 (*)()>(code);

	ASSERT_EQ(entry(), 1u);
	ASSERT_FALSE(jit_patch::branch_matches(code, code + 16));

	ASSERT_TRUE(jit_patch::patch_branch(code, code + 16));
	EXPECT_TRUE(jit_patch::branch_matches(code, code + 16));

	// The first instruction is now `b code+16`, so the call returns 2.
	EXPECT_EQ(entry(), 2u);

	// Patching an already patched site is not an error and does not change it.
	EXPECT_TRUE(jit_patch::patch_branch(code, code + 16));
	EXPECT_EQ(entry(), 2u);

	// A target at the far end of the branch range is refused, and the site keeps
	// its code. The distance has to exceed jit_patch::branch_reach: anything
	// closer is a legal branch and would jump into unmapped memory.
	EXPECT_FALSE(jit_patch::patch_branch(code, code + (uptr(1) << 31)));
	EXPECT_EQ(entry(), 2u);

	::munmap(code, page_size);
#else
	SUCCEED() << "MAP_JIT execution test is Apple-only";
#endif
}
#else
TEST(CpuNativePatch, PatchFormIsUnavailableOnThisHost)
{
	u8 site[4]{};
	EXPECT_FALSE(jit_patch::patch_branch(site, site + 8));
	EXPECT_FALSE(jit_patch::branch_matches(site, site + 8));
}
#endif
