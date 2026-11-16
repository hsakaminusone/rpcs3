#include <gtest/gtest.h>

#include "util/atomic.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <new>
#include <thread>

#if defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

// The 16-byte atomic sequences in atomic.hpp are hand-written asm with explicit
// operand constraints. A functional round-trip is the cheapest way to catch a
// broken constraint or a swapped operand. Ordering is not asserted here: the
// sequences are kept identical to the compiler's own lowering for the same
// __atomic_* operation, which is checked when the sequence changes.
namespace
{
	u128 make_value(u64 lo, u64 hi)
	{
		const std::array<u64, 2> parts{lo, hi};
		u128 value{};
		std::memcpy(&value, parts.data(), 16);
		return value;
	}

	bool same_value(const u128& lhs, const u128& rhs)
	{
		return std::memcmp(&lhs, &rhs, 16) == 0;
	}
}

// CPU-ATOM-001/003: the 16-byte load and store sequences preserve both halves.
TEST(CpuNativeAtomics, SixteenByteStoreLoadObserveRoundTrip)
{
	alignas(16) u128 storage = make_value(0, 0);

	const u128 value = make_value(0x0123'4567'89ab'cdef, 0xfedc'ba98'7654'3210);
	atomic_storage<u128>::store(storage, value);

	EXPECT_TRUE(same_value(atomic_storage<u128>::load(storage), value));
	EXPECT_TRUE(same_value(atomic_storage<u128>::observe(storage), value));

	const u128 other = make_value(1, 2);
	atomic_storage<u128>::release(storage, other);
	EXPECT_TRUE(same_value(atomic_storage<u128>::load(storage), other));
}

// CPU-ATOM-003: both halves must survive. A swapped operand in the hand-written
// sequence shows up as a half-word difference, not as a crash.
TEST(CpuNativeAtomics, SixteenByteHalvesAreIndependent)
{
	alignas(16) u128 storage = make_value(0, 0);

	atomic_storage<u128>::store(storage, make_value(0x1111'1111'1111'1111, 0x2222'2222'2222'2222));
	const u128 loaded = atomic_storage<u128>::load(storage);

	u64 parts[2]{};
	std::memcpy(parts, &loaded, 16);
	EXPECT_EQ(parts[0], 0x1111'1111'1111'1111u);
	EXPECT_EQ(parts[1], 0x2222'2222'2222'2222u);
}

// CPU-ATOM-006/007: the failure path must report the observed value, the success
// path must install the new one.
TEST(CpuNativeAtomics, SixteenByteCompareExchange)
{
	alignas(16) u128 storage = make_value(5, 6);

	u128 expected = make_value(5, 6);
	const u128 desired = make_value(7, 8);
	EXPECT_TRUE(atomic_storage<u128>::compare_exchange(storage, expected, desired));
	EXPECT_TRUE(same_value(atomic_storage<u128>::load(storage), desired));

	// A failed exchange must report the observed value through `expected`.
	u128 stale = make_value(5, 6);
	EXPECT_FALSE(atomic_storage<u128>::compare_exchange(storage, stale, make_value(9, 10)));
	EXPECT_TRUE(same_value(stale, desired));
	EXPECT_TRUE(same_value(atomic_storage<u128>::load(storage), desired));
}

// CPU-ATOM-009
TEST(CpuNativeAtomics, SixteenByteExchangeReturnsPrevious)
{
	alignas(16) u128 storage = make_value(0xdead, 0xbeef);

	const u128 previous = atomic_storage<u128>::exchange(storage, make_value(0x11, 0x22));
	EXPECT_TRUE(same_value(previous, make_value(0xdead, 0xbeef)));
	EXPECT_TRUE(same_value(atomic_storage<u128>::load(storage), make_value(0x11, 0x22)));
}

namespace
{
	// Long enough that a lost wakeup shows up as a slow test rather than a hang.
	constexpr atomic_wait_timeout wait_2s = static_cast<atomic_wait_timeout>(2'000'000'000ull);

	// A woken waiter must return well before the timeout; the bound is loose so a
	// loaded CI machine does not flake, but a lost notification still fails.
	constexpr auto wake_budget = std::chrono::milliseconds(1'000);

	// Give a waiter that has announced itself time to enter the wait engine, so
	// these are park/wake tests and not a measurement of how fast the value is
	// already wrong. If a waiter is still on its way in, the wait returns
	// immediately: the test then proves less, but never fails spuriously.
	void let_waiters_park()
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

// CPU-ATOM-029: a waiter parked on an atomic is released by notify_one.
TEST(CpuNativeAtomics, WaitEngineNotifyOne)
{
	atomic_t<u32> flag{0};
	atomic_t<u32> parked{0};

	std::thread waiter([&]
	{
		parked = 1;
		flag.wait(0, wait_2s);
	});

	while (!parked)
	{
		std::this_thread::yield();
	}

	let_waiters_park();

	const auto start = std::chrono::steady_clock::now();
	flag = 1;
	flag.notify_one();
	waiter.join();

	EXPECT_LT(std::chrono::steady_clock::now() - start, wake_budget);
}

// CPU-ATOM-030: notify_all releases every waiter on one address.
TEST(CpuNativeAtomics, WaitEngineNotifyAll)
{
	atomic_t<u32> flag{0};
	atomic_t<u32> woken{0};

	const auto waiter_body = [&]
	{
		woken++;
		flag.wait(0, wait_2s);
		woken++;
	};

	const auto start = std::chrono::steady_clock::now();

	std::array<std::thread, 8> waiters;
	for (auto& t : waiters)
	{
		t = std::thread(waiter_body);
	}

	// Every waiter must have entered the engine before the value changes,
	// otherwise this only measures threads that never slept.
	while (woken < 8)
	{
		std::this_thread::yield();
	}

	let_waiters_park();

	flag = 1;
	flag.notify_all();

	for (auto& t : waiters)
	{
		t.join();
	}

	EXPECT_EQ(woken.load(), 16u);
	EXPECT_LT(std::chrono::steady_clock::now() - start, wake_budget);
}

#if defined(__APPLE__)
// CPU-ATOM-032: the red-zone test for the wait engine (#18429). The waited-on
// atomics end exactly where an inaccessible page begins, and the waiter runs on
// a 16 KiB stack, so a wide access or an oversized frame faults instead of
// silently corrupting memory. A wait list is used because it is the path that
// fills the engine's fixed-size stack arrays.
TEST(CpuNativeAtomics, WaitEngineRedZoneAndSmallStack)
{
	const long page_size = ::sysconf(_SC_PAGESIZE);
	ASSERT_GT(page_size, 0);

	void* const region = ::mmap(nullptr, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	ASSERT_NE(region, MAP_FAILED);

	auto* const guard = static_cast<char*>(region) + page_size;
	ASSERT_EQ(::mprotect(guard, page_size, PROT_NONE), 0);

	// The last 8 bytes before the guard page: two distinct wait addresses, both
	// ending where the inaccessible page starts.
	atomic_t<u32>* const first = new (guard - 8) atomic_t<u32>{0};
	atomic_t<u32>* const second = new (guard - 4) atomic_t<u32>{0};

	void* const stack = ::mmap(nullptr, 16 * 1024, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	ASSERT_NE(stack, MAP_FAILED);

	atomic_t<u32> parked{0};
	atomic_t<u32> woken{0};

	pthread_attr_t attr{};
	ASSERT_EQ(pthread_attr_init(&attr), 0);
	ASSERT_EQ(pthread_attr_setstack(&attr, stack, 16 * 1024), 0);

	struct context_t
	{
		atomic_t<u32>* first;
		atomic_t<u32>* second;
		atomic_t<u32>* parked;
		atomic_t<u32>* woken;
	} context{first, second, &parked, &woken};

	pthread_t thread{};
	const auto body = [](void* arg) -> void*
	{
		auto& ctx = *static_cast<context_t*>(arg);

		atomic_wait::list<2> list;
		list.set<0>(*ctx.first, 0u);
		list.set<1>(*ctx.second, 0u);

		*ctx.parked = 1;
		list.wait(wait_2s);
		*ctx.woken = 1;
		return nullptr;
	};

	ASSERT_EQ(pthread_create(&thread, &attr, body, &context), 0);

	while (!parked)
	{
		std::this_thread::yield();
	}

	let_waiters_park();

	first->store(1);
	second->store(1);
	first->notify_all();
	second->notify_all();

	const auto start = std::chrono::steady_clock::now();
	ASSERT_EQ(pthread_join(thread, nullptr), 0);

	EXPECT_EQ(woken.load(), 1u);
	EXPECT_LT(std::chrono::steady_clock::now() - start, wake_budget);

	pthread_attr_destroy(&attr);
	::munmap(stack, 16 * 1024);
	::mprotect(guard, page_size, PROT_READ | PROT_WRITE);
	::munmap(region, 2 * page_size);
}
#endif
