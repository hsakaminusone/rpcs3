#include "ppu_region_scan.h"

#include <algorithm>
#include <map>
#include <unordered_map>

namespace ppu_region
{
	namespace
	{
		// Fall-through successor of a block that does not end in a branch.
		u32 fallthrough_of(const block& b)
		{
			return b.addr + b.size * 4;
		}

		// Every successor edge the scan keeps.
		struct successors
		{
			const block* direct = nullptr;
			const block* fallthrough = nullptr;

			const block* only() const
			{
				if (direct && fallthrough)
				{
					return nullptr;
				}

				return direct ? direct : fallthrough;
			}
		};
	}

	std::vector<u32> loop_headers(std::span<const block> blocks)
	{
		std::map<u32, std::pair<u32, u32>> spans; // header -> {span, back-edge addr}

		for (const block& b : blocks)
		{
			if (b.has_target && b.target < b.addr)
			{
				const u32 span = b.addr - b.target;

				auto found = spans.find(b.target);

				if (found == spans.end() || found->second.first < span)
				{
					spans[b.target] = {span, b.addr};
				}
			}
		}

		std::vector<std::pair<u32, u32>> ordered; // {span, header}

		for (const auto& [header, info] : spans)
		{
			ordered.emplace_back(info.first, header);
		}

		std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
		{
			return a.first != b.first ? a.first > b.first : a.second < b.second;
		});

		std::vector<u32> result;
		result.reserve(ordered.size());

		for (const auto& [span, header] : ordered)
		{
			result.push_back(header);
		}

		return result;
	}

	std::vector<region> form(std::span<const block> blocks_view, u32 budget, u32 min_instructions)
	{
		std::vector<region> regions;

		if (blocks_view.empty())
		{
			return regions;
		}

		std::map<u32, const block*> by_addr;
		std::unordered_map<u32, u32> predecessors;

		for (const block& b : blocks_view)
		{
			by_addr[b.addr] = &b;
			predecessors.try_emplace(b.addr, 0);
		}

		const auto successor_edges = [&](const block& b, auto&& fn)
		{
			if (b.has_target && by_addr.contains(b.target))
			{
				fn(b.target);
			}

			if (!b.is_exit && !b.has_target)
			{
				const u32 fall = fallthrough_of(b);

				if (by_addr.contains(fall))
				{
					fn(fall);
				}
			}
		};

		for (const block& b : blocks_view)
		{
			successor_edges(b, [&](u32 succ)
			{
				predecessors[succ]++;
			});
		}

		const auto is_entry = [&](u32 addr)
		{
			const auto found = predecessors.find(addr);
			return found == predecessors.end() || found->second != 1;
		};

		std::unordered_map<u32, bool> absorbed;

		const auto build = [&](u32 seed, bool is_loop)
		{
			region reg;
			reg.entry = seed;
			reg.is_loop = is_loop;

			u32 addr = seed;

			while (true)
			{
				const auto found = by_addr.find(addr);

				if (found == by_addr.end() || absorbed.contains(addr))
				{
					break;
				}

				if (addr != seed && is_entry(addr))
				{
					// Reachable from outside: it keeps its own implementation.
					break;
				}

				const block& b = *found->second;

				if (reg.instructions + b.size > budget)
				{
					break;
				}

				absorbed[addr] = true;
				reg.blocks.push_back(addr);
				reg.instructions += b.size;

				// A branch inside the region is an edge, not a successor to walk:
				// only a block whose single successor continues the region is
				// followed.
				const block* next = nullptr;
				const bool direct_inside = b.has_target && by_addr.contains(b.target) && b.target > b.addr;

				if (b.is_exit)
				{
					break;
				}

				if (b.has_target && direct_inside)
				{
					// Forward branch inside the region: follow it only when it is the
					// block's only successor.
					next = by_addr.find(b.target)->second;
				}
				else if (!b.has_target)
				{
					const auto fall = by_addr.find(fallthrough_of(b));
					next = fall == by_addr.end() ? nullptr : fall->second;
				}

				if (!next || next->addr <= b.addr)
				{
					break;
				}

				addr = next->addr;
			}

			if (reg.blocks.empty())
			{
				return;
			}

			if (is_loop || reg.instructions >= min_instructions)
			{
				regions.push_back(std::move(reg));
			}
		};

		for (u32 header : loop_headers(blocks_view))
		{
			build(header, true);
		}

		for (const block& b : blocks_view)
		{
			if (!absorbed.contains(b.addr))
			{
				build(b.addr, false);
			}
		}

		return regions;
	}
}
