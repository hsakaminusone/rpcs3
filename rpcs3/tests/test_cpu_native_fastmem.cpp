#include <gtest/gtest.h>

#include "Utilities/CPUStats.h"
#include "Utilities/fastmem.h"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// The site table is the gate for the fault-retry tier: the tier will decide
// whether to patch an access from the history this table keeps, and it will do so
// from a signal handler while other threads are inserting. What has to hold is
// weaker than "the numbers are exact" and stronger than "it does not crash":
// every fault is accounted for somewhere, and a full table degrades precision
// without dropping a fault or affecting the caller.
namespace
{
	// Host instructions are 4-byte aligned; this base keeps the keys non-zero and
	// far away from any real code range.
	constexpr uptr c_code_base = 0x1'0000'0000ull;

	struct totals
	{
		u64 first = 0;
		u64 recurring = 0;
		u64 overflow = 0;

		u64 sum() const
		{
			return first + recurring + overflow;
		}
	};

	totals read_totals()
	{
		totals t;
		t.first = +cpu_stats::g_counters.fastmem_first;
		t.recurring = +cpu_stats::g_counters.fastmem_recurring;
		t.overflow = +cpu_stats::g_counters.fastmem_overflow;
		return t;
	}

	uptr site(u64 index, u64 distinct)
	{
		return c_code_base + (index % distinct) * 4;
	}
}

// CPU-FASTMEM-001: concurrent faults never lose an accounting event, and a table
// that has room records every distinct site exactly once.
TEST(CpuNativeFastmem, AccountingHoldsUnderContention)
{
	fastmem::reset();
	cpu_stats::reset();

	constexpr u64 distinct = 1024;
	constexpr u64 threads = 8;
	constexpr u64 per_thread = 125'000;

	std::vector<std::thread> workers;
	workers.reserve(threads);

	for (u64 t = 0; t < threads; ++t)
	{
		workers.emplace_back([t]
		{
			for (u64 i = 0; i < per_thread; ++i)
			{
				fastmem::note_fault(site(t * per_thread + i, distinct), static_cast<u32>(i << 12), (i & 1) != 0);
			}
		});
	}

	for (auto& worker : workers)
	{
		worker.join();
	}

	const totals t = read_totals();

	EXPECT_EQ(t.sum(), threads * per_thread);
	EXPECT_EQ(t.overflow, 0u);

	// Every insert succeeded, so first-time faults are exactly the distinct sites
	// and the rest are repeats. This is the property the tier's decision rests on.
	EXPECT_EQ(fastmem::site_count(), distinct);
	EXPECT_EQ(t.first, distinct);
	EXPECT_EQ(t.recurring, threads * per_thread - distinct);
}

// CPU-FASTMEM-002: a table that is too small still accounts for every fault and
// never reports a site it did not record as stable.
TEST(CpuNativeFastmem, FullTableLosesPrecisionNotFaults)
{
	fastmem::reset();
	cpu_stats::reset();

	constexpr u64 distinct = 200'000;

	for (u64 i = 0; i < distinct; ++i)
	{
		fastmem::note_fault(site(i, distinct), static_cast<u32>(i << 12), false);
	}

	const totals t = read_totals();

	EXPECT_EQ(t.sum(), distinct);
	EXPECT_GT(t.overflow, 0u);
	EXPECT_LT(fastmem::site_count(), distinct);
}

// CPU-FASTMEM-003: reset is the invalidation point. Host PCs only mean anything
// while their JIT object is alive, so a site seen again after a reset must be
// reported as first-time rather than inheriting the old history.
TEST(CpuNativeFastmem, ResetForgetsEverySite)
{
	fastmem::reset();
	cpu_stats::reset();

	const uptr pc = site(0, 1);
	EXPECT_EQ(fastmem::note_fault(pc, 0x2'000, false), fastmem::site_state::first);
	EXPECT_EQ(fastmem::note_fault(pc, 0x2'000, true), fastmem::site_state::recurring);
	EXPECT_EQ(fastmem::site_count(), 1u);

	fastmem::reset();

	EXPECT_EQ(fastmem::site_count(), 0u);
	EXPECT_EQ(fastmem::note_fault(pc, 0x2'000, false), fastmem::site_state::first);
}

// CPU-FASTMEM-004: a null program counter cannot identify a site, so it is not
// recorded. The handler only calls in with a decoded PC, but a fault raised
// outside any JIT object must not poison the table.
TEST(CpuNativeFastmem, NullProgramCounterIsNotRecorded)
{
	fastmem::reset();
	cpu_stats::reset();

	EXPECT_EQ(fastmem::note_fault(0, 0x1'000, false), fastmem::site_state::first);
	EXPECT_EQ(fastmem::site_count(), 0u);
	EXPECT_EQ(read_totals().sum(), 0u);
}

// CPU-FASTMEM-005: the record has to be far cheaper than the fault it describes,
// because it runs inside the signal handler. The bound is loose enough not to
// flake and tight enough to catch a lock or an allocation appearing here.
TEST(CpuNativeFastmem, RecordingIsCheapNextToTheFault)
{
	fastmem::reset();
	cpu_stats::reset();

	constexpr u32 count = 1'000'000;

	const auto start = std::chrono::steady_clock::now();

	for (u32 i = 0; i < count; ++i)
	{
		fastmem::note_fault(site(i, 1024), i << 12, (i & 1) != 0);
	}

	const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / count;

	std::printf("[info] %.1f ns per recorded fault\n", ns);

	EXPECT_LT(ns, 200.0);
}

// CPU-FASTMEM-006: releasing a JIT object must revoke the sites inside it and
// leave the rest alone. A stale key would otherwise match an unrelated
// instruction once the address space is reused, and hand it someone else's
// history.
TEST(CpuNativeFastmem, InvalidateRangeForgetsOnlyItsOwnSites)
{
	fastmem::reset();
	cpu_stats::reset();

	// Three sites inside the range that will be released, one outside it.
	for (u64 i = 0; i < 3; ++i)
	{
		EXPECT_EQ(fastmem::note_fault(site(i, 4096), 0x1'000, false), fastmem::site_state::first);
	}

	const uptr outside = c_code_base + 0x10'000;
	EXPECT_EQ(fastmem::note_fault(outside, 0x1'000, false), fastmem::site_state::first);
	EXPECT_EQ(fastmem::site_count(), 4u);

	const uptr begin = c_code_base;
	const uptr end = c_code_base + 0x100;
	fastmem::invalidate_range(begin, end);

	EXPECT_EQ(fastmem::site_count(), 1u);

	// A site inside the released range starts over.
	EXPECT_EQ(fastmem::note_fault(site(0, 4096), 0x1'000, false), fastmem::site_state::first);

	// The one outside it kept its history.
	EXPECT_EQ(fastmem::note_fault(outside, 0x1'000, false), fastmem::site_state::recurring);

	// An empty range is a no-op.
	const u64 before = fastmem::site_count();
	fastmem::invalidate_range(end, begin);
	EXPECT_EQ(fastmem::site_count(), before);
}

// CPU-FASTMEM-007: invalidation runs while other threads fault, since a block can
// be released while a guest thread is in the handler. Nothing may be lost or
// double counted: every call still lands in exactly one bucket.
TEST(CpuNativeFastmem, InvalidateRaceKeepsAccounting)
{
	fastmem::reset();
	cpu_stats::reset();

	constexpr u64 distinct = 512;
	constexpr u64 threads = 4;
	constexpr u64 per_thread = 50'000;

	std::vector<std::thread> workers;
	workers.reserve(threads);

	for (u64 t = 0; t < threads; ++t)
	{
		workers.emplace_back([t]
		{
			for (u64 i = 0; i < per_thread; ++i)
			{
				fastmem::note_fault(site(t * per_thread + i, distinct), static_cast<u32>(i << 12), false);
			}
		});
	}

	for (u64 round = 0; round < 8; ++round)
	{
		fastmem::invalidate_range(c_code_base, c_code_base + distinct * 4);
		std::this_thread::yield();
	}

	for (auto& worker : workers)
	{
		worker.join();
	}

	EXPECT_EQ(read_totals().sum(), threads * per_thread);
	EXPECT_LT(fastmem::site_count(), distinct + 1);
}
