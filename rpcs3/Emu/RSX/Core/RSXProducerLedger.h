#pragma once

// EXCISE-BEGIN fp-producer-ledger
// TODO(excise): level 5; the producer ledger.  Answers "who wrote these bytes"
// for a fragment-program literal by recording every instrumented store in a
// bounded ring, independent of the stage watch's address filters, and by
// keeping a per-writer census.  A store the ring missed because it predates the
// program's first sight is recovered by the origin report: the quad is searched
// for in guest RAM and in the decrypted boot executable, and a copy/render
// destination that overlaps it is named from the copy ledger.
//
// Switches (all default off):
//   RSX_FP_STAGE_WATCH   the existing stage watcher (not required by the ledger)
//   RSX_FP_LEDGER        non-empty enables the ledger; "all" records every
//                        store, "ranges" records only stores inside a seeded
//                        range (program image, copy source/destination, defect
//                        quad) and the first store of each writer.
//   RSX_FP_LEDGER_LOG    output file (default /tmp/rpcs3-fp-ledger.log)
//   RSX_FP_LEDGER_SEED   extra ranges, "addr:len[,addr:len...]"
//   RSX_FP_LEDGER_SAMPLE record one store in N (default 1)
//   RSX_FP_LEDGER_MS     harvest length in milliseconds (default 4000)
//
// The ring is written by the store path with one relaxed atomic and a plain
// record write; the harvest thread copies it out.  Nothing here allocates or
// formats on the store path.

#include "Emu/Memory/vm.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace rsx::producer_ledger
{
	enum : u16
	{
		flag_stage = 1u << 0,   // inside a seeded program-image / copy range
		flag_gpu = 1u << 1,     // an RSX copy destination (nv308a/nv3089)
		flag_surface = 1u << 2, // an RSX render target / resolve destination
		flag_defect = 1u << 3,  // the store word matched a tracked defect
		flag_seed = 1u << 4,    // inside an operator-supplied seed range
		flag_heartbeat = 1u << 5, // first store of this writer
	};

	struct record
	{
		std::atomic<u64> seq{0};
		u64 addr = 0;
		u32 size = 0;
		u32 pc = 0;
		const char* writer = nullptr;
		u32 word = 0;
		u16 flags = 0;
		u16 writer_id = 0;
	};

	struct defect
	{
		struct copy_note
		{
			u32 dst = 0;
			u32 size = 0;
			u32 src = 0;
			const char* writer = nullptr;
			bool gpu = false;
			bool surface = false;
		};

		u32 addr = 0;       // the lane the fill found
		u32 word = 0;       // the guest word at the lane
		u32 quad = 0;       // the 16-byte aligned base
		u8 bytes[16] = {};  // the quad as it was when first seen
		u64 first_seq = 0;
		const char* source = nullptr;
		u32 hits = 0;
		bool gpu_overlap = false;
		bool surface_overlap = false;
		u32 copy_count = 0;
		copy_note copies[8] = {};
	};

	struct writer_census
	{
		std::atomic<const char*> name{nullptr};
		std::atomic<u64> calls{0};
		std::atomic<u64> stage{0};
		std::atomic<u64> gpu{0};
		std::atomic<u64> surface{0};
		std::atomic<u32> last_addr{0};
		std::atomic<u32> last_pc{0};
	};

	static constexpr u32 max_writers = 32;
	static constexpr u32 max_defects = 64;
	// 1.5 M lines is roughly 60 MB, enough for the pre-arm tail plus the post-arm
	// seeded stores, and small enough that a capture is not dominated by the log.
	static constexpr u64 max_harvest_lines = 1'500'000;
	static constexpr u32 page_shift = 12;
	static constexpr u32 page_words = (0x1'0000'0000ull >> page_shift) / 64; // 4 GiB / 4 KiB / 64 bits

	struct state
	{
		bool enabled = false;
		bool all = false;
		std::atomic<bool> initialized{false};
		std::once_flag init_once;
		u32 sample = 1;
		std::atomic<u64> seq{0};
		std::unique_ptr<record[]> ring;
		std::unique_ptr<std::atomic<u64>[]> pages;
		u32 ring_slots = 0;
		std::atomic<u32> writer_count{0};
		writer_census writers[max_writers];
		std::mutex defect_mutex;
		defect defects[max_defects];
		u32 defect_count = 0;
		std::mutex log_mutex;
		std::FILE* log = nullptr;
		std::atomic<bool> harvest_active{false};
		std::atomic<u64> harvest_until{0};
		// Total records the harvest may write.  The pre-arm dump is bounded by the
		// ring; the continuous drain is bounded here so a busy title cannot turn a
		// capture into hundreds of megabytes.
		std::atomic<u64> harvest_lines{0};
		std::atomic<u64> dumped_seq{0};
		std::atomic<u64> dumped_records{0};
		std::atomic<u64> dropped_records{0};
		std::once_flag thread_once;
		std::atomic<bool> once_report{false};
	};

	inline state& get()
	{
		static state s;
		return s;
	}

	inline u64 now_ms()
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}

	inline void seed_range(u32 addr, u32 len);

	inline const char* log_path()
	{
		if (const char* p = std::getenv("RSX_FP_LEDGER_LOG"))
		{
			return p;
		}

		return "/tmp/rpcs3-fp-ledger.log";
	}

	inline u32 parse_u32(const char* text, u32 fallback)
	{
		if (!text || !*text)
		{
			return fallback;
		}

		char* end = nullptr;
		const unsigned long value = std::strtoul(text, &end, 0);
		return (end && end != text) ? static_cast<u32>(value) : fallback;
	}

	inline void init_once();

	inline void init()
	{
		state& s = get();

		if (s.initialized.load(std::memory_order_relaxed))
		{
			return;
		}

		std::call_once(s.init_once, [] {
			init_once();
		});
	}

	inline void init_once()
	{
		state& s = get();
		const char* mode = std::getenv("RSX_FP_LEDGER");

		if (!mode || !*mode)
		{
			s.initialized.store(true, std::memory_order_relaxed);
			return;
		}

		s.enabled = true;
		s.all = std::strcmp(mode, "ranges") != 0;
		s.sample = std::max<u32>(1, parse_u32(std::getenv("RSX_FP_LEDGER_SAMPLE"), 1));

		// 1 M records is 40 MiB and covers ~50 ms of a 20 M store/s stream; the
		// pre-F12 tail only has to span the frame that produced the defect.
		s.ring_slots = 1u << 20;
		s.ring = std::make_unique<record[]>(s.ring_slots);
		s.pages = std::make_unique<std::atomic<u64>[]>(page_words);

		for (u32 i = 0; i < page_words; ++i)
		{
			s.pages[i].store(0, std::memory_order_relaxed);
		}

		s.log = std::fopen(log_path(), "a");

		if (s.log)
		{
			// Flushed at once: a run that never sees F12 is killed with no
			// dump, and an unflushed start line makes "no init" and "no
			// press" indistinguishable.
			std::fprintf(s.log, "ledger start mode=%s sample=%u slots=%u\n", mode, s.sample, s.ring_slots);
			std::fflush(s.log);
		}

		// Seed ranges from the environment: addr:len pairs, comma separated.
		if (const char* seeds = std::getenv("RSX_FP_LEDGER_SEED"))
		{
			const char* p = seeds;

			while (*p)
			{
				char* end = nullptr;
				const unsigned long long addr = std::strtoull(p, &end, 0);

				if (end == p)
				{
					break;
				}

				p = end;

				if (*p == ':')
				{
					++p;
				}

				const unsigned long long len = std::strtoull(p, &end, 0);

				if (end == p)
				{
					break;
				}

				p = end;

				if (*p == ',')
				{
					++p;
				}

				seed_range(static_cast<u32>(addr), static_cast<u32>(len));
			}
		}

		s.initialized.store(true, std::memory_order_relaxed);
	}

	inline void seed_page(u32 addr)
	{
		state& s = get();

		if (!s.enabled || !s.pages)
		{
			return;
		}

		const u32 page = addr >> page_shift;

		if (page >= page_words * 64)
		{
			return;
		}

		s.pages[page >> 6].fetch_or(1ull << (page & 63), std::memory_order_relaxed);
	}

	inline bool page_seeded(u32 addr)
	{
		state& s = get();

		if (!s.enabled || !s.pages)
		{
			return false;
		}

		const u32 page = addr >> page_shift;

		if (page >= page_words * 64)
		{
			return false;
		}

		return (s.pages[page >> 6].load(std::memory_order_relaxed) >> (page & 63)) & 1;
	}

	inline void seed_range(u32 addr, u32 len)
	{
		if (!addr || !len)
		{
			return;
		}

		state& s = get();

		if (!s.enabled)
		{
			return;
		}

		const u64 lo = static_cast<u64>(addr) & ~0xfffull;
		const u64 hi = std::min<u64>(static_cast<u64>(addr) + len, 0x1'0000'0000ull);

		for (u64 p = lo; p < hi; p += 0x1000)
		{
			seed_page(static_cast<u32>(p));
		}
	}

	inline bool overlaps_seed(u32 addr, u32 size)
	{
		state& s = get();

		if (!s.enabled || !s.pages || !size)
		{
			return false;
		}

		const u64 hi = static_cast<u64>(addr) + size;
		u64 p = static_cast<u64>(addr) & ~0xfffull;

		for (; p < hi; p += 0x1000)
		{
			if (page_seeded(static_cast<u32>(p)))
			{
				return true;
			}
		}

		return false;
	}

	inline u32 writer_id(const char* writer)
	{
		state& s = get();
		thread_local const char* cached_name = nullptr;
		thread_local u32 cached_id = 0;

		if (writer == cached_name && writer)
		{
			return cached_id;
		}

		u32 count = s.writer_count.load(std::memory_order_acquire);

		for (u32 i = 0; i < count; ++i)
		{
			if (s.writers[i].name.load(std::memory_order_relaxed) == writer)
			{
				cached_name = writer;
				cached_id = i;
				return i;
			}
		}

		if (count < max_writers)
		{
			s.writers[count].name.store(writer, std::memory_order_relaxed);
			s.writer_count.store(count + 1, std::memory_order_release);
			cached_name = writer;
			cached_id = count;
			return count;
		}

		return 0;
	}

	inline void push(const char* writer, u32 addr, u32 size, u32 pc, u32 word, u16 flags)
	{
		state& s = get();

		const u64 n = s.seq.fetch_add(1, std::memory_order_relaxed) + 1;

		if (s.sample > 1 && (n % s.sample) != 0)
		{
			return;
		}

		record& r = s.ring[static_cast<u32>(n - 1) % s.ring_slots];
		r.addr = addr;
		r.size = size;
		r.pc = pc;
		r.writer = writer;
		r.word = word;
		r.flags = flags;
		r.writer_id = static_cast<u16>(writer_id(writer));
		r.seq.store(n, std::memory_order_release);

		if (flags & flag_stage)
		{
			s.writers[r.writer_id].stage.fetch_add(1, std::memory_order_relaxed);
		}

		if (flags & flag_gpu)
		{
			s.writers[r.writer_id].gpu.fetch_add(1, std::memory_order_relaxed);
		}

		if (flags & flag_surface)
		{
			s.writers[r.writer_id].surface.fetch_add(1, std::memory_order_relaxed);
		}
	}

	// Called for every instrumented store, from every layer, before the stage
	// watch's own filters.  `size` is the byte span the store wrote.
	inline void record_store(u32 addr, u32 size, const char* writer, u32 pc)
	{
		init();
		state& s = get();

		if (!s.enabled)
		{
			return;
		}

		const u32 id = writer_id(writer);
		writer_census& c = s.writers[id];
		const u64 calls = c.calls.fetch_add(1, std::memory_order_relaxed) + 1;
		c.last_addr.store(addr, std::memory_order_relaxed);
		c.last_pc.store(pc, std::memory_order_relaxed);

		const bool seeded = overlaps_seed(addr, size);
		const bool first = calls == 1;

		if (!s.all && !seeded && !first)
		{
			return;
		}

		u32 word = 0;

		if (seeded && vm::check_addr(addr, vm::page_readable, std::min<u32>(size, 4)))
		{
			std::memcpy(&word, vm::base(addr), std::min<u32>(size, 4));
		}

		push(writer, addr, size, pc, word, (seeded ? flag_stage : 0) | (first ? flag_heartbeat : 0));
	}

	// Called after an RSX copy wrote guest memory.  `gpu` marks a copy that may
	// complete asynchronously (the bytes at `dst` are not authoritative yet).
	inline void record_copy(u32 dst, u32 size, const char* writer, u32 src, [[maybe_unused]] u32 src_size, bool gpu)
	{
		init();
		state& s = get();

		if (!s.enabled)
		{
			return;
		}

		// The copy itself is recorded below; seeding the source and destination
		// would turn every later store into those ranges into a logged "stage"
		// record and is what inflated a harvest to 722 MB.  The defect quad is
		// seeded by note_defect, which is the only range that matters here.

		u32 word = 0;

		if (!gpu && vm::check_addr(dst, vm::page_readable, std::min<u32>(size, 4)))
		{
			std::memcpy(&word, vm::base(dst), std::min<u32>(size, 4));
		}

		push(writer, dst, size, src, word, flag_gpu);

		// A copy whose destination is a tracked defect quad is a GPU producer
		// even when the bytes are not readable yet.
		std::lock_guard lock(s.defect_mutex);

		for (u32 i = 0; i < s.defect_count; ++i)
		{
			defect& d = s.defects[i];

			if (dst < d.quad + 16 && d.quad < dst + size)
			{
				d.gpu_overlap = true;

				if (d.copy_count < 8)
				{
					defect::copy_note& n = d.copies[d.copy_count++];
					n.dst = dst;
					n.size = size;
					n.src = src;
					n.writer = writer;
					n.gpu = gpu;
				}
			}
		}
	}

	inline void record_surface(u32 addr, u32 size, const char* writer)
	{
		init();
		state& s = get();

		if (!s.enabled || !addr || !size)
		{
			return;
		}

		// No seed_range here: the surface record itself is the evidence, and
		// seeding the whole attachment makes every store into it a stage record
		// (see record_copy).
		push(writer, addr, size, 0, 0, flag_surface);

		std::lock_guard lock(s.defect_mutex);

		for (u32 i = 0; i < s.defect_count; ++i)
		{
			defect& d = s.defects[i];

			if (addr < d.quad + 16 && d.quad < addr + size)
			{
				d.surface_overlap = true;

				if (d.copy_count < 8)
				{
					defect::copy_note& n = d.copies[d.copy_count++];
					n.dst = addr;
					n.size = size;
					n.writer = writer;
					n.gpu = true;
					n.surface = true;
				}
			}
		}
	}

	// Register the 16-byte quad that contains a lane the fill found out of band.
	inline void note_defect(u32 addr, u32 word, const char* source)
	{
		init();
		state& s = get();

		if (!s.enabled || !addr)
		{
			return;
		}

		const u32 quad = addr & ~15u;
		seed_range(quad, 16);

		std::lock_guard lock(s.defect_mutex);

		for (u32 i = 0; i < s.defect_count; ++i)
		{
			if (s.defects[i].quad == quad)
			{
				++s.defects[i].hits;
				return;
			}
		}

		if (s.defect_count >= max_defects)
		{
			return;
		}

		defect& d = s.defects[s.defect_count++];
		d.addr = addr;
		d.word = word;
		d.quad = quad;
		d.source = source;
		d.first_seq = s.seq.load(std::memory_order_relaxed);
		d.hits = 1;

		if (vm::check_addr(quad, vm::page_readable, 16))
		{
			std::memcpy(d.bytes, vm::base(quad), 16);
		}
	}

	inline u32 defect_snapshot(defect* out, u32 max)
	{
		state& s = get();
		std::lock_guard lock(s.defect_mutex);
		const u32 n = std::min<u32>(s.defect_count, max);

		for (u32 i = 0; i < n; ++i)
		{
			out[i] = s.defects[i];
		}

		return n;
	}

	inline bool enabled()
	{
		init();
		return get().enabled;
	}

	inline void dump_census(std::FILE* out, const char* reason)
	{
		state& s = get();

		if (!out)
		{
			return;
		}

		const u32 count = s.writer_count.load(std::memory_order_acquire);
		std::fprintf(out, "census %s writers=%u\n", reason, count);

		for (u32 i = 0; i < count; ++i)
		{
			const writer_census& c = s.writers[i];
			std::fprintf(out, "  writer=%s calls=%llu stage=%llu gpu=%llu surface=%llu last=%08x pc=%08x\n",
				c.name.load(std::memory_order_relaxed) ? c.name.load(std::memory_order_relaxed) : "-",
				static_cast<unsigned long long>(c.calls.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(c.stage.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(c.gpu.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(c.surface.load(std::memory_order_relaxed)),
				c.last_addr.load(std::memory_order_relaxed),
				c.last_pc.load(std::memory_order_relaxed));
		}
	}

	inline void dump_defects(std::FILE* out)
	{
		defect list[max_defects];
		const u32 n = defect_snapshot(list, max_defects);

		for (u32 i = 0; i < n; ++i)
		{
			const defect& d = list[i];
			std::fprintf(out, "defect quad=%08x lane=%08x word=%08x source=%s hits=%u gpu=%u surface=%u bytes=",
				d.quad, d.addr, d.word, d.source ? d.source : "-", d.hits,
				d.gpu_overlap ? 1u : 0u, d.surface_overlap ? 1u : 0u);

			for (u32 b = 0; b < 16; ++b)
			{
				std::fprintf(out, "%02x", d.bytes[b]);
			}

			std::fprintf(out, "\n");
		}
	}

	inline u64 dump_records(std::FILE* out, const char* reason, bool all_records, u64 max_lines = ~0ull)
	{
		state& s = get();

		if (!out || !s.ring)
		{
			return 0;
		}

		const u64 head = s.seq.load(std::memory_order_acquire);
		u64 cursor = s.dumped_seq.load(std::memory_order_relaxed);

		if (head <= cursor)
		{
			return 0;
		}

		// If writes outran the ring, the oldest records are gone; skip to the
		// oldest slot still valid and count the loss.
		if (head - cursor > s.ring_slots)
		{
			const u64 lost = head - cursor - s.ring_slots;
			s.dropped_records.fetch_add(lost, std::memory_order_relaxed);
			cursor = head - s.ring_slots;
		}

		std::fprintf(out, "records %s from=%llu to=%llu\n", reason,
			static_cast<unsigned long long>(cursor), static_cast<unsigned long long>(head));

		u64 written = 0;
		bool truncated = false;

		for (u64 n = cursor + 1; n <= head; ++n)
		{
			const record& r = s.ring[static_cast<u32>(n - 1) % s.ring_slots];

			if (r.seq.load(std::memory_order_acquire) != n)
			{
				continue;
			}

			// Default output is the interesting subset: seeded ranges, copies,
			// surfaces, and each writer's first store.  `all` writes every record.
			if (!all_records && !(r.flags & (flag_stage | flag_gpu | flag_surface | flag_heartbeat)))
			{
				continue;
			}

			if (written >= max_lines)
			{
				truncated = true;
				break;
			}

			std::fprintf(out, "  %llu %s addr=%08llx size=%u pc=%08x word=%08x flags=%x\n",
				static_cast<unsigned long long>(n),
				r.writer ? r.writer : "-",
				static_cast<unsigned long long>(r.addr), r.size, r.pc, r.word,
				static_cast<unsigned>(r.flags));
			++written;
		}

		if (truncated)
		{
			std::fprintf(out, "records %s truncated at %llu\n", reason,
				static_cast<unsigned long long>(written));
		}

		s.dumped_seq.store(head, std::memory_order_relaxed);
		s.dumped_records.fetch_add(written, std::memory_order_relaxed);
		return written;
	}

	[[noreturn]] inline void harvest_thread()
	{
		state& s = get();

		while (true)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20));

			if (!s.harvest_active.load(std::memory_order_relaxed))
			{
				continue;
			}

			{
				std::lock_guard lock(s.log_mutex);

				if (s.log)
				{
					const u64 used = std::min<u64>(s.harvest_lines.load(std::memory_order_relaxed), max_harvest_lines);
					const u64 remaining = max_harvest_lines - used;

					if (remaining)
					{
						const u64 written = dump_records(s.log, "harvest", false, remaining);
						s.harvest_lines.fetch_add(written, std::memory_order_relaxed);
					}
				}
			}

			if (now_ms() >= s.harvest_until.load(std::memory_order_relaxed))
			{
				std::lock_guard lock(s.log_mutex);

				if (s.log)
				{
					const u64 used = std::min<u64>(s.harvest_lines.load(std::memory_order_relaxed), max_harvest_lines);
					const u64 remaining = max_harvest_lines - used;

					if (remaining)
					{
						const u64 written = dump_records(s.log, "harvest-tail", false, remaining);
						s.harvest_lines.fetch_add(written, std::memory_order_relaxed);
					}

					dump_census(s.log, "harvest-end");
					dump_defects(s.log);
					std::fprintf(s.log, "harvest end dumped=%llu dropped=%llu\n",
						static_cast<unsigned long long>(s.dumped_records.load(std::memory_order_relaxed)),
						static_cast<unsigned long long>(s.dropped_records.load(std::memory_order_relaxed)));
					std::fflush(s.log);
				}

				s.harvest_active.store(false, std::memory_order_relaxed);
			}
		}
	}

	// F12.  Dumps the pre-press ring (the frame that produced the defect is in
	// it), then keeps draining for `ms` so the next few frames are covered too.
	inline void arm(u32 ms)
	{
		init();
		state& s = get();

		if (!s.enabled)
		{
			return;
		}

		std::call_once(s.thread_once, [] { std::thread(harvest_thread).detach(); });

		if (ms == 0)
		{
			ms = parse_u32(std::getenv("RSX_FP_LEDGER_MS"), 4000);
		}

		{
			std::lock_guard lock(s.log_mutex);

			if (s.log)
			{
				std::fprintf(s.log, "arm t=%llu ms=%u head=%llu\n",
					static_cast<unsigned long long>(now_ms()), ms,
					static_cast<unsigned long long>(s.seq.load(std::memory_order_relaxed)));
				dump_census(s.log, "arm-first");
				dump_defects(s.log);

				// The whole pre-press tail, not just the seeded subset: the store
				// that produced the word on screen is the last few milliseconds.
				// It is bounded by the ring, and the continuous drain below has its
				// own budget.
				dump_records(s.log, "pre-arm", true, s.ring_slots);
				std::fflush(s.log);
			}
		}

		s.harvest_until.store(now_ms() + ms, std::memory_order_relaxed);
		s.harvest_lines.store(0, std::memory_order_relaxed);
		s.harvest_active.store(true, std::memory_order_relaxed);
	}
}
// EXCISE-END fp-producer-ledger
