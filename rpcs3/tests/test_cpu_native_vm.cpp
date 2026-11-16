#include <gtest/gtest.h>

#include "util/vm.hpp"

#include <cstdio>
#include <cstring>

#if defined(__APPLE__)
#include <sys/mman.h>
#endif

// The Apple decommit path must keep the reservation: the whole point of §8.1.5 is
// that the address stays valid and race-free across decommit, so a `munmap` that
// silently succeeded would not be caught by a crash here.
namespace
{
	class reservation
	{
		void* m_ptr = nullptr;
		usz m_size = 0;

	public:
		explicit reservation(usz size)
			: m_size(size)
		{
			m_ptr = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

			if (m_ptr == MAP_FAILED)
			{
				m_ptr = nullptr;
			}
		}

		~reservation()
		{
			if (m_ptr)
			{
				::munmap(m_ptr, m_size);
			}
		}

		reservation(const reservation&) = delete;
		reservation& operator=(const reservation&) = delete;

		u8* get() const
		{
			return static_cast<u8*>(m_ptr);
		}
	};
}

// CPU-VM-001
TEST(CpuNativeVm, PageSizeIsTheAppleGranule)
{
#if defined(__APPLE__)
	EXPECT_EQ(utils::get_page_size(), 16384);
#else
	EXPECT_GT(utils::get_page_size(), 0);
#endif
}

// CPU-VM-003/004: a decommit keeps the range reserved, so committing it again
// must succeed at the same address and be writable.
TEST(CpuNativeVm, DecommitKeepsTheReservation)
{
	constexpr usz size = 64 * 1024;
	reservation range(size);
	ASSERT_NE(range.get(), nullptr);

	utils::memory_commit(range.get(), size);
	std::memset(range.get(), 0x5a, size);

	utils::memory_decommit(range.get(), size);

	// Nothing about the address changed, only its protection.
	utils::memory_commit(range.get(), size);
	range.get()[0] = 0x11;
	EXPECT_EQ(range.get()[0], 0x11);
}

// CPU-VM-008: reset must leave the whole range usable, and must stop handing the
// previous contents back as if they were fresh. MADV_FREE_REUSABLE is a lazy
// free, so the call asks for the pages to be dropped without guaranteeing it;
// this test records what actually happened instead of asserting a guarantee the
// advice does not make.
TEST(CpuNativeVm, ResetLeavesTheRangeUsable)
{
	constexpr usz size = 64 * 1024;
	reservation range(size);
	ASSERT_NE(range.get(), nullptr);

	utils::memory_commit(range.get(), size);
	std::memset(range.get(), 0xab, size);

	utils::memory_reset(range.get(), size, utils::protection::rw);

	range.get()[0] = 0x11;
	EXPECT_EQ(range.get()[0], 0x11);

	// Still usable across the whole range, not just the first page.
	std::memset(range.get() + size - 4, 0x22, 4);
	EXPECT_EQ(range.get()[size - 4], 0x22);

#if defined(__APPLE__)
	if (range.get()[8] == 0xab)
	{
		std::printf("[info] reset kept the previous contents on this run: "
			"MADV_FREE_REUSABLE is a lazy free, so the guest must not read "
			"decommitted memory expecting zeroes\n");
	}
#endif
}
