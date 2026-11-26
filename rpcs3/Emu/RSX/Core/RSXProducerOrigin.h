#pragma once

// EXCISE-BEGIN fp-producer-origin
// TODO(excise): level 5; the static-vs-runtime origin report.  For every defect
// quad the ledger registered, search guest RAM and the decrypted boot
// executable for the exact 16-byte sequence, and report the RSX copies/renders
// whose destination covers the quad.
//
//  * a hit in the decrypted executable       -> the bytes are authored (static)
//  * no executable hit, a hit elsewhere in RAM -> they arrived as data (copied in)
//  * neither, and no store/copy recorded      -> they were built in place by an
//                                                uninstrumented writer
//
// Runs on a detached thread, ~4.5 s after F12 so it does not steal the harvest
// window, and is bounded.  Output: RSX_FP_LEDGER_ORIGIN or <ledger>.origin.

#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Crypto/unself.h"
#include "Utilities/File.h"
#include "Emu/RSX/Core/RSXProducerLedger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rsx::producer_origin
{
	inline const char* path()
	{
		if (const char* p = std::getenv("RSX_FP_LEDGER_ORIGIN"))
		{
			return p;
		}

		return "/tmp/rpcs3-fp-ledger.origin";
	}

	// First occurrence of `needle` in `hay`; returns the count found and fills
	// `hits` up to `max_hits`.
	inline u32 find_all(const u8* hay, usz n, const u8* needle, usz m, u64 base, u64* hits, u32 max_hits)
	{
		u32 found = 0;
		const u8* p = hay;

		while (p + m <= hay + n)
		{
			const u8* hit = static_cast<const u8*>(std::search(p, hay + n, needle, needle + m));

			if (hit == hay + n)
			{
				break;
			}

			if (found < max_hits)
			{
				hits[found] = base + static_cast<u64>(hit - hay);
			}

			++found;
			p = hit + 1;
		}

		return found;
	}

	struct scan_result
	{
		u64 bytes = 0;
		u32 hits = 0;
		bool complete = false;
		u64 addrs[16] = {};
	};

	inline scan_result scan_guest_range(const u8* needle, u32 self_lo, u32 self_hi, u32 begin, u32 end, u64 budget_ms)
	{
		scan_result r;
		const auto start = std::chrono::steady_clock::now();
		std::vector<u8> buffer(0x1000 + 16);

		for (u32 page = begin; page < end; page += 0x1000)
		{
			// The scan runs on a detached thread, so the emulator can stop and
			// unmap guest memory while it is in flight.  Check the stop flag per
			// page: at the 1 MiB budget tick it is too coarse and the host mapping
			// can already be gone even though check_addr still reports the pages
			// as allocated (the teardown clears the host mapping before the page
			// table).  Returning incomplete is the right answer in that case.
			if (Emu.IsStopped())
			{
				return r;
			}

			if ((page & 0xfffff) == 0)
			{
				const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - start).count();

				if (static_cast<u64>(elapsed) > budget_ms)
				{
					return r;
				}
			}

			if (!vm::check_addr(page, vm::page_readable, 0x1000))
			{
				continue;
			}

			const u32 next = (page + 0x1000 < end) ? page + 0x1000 : 0;
			const bool have_next = next && vm::check_addr(next, vm::page_readable, 16);

			std::memcpy(buffer.data(), vm::base(page), 0x1000);

			if (have_next)
			{
				std::memcpy(buffer.data() + 0x1000, vm::base(next), 16);
			}

			const usz n = have_next ? 0x1000 + 16 : 0x1000;
			u64 hits[16] = {};
			const u32 found = find_all(buffer.data(), n, needle, 16, page, hits, 16);
			r.bytes += n;

			for (u32 i = 0; i < std::min<u32>(found, 16); ++i)
			{
				// The defect quad itself is not evidence of an author.
				if (hits[i] >= self_lo && hits[i] < self_hi)
				{
					continue;
				}

				if (r.hits < 16)
				{
					r.addrs[r.hits++] = hits[i];
				}
			}
		}

		r.complete = true;
		return r;
	}

	inline scan_result scan_guest_ram(const u8* needle, u32 self_lo, u32 self_hi, u64 budget_ms)
	{
		// The defect quads are guest *images*, and the program image lives in RSX
		// local memory (F56).  Scanning only main RAM can never find one, so local
		// memory is searched first and main RAM with the remaining budget.
		constexpr u32 local_base = 0xC000'0000u;
		constexpr u32 local_end = local_base + 0x1000'0000u;
		scan_result local = scan_guest_range(needle, self_lo, self_hi, local_base, local_end, budget_ms / 2);

		if (local.hits)
		{
			return local;
		}

		scan_result main = scan_guest_range(needle, self_lo, self_hi, 0x1'0000u, 0x4000'0000u, budget_ms / 2);
		main.bytes += local.bytes;
		main.complete = main.complete && local.complete;
		return main;
	}

	inline scan_result scan_file(const u8* needle, u64 budget_ms)
	{
		scan_result r;

		if (Emu.GetBoot().empty())
		{
			return r;
		}

		fs::file file = fs::file(Emu.GetBoot());

		if (!file)
		{
			return r;
		}

		fs::file elf = decrypt_self(file, Emu.klic.empty() ? nullptr : reinterpret_cast<u8*>(Emu.klic.data()));

		if (!elf)
		{
			return r;
		}

		const u64 total = elf.size();
		const auto start = std::chrono::steady_clock::now();
		std::vector<u8> buffer(8u << 20);

		for (u64 pos = 0; pos < total;)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - start).count();

			if (static_cast<u64>(elapsed) > budget_ms)
			{
				return r;
			}

			const usz want = static_cast<usz>(std::min<u64>(buffer.size() - 16, total - pos));
			const u64 got = elf.read_at(pos, buffer.data(), want);

			if (!got)
			{
				break;
			}

			u64 hits[16] = {};
			const u32 found = find_all(buffer.data(), static_cast<usz>(got), needle, 16, pos, hits, 16);
			r.bytes += got;

			for (u32 i = 0; i < std::min<u32>(found, 16) && r.hits < 16; ++i)
			{
				r.addrs[r.hits++] = hits[i];
			}

			if (got < 16)
			{
				break;
			}

			pos += got - 15;
		}

		r.complete = true;
		return r;
	}

	inline void run()
	{
		producer_ledger::defect list[producer_ledger::max_defects];
		const u32 count = producer_ledger::defect_snapshot(list, producer_ledger::max_defects);

		if (!count)
		{
			return;
		}

		if (Emu.IsStopped())
		{
			return;
		}

		std::FILE* out = std::fopen(path(), "a");

		if (!out)
		{
			return;
		}

		const u32 max_quads = std::min<u32>(count, 8);
		const u64 budget = 2000;

		for (u32 i = 0; i < max_quads; ++i)
		{
			const producer_ledger::defect& d = list[i];
			const u8* needle = d.bytes;

			std::fprintf(out, "\norigin quad=%08x lane=%08x word=%08x source=%s gpu=%u surface=%u hits=%u bytes=",
				d.quad, d.addr, d.word, d.source ? d.source : "-",
				d.gpu_overlap ? 1u : 0u, d.surface_overlap ? 1u : 0u, d.hits);

			for (u32 b = 0; b < 16; ++b)
			{
				std::fprintf(out, "%02x", d.bytes[b]);
			}

			std::fprintf(out, "\n");
			std::fflush(out);

			const scan_result ram = scan_guest_ram(needle, d.quad, d.quad + 16, budget);
			const scan_result elf = scan_file(needle, budget);

			const char* verdict = "constructed-in-place";

			if (elf.hits)
			{
				verdict = "static-executable";
			}
			else if (ram.hits)
			{
				verdict = "data-in-ram";
			}
			else if (d.gpu_overlap || d.surface_overlap)
			{
				verdict = "rsx-write";
			}
			else if (!ram.complete || !elf.complete)
			{
				verdict = "inconclusive";
			}

			std::fprintf(out, "  verdict=%s ram_hits=%u ram_bytes=%llu ram_complete=%u elf_hits=%u elf_bytes=%llu elf_complete=%u\n",
				verdict, ram.hits, static_cast<unsigned long long>(ram.bytes), ram.complete ? 1u : 0u,
				elf.hits, static_cast<unsigned long long>(elf.bytes), elf.complete ? 1u : 0u);

			for (u32 h = 0; h < ram.hits; ++h)
			{
				std::fprintf(out, "  ram_hit addr=%08llx\n", static_cast<unsigned long long>(ram.addrs[h]));
			}

			for (u32 h = 0; h < elf.hits; ++h)
			{
				std::fprintf(out, "  elf_hit off=%llx\n", static_cast<unsigned long long>(elf.addrs[h]));
			}

			for (u32 c = 0; c < d.copy_count; ++c)
			{
				const auto& n = d.copies[c];
				std::fprintf(out, "  copy writer=%s dst=%08x size=%u src=%08x gpu=%u surface=%u\n",
					n.writer ? n.writer : "-", n.dst, n.size, n.src, n.gpu ? 1u : 0u, n.surface ? 1u : 0u);
			}

			std::fflush(out);
		}

		std::fclose(out);
	}

	// Called from the F12 handler.  Waits for the harvest window to finish, then
	// scans.  Detached: the emulator keeps running and the report is appended to
	// the origin file.
	inline void scan_async()
	{
		if (!producer_ledger::enabled())
		{
			return;
		}

		std::thread([] {
			std::this_thread::sleep_for(std::chrono::milliseconds(4500));

			if (!Emu.IsStopped())
			{
				run();
			}
		}).detach();
	}
}
// EXCISE-END fp-producer-origin
