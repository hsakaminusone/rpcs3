#pragma once

#include <util/types.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// EXCISE-BEGIN fp-literal-write-watch
// TODO(excise): level 5; attributes a change in a fragment program's trailing
// literal block to the copy that made it.  A watch entry is created for every
// prefetched program (bounded to 16) and holds a shadow of its last 384 bytes,
// so a write is checked against every program the guest has used, not only the
// one bound at the time -- the gap that hid the Demon's Souls world patch.  The
// watch is keyed on the host pointer the copy actually wrote through, so a blit
// that writes at an element offset inside its destination is still matched.
// Only a 4-byte lane that differs from the shadow is reported, and the report
// carries the source bytes, so the byte order the writer produced can be read
// directly instead of inferred.  Bounded, deduplicated by entry, default off.
namespace rsx
{
	class program_write_watch
	{
	public:
		static constexpr unsigned max_entries = 64;
		static constexpr unsigned tail_bytes = 384;
		static constexpr unsigned max_reports = 1024;
		static constexpr unsigned max_reports_per_entry = 8;
		static constexpr unsigned dedupe_slots = 128;

		// EXCISE-BEGIN fp-literal-write-ring
		// TODO(excise): level 5; dump the ring for an arbitrary host-pointer range.
		// Called from the fragment-constant fill when a world-family image with a
		// reversed literal is observed, so the writes that produced it are captured
		// even if the ring wrapped since the program's first prefetch.
		static void dump_range(const void* lo, u32 len, const char* reason)
		{
			if (!enabled() || !lo)
			{
				return;
			}

			const u64 base = reinterpret_cast<u64>(lo);
			dump_ring_for(base, base + len, reason);
		}
		// EXCISE-END fp-literal-write-ring

		// EXCISE-BEGIN fp-literal-write-ring
		// TODO(excise): level 5; a global ring of EVERY instrumented blit write,
		// independent of whether a watch entry already exists.  The watch can only
		// attribute writes to a program that was already prefetched; the bytes of
		// interest can be written before the first prefetch, which is invisible to
		// the watch.  At claim() the ring is dumped for the program's address range,
		// so the write that first produced the literal is recovered.
		static constexpr unsigned write_ring_slots = 16384;
		static constexpr unsigned ring_dump_limit = 64;

		struct write_record
		{
			bool used = false;
			const char* writer = nullptr;
			u64 dst = 0;
			u32 size = 0;
			u64 src = 0;
			u8 src_bytes[32] = {};
			u8 dst_bytes[32] = {};
			char detail[192] = {};
		};
		// EXCISE-END fp-literal-write-ring

		struct entry
		{
			bool used = false;
			u64 addr = 0;
			u32 offset = 0;
			u32 len = 0;
			const u8* tail = nullptr;
			u32 tail_len = 0;
			u8 shadow[tail_bytes] = {};
			u32 report_count = 0;
		};

		static bool enabled()
		{
			return std::getenv("RSX_FP_WATCH_LOG") != nullptr;
		}

		static entry* slots()
		{
			static entry s_entries[max_entries];
			return s_entries;
		}

		static entry* find(u64 addr)
		{
			for (unsigned i = 0; i < max_entries; ++i)
			{
				if (slots()[i].used && slots()[i].addr == addr)
				{
					return &slots()[i];
				}
			}

			return nullptr;
		}

		// Called by the prefetch of a fragment program with the image it just
		// analysed.  A change since the previous prefetch of the same image that
		// no writer claimed is itself a result: it proves the mutation happened
		// outside the instrumented copies.
		static void claim(u64 addr, u32 offset, u32 len, const u8* image)
		{
			if (!enabled() || !image || len < 64)
			{
				return;
			}

			const u32 n = std::min<u32>(tail_bytes, len);
			entry* e = find(addr);

			if (e)
			{
				if (e->tail_len == n && std::memcmp(e->shadow, image + len - n, n) != 0)
				{
					report(e, "drift", 0, 0, nullptr, 0, "", image + len - n);
				}

				e->tail = image + len - n;
				e->tail_len = n;
				std::memcpy(e->shadow, e->tail, n);
				return;
			}

			// No existing entry: take a slot, evicting the oldest one when the
			// table is full (round-robin).  find(addr) ran first, so a live entry
			// for this image is never chosen for eviction.
			e = allocate();
			e->used = true;
			e->addr = addr;
			e->offset = offset;
			e->len = len;
			e->tail = image + len - n;
			e->tail_len = n;
			std::memcpy(e->shadow, e->tail, n);

			// EXCISE-BEGIN fp-literal-write-ring
			// TODO(excise): level 5; first sight of this image.  Dump every
			// instrumented write that landed inside its range before now -- those
			// are the writes the watch could not have attributed (they predate the
			// entry), and one of them is where a reversed literal came from.
			dump_ring_for(addr, addr + len, "claim-first");
			// EXCISE-END fp-literal-write-ring
		}

		// Called by a copy after it wrote `size` bytes at `addr`.  `src` and
		// `src_len` are the bytes the writer consumed, so the log shows both ends
		// of the copy.  `detail` fills a short register summary.
		template <typename Detail>
		static void note(const char* writer, u64 addr, u32 size, const void* src, u32 src_len, Detail&& detail)
		{
			if (!enabled() || !size)
			{
				return;
			}

			// EXCISE-BEGIN fp-literal-write-ring
			// TODO(excise): level 5; record every write in the global ring before
			// any watch matching, so writes that predate a program's first prefetch
			// are still recoverable when that program is later claimed.
			record_write(writer, addr, size, src, src_len, detail);
			// EXCISE-END fp-literal-write-ring

			for (unsigned i = 0; i < max_entries; ++i)
			{
				entry* e = &slots()[i];

				if (!e->used || !e->tail || addr + size <= e->addr || e->addr + e->len <= addr)
				{
					continue;
				}

				if (std::memcmp(e->shadow, e->tail, e->tail_len) == 0)
				{
					continue;
				}

				char buffer[192] = {};
				detail(buffer, sizeof(buffer));

				report(e, writer, addr, size, src, src_len, buffer, e->tail);
				std::memcpy(e->shadow, e->tail, e->tail_len);
			}
		}

	private:
		// EXCISE-BEGIN fp-literal-write-ring
		// TODO(excise): level 5; global ring of all instrumented blit writes.
		static write_record* ring()
		{
			static write_record s_ring[write_ring_slots];
			return s_ring;
		}

		template <typename Detail>
		static void record_write(const char* writer, u64 addr, u32 size, const void* src, u32 src_len, Detail& detail)
		{
			static unsigned cursor = 0;
			write_record& r = ring()[cursor];
			cursor = (cursor + 1) % write_ring_slots;

			r = write_record{};
			r.used = true;
			r.writer = writer;
			r.dst = addr;
			r.size = size;
			r.src = reinterpret_cast<u64>(src);

			if (src && src_len)
			{
				std::memcpy(r.src_bytes, src, std::min<u32>(sizeof(r.src_bytes), src_len));
			}

			// The bytes actually present at the destination right after the copy.
			// Guard the read: the destination is a 64-bit host pointer and size is
			// bounded by the copy, but clamp to the record capacity regardless.
			if (addr)
			{
				const u32 n = std::min<u32>(sizeof(r.dst_bytes), size);
				std::memcpy(r.dst_bytes, reinterpret_cast<const void*>(addr), n);
			}

			detail(r.detail, sizeof(r.detail));
		}

		static void dump_ring_for(u64 lo, u64 hi, const char* reason)
		{
			std::FILE* out = std::fopen(std::getenv("RSX_FP_WATCH_LOG"), "a");
			if (!out)
			{
				return;
			}

			std::fprintf(out, "ring-scan %s range %llx..%llx\n", reason,
				static_cast<unsigned long long>(lo), static_cast<unsigned long long>(hi));

			unsigned shown = 0;

			for (unsigned i = 0; i < write_ring_slots; ++i)
			{
				const write_record& r = ring()[i];

				if (!r.used || r.dst + r.size <= lo || hi <= r.dst)
				{
					continue;
				}

				u32 dstw[4], srcw[4];
				std::memcpy(dstw, r.dst_bytes, sizeof(dstw));
				std::memcpy(srcw, r.src_bytes, sizeof(srcw));
				std::fprintf(out, "  ring %s dst=%llx size=%u src=%llx %s\n",
					r.writer, static_cast<unsigned long long>(r.dst), r.size,
					static_cast<unsigned long long>(r.src), r.detail);
				std::fprintf(out, "    dst_bytes=%08x %08x %08x %08x src_bytes=%08x %08x %08x %08x\n",
					dstw[0], dstw[1], dstw[2], dstw[3], srcw[0], srcw[1], srcw[2], srcw[3]);

				if (++shown >= ring_dump_limit)
				{
					std::fprintf(out, "  ring-scan truncated at %u\n", ring_dump_limit);
					break;
				}
			}

			std::fclose(out);
		}
		// EXCISE-END fp-literal-write-ring

		// Round-robin slot allocation.  Prefer an unused slot starting from the
		// cursor; if the table is full, reuse (evict) the slot at the cursor and
		// advance it, so every entry eventually gets replaced in turn.
		static entry* allocate()
		{
			static unsigned cursor = 0;

			for (unsigned i = 0; i < max_entries; ++i)
			{
				const unsigned idx = (cursor + i) % max_entries;

				if (!slots()[idx].used)
				{
					cursor = (idx + 1) % max_entries;
					entry* e = &slots()[idx];
					*e = entry{};
					return e;
				}
			}

			const unsigned idx = cursor;
			cursor = (idx + 1) % max_entries;
			entry* e = &slots()[idx];
			*e = entry{};
			return e;
		}

		static f32 read_word(u32 word)
		{
			return std::bit_cast<f32>(word);
		}

		// The image byte order the fill expects: the RSX stores a 32-bit word as
		// two 16-bit halves, each byte-swapped.
		static u32 expected(u32 word)
		{
			return ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
		}

		static void report(entry* e, const char* writer, u64 addr, u32 size, const void* src, u32 src_len, const char* detail, const u8* after)
		{
			static unsigned s_reports = 0;
			static unsigned s_dedupe_cursor = 0;

			// Identity of a report, used to collapse repeated writes (e.g. an
			// alternating 4-byte flip) into a single log entry.  The `owner` is
			// the watched program, so two programs writing identical bytes stay
			// distinct.
			struct signature
			{
				u64 owner = 0;
				u64 addr = 0;
				u32 size = 0;
				u32 before[4] = {};
				u32 after[4] = {};
				bool used = false;
			};

			static signature s_recent[dedupe_slots];

			if (s_reports >= max_reports || !after || e->tail_len < 16)
			{
				return;
			}

			u32 first = 0;

			while (first < e->tail_len && e->shadow[first] == after[first])
			{
				++first;
			}

			if (first == e->tail_len)
			{
				return;
			}

			const u32 window = std::min<u32>(first & ~3u, e->tail_len - 16);
			const u32 at = e->offset + e->len - e->tail_len + window;

			u32 before_words[4] = {};
			u32 after_words[4] = {};
			u32 src_words[4] = {};

			std::memcpy(before_words, e->shadow + window, 16);
			std::memcpy(after_words, after + window, 16);

			// Bounded, round-robin dedupe ring.  A signature already seen for
			// this program is skipped without spending the global report budget.
			for (unsigned i = 0; i < dedupe_slots; ++i)
			{
				const signature& sig = s_recent[i];

				if (sig.used && sig.owner == e->addr && sig.addr == addr && sig.size == size &&
					std::memcmp(sig.before, before_words, 16) == 0 &&
					std::memcmp(sig.after, after_words, 16) == 0)
				{
					return;
				}
			}

			// Per-entry budget: even a program with many distinct writes cannot
			// starve the other watched programs.
			if (e->report_count >= max_reports_per_entry)
			{
				return;
			}

			signature& sig = s_recent[s_dedupe_cursor];
			s_dedupe_cursor = (s_dedupe_cursor + 1) % dedupe_slots;
			sig = signature{};
			sig.used = true;
			sig.owner = e->addr;
			sig.addr = addr;
			sig.size = size;
			std::memcpy(sig.before, before_words, 16);
			std::memcpy(sig.after, after_words, 16);

			++s_reports;
			++e->report_count;

			if (src && src_len)
			{
				std::memcpy(src_words, src, std::min<u32>(16, src_len));
			}

			if (std::FILE* out = std::fopen(std::getenv("RSX_FP_WATCH_LOG"), "a"))
			{
				std::fprintf(out, "watch %s writer=%s program=%u+%u at=%u waddr=%llx wsize=%u src=%llx %s\n",
					writer, writer, e->offset, e->len, at,
					static_cast<unsigned long long>(addr), size,
					static_cast<unsigned long long>(reinterpret_cast<u64>(src)), detail);

				std::fprintf(out, "  before=%08x %08x %08x %08x after=%08x %08x %08x %08x src=%08x %08x %08x %08x\n",
					before_words[0], before_words[1], before_words[2], before_words[3],
					after_words[0], after_words[1], after_words[2], after_words[3],
					src_words[0], src_words[1], src_words[2], src_words[3]);

				std::fprintf(out, "  read=%.6g %.6g %.6g %.6g rev=%.6g %.6g %.6g %.6g\n",
					static_cast<f64>(read_word(expected(after_words[0]))),
					static_cast<f64>(read_word(expected(after_words[1]))),
					static_cast<f64>(read_word(expected(after_words[2]))),
					static_cast<f64>(read_word(expected(after_words[3]))),
					static_cast<f64>(read_word(std::byteswap(expected(after_words[0])))),
					static_cast<f64>(read_word(std::byteswap(expected(after_words[1])))),
					static_cast<f64>(read_word(std::byteswap(expected(after_words[2])))),
					static_cast<f64>(read_word(std::byteswap(expected(after_words[3])))));

				std::fclose(out);
			}
		}
	};
}
// EXCISE-END fp-literal-write-watch
