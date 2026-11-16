#include <gtest/gtest.h>

#include "Utilities/JIT.h"
#include "Emu/CPU/CPUThread.h"

#include <cstring>

// These cover the code-emission path the macOS CI smoke test cannot reach: that
// test exercises the host's MAP_JIT API, while these go through the emulator's
// own builder, write guard, and instruction-cache publish step. A regression in
// the guard or in the publish path has to fail here, not only at runtime.
namespace
{
#if defined(ARCH_X64)
	// mov eax, #1; ret
	constexpr u8 c_return_one[]{0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
	constexpr u8 c_return_two[]{0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
#else
	// movz w0, #1; ret  /  movz w0, #2; ret
	constexpr u8 c_return_one[]{0x20, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
	constexpr u8 c_return_two[]{0x40, 0x00, 0x80, 0x52, 0xC0, 0x03, 0x5F, 0xD6};
#endif

	using constant_fn = int (*)();

	// CPU-JIT-001: a function built by the runtime runs and returns its value.
	int build_constant(int value)
	{
		auto fn = build_function_asm<constant_fn>("CPU_JIT_001", [value](native_asm& c, auto&)
		{
#if defined(ARCH_X64)
			c.mov(asmjit::x86::eax, asmjit::Imm(value));
			c.ret();
#else
			c.mov(asmjit::a64::w0, asmjit::Imm(value));
			c.ret(asmjit::a64::x30);
#endif
		});

		return fn ? fn() : -1;
	}
}

TEST(CpuNativeJit, BuildFunctionAsmRuns)
{
	EXPECT_EQ(build_constant(42), 42);
	EXPECT_EQ(build_constant(7), 7);
}

// CPU-JIT-002: rewriting code publishes it to the instruction cache, so the next
// call executes the new bytes rather than the previously fetched ones.
TEST(CpuNativeJit, PatchedCodeIsPublishedToTheInstructionCache)
{
	u8* page = nullptr;

	{
		jit_write_guard guard;
		page = jit_runtime::alloc(sizeof(c_return_one), 16, true);
		ASSERT_NE(page, nullptr);
		std::memcpy(page, c_return_one, sizeof(c_return_one));
		jit_flush_icache(page, sizeof(c_return_one));
	}

	const auto fn = reinterpret_cast<constant_fn>(page);
	ASSERT_EQ(fn(), 1);

	{
		jit_write_guard guard;
		std::memcpy(page, c_return_two, sizeof(c_return_two));
		jit_flush_icache(page, sizeof(c_return_two));
	}

	EXPECT_EQ(fn(), 2);
}

#if defined(__APPLE__)
// CPU-JIT-003: the guard is a nesting counter. A scope taken inside another one
// must not close the outer write window; if it did, the store below would fault
// and every later compile on this thread would fail.
TEST(CpuNativeJit, NestedWriteGuardKeepsTheWriteWindowOpen)
{
	jit_write_guard outer;
	ASSERT_EQ(jit_write_guard::depth, 1u);

	EXPECT_EQ(build_constant(11), 11);
	EXPECT_EQ(jit_write_guard::depth, 1u);

	u8* const page = jit_runtime::alloc(sizeof(c_return_one), 16, true);
	ASSERT_NE(page, nullptr);
	page[0] = 0;
}

// CPU-JIT-004: leaving the outermost scope restores execute mode.
TEST(CpuNativeJit, OutermostScopeRestoresExecuteMode)
{
	{
		jit_write_guard outer;
		EXPECT_EQ(jit_write_guard::depth, 1u);
	}

	EXPECT_EQ(jit_write_guard::depth, 0u);
}
#endif

// CPU-FRAME-017/018: the frame budget and the gateway reservation are the same
// number for a reason. The .stack_sizes listener refuses a function whose frame
// exceeds cpu_thread::max_frame_size, and the PPU gateway reserves
// cpu_thread::gateway_slab_size with a single `sub sp`. Raising the budget
// without the slab would move the rejection bound above what the gateway can
// actually hold, which is exactly the corruption band the budget is there to
// close. The decode and rejection path itself needs a compiled module, so it is
// covered by the emulator build and not here.
TEST(CpuNativeJit, FrameBudgetFitsTheGatewaySlab)
{
	EXPECT_GE(cpu_thread::gateway_slab_size, cpu_thread::max_frame_size);
	EXPECT_GE(cpu_thread::max_frame_size, 0x1000u);

	// The slab is subtracted from the stack pointer, which must stay 16-byte
	// aligned across the call into compiled guest code.
	EXPECT_EQ(cpu_thread::gateway_slab_size % 16, 0u);
}
