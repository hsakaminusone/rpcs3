#include "ppu_cache_meta.h"

#include <cstring>

namespace ppu_cache_meta
{
	namespace
	{
		constexpr u8 magic[4] = {'R', 'P', 'M', '1'};

		// magic + codegen version
		constexpr usz header_size = sizeof(magic) + sizeof(u32);
		// region_count, site_table_offset, guest_addr, guest_size + histogram
		constexpr usz body_size = sizeof(u32) * 4 + sizeof(u16) * histogram_buckets;
		constexpr usz record_size = header_size + body_size;

		void put_u32(std::vector<u8>& out, u32 value)
		{
			for (u32 i = 0; i < 4; i++)
			{
				out.push_back(static_cast<u8>(value >> (i * 8)));
			}
		}

		void put_u16(std::vector<u8>& out, u16 value)
		{
			out.push_back(static_cast<u8>(value));
			out.push_back(static_cast<u8>(value >> 8));
		}

		u32 get_u32(std::span<const u8> bytes, usz offset)
		{
			u32 value = 0;

			for (u32 i = 0; i < 4; i++)
			{
				value |= static_cast<u32>(bytes[offset + i]) << (i * 8);
			}

			return value;
		}

		u16 get_u16(std::span<const u8> bytes, usz offset)
		{
			return static_cast<u16>(bytes[offset] | (static_cast<u16>(bytes[offset + 1]) << 8));
		}
	}

	std::vector<u8> serialize(const record& rec)
	{
		std::vector<u8> out;
		out.reserve(record_size);

		out.insert(out.end(), std::begin(magic), std::end(magic));

		// The codegen version doubles as the record version: an object and its
		// sidecar are written by the same build or they are not used together.
		put_u32(out, rec.codegen_version);
		put_u32(out, rec.region_count);
		put_u32(out, rec.site_table_offset);
		put_u32(out, rec.guest_addr);
		put_u32(out, rec.guest_size);

		for (u16 bucket : rec.block_length_histogram)
		{
			put_u16(out, bucket);
		}

		return out;
	}

	std::optional<record> parse(std::span<const u8> bytes)
	{
		if (bytes.size() != record_size)
		{
			return std::nullopt;
		}

		if (std::memcmp(bytes.data(), magic, sizeof(magic)) != 0)
		{
			return std::nullopt;
		}

		if (get_u32(bytes, sizeof(magic)) != version)
		{
			return std::nullopt;
		}

		record rec;
		rec.codegen_version = version;

		usz pos = header_size;
		rec.region_count = get_u32(bytes, pos);
		pos += sizeof(u32);
		rec.site_table_offset = get_u32(bytes, pos);
		pos += sizeof(u32);
		rec.guest_addr = get_u32(bytes, pos);
		pos += sizeof(u32);
		rec.guest_size = get_u32(bytes, pos);
		pos += sizeof(u32);

		for (u16& bucket : rec.block_length_histogram)
		{
			bucket = get_u16(bytes, pos);
			pos += sizeof(u16);
		}

		return rec;
	}

	std::vector<u32> pages(const record& rec)
	{
		std::vector<u32> result;

		if (!rec.guest_size)
		{
			return result;
		}

		const u32 first = rec.guest_addr >> 12;
		const u64 last64 = (static_cast<u64>(rec.guest_addr) + rec.guest_size - 1) >> 12;

		if (last64 - first + 1 > max_pages)
		{
			return result;
		}

		for (u32 page = first; page <= static_cast<u32>(last64); page++)
		{
			result.push_back(page);
		}

		return result;
	}
}
