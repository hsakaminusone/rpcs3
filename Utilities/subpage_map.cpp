#include "subpage_map.h"

namespace vm::subpage
{
	atomic_t<u8> g_host_page_state[host_page_count] = {};
	atomic_t<u8> g_subpage_mapped[host_page_count] = {};
	atomic_t<u32> g_mem_subpage_active{0};

	namespace
	{
		// Walk the guest pages of [addr, addr + size).
		template <typename F>
		void for_each_page(u32 addr, u32 size, F&& fn)
		{
			const u64 end = static_cast<u64>(addr) + size;
			u32 page = addr >> 12;

			do
			{
				fn(page);
			}
			while (++page != (end + 0xFFF) >> 12);
		}

		// Derive the host page's state from its mapped bits.
		void republish(u32 host)
		{
			auto& state = g_host_page_state[host];

			// Outside a notification region the page carries no check, so its bits
			// are never read by generated code and there is nothing to derive.
			if (state.load() == not_notify)
			{
				return;
			}

			// The bits can move under us. Republishing and re-reading is enough:
			// whichever thread changes them last runs this again, so the settled
			// value agrees with the settled bits.
			for (int attempt = 0; attempt < 4; attempt++)
			{
				const u8 bits = g_subpage_mapped[host];
				state = (bits == 0xF) ? full : (bits == 0) ? empty : partial;

				if (g_subpage_mapped[host].load() == bits)
				{
					break;
				}
			}
		}
	}

	void note_mapped(u32 addr, u32 size)
	{
		if (!size)
		{
			return;
		}

		for_each_page(addr, size, [](u32 page)
		{
			g_subpage_mapped[page >> 2].fetch_or(static_cast<u8>(1u << (page & 3)));
		});

		for_each_page(addr, size, [](u32 page) { republish(page >> 2); });
	}

	void note_unmapped(u32 addr, u32 size)
	{
		if (!size)
		{
			return;
		}

		for_each_page(addr, size, [](u32 page)
		{
			g_subpage_mapped[page >> 2].fetch_and(static_cast<u8>(~(1u << (page & 3))));
		});

		for_each_page(addr, size, [](u32 page) { republish(page >> 2); });
	}

	void note_notification(u32 addr, u32 size, bool enabled)
	{
		if (!size)
		{
			return;
		}

		for_each_page(addr, size, [enabled](u32 page)
		{
			const u32 host = page >> 2;
			const u8 bits = g_subpage_mapped[host];
			const u8 next = enabled
				? ((bits == 0xF) ? full : (bits == 0) ? empty : partial)
				: not_notify;

			// `active` follows the exchange that publishes the state, so two threads
			// enabling one page cannot count it twice.
			if (const u8 old = g_host_page_state[host].exchange(next); enabled)
			{
				if (old == not_notify)
				{
					g_mem_subpage_active++;
				}
			}
			else if (old != not_notify)
			{
				g_mem_subpage_active--;
			}
		});
	}

	state query(u32 addr)
	{
		return static_cast<state>(g_host_page_state[addr >> host_page_shift].load());
	}

	void reset()
	{
		for (u32 i = 0; i < host_page_count; i++)
		{
			g_host_page_state[i] = not_notify;
			g_subpage_mapped[i] = 0;
		}

		g_mem_subpage_active = 0;
	}
}
