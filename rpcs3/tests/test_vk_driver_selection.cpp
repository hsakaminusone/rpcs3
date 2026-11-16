/*
 * test_vk_driver_selection.cpp — aggressive coverage for the MoltenVK / KosmicKrisp
 * coexistence seams (docs/research/04-backend-selection-design.md and
 * docs/research/11-mvk-kk-coexistence.md, problem catalogue C1..C10).
 *
 * These tests exercise pure, GPU-free logic only. Anything requiring a real Vulkan
 * loader/ICD/GPU cannot be covered here (see the notes at the bottom of the file).
 */

#include <gtest/gtest.h>

#include "Emu/RSX/VK/vkutils/vk_driver_selection.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
	using namespace vk::driver_selection;

	device_info dev(driver_kind kind, std::string name, std::uint32_t driver_id = 0, int index = 0)
	{
		device_info d;
		d.kind = kind;
		d.name = std::move(name);
		d.driver_id = driver_id;
		d.index = index;
		return d;
	}

	std::uint32_t make_version(std::uint32_t major, std::uint32_t minor, std::uint32_t patch)
	{
		return (major << 22) | (minor << 12) | patch;
	}

	const host_info host_macos26_arm{ 26, host_arch::arm64 };
	const host_info host_macos27_arm{ 27, host_arch::arm64 };
	const host_info host_macos25_arm{ 25, host_arch::arm64 };
	const host_info host_macos15_arm{ 15, host_arch::arm64 };
	const host_info host_macos26_x86{ 26, host_arch::x86_64 };
	const host_info host_macos15_x86{ 15, host_arch::x86_64 };
	const host_info host_unknown{ 0, host_arch::unknown };

	// -------------------------------------------------------------------------
	// C2 — driver id classification (replaces hard-coded MVK-on-Apple)
	// -------------------------------------------------------------------------

	TEST(KKDriverId, KnownDriverIds)
	{
		EXPECT_EQ(classify_driver_id(driver_id_moltenvk), driver_kind::moltenvk);
		EXPECT_EQ(classify_driver_id(driver_id_mesa_kosmickrisp), driver_kind::kosmickrisp);
		EXPECT_EQ(classify_driver_id(driver_id_mesa_honeykrisp), driver_kind::honeykrisp);
	}

	TEST(KKDriverId, ZeroIsUnknownNotMVK)
	{
		// C2 regression guard: device.cpp claimed "MoltenVK currently returns DRIVER_ID_MOLTENVK (0)".
		// That comment is wrong; 0 is unset and 14 is MoltenVK. Never map 0 -> MVK.
		EXPECT_EQ(classify_driver_id(0), driver_kind::unknown);
		EXPECT_NE(classify_driver_id(0), driver_kind::moltenvk);
	}

	TEST(KKDriverId, OtherKnownNonAppleDriversAreOther)
	{
		EXPECT_EQ(classify_driver_id(driver_id_amd_proprietary), driver_kind::other);
		EXPECT_EQ(classify_driver_id(driver_id_mesa_radv), driver_kind::other);
		EXPECT_EQ(classify_driver_id(driver_id_nvidia_proprietary), driver_kind::other);
		EXPECT_EQ(classify_driver_id(driver_id_mesa_llvmpipe), driver_kind::other);
		EXPECT_EQ(classify_driver_id(driver_id_mesa_dozen), driver_kind::other);
		EXPECT_EQ(classify_driver_id(driver_id_vulkan_sc_emulation), driver_kind::other);
	}

	TEST(KKDriverId, BoundarySweep)
	{
		EXPECT_EQ(classify_driver_id(0u), driver_kind::unknown);
		EXPECT_EQ(classify_driver_id(13u), driver_kind::other);          // LLVMpipe
		EXPECT_EQ(classify_driver_id(14u), driver_kind::moltenvk);       // MoltenVK
		EXPECT_EQ(classify_driver_id(15u), driver_kind::other);
		EXPECT_EQ(classify_driver_id(25u), driver_kind::other);
		EXPECT_EQ(classify_driver_id(26u), driver_kind::honeykrisp);     // Honeykrisp
		EXPECT_EQ(classify_driver_id(27u), driver_kind::other);
		EXPECT_EQ(classify_driver_id(28u), driver_kind::kosmickrisp);    // KosmicKrisp
		EXPECT_EQ(classify_driver_id(29u), driver_kind::other);
		EXPECT_EQ(classify_driver_id(0xFFFFFFFFu), driver_kind::other);
		EXPECT_EQ(classify_driver_id(0xFFFFFFFEu), driver_kind::other);
	}

	TEST(KKDriverId, ExhaustiveSweepNeverCrashesAndIsDeterministic)
	{
		for (std::uint32_t id = 0; id < 256; ++id)
		{
			const driver_kind a = classify_driver_id(id);
			const driver_kind b = classify_driver_id(id);
			EXPECT_EQ(a, b) << "id=" << id;

			// Every id maps to exactly one of the enumerated kinds.
			EXPECT_TRUE(a == driver_kind::unknown || a == driver_kind::moltenvk || a == driver_kind::kosmickrisp ||
			            a == driver_kind::honeykrisp || a == driver_kind::other) << "id=" << id;
		}
	}

	TEST(KKDriverId, DisplayNameIsStableAndDistinct)
	{
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };
		std::set<std::string> names;
		std::set<std::string> ids;

		for (driver_kind k : kinds)
		{
			const std::string n = driver_display_name(k);
			const std::string s = driver_kind_name(k);
			EXPECT_FALSE(n.empty()) << static_cast<int>(k);
			EXPECT_FALSE(s.empty()) << static_cast<int>(k);
			names.insert(n);
			ids.insert(s);
		}

		EXPECT_EQ(names.size(), 5u); // all display names unique
		EXPECT_EQ(ids.size(), 5u);
		EXPECT_STREQ(driver_display_name(driver_kind::moltenvk), "MoltenVK");
		EXPECT_STREQ(driver_display_name(driver_kind::kosmickrisp), "KosmicKrisp");
	}

	// -------------------------------------------------------------------------
	// C1 — adapter labels must disambiguate two drivers with the same deviceName
	// -------------------------------------------------------------------------

	TEST(KKAdapterLabel, SameNameDifferentDriversYieldDifferentLabels)
	{
		const std::string name = "Apple M4 Pro";
		const std::string mvk = make_adapter_label(name, driver_kind::moltenvk);
		const std::string kk = make_adapter_label(name, driver_kind::kosmickrisp);

		EXPECT_NE(mvk, kk);
		EXPECT_EQ(mvk, "Apple M4 Pro (MoltenVK)");
		EXPECT_EQ(kk, "Apple M4 Pro (KosmicKrisp)");
		EXPECT_NE(mvk.find(name), std::string::npos);
		EXPECT_NE(kk.find(name), std::string::npos);
	}

	TEST(KKAdapterLabel, UniquenessAcrossNamesAndDrivers)
	{
		const std::vector<std::string> names = { "Apple M4 Pro", "", "A(B)", "\xE6\x97\xA5\xE6\x9C\xAC", "  spaced  ", "Apple M1 Ultra", "x)" };
		const driver_kind kinds[] = { driver_kind::moltenvk, driver_kind::kosmickrisp };

		std::set<std::string> labels;

		for (const auto& n : names)
		{
			for (driver_kind k : kinds)
			{
				const std::string label = make_adapter_label(n, k);
				EXPECT_FALSE(label.empty());
				EXPECT_NE(label.back(), '\0');
				EXPECT_NE(label.find(" ("), std::string::npos); // '(' always preceded by a space
				labels.insert(label);
			}
		}

		EXPECT_EQ(labels.size(), names.size() * 2u); // fully unique
	}

	TEST(KKAdapterLabel, Deterministic)
	{
		for (int i = 0; i < 100; ++i)
		{
			EXPECT_EQ(make_adapter_label("GPU", driver_kind::moltenvk), make_adapter_label("GPU", driver_kind::moltenvk));
			EXPECT_EQ(make_adapter_label("GPU", driver_kind::kosmickrisp), make_adapter_label("GPU", driver_kind::kosmickrisp));
		}
	}

	// -------------------------------------------------------------------------
	// C3 — chip family keyed on driver, not __APPLE__
	// -------------------------------------------------------------------------

	TEST(KKChipFamily, AppleVendorSplitsByDriver)
	{
		EXPECT_EQ(classify_chip_family(apple_vendor_id, driver_kind::moltenvk), chip_family::apple_mvk);
		EXPECT_EQ(classify_chip_family(apple_vendor_id, driver_kind::kosmickrisp), chip_family::apple_kk);
		EXPECT_EQ(classify_chip_family(apple_vendor_id, driver_kind::honeykrisp), chip_family::apple_hk_generic);
		EXPECT_EQ(classify_chip_family(apple_vendor_id, driver_kind::unknown), chip_family::apple_hk_generic);
	}

	TEST(KKChipFamily, NonAppleVendorAlwaysNonApple)
	{
		const std::uint32_t vendors[] = { 0x1002, 0x10DE, 0x8086, 0x5143, 0x0000, 0xFFFF, 0x106A, 0x106C };
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };

		for (std::uint32_t v : vendors)
		{
			for (driver_kind k : kinds)
			{
				EXPECT_EQ(classify_chip_family(v, k), chip_family::non_apple) << "vendor=" << v << " kind=" << static_cast<int>(k);
			}
		}
	}

	TEST(KKChipFamily, AppleBoundaryExactly)
	{
		EXPECT_EQ(classify_chip_family(0x106B, driver_kind::moltenvk), chip_family::apple_mvk);
		EXPECT_EQ(classify_chip_family(0x106B, driver_kind::kosmickrisp), chip_family::apple_kk);
		EXPECT_EQ(classify_chip_family(0x106A, driver_kind::kosmickrisp), chip_family::non_apple);
		EXPECT_EQ(classify_chip_family(0x106C, driver_kind::kosmickrisp), chip_family::non_apple);
	}

	// -------------------------------------------------------------------------
	// C4 — driver workaround profile
	// -------------------------------------------------------------------------

	TEST(KKWorkaround, MoltenVKProfile)
	{
		const auto p = make_workaround_profile(driver_kind::moltenvk);
		EXPECT_TRUE(p.unroll_loops);
		EXPECT_EQ(p.optimal_group_size, 256u);
		EXPECT_FALSE(p.supports_hw_conditional_render); // VKGSRender: vendor != MVK
		EXPECT_TRUE(p.render_target_flags_noop);
		EXPECT_TRUE(p.swapchain_noop);
		EXPECT_TRUE(p.apply_mvk_instance_settings);
		EXPECT_TRUE(p.disable_msl_fast_math_relevant); // MVK-only MSL knob
	}

	TEST(KKWorkaround, KosmicKrispProfileMatchesComputeAndFlags)
	{
		// C4 regression guard: KK must get the same unroll/group-size treatment as
		// Honeykrisp/Apple, and must be a no-op in the render-target and swapchain switches.
		const auto p = make_workaround_profile(driver_kind::kosmickrisp);
		EXPECT_TRUE(p.unroll_loops);
		EXPECT_EQ(p.optimal_group_size, 256u);
		EXPECT_TRUE(p.render_target_flags_noop); // else logs "Unknown driver vendor!"
		EXPECT_TRUE(p.swapchain_noop);

		// KK is NOT MVK: conditional render speedhack is allowed, MVK settings/MSL knob are not.
		EXPECT_TRUE(p.supports_hw_conditional_render);
		EXPECT_FALSE(p.apply_mvk_instance_settings);
		EXPECT_FALSE(p.disable_msl_fast_math_relevant);

		// shadPS4 KK quirk analogues.
		EXPECT_FALSE(p.allows_primitive_restart); // KK unrolls => slow
		EXPECT_EQ(p.max_unique_samplers, 1024u);  // KK OOM past 1024 unique samplers
	}

	TEST(KKWorkaround, HoneykrispMirrorsKKComputeSettings)
	{
		const auto hk = make_workaround_profile(driver_kind::honeykrisp);
		const auto kk = make_workaround_profile(driver_kind::kosmickrisp);
		EXPECT_EQ(hk.unroll_loops, kk.unroll_loops);
		EXPECT_EQ(hk.optimal_group_size, kk.optimal_group_size);
		EXPECT_EQ(hk.render_target_flags_noop, kk.render_target_flags_noop);
	}

	TEST(KKWorkaround, UnknownAndOtherAreSafeDefaults)
	{
		for (driver_kind k : { driver_kind::unknown, driver_kind::other })
		{
			const auto p = make_workaround_profile(k);
			EXPECT_FALSE(p.unroll_loops);
			EXPECT_EQ(p.optimal_group_size, 1u);
			EXPECT_TRUE(p.supports_hw_conditional_render);
			EXPECT_TRUE(p.render_target_flags_noop);
			EXPECT_TRUE(p.swapchain_noop);
			EXPECT_FALSE(p.apply_mvk_instance_settings);
			EXPECT_FALSE(p.disable_msl_fast_math_relevant);
			EXPECT_GT(p.max_unique_samplers, 0u);
		}
	}

	TEST(KKWorkaround, PropertiesAcrossAllKinds)
	{
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };

		for (driver_kind k : kinds)
		{
			const auto p = make_workaround_profile(k);

			// MVK instance settings / MSL knob are strictly MVK-only.
			EXPECT_EQ(p.apply_mvk_instance_settings, k == driver_kind::moltenvk) << static_cast<int>(k);
			EXPECT_EQ(p.disable_msl_fast_math_relevant, k == driver_kind::moltenvk) << static_cast<int>(k);

			// No driver is ever left in the "unknown vendor" default for render targets.
			EXPECT_TRUE(p.render_target_flags_noop) << static_cast<int>(k);

			// group size is a sane positive power-of-two-ish value.
			EXPECT_GT(p.optimal_group_size, 0u) << static_cast<int>(k);
			EXPECT_GT(p.max_unique_samplers, 0u) << static_cast<int>(k);
		}
	}

	// -------------------------------------------------------------------------
	// Minimal JSON parser
	// -------------------------------------------------------------------------

	TEST(KKJson, ParsesFlatScalars)
	{
		const auto r = parse_flat_json_object(R"({ "a": "x", "b": true, "c": false, "d": 42, "e": null })");
		ASSERT_TRUE(r.ok) << r.error;
		ASSERT_EQ(r.fields.size(), 5u);
		EXPECT_EQ(r.fields.at("a").kind, json_scalar::type::string);
		EXPECT_EQ(r.fields.at("a").str, "x");
		EXPECT_EQ(r.fields.at("b").kind, json_scalar::type::boolean);
		EXPECT_TRUE(r.fields.at("b").boolean);
		EXPECT_FALSE(r.fields.at("c").boolean);
		EXPECT_EQ(r.fields.at("d").kind, json_scalar::type::number);
		EXPECT_DOUBLE_EQ(r.fields.at("d").number, 42.0);
		EXPECT_EQ(r.fields.at("e").kind, json_scalar::type::null_);
	}

	TEST(KKJson, EmptyObject)
	{
		const auto r = parse_flat_json_object("{}");
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_TRUE(r.fields.empty());

		const auto r2 = parse_flat_json_object("  {   }  \n");
		ASSERT_TRUE(r2.ok) << r2.error;
		EXPECT_TRUE(r2.fields.empty());
	}

	TEST(KKJson, ToleratesUtf8Bom)
	{
		const std::string with_bom = "\xEF\xBB\xBF{ \"a\": \"x\" }";
		const auto r = parse_flat_json_object(with_bom);
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_EQ(r.fields.at("a").str, "x");
	}

	TEST(KKJson, SkipsNestedContainers)
	{
		const auto r = parse_flat_json_object(R"({ "ICD": { "library_path": "x", "n": [1,2,3] }, "top": "y" })");
		ASSERT_TRUE(r.ok) << r.error;
		ASSERT_EQ(r.fields.size(), 2u);
		EXPECT_EQ(r.fields.at("ICD").kind, json_scalar::type::skipped);
		EXPECT_EQ(r.fields.at("top").str, "y");
		EXPECT_NE(r.fields.at("ICD").raw.find("library_path"), std::string::npos);
	}

	TEST(KKJson, StringEscapes)
	{
		const auto r = parse_flat_json_object(R"({ "p": "a\"b\\c\/d\t" })");
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_EQ(r.fields.at("p").str, std::string("a\"b\\c/d\t"));
	}

	TEST(KKJson, UnicodeEscapeKeptRaw)
	{
		const auto r = parse_flat_json_object(R"({ "n": "\u00e9x" })");
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_EQ(r.fields.at("n").str, std::string("\\u00e9x"));
	}

	TEST(KKJson, DuplicateKeysLastWins)
	{
		const auto r = parse_flat_json_object(R"({ "a": "1", "a": "2" })");
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_EQ(r.fields.at("a").str, "2");
	}

	TEST(KKJson, NegativeNumbersAndExponents)
	{
		const auto r = parse_flat_json_object(R"({ "a": -3.5e2, "b": 0 })");
		ASSERT_TRUE(r.ok) << r.error;
		EXPECT_DOUBLE_EQ(r.fields.at("a").number, -350.0);
		EXPECT_DOUBLE_EQ(r.fields.at("b").number, 0.0);
	}

	TEST(KKJson, MalformedInputsRejected)
	{
		const std::vector<std::string> bad = {
			"",
			"   ",
			"{",
			"}",
			"[]",
			"\"x\"",
			"{ \"a\" }",
			"{ \"a\": }",
			"{ \"a\": \"unterminated }",
			"{ \"a\": \"x\" ",
			"{ \"a\": \"x\" } trailing",
			"{ \"a\": @ }",
			"{ \"a\": \"\\q\" }",
			"{ \"a\": \"x\", }",
		};

		for (const auto& s : bad)
		{
			const auto r = parse_flat_json_object(s);
			EXPECT_FALSE(r.ok) << "should reject: [" << s << "]";
			EXPECT_FALSE(r.error.empty()) << "empty error for: [" << s << "]";
		}
	}

	TEST(KKJson, UnterminatedNestedContainerRejected)
	{
		EXPECT_FALSE(parse_flat_json_object(R"({ "ICD": { "x": 1 )").ok);
		EXPECT_FALSE(parse_flat_json_object(R"({ "ICD": [1, 2 )").ok);
	}

	// -------------------------------------------------------------------------
	// C7 — ICD manifest parsing / classification / rewriting
	// -------------------------------------------------------------------------

	TEST(KKManifest, ClassifyFilenames)
	{
		EXPECT_EQ(classify_manifest_filename("MoltenVK_icd.json"), driver_kind::moltenvk);
		EXPECT_EQ(classify_manifest_filename("moltenvk_icd.json"), driver_kind::moltenvk);
		EXPECT_EQ(classify_manifest_filename("libkosmickrisp_icd.json"), driver_kind::kosmickrisp);
		EXPECT_EQ(classify_manifest_filename("kosmickrisp_mesa_icd.aarch64.json"), driver_kind::kosmickrisp);
		EXPECT_EQ(classify_manifest_filename("KosmicKrisp_icd.json"), driver_kind::kosmickrisp);
		EXPECT_EQ(classify_manifest_filename("honeykrisp_icd.json"), driver_kind::honeykrisp);
		EXPECT_EQ(classify_manifest_filename("swrast_icd.json"), driver_kind::unknown);
		EXPECT_EQ(classify_manifest_filename(""), driver_kind::unknown);
		EXPECT_EQ(classify_manifest_filename("random.json"), driver_kind::unknown);
	}

	TEST(KKManifest, ParseMoltenVKStyleNestedManifest)
	{
		const std::string json = R"({
			"file_format_version": "1.0.0",
			"ICD": {
				"library_path": "./libMoltenVK.dylib",
				"api_version": "1.1.0",
				"is_portability_driver": true
			}
		})";

		const auto m = parse_icd_manifest("MoltenVK_icd.json", json);
		EXPECT_TRUE(m.error.empty()) << m.error;
		EXPECT_EQ(m.kind, driver_kind::moltenvk);
		EXPECT_TRUE(m.has_library_path);
		EXPECT_EQ(m.library_path, "./libMoltenVK.dylib");
		EXPECT_TRUE(m.has_portability_key);
		EXPECT_TRUE(m.is_portability_driver);
	}

	TEST(KKManifest, ParseKosmickrispStyleNoPortability)
	{
		const std::string json = R"({
			"file_format_version": "1.0.0",
			"ICD": { "library_path": "libvulkan_kosmickrisp.dylib", "api_version": "1.4.0" }
		})";

		const auto m = parse_icd_manifest("kosmickrisp_mesa_icd.aarch64.json", json);
		EXPECT_TRUE(m.error.empty()) << m.error;
		EXPECT_EQ(m.kind, driver_kind::kosmickrisp);
		EXPECT_EQ(m.library_path, "libvulkan_kosmickrisp.dylib");
		EXPECT_FALSE(m.has_portability_key);
		EXPECT_FALSE(m.is_portability_driver); // default: not a portability driver
	}

	TEST(KKManifest, ParseFlatManifestAlsoAccepted)
	{
		const auto m = parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": "x", "is_portability_driver": false })");
		EXPECT_TRUE(m.error.empty()) << m.error;
		EXPECT_TRUE(m.has_library_path);
		EXPECT_TRUE(m.has_portability_key);
		EXPECT_FALSE(m.is_portability_driver);
	}

	TEST(KKManifest, MissingLibraryPathIsError)
	{
		const auto m = parse_icd_manifest("MoltenVK_icd.json", R"({ "file_format_version": "1.0.0" })");
		EXPECT_FALSE(m.error.empty());
		EXPECT_FALSE(m.has_library_path);
	}

	TEST(KKManifest, WrongTypeLibraryPathIsError)
	{
		const auto m = parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": 123 })");
		EXPECT_FALSE(m.error.empty());
		EXPECT_FALSE(m.has_library_path);
	}

	TEST(KKManifest, WrongTypePortabilityIsError)
	{
		const auto m = parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": "x", "is_portability_driver": "yes" })");
		EXPECT_FALSE(m.error.empty());
		EXPECT_FALSE(m.has_portability_key);
	}

	TEST(KKManifest, MalformedJsonIsError)
	{
		for (const std::string& s : { std::string("{"), std::string("{ \"library_path\" }"), std::string("{ \"ICD\": { \"library_path\": \"x\" ") })
		{
			const auto m = parse_icd_manifest("MoltenVK_icd.json", s);
			EXPECT_FALSE(m.error.empty()) << s;
		}
	}

	TEST(KKManifest, RewriteLibraryPathVariants)
	{
		EXPECT_EQ(rewrite_library_path("./libMoltenVK.dylib"), "../../../Frameworks/libMoltenVK.dylib");
		EXPECT_EQ(rewrite_library_path("./libvulkan_kosmickrisp.dylib"), "../../../Frameworks/libvulkan_kosmickrisp.dylib");
		EXPECT_EQ(rewrite_library_path("libvulkan_kosmickrisp.dylib"), "../../../Frameworks/libvulkan_kosmickrisp.dylib");
		EXPECT_EQ(rewrite_library_path("./"), "../../../Frameworks/");
		EXPECT_EQ(rewrite_library_path("./sub/dir/x.dylib"), "../../../Frameworks/sub/dir/x.dylib");

		// Already-rewritten / loader-relative / absolute paths are left untouched.
		EXPECT_EQ(rewrite_library_path("../../../Frameworks/libMoltenVK.dylib"), "../../../Frameworks/libMoltenVK.dylib");
		EXPECT_EQ(rewrite_library_path("/opt/local/lib/libvulkan_kosmickrisp.dylib"), "/opt/local/lib/libvulkan_kosmickrisp.dylib");
		EXPECT_EQ(rewrite_library_path("@rpath/libMoltenVK.dylib"), "@rpath/libMoltenVK.dylib");
		EXPECT_EQ(rewrite_library_path("@loader_path/../Frameworks/x.dylib"), "@loader_path/../Frameworks/x.dylib");
		EXPECT_EQ(rewrite_library_path("@executable_path/../Frameworks/x.dylib"), "@executable_path/../Frameworks/x.dylib");
		EXPECT_EQ(rewrite_library_path("../x.dylib"), "../x.dylib");
		EXPECT_EQ(rewrite_library_path(""), "");
	}

	TEST(KKManifest, RewriteIsIdempotent)
	{
		for (const std::string& p : { std::string("./a.dylib"), std::string("a.dylib"), std::string("/a"), std::string("@rpath/a"), std::string("") })
		{
			const std::string once = rewrite_library_path(p);
			const std::string twice = rewrite_library_path(once);
			EXPECT_EQ(once, twice) << p;
		}
	}

	TEST(KKManifest, RenderRoundTripsAndInjectsRewrittenPath)
	{
		icd_manifest m;
		m.filename = "MoltenVK_icd.json";
		m.library_path = "./libMoltenVK.dylib";
		m.is_portability_driver = true;
		m.kind = driver_kind::moltenvk;

		const std::string json = render_icd_manifest(m);
		EXPECT_NE(json.find("../../../Frameworks/libMoltenVK.dylib"), std::string::npos);
		EXPECT_NE(json.find("true"), std::string::npos);

		const auto parsed = parse_icd_manifest(m.filename, json);
		EXPECT_TRUE(parsed.error.empty()) << parsed.error;
		EXPECT_EQ(parsed.library_path, "../../../Frameworks/libMoltenVK.dylib");
		EXPECT_TRUE(parsed.is_portability_driver);

		// rewrite=false keeps the original path.
		const std::string raw = render_icd_manifest(m, false);
		EXPECT_NE(raw.find("./libMoltenVK.dylib"), std::string::npos);
	}

	// -------------------------------------------------------------------------
	// C5 — duplicate ICD discovery
	// -------------------------------------------------------------------------

	TEST(KKDedup, BundleWinsOverSystemByFilename)
	{
		std::vector<icd_manifest> manifests;
		manifests.push_back(parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": "./bundle/libMoltenVK.dylib" })"));
		manifests.push_back(parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": "/opt/local/lib/libMoltenVK.dylib" })"));
		manifests.push_back(parse_icd_manifest("kosmickrisp_mesa_icd.aarch64.json", R"({ "library_path": "libvulkan_kosmickrisp.dylib" })"));

		const auto deduped = dedup_by_filename(manifests);
		ASSERT_EQ(deduped.size(), 2u);
		EXPECT_EQ(deduped[0].filename, "MoltenVK_icd.json");
		EXPECT_EQ(deduped[0].library_path, "./bundle/libMoltenVK.dylib"); // first (bundle) kept
		EXPECT_EQ(count_kind(deduped, driver_kind::moltenvk), 1u);
		EXPECT_EQ(count_kind(deduped, driver_kind::kosmickrisp), 1u);
	}

	TEST(KKDedup, DifferentFilenamesSameDriverBothSurvive)
	{
		// Two distinct KK manifests (LunarG SDK name + Mesa name) => duplicate-driver hazard.
		std::vector<icd_manifest> manifests;
		manifests.push_back(parse_icd_manifest("libkosmickrisp_icd.json", R"({ "library_path": "libvulkan_kosmickrisp.dylib" })"));
		manifests.push_back(parse_icd_manifest("kosmickrisp_mesa_icd.aarch64.json", R"({ "library_path": "libvulkan_kosmickrisp.dylib" })"));
		manifests.push_back(parse_icd_manifest("MoltenVK_icd.json", R"({ "library_path": "./libMoltenVK.dylib" })"));

		const auto deduped = dedup_by_filename(manifests);
		EXPECT_EQ(deduped.size(), 3u);
		EXPECT_EQ(count_kind(deduped, driver_kind::kosmickrisp), 2u); // hazard detectable
		EXPECT_EQ(count_kind(deduped, driver_kind::moltenvk), 1u);
	}

	TEST(KKDedup, EmptyAndNoDuplicates)
	{
		EXPECT_TRUE(dedup_by_filename({}).empty());

		std::vector<icd_manifest> manifests;
		manifests.push_back(parse_icd_manifest("a.json", R"({ "library_path": "x" })"));
		manifests.push_back(parse_icd_manifest("b.json", R"({ "library_path": "y" })"));
		const auto deduped = dedup_by_filename(manifests);
		EXPECT_EQ(deduped.size(), 2u);
	}

	TEST(KKDedup, IdempotentAndBounded)
	{
		std::vector<icd_manifest> manifests;
		for (int i = 0; i < 10; ++i)
		{
			manifests.push_back(parse_icd_manifest("dup.json", R"({ "library_path": "x" })"));
			manifests.push_back(parse_icd_manifest("unique" + std::to_string(i) + ".json", R"({ "library_path": "y" })"));
		}

		const auto once = dedup_by_filename(manifests);
		const auto twice = dedup_by_filename(once);
		EXPECT_LE(once.size(), manifests.size());
		EXPECT_EQ(once.size(), twice.size());
		for (std::size_t i = 0; i < once.size(); ++i)
		{
			EXPECT_EQ(once[i].filename, twice[i].filename);
		}
	}

	// -------------------------------------------------------------------------
	// C6 — loader requirement + portability gate
	// -------------------------------------------------------------------------

	TEST(KKLoader, OnlyMoltenVKExportsPublicApi)
	{
		EXPECT_TRUE(icd_exports_public_vulkan_api(driver_kind::moltenvk));
		EXPECT_FALSE(icd_exports_public_vulkan_api(driver_kind::kosmickrisp));
		EXPECT_FALSE(icd_exports_public_vulkan_api(driver_kind::honeykrisp));
		EXPECT_FALSE(icd_exports_public_vulkan_api(driver_kind::unknown));
	}

	TEST(KKLoader, KosmickrispForcesSeparateLoader)
	{
		EXPECT_TRUE(needs_separate_loader(driver_kind::kosmickrisp));
		EXPECT_FALSE(needs_separate_loader(driver_kind::moltenvk));
		EXPECT_FALSE(needs_separate_loader(driver_kind::unknown));

		EXPECT_FALSE(bundle_needs_separate_loader({ driver_kind::moltenvk }));
		EXPECT_TRUE(bundle_needs_separate_loader({ driver_kind::moltenvk, driver_kind::kosmickrisp }));
		EXPECT_TRUE(bundle_needs_separate_loader({ driver_kind::kosmickrisp }));
		EXPECT_FALSE(bundle_needs_separate_loader({}));
	}

	TEST(KKLoader, PortabilityGateMustStayOn)
	{
		EXPECT_TRUE(must_enable_portability_enumeration());
	}

	TEST(KKLoader, EnumeratedDriversDependOnPortability)
	{
		// C-regression: "no portability flag => only KK; with the flag => MVK + KK".
		const auto with = enumerated_drivers(true);
		const auto without = enumerated_drivers(false);

		ASSERT_EQ(with.size(), 2u);
		ASSERT_EQ(without.size(), 1u);
		EXPECT_EQ(without[0], driver_kind::kosmickrisp);

		// with-flag set is a superset of without-flag set and always contains KK.
		for (driver_kind k : without)
		{
			EXPECT_NE(std::find(with.begin(), with.end(), k), with.end());
		}
		EXPECT_NE(std::find(with.begin(), with.end(), driver_kind::kosmickrisp), with.end());
		EXPECT_NE(std::find(with.begin(), with.end(), driver_kind::moltenvk), with.end());
	}

	// -------------------------------------------------------------------------
	// C8/C10 — host + bundle gating
	// -------------------------------------------------------------------------

	TEST(KKHostGating, VersionAndArchBoundaries)
	{
		EXPECT_FALSE(host_supports_kosmickrisp(25, host_arch::x86_64));
		EXPECT_FALSE(host_supports_kosmickrisp(25, host_arch::arm64));
		EXPECT_FALSE(host_supports_kosmickrisp(26, host_arch::x86_64));
		EXPECT_TRUE(host_supports_kosmickrisp(26, host_arch::arm64));
		EXPECT_TRUE(host_supports_kosmickrisp(27, host_arch::arm64));
		EXPECT_TRUE(host_supports_kosmickrisp(100, host_arch::arm64));
		EXPECT_FALSE(host_supports_kosmickrisp(0, host_arch::arm64));
		EXPECT_FALSE(host_supports_kosmickrisp(-1, host_arch::arm64));
		EXPECT_FALSE(host_supports_kosmickrisp(26, host_arch::unknown));
	}

	TEST(KKHostGating, ExperimentalKosmickrispIsAlwaysArm64)
	{
		for (int major = 0; major <= 40; ++major)
		{
			EXPECT_FALSE(host_supports_kosmickrisp(major, host_arch::x86_64)) << "macOS " << major;
			EXPECT_EQ(host_supports_kosmickrisp(major, host_arch::arm64), major >= 26) << "macOS " << major;
		}
	}

	TEST(KKHostGating, BundleInclusion)
	{
		EXPECT_TRUE(bundle_should_include(driver_kind::kosmickrisp, host_arch::arm64));
		EXPECT_FALSE(bundle_should_include(driver_kind::kosmickrisp, host_arch::x86_64));
		EXPECT_FALSE(bundle_should_include(driver_kind::kosmickrisp, host_arch::unknown));
		EXPECT_TRUE(bundle_should_include(driver_kind::moltenvk, host_arch::arm64));
		EXPECT_TRUE(bundle_should_include(driver_kind::moltenvk, host_arch::x86_64));
		EXPECT_FALSE(bundle_should_include(driver_kind::moltenvk, host_arch::unknown));
		EXPECT_FALSE(bundle_should_include(driver_kind::unknown, host_arch::arm64));
	}

	// -------------------------------------------------------------------------
	// C9 — instance api version
	// -------------------------------------------------------------------------

	TEST(KKApiVersion, RequestedTwelveAgainstExposedFourteen)
	{
		const std::uint32_t v12 = make_version(1, 2, 0);
		const std::uint32_t v14 = make_version(1, 4, 0);
		const std::uint32_t v11 = make_version(1, 1, 0);

		EXPECT_TRUE(driver_api_version_compatible(v12, v14));
		EXPECT_TRUE(driver_api_version_compatible(v12, v12));
		EXPECT_TRUE(driver_api_version_compatible(v11, v12));
		EXPECT_FALSE(driver_api_version_compatible(v14, v12));
		EXPECT_TRUE(driver_api_version_compatible(v14, v14));

		// Patch field is ignored: a 1.2.0 request is satisfied by 1.2.255, but a 1.3 request is not.
		EXPECT_TRUE(driver_api_version_compatible(make_version(1, 2, 0), make_version(1, 2, 255)));
		EXPECT_FALSE(driver_api_version_compatible(make_version(1, 3, 0), make_version(1, 2, 255)));
		EXPECT_TRUE(driver_api_version_compatible(make_version(1, 2, 255), make_version(1, 2, 0)));
	}

	TEST(KKApiVersion, SweepMajorVersions)
	{
		for (std::uint32_t exposed = 0; exposed <= 4; ++exposed)
		{
			for (std::uint32_t requested = 1; requested <= 4; ++requested)
			{
				EXPECT_EQ(driver_api_version_compatible(make_version(requested, 0, 0), make_version(exposed, 9, 9)), exposed >= requested)
					<< "req=" << requested << " exposed=" << exposed;
			}
		}
	}

	// -------------------------------------------------------------------------
	// Selection policy
	// -------------------------------------------------------------------------

	TEST(KKSelection, AutoPrefersMoltenVKOnEveryHost)
	{
		// MoltenVK is the default in all cases; KosmicKrisp is opt-in per machine.
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
		};

		for (const host_info& host : { host_macos27_arm, host_macos26_arm, host_macos25_arm, host_macos15_arm, host_macos26_x86, host_macos15_x86, host_unknown })
		{
			const auto r = select_device(driver_preference::auto_, devices, host);
			ASSERT_TRUE(r.found) << "macOS " << host.macos_major;
			EXPECT_EQ(r.chosen, driver_kind::moltenvk);
			EXPECT_EQ(r.chosen_index, 0);
			EXPECT_FALSE(r.fell_back);
			EXPECT_TRUE(r.diagnostic.empty());
		}
	}

	TEST(KKSelection, AutoOnlyKosmickrisp)
	{
		const std::vector<device_info> devices = { dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 0) };

		const auto ok = select_device(driver_preference::auto_, devices, host_macos27_arm);
		ASSERT_TRUE(ok.found);
		EXPECT_EQ(ok.chosen, driver_kind::kosmickrisp);

		// Impossible-in-reality: a KK device on an ineligible host. Auto must not pick it.
		const auto bad = select_device(driver_preference::auto_, devices, host_macos15_arm);
		EXPECT_FALSE(bad.found);
		EXPECT_FALSE(bad.diagnostic.empty());
	}

	TEST(KKSelection, AutoOnlyMoltenVK)
	{
		const std::vector<device_info> devices = { dev(driver_kind::moltenvk, "Intel Iris", driver_id_moltenvk, 0) };

		for (const host_info& host : { host_macos26_arm, host_macos15_arm, host_macos26_x86, host_macos15_x86, host_unknown })
		{
			const auto r = select_device(driver_preference::auto_, devices, host);
			ASSERT_TRUE(r.found);
			EXPECT_EQ(r.chosen, driver_kind::moltenvk);
		}
	}

	TEST(KKSelection, AutoEmptyDeviceList)
	{
		const auto r = select_device(driver_preference::auto_, {}, host_macos26_arm);
		EXPECT_FALSE(r.found);
		EXPECT_EQ(r.chosen_index, -1);
		EXPECT_EQ(r.chosen, driver_kind::unknown);
		EXPECT_FALSE(r.diagnostic.empty());
	}

	TEST(KKSelection, AutoIgnoresNonAppleDrivers)
	{
		// Homebrew mesa on Tahoe adds lavapipe (a "software" device). Auto must never pick it.
		const std::vector<device_info> devices = {
			dev(driver_kind::other, "llvmpipe (LLVM 20.1.8)", driver_id_mesa_llvmpipe, 0),
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 1),
		};

		const auto r = select_device(driver_preference::auto_, devices, host_macos26_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::moltenvk);
		EXPECT_NE(r.chosen, driver_kind::other);
	}

	TEST(KKSelection, AutoIgnoresLavapipeEvenAlone)
	{
		const std::vector<device_info> devices = { dev(driver_kind::other, "llvmpipe", driver_id_mesa_llvmpipe, 0) };
		const auto r = select_device(driver_preference::auto_, devices, host_macos26_arm);
		EXPECT_FALSE(r.found); // no MVK/KK => no usable driver
	}

	TEST(KKSelection, ExplicitMoltenVKPresent)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
		};

		const auto r = select_device(driver_preference::moltenvk, devices, host_macos26_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::moltenvk);
		EXPECT_FALSE(r.fell_back);
		EXPECT_TRUE(r.diagnostic.empty());
	}

	TEST(KKSelection, ExplicitMoltenVKFallsBackToKosmickrisp)
	{
		const std::vector<device_info> devices = { dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 0) };

		const auto r = select_device(driver_preference::moltenvk, devices, host_macos27_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::kosmickrisp);
		EXPECT_TRUE(r.fell_back);
		EXPECT_FALSE(r.diagnostic.empty());
	}

	TEST(KKSelection, ExplicitMoltenVKWithOnlyIneligibleKosmickrisp)
	{
		const std::vector<device_info> devices = { dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 0) };

		const auto r = select_device(driver_preference::moltenvk, devices, host_macos15_arm);
		EXPECT_FALSE(r.found);
		EXPECT_FALSE(r.diagnostic.empty());
	}

	TEST(KKSelection, ExplicitKosmickrispPresent)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
		};

		const auto r = select_device(driver_preference::kosmickrisp, devices, host_macos27_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::kosmickrisp);
		EXPECT_EQ(r.chosen_index, 1);
		EXPECT_FALSE(r.fell_back);
	}

	TEST(KKSelection, ExplicitKosmickrispFallsBackOnIntelAndOldOS)
	{
		const std::vector<device_info> devices = { dev(driver_kind::moltenvk, "Intel Iris", driver_id_moltenvk, 0) };

		for (const host_info& host : { host_macos15_arm, host_macos25_arm, host_macos26_arm, host_macos26_x86, host_macos15_x86 })
		{
			const auto r = select_device(driver_preference::kosmickrisp, devices, host);
			ASSERT_TRUE(r.found);
			EXPECT_EQ(r.chosen, driver_kind::moltenvk);
			EXPECT_TRUE(r.fell_back);
			EXPECT_FALSE(r.diagnostic.empty());
		}
	}

	TEST(KKSelection, ExplicitKosmickrispOnlyIneligible)
	{
		const std::vector<device_info> devices = { dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 0) };
		const auto r = select_device(driver_preference::kosmickrisp, devices, host_macos15_arm);
		EXPECT_FALSE(r.found);
		EXPECT_FALSE(r.diagnostic.empty());
	}

	TEST(KKSelection, EverythingAbsent)
	{
		for (driver_preference pref : { driver_preference::auto_, driver_preference::moltenvk, driver_preference::kosmickrisp })
		{
			const auto r = select_device(pref, {}, host_macos26_arm);
			EXPECT_FALSE(r.found) << preference_name(pref);
			EXPECT_EQ(r.chosen_index, -1);
			EXPECT_FALSE(r.diagnostic.empty());
		}
	}

	TEST(KKSelection, NeverSelectsByIndexAlone)
	{
		// Wanted driver sits far from index 0; selection must find it regardless of order.
		const std::vector<device_info> devices = {
			dev(driver_kind::other, "llvmpipe", driver_id_mesa_llvmpipe, 0),
			dev(driver_kind::other, "software rasterizer", driver_id_mesa_llvmpipe, 1),
			dev(driver_kind::other, "swrast", driver_id_mesa_llvmpipe, 2),
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 3),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 4),
		};

		const auto r = select_device(driver_preference::kosmickrisp, devices, host_macos27_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::kosmickrisp);
		EXPECT_EQ(r.chosen_index, 4);
	}

	TEST(KKSelection, ChoosesCanonicallyAmongSameKind)
	{
		// Two MVK devices (duplicate ICD); selection must be order-independent.
		const std::vector<device_info> a = {
			dev(driver_kind::moltenvk, "Zed GPU", driver_id_moltenvk, 0),
			dev(driver_kind::moltenvk, "Apple GPU", driver_id_moltenvk, 1),
		};
		const std::vector<device_info> b = {
			dev(driver_kind::moltenvk, "Apple GPU", driver_id_moltenvk, 0),
			dev(driver_kind::moltenvk, "Zed GPU", driver_id_moltenvk, 1),
		};

		const auto ra = select_device(driver_preference::moltenvk, a, host_macos26_arm);
		const auto rb = select_device(driver_preference::moltenvk, b, host_macos26_arm);
		ASSERT_TRUE(ra.found);
		ASSERT_TRUE(rb.found);
		EXPECT_EQ(ra.chosen_name, "Apple GPU");
		EXPECT_EQ(ra.chosen_name, rb.chosen_name);
	}

	TEST(KKSelection, PrefersMoltenVKWhenBothKindsHaveSameNameMixedOrder)
	{
		// Same name, both drivers, MVK first.
		const auto r = select_device(driver_preference::moltenvk,
			{ dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			  dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1) },
			host_macos26_arm);
		ASSERT_TRUE(r.found);
		EXPECT_EQ(r.chosen, driver_kind::moltenvk);
		EXPECT_EQ(r.chosen_name, "Apple M4 Pro");
	}

	// -------------------------------------------------------------------------
	// C1 — adapter lookup must be (kind, name), never name alone
	// -------------------------------------------------------------------------

	TEST(KKAdapterLookup, MatchesKindAndNameTogether)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
		};

		EXPECT_EQ(find_adapter_index(devices, driver_kind::moltenvk, "Apple M4 Pro"), 0);
		EXPECT_EQ(find_adapter_index(devices, driver_kind::kosmickrisp, "Apple M4 Pro"), 1);
		EXPECT_EQ(find_adapter_index(devices, driver_kind::unknown, "Apple M4 Pro"), -1);
		EXPECT_EQ(find_adapter_index(devices, driver_kind::kosmickrisp, "Nonexistent"), -1);
	}

	TEST(KKAdapterLookup, NameAloneNeverMatchesWrongKind)
	{
		// A name that exists only under MVK must not resolve for KK.
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Only-MVK", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Only-KK", driver_id_mesa_kosmickrisp, 1),
		};

		EXPECT_EQ(find_adapter_index(devices, driver_kind::kosmickrisp, "Only-MVK"), -1);
		EXPECT_EQ(find_adapter_index(devices, driver_kind::moltenvk, "Only-KK"), -1);
		EXPECT_EQ(find_adapter_index({}, driver_kind::moltenvk, "x"), -1);
	}

	// -------------------------------------------------------------------------
	// Invariants / properties
	// -------------------------------------------------------------------------

	TEST(KKProperties, SelectionResultIsSelfConsistent)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
			dev(driver_kind::other, "llvmpipe", driver_id_mesa_llvmpipe, 2),
		};

		for (driver_preference pref : { driver_preference::auto_, driver_preference::moltenvk, driver_preference::kosmickrisp })
		{
			for (const host_info& host : { host_macos26_arm, host_macos27_arm, host_macos25_arm, host_macos15_arm, host_macos26_x86, host_macos15_x86 })
			{
				const auto r = select_device(pref, devices, host);

				if (r.found)
				{
					ASSERT_GE(r.chosen_index, 0);
					ASSERT_LT(r.chosen_index, static_cast<int>(devices.size()));
					EXPECT_EQ(devices[static_cast<std::size_t>(r.chosen_index)].kind, r.chosen);
					EXPECT_EQ(devices[static_cast<std::size_t>(r.chosen_index)].name, r.chosen_name);
					EXPECT_TRUE(r.chosen == driver_kind::moltenvk || r.chosen == driver_kind::kosmickrisp);

					if (r.chosen == driver_kind::kosmickrisp)
					{
						EXPECT_TRUE(host_supports_kosmickrisp(host.macos_major, host.arch)); // never pick ineligible KK
					}
					if (!r.fell_back)
					{
						EXPECT_TRUE(r.diagnostic.empty());
					}
					else
					{
						EXPECT_FALSE(r.diagnostic.empty());
					}
				}
				else
				{
					EXPECT_EQ(r.chosen_index, -1);
					EXPECT_EQ(r.chosen, driver_kind::unknown);
					EXPECT_FALSE(r.diagnostic.empty());
				}
			}
		}
	}

	TEST(KKProperties, FallbackNeverThrows)
	{
		const std::vector<device_info> weird = {
			dev(driver_kind::unknown, std::string(1000, 'x'), 0, 0),
			dev(driver_kind::other, "\x00\x01\x02", 0xFFFFFFFFu, 1),
			dev(driver_kind::moltenvk, "", 14, 2),
		};

		for (driver_preference pref : { driver_preference::auto_, driver_preference::moltenvk, driver_preference::kosmickrisp })
		{
			for (const host_info& host : { host_macos26_arm, host_macos15_x86, host_unknown })
			{
				const auto r = select_device(pref, weird, host);
				(void)r;
				const auto r_empty = select_device(pref, {}, host);
				(void)r_empty;
			}
		}
	}

	TEST(KKProperties, ManifestHelpersNeverThrow)
	{
		const std::vector<std::string> inputs = {
			"", "{", "}", "\"", "{ \"library_path\": }", "{\"ICD\":{}}",
			std::string("\xff\xfe"), std::string(4096, '{'),
		};

		for (const auto& s : inputs)
		{
			const auto m = parse_icd_manifest("MoltenVK_icd.json", s);
			(void)m;
			const auto p = parse_flat_json_object(s);
			(void)p;
			const auto r = rewrite_library_path(s);
			(void)r;
		}
	}

	// -------------------------------------------------------------------------
	// Seeded randomized property tests
	// -------------------------------------------------------------------------

	TEST(KKRandomized, SelectionInvariants)
	{
		std::mt19937 rng(0xC0FFEE);
		std::uniform_int_distribution<int> kind_dist(0, 4);
		std::uniform_int_distribution<int> count_dist(0, 6);
		std::uniform_int_distribution<int> name_dist(0, 3);
		std::uniform_int_distribution<int> os_dist(0, 30);
		std::uniform_int_distribution<int> arch_dist(0, 2);

		const char* names[] = { "Apple M4 Pro", "Intel Iris", "llvmpipe", "" };
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };
		const host_arch archs[] = { host_arch::unknown, host_arch::x86_64, host_arch::arm64 };
		const driver_preference prefs[] = { driver_preference::auto_, driver_preference::moltenvk, driver_preference::kosmickrisp };

		for (int iter = 0; iter < 3000; ++iter)
		{
			const int n = count_dist(rng);
			std::vector<device_info> devices;

			for (int i = 0; i < n; ++i)
			{
				const driver_kind k = kinds[kind_dist(rng)];
				std::uint32_t id = 0;
				if (k == driver_kind::moltenvk) id = driver_id_moltenvk;
				else if (k == driver_kind::kosmickrisp) id = driver_id_mesa_kosmickrisp;
				else if (k == driver_kind::honeykrisp) id = driver_id_mesa_honeykrisp;
				devices.push_back(dev(k, names[name_dist(rng)], id, i));
			}

			host_info host;
			host.macos_major = os_dist(rng);
			host.arch = archs[arch_dist(rng)];

			for (driver_preference pref : prefs)
			{
				const auto r = select_device(pref, devices, host);

				if (r.found)
				{
					ASSERT_GE(r.chosen_index, 0);
					ASSERT_LT(r.chosen_index, static_cast<int>(devices.size()));
					EXPECT_EQ(devices[static_cast<std::size_t>(r.chosen_index)].kind, r.chosen);
					EXPECT_EQ(devices[static_cast<std::size_t>(r.chosen_index)].name, r.chosen_name);
					EXPECT_TRUE(r.chosen == driver_kind::moltenvk || r.chosen == driver_kind::kosmickrisp);
					if (r.chosen == driver_kind::kosmickrisp)
					{
						EXPECT_TRUE(host_supports_kosmickrisp(host.macos_major, host.arch));
					}
				}
				else
				{
					EXPECT_EQ(r.chosen_index, -1);
					EXPECT_EQ(r.chosen, driver_kind::unknown);
				}
			}
		}
	}

	TEST(KKRandomized, SelectionIsOrderIndependent)
	{
		std::mt19937 rng(0xC0FFEE);
		std::uniform_int_distribution<int> kind_dist(0, 4);
		std::uniform_int_distribution<int> count_dist(1, 8);
		std::uniform_int_distribution<int> name_dist(0, 4);
		std::uniform_int_distribution<int> os_dist(0, 30);
		std::uniform_int_distribution<int> arch_dist(0, 2);

		const char* names[] = { "A", "B", "C", "D", "E" };
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };
		const host_arch archs[] = { host_arch::unknown, host_arch::x86_64, host_arch::arm64 };
		const driver_preference prefs[] = { driver_preference::auto_, driver_preference::moltenvk, driver_preference::kosmickrisp };
		std::uniform_int_distribution<std::uint32_t> id_dist(1, 10);

		for (int iter = 0; iter < 500; ++iter)
		{
			const int n = count_dist(rng);
			std::vector<device_info> devices;

			for (int i = 0; i < n; ++i)
			{
				devices.push_back(dev(kinds[kind_dist(rng)], names[name_dist(rng)], id_dist(rng), i));
			}

			host_info host;
			host.macos_major = os_dist(rng);
			host.arch = archs[arch_dist(rng)];

			for (driver_preference pref : prefs)
			{
				const auto base = select_device(pref, devices, host);

				for (int trial = 0; trial < 5; ++trial)
				{
					std::vector<device_info> permuted = devices;
					std::shuffle(permuted.begin(), permuted.end(), rng);
					const auto shuffled = select_device(pref, permuted, host);

					ASSERT_EQ(base.found, shuffled.found);
					EXPECT_EQ(base.chosen, shuffled.chosen);
					EXPECT_EQ(base.chosen_name, shuffled.chosen_name);
					EXPECT_EQ(base.fell_back, shuffled.fell_back);
				}
			}
		}
	}

	TEST(KKRandomized, DedupProperty)
	{
		std::mt19937 rng(0xC0FFEE ^ 0x2u);
		std::uniform_int_distribution<int> count_dist(0, 12);
		std::uniform_int_distribution<int> name_dist(0, 4);
		const char* filenames[] = { "MoltenVK_icd.json", "kosmickrisp_mesa_icd.aarch64.json", "libkosmickrisp_icd.json", "a.json", "b.json" };

		for (int iter = 0; iter < 1000; ++iter)
		{
			const int n = count_dist(rng);
			std::vector<icd_manifest> manifests;

			for (int i = 0; i < n; ++i)
			{
				manifests.push_back(parse_icd_manifest(filenames[name_dist(rng)], R"({ "library_path": "x" })"));
			}

			const auto deduped = dedup_by_filename(manifests);

			// No duplicate filenames survive.
			for (std::size_t i = 0; i < deduped.size(); ++i)
			{
				for (std::size_t j = i + 1; j < deduped.size(); ++j)
				{
					EXPECT_NE(deduped[i].filename, deduped[j].filename);
				}
			}

			EXPECT_LE(deduped.size(), manifests.size());

			// Idempotent.
			EXPECT_EQ(dedup_by_filename(deduped).size(), deduped.size());

			// count_kind matches a manual count.
			for (driver_kind k : { driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::unknown })
			{
				std::size_t manual = 0;
				for (const auto& m : deduped)
				{
					if (m.kind == k) ++manual;
				}
				EXPECT_EQ(count_kind(deduped, k), manual);
			}
		}
	}

	TEST(KKRandomized, LabelUniquenessIsTotal)
	{
		std::mt19937 rng(0xC0FFEE ^ 0x3u);
		std::uniform_int_distribution<int> name_dist(0, 50);

		for (int iter = 0; iter < 300; ++iter)
		{
			const int n = 1 + name_dist(rng) % 20;
			std::set<std::string> labels;
			std::size_t expected = 0;

			for (int i = 0; i < n; ++i)
			{
				const std::string base = "GPU-" + std::to_string(iter) + "-" + std::to_string(i);
				labels.insert(make_adapter_label(base, driver_kind::moltenvk));
				labels.insert(make_adapter_label(base, driver_kind::kosmickrisp));
				expected += 2;
			}

			EXPECT_EQ(labels.size(), expected);
		}
	}

	// -------------------------------------------------------------------------
	// C1/C5 — adapter list construction: dedup by (kind, name), labels, round-trip
	// -------------------------------------------------------------------------

	TEST(KKAdapterList, SingleDriverNamesStayPlainAndOrdered)
	{
		EXPECT_TRUE(build_adapter_labels({}).empty());

		// Bundled + system ICD of the same driver enumerate the same GPU twice.
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 1),
			dev(driver_kind::moltenvk, "Intel Iris", driver_id_moltenvk, 2),
		};

		const auto labels = build_adapter_labels(devices);
		ASSERT_EQ(labels.size(), 2u);
		EXPECT_EQ(labels[0], "Apple M4 Pro"); // one driver owns the name => no tag
		EXPECT_EQ(labels[1], "Intel Iris");
	}

	TEST(KKAdapterList, CoexistingDriversAreTaggedAndUnique)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 1),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 2),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 3),
			dev(driver_kind::other, "llvmpipe", driver_id_mesa_llvmpipe, 4),
		};

		const auto labels = build_adapter_labels(devices);
		ASSERT_EQ(labels.size(), 3u);
		EXPECT_EQ(labels[0], "Apple M4 Pro (MoltenVK)");
		EXPECT_EQ(labels[1], "Apple M4 Pro (KosmicKrisp)");
		EXPECT_EQ(labels[2], "llvmpipe");

		const std::set<std::string> unique(labels.begin(), labels.end());
		EXPECT_EQ(unique.size(), labels.size());
	}

	TEST(KKAdapterList, DedupKeepsFirstOccurrencePerKindAndName)
	{
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "B", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "B", driver_id_mesa_kosmickrisp, 1),
			dev(driver_kind::moltenvk, "A", driver_id_moltenvk, 2),
			dev(driver_kind::moltenvk, "B", driver_id_moltenvk, 3),
		};

		const auto once = dedup_devices(devices);
		ASSERT_EQ(once.size(), 3u);
		EXPECT_EQ(once[0].kind, driver_kind::moltenvk);
		EXPECT_EQ(once[0].name, "B");
		EXPECT_EQ(once[0].index, 0); // first occurrence wins
		EXPECT_EQ(once[1].kind, driver_kind::kosmickrisp);
		EXPECT_EQ(once[2].name, "A");

		EXPECT_EQ(dedup_devices(once).size(), once.size()); // idempotent
	}

	TEST(KKAdapterLabelName, StripsLegacyTagsForMigration)
	{
		// Configs written before the driver split stored a tagged adapter label; the raw
		// device name is recovered so the device still matches now the list is plain.
		EXPECT_EQ(adapter_label_name("Apple M4 Pro (MoltenVK)"), "Apple M4 Pro");
		EXPECT_EQ(adapter_label_name("Apple M4 Pro (KosmicKrisp)"), "Apple M4 Pro");
		EXPECT_EQ(adapter_label_name("Apple M4 Pro (Honeykrisp)"), "Apple M4 Pro");

		// Plain names pass through untouched.
		EXPECT_EQ(adapter_label_name("Apple M4 Pro"), "Apple M4 Pro");
		EXPECT_EQ(adapter_label_name(""), "");
		EXPECT_EQ(adapter_label_name("X (MoltenVK"), "X (MoltenVK"); // not a suffix
	}

	TEST(KKAdapterList, DeviceLabelsNameDevicesOnly)
	{
		EXPECT_TRUE(build_device_labels({}).empty());

		// One plain row per deviceName: MVK and KK exposing the same GPU collapse.
		const std::vector<device_info> devices = {
			dev(driver_kind::moltenvk, "Apple M4 Pro", driver_id_moltenvk, 0),
			dev(driver_kind::kosmickrisp, "Apple M4 Pro", driver_id_mesa_kosmickrisp, 1),
			dev(driver_kind::moltenvk, "Intel Iris", driver_id_moltenvk, 2),
			dev(driver_kind::other, "llvmpipe", driver_id_mesa_llvmpipe, 3),
		};

		const auto labels = build_device_labels(devices);
		ASSERT_EQ(labels.size(), 3u);
		EXPECT_EQ(labels[0], "Apple M4 Pro");
		EXPECT_EQ(labels[1], "Intel Iris");
		EXPECT_EQ(labels[2], "llvmpipe");

		// The driver is a separate setting, so the row resolves under either one.
		for (driver_kind kind : { driver_kind::moltenvk, driver_kind::kosmickrisp })
		{
			const int index = find_adapter_index(devices, kind, labels[0]);
			ASSERT_GE(index, 0);
			EXPECT_EQ(devices[static_cast<std::size_t>(index)].kind, kind);
		}
	}

	TEST(KKPreference, DriverKindMapping)
	{
		EXPECT_EQ(preference_driver_kind(driver_preference::moltenvk), driver_kind::moltenvk);
		EXPECT_EQ(preference_driver_kind(driver_preference::kosmickrisp), driver_kind::kosmickrisp);
		EXPECT_EQ(preference_driver_kind(driver_preference::auto_), driver_kind::unknown);
	}

	TEST(KKRandomized, AdapterListDedupAndUniqueness)
	{
		std::mt19937 rng(0xC0FFEE ^ 0x4u);
		std::uniform_int_distribution<int> kind_dist(0, 4);
		std::uniform_int_distribution<int> count_dist(0, 10);
		std::uniform_int_distribution<int> name_dist(0, 3);
		const char* names[] = { "Apple M4 Pro", "Intel Iris", "", "Apple M1" };
		const driver_kind kinds[] = { driver_kind::unknown, driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other };

		for (int iter = 0; iter < 2000; ++iter)
		{
			const int n = count_dist(rng);
			std::vector<device_info> devices;

			for (int i = 0; i < n; ++i)
			{
				devices.push_back(dev(kinds[kind_dist(rng)], names[name_dist(rng)], 0, i));
			}

			const auto deduped = dedup_devices(devices);

			// No two surviving devices share (kind, name).
			for (std::size_t i = 0; i < deduped.size(); ++i)
			{
				for (std::size_t j = i + 1; j < deduped.size(); ++j)
				{
					EXPECT_FALSE(deduped[i].kind == deduped[j].kind && deduped[i].name == deduped[j].name);
				}
			}

			const auto labels = build_adapter_labels(devices);
			ASSERT_EQ(labels.size(), deduped.size());

			const std::set<std::string> unique(labels.begin(), labels.end());
			EXPECT_EQ(unique.size(), labels.size());

			for (std::size_t i = 0; i < deduped.size(); ++i)
			{
				// Tagged or not, every label carries the raw device name.
				EXPECT_EQ(adapter_label_name(labels[i]), deduped[i].name);
			}

			// The Apple list drops the driver from the label: one entry per distinct name.
			const auto device_labels = build_device_labels(devices);
			const std::set<std::string> device_unique(device_labels.begin(), device_labels.end());
			EXPECT_EQ(device_unique.size(), device_labels.size());

			for (const std::string& label : device_labels)
			{
				EXPECT_EQ(adapter_label_name(label), label);
			}
		}
	}

	// -------------------------------------------------------------------------
	// C5 — bundle-only ICD discovery
	// -------------------------------------------------------------------------

	TEST(KKBundleSearch, IcdDirFromExecutable)
	{
		EXPECT_EQ(bundle_icd_dir_from_executable("/Applications/RPCS3.app/Contents/MacOS/rpcs3"),
			"/Applications/RPCS3.app/Contents/Resources/vulkan/icd.d");
		EXPECT_EQ(bundle_icd_dir_from_executable("/x/y.app/Contents/MacOS/rpcs3"),
			"/x/y.app/Contents/Resources/vulkan/icd.d");

		// Not enough components to be a bundle executable.
		EXPECT_TRUE(bundle_icd_dir_from_executable("rpcs3").empty());
		EXPECT_TRUE(bundle_icd_dir_from_executable("/rpcs3").empty());
		EXPECT_TRUE(bundle_icd_dir_from_executable("").empty());

		// Deterministic and idempotent w.r.t. its own input shape.
		for (int i = 0; i < 50; ++i)
		{
			EXPECT_EQ(bundle_icd_dir_from_executable("/a/b.app/Contents/MacOS/rpcs3"),
				"/a/b.app/Contents/Resources/vulkan/icd.d");
		}
	}

	TEST(KKBundleSearch, ManifestFilenameDetection)
	{
		EXPECT_TRUE(is_icd_manifest_filename("MoltenVK_icd.json"));
		EXPECT_TRUE(is_icd_manifest_filename("libkosmickrisp_icd.json"));
		EXPECT_TRUE(is_icd_manifest_filename("UPPER.JSON"));
		EXPECT_TRUE(is_icd_manifest_filename(".json"));

		EXPECT_FALSE(is_icd_manifest_filename(""));
		EXPECT_FALSE(is_icd_manifest_filename("json"));
		EXPECT_FALSE(is_icd_manifest_filename("manifest.json.bak"));
		EXPECT_FALSE(is_icd_manifest_filename("MoltenVK_icd"));
		EXPECT_FALSE(is_icd_manifest_filename(".."));
	}

	TEST(KKBundleSearch, BundleOnlyDecisionTruthTable)
	{
		// Only when the bundle ships an ICD and the user did not override.
		EXPECT_TRUE(should_enable_bundle_only_search(true, false));
		EXPECT_FALSE(should_enable_bundle_only_search(true, true));   // user wins
		EXPECT_FALSE(should_enable_bundle_only_search(false, false)); // fall back to system ICDs
		EXPECT_FALSE(should_enable_bundle_only_search(false, true));

		// Exhaustive and deterministic.
		for (int i = 0; i < 20; ++i)
		{
			EXPECT_EQ(should_enable_bundle_only_search(true, false), true);
			EXPECT_EQ(should_enable_bundle_only_search(false, false), false);
		}
	}

	TEST(KKBundleSearch, EnvVarNameIsStable)
	{
		EXPECT_STREQ(bundle_only_search_env_var, "VK_LOADER_SEARCH_ONLY_IN_BUNDLE");
	}
}
