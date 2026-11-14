#pragma once

#include "util/atomic.hpp"
#include "util/types.hpp"

// Always-on counters for the native CPU runtime. They are relaxed atomics and
// are incremented once per event, never per instruction, so they stay off the
// hot path. The summary is printed when the last CPU thread exits.
namespace cpu_stats
{
	struct counter_set
	{
		atomic_t<u64> ppu_modules{};       // PPU modules compiled
		atomic_t<u64> spu_functions{};     // SPU functions compiled
		atomic_t<u64> compile_failures{};  // recovered LLVM compile failures
		atomic_t<u64> block_invalidations{}; // compiled modules dropped
		atomic_t<u64> ppu_fallbacks{};     // entries that ran the interpreter
		atomic_t<u64> spu_fallbacks{};     // entries that ran the interpreter
		atomic_t<u64> faults_all{};
		atomic_t<u64> faults_rsx{};
		atomic_t<u64> faults_spu{};
		atomic_t<u64> mmio_sites{};        // accesses classified as the MMIO tier
		atomic_t<u64> frame_warnings{};    // frames above 75% of the gateway slab
		atomic_t<u64> frame_overflows{};   // frames above the gateway slab
		atomic_t<u64> jit_compile_ns{};
	};

	inline counter_set g_counters{};

	inline void reset()
	{
		g_counters.ppu_modules = 0;
		g_counters.spu_functions = 0;
		g_counters.compile_failures = 0;
		g_counters.block_invalidations = 0;
		g_counters.ppu_fallbacks = 0;
		g_counters.spu_fallbacks = 0;
		g_counters.faults_all = 0;
		g_counters.faults_rsx = 0;
		g_counters.faults_spu = 0;
		g_counters.mmio_sites = 0;
		g_counters.frame_warnings = 0;
		g_counters.frame_overflows = 0;
		g_counters.jit_compile_ns = 0;
	}
}