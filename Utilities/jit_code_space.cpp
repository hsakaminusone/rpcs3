#include "jit_code_space.h"

#include "mutex.h"

namespace jit_code_space
{
	namespace
	{
		struct slot
		{
			u8* base = nullptr;
			u64 epoch = 0;
			bool in_use = false;
		};

		struct space
		{
			shared_mutex mutex;
			std::vector<slot> slots;
		};

		space& get()
		{
			static space s_space;
			return s_space;
		}
	}

	void add(u8* base)
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		self.slots.push_back({base, 0, false});
	}

	u8* acquire(u64 epoch)
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		slot* best = nullptr;

		for (auto& candidate : self.slots)
		{
			// A chunk written at or after this epoch may still have a live link
			// pointing into it, so it is not a candidate.
			if (candidate.in_use || candidate.epoch >= epoch)
			{
				continue;
			}

			if (!best || candidate.epoch < best->epoch)
			{
				best = &candidate;
			}
		}

		if (!best)
		{
			return nullptr;
		}

		best->in_use = true;
		best->epoch = epoch;

		return best->base;
	}

	void release(u8* base, u64 epoch)
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		for (auto& candidate : self.slots)
		{
			if (candidate.base == base)
			{
				candidate.in_use = false;
				candidate.epoch = epoch;
				return;
			}
		}
	}

	usz live_count()
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		usz count = 0;

		for (const auto& candidate : self.slots)
		{
			count += candidate.in_use ? 1 : 0;
		}

		return count;
	}

	usz free_count()
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		usz count = 0;

		for (const auto& candidate : self.slots)
		{
			count += candidate.in_use ? 0 : 1;
		}

		return count;
	}

	void reset()
	{
		auto& self = get();
		std::lock_guard lock(self.mutex);

		self.slots.clear();
	}
}
