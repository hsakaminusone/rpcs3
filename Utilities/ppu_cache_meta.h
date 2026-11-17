#pragma once

#include <optional>
#include <span>
#include <vector>

#include "util/types.hpp"

// Sidecar metadata for a cached PPU module object (§8.5.1).
//
// The .obj is machine code. This record travels next to it so the next process
// can size its tables and re-seed the page index (§8.1.4) without decoding the
// object. It holds no host addresses: those are not stable across runs.
namespace ppu_cache_meta
{
	// Bumped with the cache key prefix in PPUThread.cpp. A record written under
	// another version is rejected, never reinterpreted.
	constexpr u32 version = 10;

	// A sidecar covering more pages than this is treated as corrupt rather than
	// used to size a table.
	constexpr u32 max_pages = 4096;

	constexpr u32 histogram_buckets = 8;

	struct record
	{
		u32 codegen_version = version;
		u32 region_count = 0;
		u32 site_table_offset = 0;
		u32 guest_addr = 0;
		u32 guest_size = 0;
		u16 block_length_histogram[histogram_buckets]{};
	};

	// Serialise a record: magic, codegen version, then the remaining fields and
	// the histogram in little-endian order.
	std::vector<u8> serialize(const record& rec);

	// Parse a record. Returns nothing when the bytes were not written by this
	// version, or are not the exact size a record has.
	std::optional<record> parse(std::span<const u8> bytes);

	// Page numbers (addr >> 12) covered by the record, ascending. Empty when the
	// range is empty or too large to be a module.
	std::vector<u32> pages(const record& rec);
}
