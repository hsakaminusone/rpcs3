#include "jit_page_index.h"

#include "mutex.h"

#include <algorithm>
#include <unordered_map>

namespace jit_page
{
	namespace
	{
		atomic_t<u64> g_epoch{1};

		struct index_t
		{
			shared_mutex mutex;
			std::unordered_map<page_t, std::vector<u64>> pages;

			void for_each_page(u32 addr, u32 size, auto&& fn)
			{
				if (!size)
				{
					return;
				}

				const u64 end = static_cast<u64>(addr) + size - 1;
				const page_t last = static_cast<page_t>(end >> 12);

				for (page_t page = addr >> 12;; page++)
				{
					fn(page);

					if (page == last)
					{
						break;
					}
				}
			}
		};

		index_t& index()
		{
			static index_t s_index;
			return s_index;
		}
	}

	u64 epoch() noexcept
	{
		return g_epoch;
	}

	void note_compiled(u32 addr, u32 size, u64 module)
	{
		auto& idx = index();
		std::lock_guard lock(idx.mutex);

		idx.for_each_page(addr, size, [&](page_t page)
		{
			auto& modules = idx.pages[page];

			if (std::find(modules.begin(), modules.end(), module) == modules.end())
			{
				modules.push_back(module);
			}
		});
	}

	u64 note_invalidated(u32 addr, u32 size)
	{
		// Bump before touching the map: a reader that saw the old epoch has to
		// re-validate, and it does not have to wait for the map update to do so.
		const u64 after = ++g_epoch;

		auto& idx = index();
		std::lock_guard lock(idx.mutex);

		// The pages keep their modules: the index is what a recompile consults to
		// find the old implementation, and forget_module() is what removes one.
		idx.for_each_page(addr, size, [](page_t) {});

		return after;
	}

	std::vector<u64> modules_over(page_t page)
	{
		auto& idx = index();
		reader_lock lock(idx.mutex);

		const auto found = idx.pages.find(page);

		if (found == idx.pages.end())
		{
			return {};
		}

		return found->second;
	}

	void forget_module(u64 module)
	{
		auto& idx = index();
		std::lock_guard lock(idx.mutex);

		for (auto it = idx.pages.begin(); it != idx.pages.end();)
		{
			auto& modules = it->second;
			modules.erase(std::remove(modules.begin(), modules.end(), module), modules.end());

			if (modules.empty())
			{
				it = idx.pages.erase(it);
				continue;
			}

			++it;
		}
	}

	void reset()
	{
		auto& idx = index();
		std::lock_guard lock(idx.mutex);

		idx.pages.clear();
		g_epoch = 1;
	}
}
