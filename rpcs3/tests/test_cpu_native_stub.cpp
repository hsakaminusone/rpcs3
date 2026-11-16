#include <gtest/gtest.h>

#include "Utilities/a64_site_stub.h"
#include "Utilities/jit_patch.h"

#include <cstring>

#if defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(ARCH_ARM64)
namespace
{
	// Reference words produced by assembling the same sequence with the platform
	// toolchain, for site=0x1000, helper=0x2000, resume=0x3000, size_log2=3.
	constexpr u32 c_reference[a64_stub::word_count]
	{
		0xF81F0FFEu, 0x58000120u, 0xF9400401u, 0x52800062u,
		0x52800003u, 0x580000F0u, 0xD63F0200u, 0xF84107FEu,
		0x580000D0u, 0xD61F0200u, 0x00001000u, 0x00000000u,
		0x00002000u, 0x00000000u, 0x00003000u, 0x00000000u,
	};

	struct site_entry
	{
		uptr host_pc;
		u32 guest_addr;
		u32 faults;
	};

	uptr g_site = 0;
	u32 g_addr = 0;
	u32 g_width = 0;
	u32 g_write = 0;
	int g_helper_calls = 0;
	int g_resume_calls = 0;
}

extern "C" void stub_helper(uptr site, u32 addr, u32 width, u32 write)
{
	g_site = site;
	g_addr = addr;
	g_width = width;
	g_write = write;
	g_helper_calls++;
}

// Reached with `br`, never called: it returns through the x30 the stub restored,
// which is the caller's return address. If the stub forgot to save x30, this
// returns into the stub and the test hangs instead of passing.
extern "C" void stub_resume()
{
	g_resume_calls++;
}

// CPU-FASTMEM-010: the emitted stub is the sequence the assembler produces.
TEST(CpuNativeStub, WordsMatchTheAssembler)
{
	const a64_stub::site_stub stub = a64_stub::make(0x1000, 0x2000, 0x3000, 3, false);

	EXPECT_EQ(a64_stub::size_bytes, 64u);

	for (u32 i = 0; i < a64_stub::word_count; ++i)
	{
		EXPECT_EQ(stub.data[i], c_reference[i]) << "word " << i;
	}

	// The two immediates follow the arguments.
	const a64_stub::site_stub write16 = a64_stub::make(0x1000, 0x2000, 0x3000, 2, true);
	EXPECT_EQ(write16.data[3], 0x5280'0042u); // mov w2, #2
	EXPECT_EQ(write16.data[4], 0x5280'0023u); // mov w3, #1
}

// CPU-FASTMEM-011: the whole stub on real code — it calls the helper with the
// site, the recorded guest address, the width and the direction, then resumes at
// the instruction after the patched access.
TEST(CpuNativeStub, CallsHelperAndResumesAtTheSite)
{
#if defined(__APPLE__)
	const long page_size = ::sysconf(_SC_PAGESIZE);
	ASSERT_GT(page_size, 0);

	u8* const code = static_cast<u8*>(::mmap(nullptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
	ASSERT_NE(code, MAP_FAILED);

	site_entry entry{};
	entry.guest_addr = 0x1234'5678u;

	const a64_stub::site_stub stub = a64_stub::make(uptr(&entry), uptr(&stub_helper), uptr(&stub_resume), 3, false);

	{
		jit_write_guard write_guard;
		std::memcpy(code, stub.data, a64_stub::size_bytes);
		jit_flush_icache(code, a64_stub::size_bytes);
	}

	const auto run = reinterpret_cast<void (*)()>(code);
	run();

	EXPECT_EQ(g_helper_calls, 1);
	EXPECT_EQ(g_site, uptr(&entry));
	EXPECT_EQ(g_addr, 0x1234'5678u);
	EXPECT_EQ(g_width, 3u);
	EXPECT_EQ(g_write, 0u);
	EXPECT_EQ(g_resume_calls, 1);

	// A second entry with a write access must reach the helper the same way.
	g_helper_calls = 0;
	g_resume_calls = 0;

	const a64_stub::site_stub write16 = a64_stub::make(uptr(&entry), uptr(&stub_helper), uptr(&stub_resume), 2, true);

	{
		jit_write_guard write_guard;
		std::memcpy(code, write16.data, a64_stub::size_bytes);
		jit_flush_icache(code, a64_stub::size_bytes);
	}

	run();

	EXPECT_EQ(g_helper_calls, 1);
	EXPECT_EQ(g_width, 2u);
	EXPECT_EQ(g_write, 1u);
	EXPECT_EQ(g_resume_calls, 1);

	::munmap(code, page_size);
#else
	SUCCEED() << "MAP_JIT execution test is Apple-only";
#endif
}
#else
TEST(CpuNativeStub, StubIsAArch64Only)
{
	SUCCEED();
}
#endif
