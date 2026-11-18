#pragma once

#include <util/types.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// EXCISE-BEGIN fp-stage-store-watch-include
// TODO(excise): level 5; the stage watcher reads guest memory at a post-store
// address, so it needs vm::base and the guest address space.
#include "Emu/Memory/vm.h"
// EXCISE-END fp-stage-store-watch-include

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
			u64 seq = 0;
			u64 dst = 0;
			u32 size = 0;
			u64 src = 0;
			u8 src_bytes[32] = {};
			u8 dst_bytes[32] = {};
			char detail[512] = {};
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
			// TODO(excise): level 5; first sight of a program image.  The host
			// pointer, the ucode offset/length and the tail hash make the fill,
			// draw and ring lines joinable without guessing which image is
			// which.  Bounded to the first 256 claims.
			static unsigned s_claims = 0;

			if (s_claims < 256)
			{
				u64 claim_hash = 0xcbf29ce484222325ull;

				for (u32 i = 0; i < n; ++i)
				{
					claim_hash = (claim_hash ^ e->tail[i]) * 0x100000001b3ull;
				}

				if (std::FILE* out = std::fopen(std::getenv("RSX_FP_WATCH_LOG"), "a"))
				{
					std::fprintf(out, "claim #%u img=%llx off=%u len=%u tail=%u hash=%016llx\n",
						++s_claims, static_cast<unsigned long long>(addr), offset, len, n,
						static_cast<unsigned long long>(claim_hash));
					std::fclose(out);
				}
			}
			// EXCISE-END fp-literal-write-ring

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

				char buffer[512] = {};
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
			static u64 s_seq = 0;
			write_record& r = ring()[cursor];
			cursor = (cursor + 1) % write_ring_slots;

			r = write_record{};
			r.used = true;
			r.writer = writer;
			r.seq = ++s_seq;
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
				std::fprintf(out, "  ring seq=%llu %s dst=%llx size=%u src=%llx %s\n",
					static_cast<unsigned long long>(r.seq),
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

// EXCISE-BEGIN fp-stage-store-watch
// TODO(excise): level 5; the phase-3 producer watch.  Names the code that wrote
// a byte-reversed staging word, by hooking every layer that can store into the
// RSX command ring: guest PPU stores (LLVM and interpreter/HLE through
// vm::write), SPU DMA, HLE command builders, and the RSX blit engines.  The
// watched set is the io region armed at cellGcmInit plus every guest range an
// nv308a chunk has read from, so a store is reported when it lands where the
// blit engine will later find it, not merely when its value looks odd.
//
// A 4-byte word is logged when any of these holds:
//   - its address is inside an nv308a source range (unconditional; this is the
//     precise trap: the ring is re-written every frame, so the producer repeats);
//   - `!plausible(T(word)) && plausible(bswap32(T(word)))` (the F31 shape, used
//     for the io region outside a known staging range);
//   - it equals the configured needle word (RSX_FP_STAGE_NEEDLE, default the
//     observed a1273fbd), so the exact defect value is never filtered out.
// Each distinct (site, address, word) prints once; repeats are counted into a
// per-site cap.  The line carries the previous word seen at that address, the
// de-transposed decode, both endian conventions, the writer label, the guest
// PC (writer layers that have one) and the writer-specific detail string.
// Bounded (65536 lines, 4096 per site, 2048 ranges) and inert unless
// RSX_FP_STAGE_WATCH names a file.
namespace rsx
{
	class fp_stage_watch
	{
	public:
		static constexpr u32 max_ranges = 2048;
		static constexpr u32 max_lines = 65536;
		static constexpr u32 max_lines_per_site = 4096;
		static constexpr u32 dedupe_slots = 65536;
		static constexpr u32 shadow_slots = 16384;
		static constexpr u32 scan_limit = 0x8000;

		static bool enabled()
		{
			static const bool value = [] {
				const char* text = std::getenv("RSX_FP_STAGE_WATCH");
				return text && *text;
			}();

			return value;
		}

		// Per-layer kill switches, for bisecting a crash without a rebuild.
		// RSX_FP_STAGE_NO_JIT / _NO_SPU / _NO_VMW each disable one hook family.
		static bool jit_hook()
		{
			static const bool value = !std::getenv("RSX_FP_STAGE_NO_JIT");
			return value;
		}

		static bool spu_hook()
		{
			static const bool value = !std::getenv("RSX_FP_STAGE_NO_SPU");
			return value;
		}

		static bool vmw_hook()
		{
			static const bool value = !std::getenv("RSX_FP_STAGE_NO_VMW");
			return value;
		}

		static const char* path()
		{
			return std::getenv("RSX_FP_STAGE_WATCH");
		}

		static bool ring_armed()
		{
			return ring_len().load(std::memory_order_relaxed) != 0;
		}

		// The io region the guest builds its command stream in.  Announced as
		// early as cellGcmMapEaIoAddress, since a patch written before the first
		// nv308a is otherwise invisible to every store hook.  Repeated calls
		// extend the union instead of replacing it.
		static void arm_ring(u32 addr, u32 len, u32 cmd_size)
		{
			if (!enabled() || !addr || !len)
			{
				return;
			}

			const u32 old_base = ring_base().load(std::memory_order_relaxed);
			const u32 old_len = ring_len().load(std::memory_order_relaxed);

			if (old_len)
			{
				const u32 lo = std::min(old_base, addr);
				const u32 hi = std::max(old_base + old_len, addr + len);
				addr = lo;
				len = hi - lo;
			}

			if (ring_base().load(std::memory_order_relaxed) == addr &&
				ring_len().load(std::memory_order_relaxed) == len)
			{
				return;
			}

			ring_base().store(addr, std::memory_order_relaxed);
			ring_len().store(len, std::memory_order_relaxed);

			std::lock_guard lock(guard());
			emit("stage-arm ring=%08x len=%08x cmd=%08x\n", addr, len, cmd_size);
		}

		// Called by nv308a with the guest address a chunk consumed.  Adjacent and
		// overlapping ranges are merged, so the command stream's inline payload
		// collapses to a handful of intervals instead of thousands of quads.
		static void watch_range(u32 addr, u32 len)
		{
			if (!enabled() || !addr || !len)
			{
				return;
			}

			if (len > 0x100000)
			{
				len = 0x100000;
			}

			// The cellGcmInit arm can be missed (fifo_ctrl not up yet, ioAddress
			// zero).  A source EA is in the same io region the guest builds
			// commands in, so arm a 32 MiB window around the first one: the
			// staging writes that precede later copies then land in the ring.
			// arm_ring dedupes, so this emits one line for the run.
			if (ring_len().load(std::memory_order_relaxed) == 0)
			{
				arm_ring(addr & ~0x1ff'ffffu, 0x200'0000, 0);
			}

			std::lock_guard lock(guard());

			// Merge repeatedly: extending one range can make it touch another.
			bool merged = true;

			while (merged)
			{
				merged = false;
				u32 count = range_count().load(std::memory_order_relaxed);

				for (u32 i = 0; i < count; ++i)
				{
					range& r = ranges()[i];
					const u64 rlo = r.addr;
					const u64 rhi = static_cast<u64>(r.addr) + r.len;
					const u64 alo = addr;
					const u64 ahi = static_cast<u64>(addr) + len;

					// Overlap, or within 64 bytes (a command packet header
					// separates two payloads often enough to matter).
					if (alo <= rhi + 64 && rlo <= ahi + 64)
					{
						const u64 lo = std::min<u64>(alo, rlo);
						const u64 hi = std::max<u64>(ahi, rhi);
						r.addr = static_cast<u32>(lo);
						r.len = static_cast<u32>(hi - lo);

						// Remove the last entry into this slot, so the array stays
						// dense and lock-free membership scans stay short.
						if (i + 1 != count)
						{
							ranges()[i] = ranges()[count - 1];
						}

						range_count().store(count - 1, std::memory_order_release);
						merged = true;
						break;
					}
				}
			}

			u32 count = range_count().load(std::memory_order_relaxed);
			u32 slot = 0;

			if (count < max_ranges)
			{
				slot = count;

				// Fill the slot before publishing the count: a reader that sees
				// the new count must never observe an entry that is not yet
				// written.
				ranges()[slot].addr = addr;
				ranges()[slot].len = len;
				range_count().store(count + 1, std::memory_order_release);
			}
			else
			{
				slot = range_cursor()++ % max_ranges;
				ranges()[slot].addr = addr;
				ranges()[slot].len = len;
			}
		}

		// Cheap pre-check, so a caller on a hot path can decide whether asking
		// for the thread name and formatting a detail string is worth it.
		static bool interesting(u32 addr, u32 size)
		{
			if (!enabled() || !size || !addr || static_cast<u64>(addr) + size > 0x1'0000'0000ull)
			{
				return false;
			}

			const u64 alo = addr;
			const u64 ahi = alo + size;
			const u32 rb = ring_base().load(std::memory_order_relaxed);
			const u32 rl = ring_len().load(std::memory_order_relaxed);

			if (rl && alo < static_cast<u64>(rb) + rl && static_cast<u64>(rb) < ahi)
			{
				return true;
			}

			return range_hit(addr, size);
		}

		// A store has completed at `addr` (size bytes).  Reads the bytes back from
		// guest memory, so every layer calls the same entry point regardless of
		// whether it knows the value it wrote.
		static void note_mem(u32 addr, u32 size, const char* writer, u32 pc, const char* thread, const char* detail)
		{
			if (!enabled() || !size || !addr)
			{
				return;
			}

			// A store into a staging range whose page is not committed would
			// fault on the read-back below, so the mapping is checked first.
			if (!vm::check_addr(addr, vm::page_readable, size))
			{
				return;
			}

			const u64 alo = addr;
			const u64 ahi = alo + size;
			const u32 rb = ring_base().load(std::memory_order_relaxed);
			const u32 rl = ring_len().load(std::memory_order_relaxed);
			const bool in_ring = rl && alo < static_cast<u64>(rb) + rl && static_cast<u64>(rb) < ahi;
			const bool in_stage = range_hit(addr, size);

			// The F31 shape fires on ~13% of random words, so a shape-only hit
			// outside a watched range would flood the log with noise; an exact
			// needle hit is rare enough to log from anywhere.  Scan every store
			// for the needle, and keep the full convention census inside the
			// watched ranges.
			const u32 n = std::min<u32>(size, scan_limit);
			const u8* mem = static_cast<const u8*>(vm::base(addr));

			for (u32 off = 0; off + 4 <= n; ++off)
			{
				if (((addr + off) & 3) != 0)
				{
					continue;
				}

				u32 word = 0;
				std::memcpy(&word, mem + off, 4);

				const u32 slot = de_transpose(word);
				const u32 alt = std::byteswap(slot);
				const bool suspect = !plausible(slot) && plausible(alt);
				const bool needle = word == needle_word() || std::byteswap(word) == needle_word();
				const bool band = world_quad_shape(addr + off) && !authored_word(slot) && authored_word(alt);
				const bool stage = in_stage;

				// A shape hit is as rare as an exact needle hit and names the
				// writer for any of the game's animated words, not just the one
				// the needle happens to hold this run.  Passed through the
				// needle field of the log so a grep for needle=1 still finds it.
				const bool hit = needle || band;

				if (!in_ring && !in_stage && !hit)
				{
					continue;
				}

				// TODO(excise): level 5; the producer-side experiment.  If the
				// hypothesis holds - the animated lane is a plain big-endian store
				// of the word the fill will `T` - then byte-reversing the word the
				// store left in guest memory makes the image word the fill expects.
				// The correction is applied after the store, so the log below still
				// records what the producer wrote (`fix=0` values) and the corrected
				// word is printed alongside.  Guarded by the same `bswap32-any`
				// shape and an optional staging-range restriction, so a normal run
				// is not rewritten.  RSX_FP_STAGE_FIX=stage|io|all.
				const bool steer = (suspect || band) && quad_sibling_plausible(addr + off)
					&& ((stage && fix_level() >= 1) || (in_ring && fix_level() >= 2));

				if (steer)
				{
					const u32 corrected = std::byteswap(word);
					std::memcpy(const_cast<u8*>(mem) + off, &corrected, 4);
				}

				// With the ring armed across all mapped regions the loose F31
				// predicate would fill the line cap with noise.  Stage-range
				// stores keep the full census; outside them only a precise
				// needle/shape hit is logged.
				if (stage || hit || probe_allow(writer))
				{
					log_word(addr + off, word, slot, alt, suspect, hit, stage, in_ring, steer, writer, pc, thread, detail);
				}
			}

			// A 1- or 2-byte store cannot complete an image word by itself, but
			// inside a known staging range its bytes are part of one.  Log it so a
			// split producer is visible; the address and byte value compose later.
			if (in_stage && n < 4)
			{
				log_partial(addr, size, mem, n, writer, pc, thread, detail);
			}
		}

		// The submit boundary.  The PPU thread and its PC at the store that raised
		// the ring's put name the frame that carried a staging write.
		static void log_submit(u64 get_put, u32 pc, const char* thread)
		{
			if (!enabled())
			{
				return;
			}

			std::lock_guard lock(guard());
			emit("stage-submit get_put=%016llx pc=%08x thread=%s\n",
				static_cast<unsigned long long>(get_put), pc, thread ? thread : "-");
		}

	private:
		struct range
		{
			u32 addr = 0;
			u32 len = 0;
		};

		static std::mutex& guard()
		{
			static std::mutex value;
			return value;
		}

		static std::atomic<u32>& ring_base()
		{
			static std::atomic<u32> value{0};
			return value;
		}

		static std::atomic<u32>& ring_len()
		{
			static std::atomic<u32> value{0};
			return value;
		}

		static std::atomic<u32>& range_count()
		{
			static std::atomic<u32> value{0};
			return value;
		}

		static range* ranges()
		{
			static range value[max_ranges];
			return value;
		}

		static u32& range_cursor()
		{
			static u32 value = 0;
			return value;
		}

		// Lock-free membership test.  Writers publish the count with release and
		// never shrink below a slot a reader may be looking at, so a torn read is
		// a missed range, not a crash; the ring check catches the common case.
		static bool range_hit(u32 addr, u32 size)
		{
			const u32 count = range_count().load(std::memory_order_acquire);
			const u64 alo = addr;
			const u64 ahi = alo + size;

			for (u32 i = 0; i < count && i < max_ranges; ++i)
			{
				const range& r = ranges()[i];
				const u64 rlo = r.addr;
				const u64 rhi = rlo + r.len;

				if (alo < rhi && rlo < ahi)
				{
					return true;
				}
			}

			return false;
		}

		// `T` from the fragment-constant fill: swap the bytes inside each 16-bit
		// half.  The fill applies it to every image word, so an image word `w`
		// decodes to `T(w)`.
		static u32 de_transpose(u32 word)
		{
			return ((word & 0x00ff00ffu) << 8) | ((word & 0xff00ff00u) >> 8);
		}

		static bool plausible(u32 bits)
		{
			const f32 value = std::bit_cast<f32>(bits);
			return std::isfinite(value) && (value == 0.0f || (std::abs(value) >= 1.0e-6f && std::abs(value) <= 1.0e6f));
		}

		// The animated word is authored in a narrow band.  The generic
		// plausible() test above is ~13% likely on random words and misses the
		// transposed values that land inside [1e-6,1e6]; this shape is exact
		// enough to name the producer without guessing a literal (the literal
		// changes every frame).
		static bool authored_word(u32 bits)
		{
			const f32 value = std::bit_cast<f32>(bits);
			return std::isfinite(value) && value >= 0.315f && value <= 2.222f;
		}

		static bool world_quad_shape(u32 addr)
		{
			const u32 base = addr & ~15u;

			if (!vm::check_addr(base, vm::page_readable, 16))
			{
				return false;
			}

			const u8* mem = static_cast<const u8*>(vm::base(base));
			u32 words[4] = {};
			std::memcpy(words, mem, sizeof(words));

			const f32 v1 = std::bit_cast<f32>(de_transpose(words[1]));
			const f32 v3 = std::bit_cast<f32>(de_transpose(words[3]));

			// The patcher writes the two animated lanes one at a time, so at
			// the moment of a store the other animated lane may still hold the
			// authored value.  Only the static 2.0/1.0 siblings are required.
			return std::abs(v1 - 2.0f) < 1.0e-3f
				&& std::abs(v3 - 1.0f) < 1.0e-3f;
		}

		static u32 needle_word()
		{
			static const u32 value = [] {
				const char* text = std::getenv("RSX_FP_STAGE_NEEDLE");
				char* end = nullptr;
				const unsigned long parsed = text && *text ? std::strtoul(text, &end, 16) : 0;
				return (end && *end == '\0' && end != text) ? static_cast<u32>(parsed) : 0xa1273fbdu;
			}();

			return value;
		}

		// RSX_FP_STAGE_FIX=stage|io|all.  Level 0 is off and is the default.
		// Level 1 corrects a suspect word only inside a known nv308a source
		// range; level 2 also corrects the ring outside those ranges.  `all` is
		// accepted as an explicit spelling of level 2.
		static u32 fix_level()
		{
			static const u32 value = [] {
				const char* text = std::getenv("RSX_FP_STAGE_FIX");

				if (!text || !*text || !std::strcmp(text, "0"))
				{
					return 0u;
				}

				if (!std::strcmp(text, "stage"))
				{
					return 1u;
				}

				if (!std::strcmp(text, "io") || !std::strcmp(text, "all"))
				{
					return 2u;
				}

				return 0u;
			}();

			return value;
		}

		// The first few stores of each writer are logged regardless of shape, so
		// a run can prove a layer's hook is live.  Without it a narrow shape
		// filter is indistinguishable from a hook that is never called.  Taken
		// before the guard on purpose: the count is approximate, and a collision
		// only costs a repeated line.
		static bool probe_allow(const char* writer)
		{
			struct entry
			{
				std::atomic<const char*> writer{nullptr};
				std::atomic<u32> lines{0};
			};

			static entry table[64] = {};

			for (entry& e : table)
			{
				const char* current = e.writer.load(std::memory_order_relaxed);

				if (!current || current == writer)
				{
					e.writer.store(writer, std::memory_order_relaxed);

					if (e.lines.load(std::memory_order_relaxed) >= 8)
					{
						return false;
					}

					e.lines.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
			}

			return false;
		}

		// The defect sits in a 16-byte quad whose other words decode correctly
		// (2.0, 1.0).  Requiring two plausible siblings keeps the corrector off
		// ring data that merely happens to contain a byte-reversed float.  The
		// base is 16-byte aligned, so the read never crosses a host page.
		static bool quad_sibling_plausible(u32 addr)
		{
			if (!addr)
			{
				return false;
			}

			const u32 base = addr & ~15u;

			if (!vm::check_addr(base, vm::page_readable, 16))
			{
				return false;
			}

			const u8* mem = static_cast<const u8*>(vm::base(base));
			u32 found = 0;

			for (u32 off = 0; off < 16; off += 4)
			{
				if (base + off == addr)
				{
					continue;
				}

				u32 word = 0;
				std::memcpy(&word, mem + off, 4);

				if (plausible(de_transpose(word)) || plausible(de_transpose(std::byteswap(word))))
				{
					++found;
				}
			}

			return found >= 2;
		}

		// A fixed execed direct-mapped dedupe table.  The key is unique per
		// (caller site, address, word); a collision only costs a repeated line,
		// never a missed one.
		static u64* dedupe()
		{
			static u64 value[dedupe_slots] = {};
			return value;
		}

		// Direct-mapped index.  The tables are powers of two and only `dedupe()`
		// is 65536 entries, so the high bits of the mixed key cannot be used
		// directly: a 64-bit key shifted by 48 reaches 65535 and would run past
		// `shadows()`.  Mix first, then mask with the table size.
		static u32 hash_slot(u64 key, u32 count)
		{
			return static_cast<u32>((key * 0x9e3779b97f4a7c15ull) >> 32) & (count - 1);
		}

		static bool dedupe_insert(u64 key)
		{
			key |= 1ull;
			u64& slot = dedupe()[hash_slot(key, dedupe_slots)];

			if (slot == key)
			{
				return false;
			}

			slot = key;
			return true;
		}

		struct site_entry
		{
			u32 key = 0;
			u32 lines = 0;
		};

		static site_entry* sites()
		{
			static site_entry value[256] = {};
			return value;
		}

		static bool site_allow(u32 key)
		{
			for (u32 i = 0; i < 256; ++i)
			{
				site_entry& entry = sites()[i];

				if (entry.key == key || !entry.lines)
				{
					entry.key = key;

					if (entry.lines >= max_lines_per_site)
					{
						return false;
					}

					++entry.lines;
					return true;
				}
			}

			return false;
		}

		struct shadow_entry
		{
			u64 key = 0;
			u32 word = 0;
			bool valid = false;
		};

		static shadow_entry* shadows()
		{
			static shadow_entry value[shadow_slots] = {};
			return value;
		}

		// The previous word this site wrote at this address.  For an alternating
		// lane this is the other state, which is what makes a flip readable.
		static bool shadow_peek(u64 key, u32& word)
		{
			const shadow_entry& entry = shadows()[hash_slot(key, shadow_slots)];

			if (entry.valid && entry.key == key)
			{
				word = entry.word;
				return true;
			}

			return false;
		}

		static void shadow_store(u64 key, u32 word)
		{
			shadow_entry& entry = shadows()[hash_slot(key, shadow_slots)];
			entry.key = key;
			entry.word = word;
			entry.valid = true;
		}

		static void log_word(u32 waddr, u32 word, u32 slot, u32 alt, bool suspect, bool needle_hit,
			bool stage, bool ring, bool steer, const char* writer, u32 pc, const char* thread, const char* detail)
		{
			const u64 site_key = (static_cast<u64>(pc) << 32) ^ (static_cast<u64>(reinterpret_cast<uptr>(writer)) >> 4);
			const u64 dedupe_key = site_key ^ (static_cast<u64>(waddr) << 20) ^ (static_cast<u64>(word) << 1) ^ (stage ? 2ull : 0ull);
			const u64 shadow_key = site_key ^ (static_cast<u64>(waddr) << 20);

			// The cap is checked before the mutex, so a saturated run does not
			// serialise every store behind a lock that will discard it anyway.
			if (line_count().load(std::memory_order_relaxed) >= max_lines)
			{
				if (!limit_reported().exchange(true))
				{
					std::lock_guard lock(guard());
					emit("stage-limit lines=%u\n", max_lines);
				}

				return;
			}

			std::lock_guard lock(guard());

			if (!site_allow(static_cast<u32>(site_key ^ (site_key >> 32))))
			{
				return;
			}

			if (!dedupe_insert(dedupe_key))
			{
				return;
			}

			if (line_count() >= max_lines)
			{
				return;
			}

			++line_count();

			u32 old = 0;
			const bool has_old = shadow_peek(shadow_key, old);
			shadow_store(shadow_key, word);

			char old_text[16] = "--------";

			if (has_old)
			{
				std::snprintf(old_text, sizeof(old_text), "%08x", old);
			}

			emit("stage-store writer=%s addr=%08x new=%08x old=%s slot=%08x alt=%08x suspect=%d needle=%d stage=%d ring=%d fix=%d pc=%08x thread=%s detail=%s\n",
				writer, waddr, word, old_text, slot, alt,
				suspect ? 1 : 0, needle_hit ? 1 : 0, stage ? 1 : 0, ring ? 1 : 0,
				steer ? 1 : 0, pc, thread ? thread : "-", detail ? detail : "-");
		}

		static void log_partial(u32 addr, u32 size, const u8* bytes, u32 n, const char* writer, u32 pc, const char* thread, const char* detail)
		{
			u32 packed = 0;
			std::memcpy(&packed, bytes, std::min<u32>(n, 4));

			const u64 site_key = (static_cast<u64>(pc) << 32) ^ (static_cast<u64>(reinterpret_cast<uptr>(writer)) >> 4);
			const u64 dedupe_key = site_key ^ (static_cast<u64>(addr) << 20) ^ (static_cast<u64>(packed) << 3) ^ 0x5ull;

			std::lock_guard lock(guard());

			if (!site_allow(static_cast<u32>(site_key ^ (site_key >> 32))) || !dedupe_insert(dedupe_key))
			{
				return;
			}

			if (line_count() >= max_lines)
			{
				return;
			}

			++line_count();

			emit("stage-partial writer=%s addr=%08x size=%u bytes=%08x pc=%08x thread=%s detail=%s\n",
				writer, addr, size, packed, pc, thread ? thread : "-", detail ? detail : "-");
		}

		static std::atomic<u32>& line_count()
		{
			static std::atomic<u32> value{0};
			return value;
		}

		static std::atomic<bool>& limit_reported()
		{
			static std::atomic<bool> value{false};
			return value;
		}

		// The caller holds the guard.
		static void emit(const char* format, ...)
		{
			static std::FILE* file = nullptr;

			if (!file)
			{
				const char* name = path();

				if (!name || !*name)
				{
					return;
				}

				file = std::fopen(name, "w");
			}

			if (!file)
			{
				return;
			}

			va_list args;
			va_start(args, format);
			std::vfprintf(file, format, args);
			va_end(args);
			std::fflush(file);
		}
	};
}
// EXCISE-END fp-stage-store-watch
