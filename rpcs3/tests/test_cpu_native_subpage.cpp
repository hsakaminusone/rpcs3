#include "Utilities/subpage_map.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace
{
	constexpr u32 host_page = 1u << vm::subpage::host_page_shift; // 16 KB
	constexpr u32 guest_page = 4096;
}

class CpuNativeSubpage : public ::testing::Test
{
protected:
	void SetUp() override { vm::subpage::reset(); }
	void TearDown() override { vm::subpage::reset(); }
};

// Outside every notification region a host page carries no check at all, so its
// state must not be derived from the mapped bits.
TEST_F(CpuNativeSubpage, PagesOutsideARegionAreNotChecked)
{
	vm::subpage::note_mapped(0x100000, host_page);
	EXPECT_EQ(vm::subpage::query(0x100000), vm::subpage::not_notify);
	EXPECT_EQ(vm::subpage::query(0x100000 + host_page - 1), vm::subpage::not_notify);
}

TEST_F(CpuNativeSubpage, FourMappedGuestPagesMakeTheHostPageFull)
{
	vm::subpage::note_notification(0x200000, host_page, true);
	ASSERT_EQ(vm::subpage::query(0x200000), vm::subpage::empty);

	vm::subpage::note_mapped(0x200000, host_page);

	// Every guest page of the host page reports the same state.
	for (u32 offset = 0; offset < host_page; offset += guest_page)
	{
		EXPECT_EQ(vm::subpage::query(0x200000 + offset), vm::subpage::full) << "offset " << offset;
	}
}

// The case the hardware cannot see: a sibling guest page mapped the host page,
// so an unmapped guest page inside it never raises a fault on its own.
TEST_F(CpuNativeSubpage, OneUnmappedGuestPageMakesTheHostPagePartial)
{
	vm::subpage::note_notification(0x300000, host_page, true);
	vm::subpage::note_mapped(0x300000, host_page);

	vm::subpage::note_unmapped(0x300000 + 2 * guest_page, guest_page);

	EXPECT_EQ(vm::subpage::query(0x300000), vm::subpage::partial);
	EXPECT_EQ(vm::subpage::query(0x300000 + 2 * guest_page), vm::subpage::partial);

	// Mapping it back returns the page to full.
	vm::subpage::note_mapped(0x300000 + 2 * guest_page, guest_page);
	EXPECT_EQ(vm::subpage::query(0x300000), vm::subpage::full);

	// And unmapping everything reaches empty, which needs no generated check
	// because the host protection faults on its own.
	vm::subpage::note_unmapped(0x300000, host_page);
	EXPECT_EQ(vm::subpage::query(0x300000), vm::subpage::empty);
}

TEST_F(CpuNativeSubpage, ARegionCanStartOverArbitraryGuestPages)
{
	// A region that begins and ends inside a host page only covers part of it.
	const u32 addr = 0x400000 + 2 * guest_page;
	vm::subpage::note_notification(addr, 2 * guest_page, true);

	// The host page is empty, since none of the covered guest pages is mapped.
	EXPECT_EQ(vm::subpage::query(0x400000), vm::subpage::empty);

	// A mapped sibling outside the region still makes it partial, which is the
	// conservative direction: the check costs a load, a missed check costs a bug.
	vm::subpage::note_mapped(0x400000, guest_page);
	EXPECT_EQ(vm::subpage::query(0x400000), vm::subpage::partial);
}

TEST_F(CpuNativeSubpage, ActiveFlagFollowsTheRegionsLifetime)
{
	EXPECT_EQ(vm::subpage::g_mem_subpage_active.load(), 0u);

	vm::subpage::note_notification(0x500000, 2 * host_page, true);
	EXPECT_EQ(vm::subpage::g_mem_subpage_active.load(), 2u);

	// Re-arming the same region must not count the pages twice.
	vm::subpage::note_notification(0x500000, 2 * host_page, true);
	EXPECT_EQ(vm::subpage::g_mem_subpage_active.load(), 2u);

	vm::subpage::note_notification(0x500000, 2 * host_page, false);
	EXPECT_EQ(vm::subpage::g_mem_subpage_active.load(), 0u);
	EXPECT_EQ(vm::subpage::query(0x500000), vm::subpage::not_notify);
	EXPECT_EQ(vm::subpage::query(0x500000 + host_page), vm::subpage::not_notify);
}

TEST_F(CpuNativeSubpage, RemovingARegionLeavesMappedPagesAlone)
{
	vm::subpage::note_notification(0x600000, host_page, true);
	vm::subpage::note_mapped(0x600000, host_page);

	vm::subpage::note_notification(0x600000, host_page, false);
	EXPECT_EQ(vm::subpage::query(0x600000), vm::subpage::not_notify);

	// The mapped bits survive, so re-arming sees the page as still full.
	vm::subpage::note_notification(0x600000, host_page, true);
	EXPECT_EQ(vm::subpage::query(0x600000), vm::subpage::full);
}

// Two threads mapping and unmapping separate ranges must not disturb each
// other's pages, and the state must equal the bits at the end.
TEST_F(CpuNativeSubpage, DisjointRangesUnderConcurrencyStayConsistent)
{
	constexpr u32 base = 0x800000;
	constexpr u32 per_thread = 64 * host_page;
	constexpr u32 threads = 4;

	vm::subpage::note_notification(base, per_thread * threads, true);

	std::vector<std::thread> workers;

	for (u32 t = 0; t < threads; t++)
	{
		workers.emplace_back([t]
		{
			const u32 first = base + t * per_thread;

			for (int round = 0; round < 200; round++)
			{
				vm::subpage::note_mapped(first, per_thread);
				vm::subpage::note_unmapped(first, per_thread);
			}

			// Leave every page of this thread's range mapped except its last guest
			// page, so the expected state is partial for one page and full for the rest.
			vm::subpage::note_mapped(first, per_thread);
			vm::subpage::note_unmapped(first + per_thread - guest_page, guest_page);
		});
	}

	for (auto& worker : workers)
	{
		worker.join();
	}

	for (u32 t = 0; t < threads; t++)
	{
		const u32 first = base + t * per_thread;

		for (u32 page = 0; page < per_thread; page += host_page)
		{
			const bool last = (page == per_thread - host_page);
			EXPECT_EQ(vm::subpage::query(first + page), last ? vm::subpage::partial : vm::subpage::full)
				<< "thread " << t << " host page " << (page / host_page);
		}
	}

	EXPECT_EQ(vm::subpage::g_mem_subpage_active.load(), (per_thread * threads) / host_page);
}
