#include "fastmem.h"

#include "CPUStats.h"
#include "util/atomic.hpp"

#include <array>

namespace fastmem
{
	namespace
	{
		// 16K sites. A title faults on a small set of distinct instructions,
		// and the real bound is the number of unmapped pages it touches, not
		// the number of compiled accesses. The table is 512 KB.
		constexpr usz c_table_size = 1 << 14;
		constexpr usz c_probe_limit = 8;

		// A zero host PC means "empty"; no faulting instruction can live at 0.
		struct entry
		{
			atomic_t<uptr> host_pc{};
			atomic_t<u32> guest_addr{};  // full guest address of the last fault
			atomic_t<u32> faults{};
			atomic_t<u8> flags{};
		};

		std::array<entry, c_table_size> g_table{};
		atomic_t<u64> g_sites{};

		// Flags for entry::flags.
		enum : u8
		{
			is_write = 1,
		};

		usz hash_slot(uptr host_pc) noexcept
		{
			// Host instructions are 4-byte aligned; spread the remaining bits.
			return ((host_pc >> 2) * 0x9E37'79B1u) & (c_table_size - 1);
		}

		// `inserted` distinguishes a slot this call claimed from an entry that was
		// already there, including one found by backing out of a duplicate.
		entry* find_site(uptr host_pc, bool insert, bool& inserted) noexcept
		{
			inserted = false;

			const usz start = hash_slot(host_pc);

			for (usz i = 0; i < c_probe_limit; ++i)
			{
				entry& e = g_table[(start + i) & (c_table_size - 1)];
				if (e.host_pc == host_pc)
				{
					return &e;
				}

				if (e.host_pc == 0)
				{
					if (!insert)
					{
						return nullptr;
					}

					// Publish the key with one CAS: that is what makes the entry
					// findable.
					if (e.host_pc.compare_and_swap_test(0, host_pc))
					{
						// A searcher that walked past this slot while another thread
						// was inserting can have claimed a later slot for the same
						// key. Two entries would split one site's history, so keep
						// the earliest probe position and release this one.
						for (usz j = 0; j < i; ++j)
						{
							entry& earlier = g_table[(start + j) & (c_table_size - 1)];

							if (earlier.host_pc == host_pc)
							{
								e.host_pc = 0;
								return &earlier;
							}
						}

						g_sites++;
						inserted = true;
						return &e;
					}

					// Another thread inserted at this slot; re-read it before
					// moving on, it may be the site being looked for.
					if (e.host_pc == host_pc)
					{
						return &e;
					}
				}
			}

			return nullptr;
		}
	}

	site_state note_fault(uptr host_pc, u32 guest_addr, bool write) noexcept
	{
		// Faults inside the reservation always have a guest address; a host
		// PC outside the JIT code space is already fatal in the handler.
		if (!host_pc)
		{
			return site_state::first;
		}

		bool inserted = false;
		entry* e = find_site(host_pc, false, inserted);

		if (!e)
		{
			e = find_site(host_pc, true, inserted);

			if (!e)
			{
				// Table full: report as recurring so the caller does not treat
				// the site as stable, but do not record anything.
				cpu_stats::g_counters.fastmem_overflow++;
				return site_state::recurring;
			}

			// A duplicate found while backing out belongs to another thread: this
			// fault is a repeat at that site, not its first, and resetting the
			// history here would both miscount it and lose the evidence.
			if (!inserted)
			{
				e->faults += 1;
				e->guest_addr = guest_addr;
				e->flags = write ? is_write : 0;
				cpu_stats::g_counters.fastmem_recurring++;
				return site_state::recurring;
			}

			// The key is already visible, so these fields are the only thing a
			// concurrent reader can beat. They are advisory history, not state the
			// handler acts on, so a reader seeing the old values is harmless.
			e->guest_addr = guest_addr;
			e->flags = write ? is_write : 0;
			e->faults = 1;

			cpu_stats::g_counters.fastmem_first++;
			return site_state::first;
		}

		e->faults += 1;
		e->guest_addr = guest_addr;
		e->flags = write ? is_write : 0;
		cpu_stats::g_counters.fastmem_recurring++;
		return site_state::recurring;
	}

	u64 site_count() noexcept
	{
		return g_sites;
	}

	void invalidate_range(uptr host_begin, uptr host_end) noexcept
	{
		u64 removed = 0;

		for (entry& e : g_table)
		{
			const uptr key = e.host_pc;

			if (key == 0)
			{
				continue;
			}

			if (key >= host_begin && key < host_end)
		{
				// Only the thread that clears the key should count it, and a
				// concurrent reader may still be inside the entry: clear the key
				// first so it stops matching, then drop the payload.
				if (e.host_pc.compare_and_swap_test(key, 0))
				{
					e.guest_addr = 0;
					e.flags = 0;
					e.faults = 0;
					removed++;
				}
			}
		}

		g_sites -= removed;
	}

	void reset() noexcept
	{
		for (entry& e : g_table)
		{
			e.host_pc = 0;
			e.guest_addr = 0;
			e.faults = 0;
			e.flags = 0;
		}

		g_sites = 0;
	}
}
