#include <gtest/gtest.h>

#include "Utilities/ppu_cache_meta.h"

// CPU-CACHE-0xx: the sidecar next to a cached PPU object (§8.5.1). A different
// process reads it than the one that wrote it, so the parser has to reject
// anything it did not write rather than reinterpret it.
namespace
{
	ppu_cache_meta::record make_record()
	{
		ppu_cache_meta::record rec;
		rec.codegen_version = ppu_cache_meta::version;
		rec.region_count = 42;
		rec.site_table_offset = 0x1234;
		rec.guest_addr = 0x100000;
		rec.guest_size = 0x4000;

		for (u32 i = 0; i < ppu_cache_meta::histogram_buckets; i++)
		{
			rec.block_length_histogram[i] = static_cast<u16>(i * 3);
		}

		return rec;
	}
}

TEST(PpuCacheMeta, RoundTripsEveryField)
{
	const auto rec = make_record();
	const auto bytes = ppu_cache_meta::serialize(rec);

	const auto parsed = ppu_cache_meta::parse(bytes);
	ASSERT_TRUE(parsed.has_value());

	EXPECT_EQ(parsed->codegen_version, rec.codegen_version);
	EXPECT_EQ(parsed->region_count, rec.region_count);
	EXPECT_EQ(parsed->site_table_offset, rec.site_table_offset);
	EXPECT_EQ(parsed->guest_addr, rec.guest_addr);
	EXPECT_EQ(parsed->guest_size, rec.guest_size);

	for (u32 i = 0; i < ppu_cache_meta::histogram_buckets; i++)
	{
		EXPECT_EQ(parsed->block_length_histogram[i], rec.block_length_histogram[i]) << "bucket " << i;
	}
}

TEST(PpuCacheMeta, RejectsAForeignFile)
{
	auto bytes = ppu_cache_meta::serialize(make_record());
	bytes[0] = 'X';

	EXPECT_FALSE(ppu_cache_meta::parse(bytes).has_value());
}

TEST(PpuCacheMeta, RejectsAnotherCodegenVersion)
{
	auto bytes = ppu_cache_meta::serialize(make_record());
	bytes[4] = static_cast<u8>(ppu_cache_meta::version + 1);

	EXPECT_FALSE(ppu_cache_meta::parse(bytes).has_value());
}

TEST(PpuCacheMeta, RejectsTruncatedOrPaddedInput)
{
	auto bytes = ppu_cache_meta::serialize(make_record());

	auto shorter = bytes;
	shorter.pop_back();
	EXPECT_FALSE(ppu_cache_meta::parse(shorter).has_value());

	auto longer = bytes;
	longer.push_back(0);
	EXPECT_FALSE(ppu_cache_meta::parse(longer).has_value());
}

TEST(PpuCacheMeta, PagesCoverTheWholeGuestRange)
{
	auto rec = make_record();
	rec.guest_addr = 0x1fff0;
	rec.guest_size = 0x20;

	// Straddles two pages without being aligned to either of them.
	const auto pages = ppu_cache_meta::pages(rec);
	ASSERT_EQ(pages.size(), 2u);
	EXPECT_EQ(pages[0], 0x1f);
	EXPECT_EQ(pages[1], 0x20);
}

TEST(PpuCacheMeta, PagesOfAnEmptyRangeIsEmpty)
{
	auto rec = make_record();
	rec.guest_size = 0;

	EXPECT_TRUE(ppu_cache_meta::pages(rec).empty());
}

TEST(PpuCacheMeta, PagesAreRejectedWhenTheRangeIsUnreasonablyLarge)
{
	auto rec = make_record();
	rec.guest_addr = 0;
	rec.guest_size = 0xffffffff;

	EXPECT_TRUE(ppu_cache_meta::pages(rec).empty());
}
