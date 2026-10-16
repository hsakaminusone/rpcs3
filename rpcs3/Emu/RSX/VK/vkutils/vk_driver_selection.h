#pragma once

/*
 * vk_driver_selection.h — dependency-free seams for MoltenVK / KosmicKrisp coexistence.
 *
 * This header deliberately contains NO Vulkan headers, NO GPU access and NO mutable
 * globals. It is pure logic over plain data ("seams") that the future KosmicKrisp
 * integration in `VK/vkutils/{device,instance,swapchain,chip_class}.cpp`,
 * `VK/VKGSRender.cpp` and `rpcs3qt/render_creator.cpp` is expected to consume.
 *
 * See `docs/research/04-backend-selection-design.md` and
 * `docs/research/11-mvk-kk-coexistence.md` (problem catalogue C1..C10) for the spec.
 * Every function below maps back to one of those items; the regression tests in
 * `tests/test_vk_driver_selection.cpp` name them explicitly.
 *
 * Facts encoded here (as of 2026-10-15):
 *   - VkDriverId: MoltenVK = 14, Mesa Honeykrisp = 26, Mesa KosmicKrisp = 28.
 *   - MoltenVK is a portability driver (only enumerates with the portability gate on).
 *   - KosmicKrisp is a pure ICD (does NOT export the public vk* API), arm64 + macOS >= 26.
 *   - Both drivers report the same `deviceName` (e.g. "Apple M4 Pro").
 *   - Physical device order is unspecified on macOS => never select by index.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vk::driver_selection
{
	// -------------------------------------------------------------------------
	// C2/C3 — driver identity
	// -------------------------------------------------------------------------

	enum class driver_kind
	{
		unknown,
		moltenvk,
		kosmickrisp,
		honeykrisp,
		other,
	};

	// VkDriverId raw values (duplicated here so this seam has no Vulkan dependency).
	inline constexpr std::uint32_t driver_id_amd_proprietary         = 1;
	inline constexpr std::uint32_t driver_id_mesa_radv               = 3;
	inline constexpr std::uint32_t driver_id_nvidia_proprietary      = 4;
	inline constexpr std::uint32_t driver_id_mesa_llvmpipe           = 13;
	inline constexpr std::uint32_t driver_id_moltenvk                = 14;
	inline constexpr std::uint32_t driver_id_mesa_dozen              = 23;
	inline constexpr std::uint32_t driver_id_mesa_honeykrisp         = 26;
	inline constexpr std::uint32_t driver_id_vulkan_sc_emulation     = 27;
	inline constexpr std::uint32_t driver_id_mesa_kosmickrisp        = 28;

	// C2 — replaces the hard-coded `#ifdef __APPLE__ return driver_vendor::MVK;`.
	// 0 is the "unset/unknown" driver id (VkDriverId is never 0 for a real driver).
	[[nodiscard]] constexpr driver_kind classify_driver_id(std::uint32_t driver_id) noexcept
	{
		switch (driver_id)
		{
		case driver_id_moltenvk:        return driver_kind::moltenvk;
		case driver_id_mesa_kosmickrisp:return driver_kind::kosmickrisp;
		case driver_id_mesa_honeykrisp: return driver_kind::honeykrisp;
		case 0:                         return driver_kind::unknown;
		default:                        return driver_kind::other;
		}
	}

	// C1 — stable human-readable vendor tag used inside adapter labels.
	[[nodiscard]] constexpr const char* driver_display_name(driver_kind kind) noexcept
	{
		switch (kind)
		{
		case driver_kind::moltenvk:    return "MoltenVK";
		case driver_kind::kosmickrisp: return "KosmicKrisp";
		case driver_kind::honeykrisp:  return "Honeykrisp";
		case driver_kind::other:       return "Vulkan";
		case driver_kind::unknown:     return "Unknown";
		}
		return "Unknown";
	}

	[[nodiscard]] constexpr const char* driver_kind_name(driver_kind kind) noexcept
	{
		switch (kind)
		{
		case driver_kind::moltenvk:    return "moltenvk";
		case driver_kind::kosmickrisp: return "kosmickrisp";
		case driver_kind::honeykrisp:  return "honeykrisp";
		case driver_kind::other:       return "other";
		case driver_kind::unknown:     return "unknown";
		}
		return "unknown";
	}

	// C1 — the two drivers report the same deviceName; the label must disambiguate.
	[[nodiscard]] inline std::string make_adapter_label(std::string_view device_name, driver_kind kind)
	{
		std::string out(device_name);
		out.append(" (");
		out.append(driver_display_name(kind));
		out.append(")");
		return out;
	}

	// C1 — inverse of make_adapter_label(): recovers the raw deviceName from a label,
	// so selection can still match on (driver, name). A plain name is returned as-is.
	[[nodiscard]] inline std::string adapter_label_name(std::string_view label)
	{
		for (const driver_kind kind : { driver_kind::moltenvk, driver_kind::kosmickrisp, driver_kind::honeykrisp, driver_kind::other, driver_kind::unknown })
		{
			const std::string suffix = std::string(" (") + driver_display_name(kind) + ")";

			if (label.size() >= suffix.size() && label.compare(label.size() - suffix.size(), suffix.size(), suffix) == 0)
			{
				return std::string(label.substr(0, label.size() - suffix.size()));
			}
		}

		return std::string(label);
	}

	// -------------------------------------------------------------------------
	// C3 — chip class per driver (replaces `#ifdef __APPLE__ return APPLE_MVK;`)
	// -------------------------------------------------------------------------

	enum class chip_family
	{
		unknown,
		apple_mvk,
		apple_kk,
		apple_hk_generic,
		non_apple,
	};

	inline constexpr std::uint32_t apple_vendor_id = 0x106B;

	[[nodiscard]] constexpr chip_family classify_chip_family(std::uint32_t vendor_id, driver_kind kind) noexcept
	{
		if (vendor_id != apple_vendor_id)
		{
			return chip_family::non_apple;
		}

		switch (kind)
		{
		case driver_kind::moltenvk:    return chip_family::apple_mvk;
		case driver_kind::kosmickrisp: return chip_family::apple_kk;
		case driver_kind::honeykrisp:  return chip_family::apple_hk_generic;
		default:                       return chip_family::apple_hk_generic;
		}
	}

	// -------------------------------------------------------------------------
	// C4 — driver-specific workaround profile (keyed on driver_kind, never __APPLE__)
	// -------------------------------------------------------------------------

	struct workaround_profile
	{
		// VK/VKCompute.cpp
		bool unroll_loops = false;
		std::uint32_t optimal_group_size = 1;

		// VK/VKGSRender.cpp: supports_hw_conditional_render = (vendor != MVK)
		bool supports_hw_conditional_render = true;

		// VK/VKRenderTargets.h: `case MVK:` no-op fallthrough (else "Unknown driver vendor!")
		bool render_target_flags_noop = true;

		// VK/vkutils/swapchain.cpp: `case MVK:` no-op
		bool swapchain_noop = true;

		// MVK-only instance settings (MVK_CONFIG_*); inert/harmful for KK.
		bool apply_mvk_instance_settings = false;

		// MSL-only knob: must only apply to MoltenVK.
		bool disable_msl_fast_math_relevant = false;

		// KosmicKrisp unrolls => primitive-list restart is slow.
		bool allows_primitive_restart = true;

		// KosmicKrisp returns VK_ERROR_OUT_OF_HOST_MEMORY past 1024 unique samplers.
		std::uint32_t max_unique_samplers = 0xFFFFFFFFu;
	};

	[[nodiscard]] constexpr workaround_profile make_workaround_profile(driver_kind kind) noexcept
	{
		workaround_profile p{};

		switch (kind)
		{
		case driver_kind::moltenvk:
			p.unroll_loops = true;
			p.optimal_group_size = 256;
			p.supports_hw_conditional_render = false;
			p.render_target_flags_noop = true;
			p.swapchain_noop = true;
			p.apply_mvk_instance_settings = true;
			p.disable_msl_fast_math_relevant = true;
			p.allows_primitive_restart = true;
			p.max_unique_samplers = 0xFFFFFFFFu;
			break;

		case driver_kind::kosmickrisp:
			// C4: must mirror the Honeykrisp/Apple entries, NOT fall into `default`.
			p.unroll_loops = true;
			p.optimal_group_size = 256;
			p.supports_hw_conditional_render = true;
			p.render_target_flags_noop = true;
			p.swapchain_noop = true;
			p.apply_mvk_instance_settings = false;
			p.disable_msl_fast_math_relevant = false;
			p.allows_primitive_restart = false;
			p.max_unique_samplers = 1024;
			break;

		case driver_kind::honeykrisp:
			p.unroll_loops = true;
			p.optimal_group_size = 256;
			p.supports_hw_conditional_render = true;
			p.render_target_flags_noop = true;
			p.swapchain_noop = true;
			break;

		default:
			break;
		}

		return p;
	}

	// -------------------------------------------------------------------------
	// C5/C7 — ICD manifest parsing + rewriting
	// -------------------------------------------------------------------------

	inline constexpr const char* bundle_frameworks_prefix = "../../../Frameworks/";

	// Minimal, tolerant flat-JSON object parser sufficient for ICD manifests
	// (top-level string / bool / number scalars). Nested objects/arrays are
	// skipped so that unknown keys never break parsing.
	struct json_scalar
	{
		enum class type { string, boolean, number, null_, skipped };

		type kind = type::null_;
		std::string str;
		bool boolean = false;
		double number = 0.0;
		std::string raw; // raw source text of the value (used to descend into nested objects)
	};

	struct json_parse_result
	{
		bool ok = false;
		std::string error;
		std::map<std::string, json_scalar> fields;
	};

	namespace detail
	{
		inline void skip_ws(std::string_view s, std::size_t& i)
		{
			while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
			{
				++i;
			}
		}

		inline bool parse_json_string(std::string_view s, std::size_t& i, std::string& out)
		{
			if (i >= s.size() || s[i] != '"')
			{
				return false;
			}

			++i;
			out.clear();

			while (i < s.size())
			{
				const char c = s[i++];

				if (c == '"')
				{
					return true;
				}

				if (c == '\\')
				{
					if (i >= s.size())
					{
						return false;
					}

					const char e = s[i++];
					switch (e)
					{
					case '"':  out.push_back('"');  break;
					case '\\': out.push_back('\\'); break;
					case '/':  out.push_back('/');  break;
					case 'b':  out.push_back('\b'); break;
					case 'f':  out.push_back('\f'); break;
					case 'n':  out.push_back('\n'); break;
					case 'r':  out.push_back('\r'); break;
					case 't':  out.push_back('\t'); break;
					case 'u':
						// Keep the raw escape; enough for round-tripping non-ASCII paths.
						out.append("\\u");
						for (int k = 0; k < 4 && i < s.size(); ++k)
						{
							out.push_back(s[i++]);
						}
						break;
					default:
						return false;
					}
					continue;
				}

				out.push_back(c);
			}

			return false; // unterminated string
		}

		// Skip a balanced JSON value (object/array/scalar) without interpreting it.
		inline bool skip_json_value(std::string_view s, std::size_t& i, std::string& err)
		{
			skip_ws(s, i);

			if (i >= s.size())
			{
				err = "unexpected end of input";
				return false;
			}

			const char c = s[i];

			if (c == '{' || c == '[')
			{
				const char open = c;
				const char close = (c == '{') ? '}' : ']';
				int depth = 0;

				while (i < s.size())
				{
					const char d = s[i];

					if (d == '"')
					{
						std::string dummy;
						if (!parse_json_string(s, i, dummy))
						{
							err = "unterminated string while skipping container";
							return false;
						}
						continue;
					}

					if (d == open)
					{
						++depth;
					}
					else if (d == close)
					{
						--depth;
						++i;
						if (depth == 0)
						{
							return true;
						}
						continue;
					}
					++i;
				}

				err = "unterminated container";
				return false;
			}

			// scalar until , or }
			while (i < s.size() && s[i] != ',' && s[i] != '}')
			{
				++i;
			}
			return true;
		}
	} // namespace detail

	[[nodiscard]] inline json_parse_result parse_flat_json_object(std::string_view text)
	{
		json_parse_result result;
		std::size_t i = 0;

		// Tolerate a leading UTF-8 BOM (some editors/tools emit one).
		if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
		{
			i = 3;
		}

		detail::skip_ws(text, i);

		if (i >= text.size() || text[i] != '{')
		{
			result.error = "expected '{' at start of object";
			return result;
		}

		++i;
		detail::skip_ws(text, i);

		if (i < text.size() && text[i] == '}')
		{
			++i;
			detail::skip_ws(text, i);

			if (i != text.size())
			{
				result.error = "trailing garbage after object";
				return result;
			}

			result.ok = true;
			return result;
		}

		while (true)
		{
			detail::skip_ws(text, i);

			std::string key;
			if (!detail::parse_json_string(text, i, key))
			{
				result.error = "expected string key";
				return result;
			}

			detail::skip_ws(text, i);

			if (i >= text.size() || text[i] != ':')
			{
				result.error = "expected ':' after key '" + key + "'";
				return result;
			}

			++i;
			detail::skip_ws(text, i);

			if (i >= text.size())
			{
				result.error = "unexpected end of input after key '" + key + "'";
				return result;
			}

			json_scalar value;

			if (text[i] == '"')
			{
				value.kind = json_scalar::type::string;
				const std::size_t start = i;
				if (!detail::parse_json_string(text, i, value.str))
				{
					result.error = "unterminated string value for key '" + key + "'";
					return result;
				}
				value.raw = std::string(text.substr(start, i - start));
			}
			else if (text.compare(i, 4, "true") == 0)
			{
				value.kind = json_scalar::type::boolean;
				value.boolean = true;
				value.raw = "true";
				i += 4;
			}
			else if (text.compare(i, 5, "false") == 0)
			{
				value.kind = json_scalar::type::boolean;
				value.boolean = false;
				value.raw = "false";
				i += 5;
			}
			else if (text.compare(i, 4, "null") == 0)
			{
				value.kind = json_scalar::type::null_;
				value.raw = "null";
				i += 4;
			}
			else if (text[i] == '{' || text[i] == '[')
			{
				value.kind = json_scalar::type::skipped;
				const std::size_t start = i;
				if (!detail::skip_json_value(text, i, result.error))
				{
					return result;
				}
				value.raw = std::string(text.substr(start, i - start));
			}
			else
			{
				// number
				const std::size_t start = i;
				while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '-' || text[i] == '+' || text[i] == '.' || text[i] == 'e' || text[i] == 'E'))
				{
					++i;
				}

				if (i == start)
				{
					result.error = "unexpected character while parsing value for key '" + key + "'";
					return result;
				}

				value.kind = json_scalar::type::number;
				value.raw = std::string(text.substr(start, i - start));
				value.number = std::strtod(value.raw.c_str(), nullptr);
			}

			result.fields[key] = std::move(value);

			detail::skip_ws(text, i);

			if (i >= text.size())
			{
				result.error = "unterminated object";
				return result;
			}

			if (text[i] == ',')
			{
				++i;
				continue;
			}

			if (text[i] == '}')
			{
				++i;
				detail::skip_ws(text, i);

				if (i != text.size())
				{
					result.error = "trailing garbage after object";
					return result;
				}

				result.ok = true;
				return result;
			}

			result.error = "expected ',' or '}'";
			return result;
		}
	}

	// C7 — infer the driver from the ICD manifest filename as seen in the wild:
	//   MoltenVK_icd.json, libkosmickrisp_icd.json, kosmickrisp_mesa_icd.aarch64.json
	[[nodiscard]] inline driver_kind classify_manifest_filename(std::string_view filename)
	{
		std::string lower(filename);
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		if (lower.find("moltenvk") != std::string::npos)
		{
			return driver_kind::moltenvk;
		}

		if (lower.find("kosmickrisp") != std::string::npos)
		{
			return driver_kind::kosmickrisp;
		}

		if (lower.find("honeykrisp") != std::string::npos)
		{
			return driver_kind::honeykrisp;
		}

		return driver_kind::unknown;
	}

	struct icd_manifest
	{
		std::string filename;
		std::string library_path;
		bool has_library_path = false;
		bool has_portability_key = false;
		bool is_portability_driver = false;
		driver_kind kind = driver_kind::unknown;
		std::string error;
	};

	[[nodiscard]] inline icd_manifest parse_icd_manifest(std::string_view filename, std::string_view json)
	{
		icd_manifest out;
		out.filename = std::string(filename);
		out.kind = classify_manifest_filename(filename);

		const auto parsed = parse_flat_json_object(json);

		if (!parsed.ok)
		{
			out.error = parsed.error;
			return out;
		}

		// Real ICD manifests wrap their keys in an "ICD" object; merge those fields in
		// (inner values win) so both flat and canonical layouts parse.
		std::map<std::string, json_scalar> fields = parsed.fields;

		if (const auto it = fields.find("ICD"); it != fields.end() && it->second.kind == json_scalar::type::skipped)
		{
			const auto inner = parse_flat_json_object(it->second.raw);

			if (!inner.ok)
			{
				out.error = "malformed ICD object: " + inner.error;
				return out;
			}

			for (const auto& [k, v] : inner.fields)
			{
				fields[k] = v;
			}
		}

		if (const auto it = fields.find("library_path"); it != fields.end())
		{
			if (it->second.kind != json_scalar::type::string)
			{
				out.error = "library_path is not a string";
				return out;
			}

			out.has_library_path = true;
			out.library_path = it->second.str;
		}
		else
		{
			out.error = "missing required key 'library_path'";
			return out;
		}

		if (const auto it = fields.find("is_portability_driver"); it != fields.end())
		{
			if (it->second.kind != json_scalar::type::boolean)
			{
				out.error = "is_portability_driver is not a boolean";
				return out;
			}

			out.has_portability_key = true;
			out.is_portability_driver = it->second.boolean;
		}

		return out;
	}

	// C7 — rewrite a manifest `library_path` for the bundled layout.
	//   "./libMoltenVK.dylib"                  -> "../../../Frameworks/libMoltenVK.dylib"
	//   "libvulkan_kosmickrisp.dylib"          -> "../../../Frameworks/libvulkan_kosmickrisp.dylib"
	//   "../../../Frameworks/x.dylib"          -> unchanged (already rewritten)
	//   "/abs/x.dylib", "@rpath/x", "@loader_path/x", "" -> unchanged
	[[nodiscard]] inline std::string rewrite_library_path(std::string_view path)
	{
		const std::string prefix = bundle_frameworks_prefix;

		if (path.empty())
		{
			return std::string();
		}

		if (path.front() == '@' || path.front() == '/')
		{
			return std::string(path); // already loadable / loader-relative
		}

		if (path.size() >= 3 && path[0] == '.' && path[1] == '.' && path[2] == '/')
		{
			return std::string(path); // already relative to the manifest directory
		}

		if (path.compare(0, prefix.size(), prefix) == 0)
		{
			return std::string(path); // idempotent
		}

		if (path.size() >= 2 && path[0] == '.' && path[1] == '/')
		{
			return prefix + std::string(path.substr(2));
		}

		// Bare dylib name.
		return prefix + std::string(path);
	}

	// C7 — serialize back, injecting the rewritten library_path.
	[[nodiscard]] inline std::string render_icd_manifest(const icd_manifest& manifest, bool rewrite = true)
	{
		const std::string lib = rewrite ? rewrite_library_path(manifest.library_path) : manifest.library_path;

		std::string out;
		out += "{\n";
		out += "    \"file_format_version\": \"1.0.0\",\n";
		out += "    \"ICD\": {\n";
		out += "        \"library_path\": \"" + lib + "\",\n";
		out += std::string("        \"api_version\": \"1.4.0\",\n");
		out += std::string("        \"is_portability_driver\": ") + (manifest.is_portability_driver ? "true" : "false") + "\n";
		out += "    }\n";
		out += "}\n";
		return out;
	}

	// C5 — the loader searches the app bundle first and ignores system ICDs with the
	// same *manifest filename*. Model that: keep the first occurrence of each filename
	// (bundle entries first), drop later duplicates.
	[[nodiscard]] inline std::vector<icd_manifest> dedup_by_filename(std::vector<icd_manifest> manifests)
	{
		std::vector<icd_manifest> out;

		for (auto& m : manifests)
		{
			const bool dup = std::any_of(out.begin(), out.end(), [&](const icd_manifest& e) { return e.filename == m.filename; });

			if (!dup)
			{
				out.push_back(std::move(m));
			}
		}

		return out;
	}

	// C5 — number of *distinct* manifests for a given driver kind. > 1 means the
	// duplicate-ICD hazard (bundled + system, or two Mesa manifests) is present.
	[[nodiscard]] inline std::size_t count_kind(const std::vector<icd_manifest>& manifests, driver_kind kind)
	{
		return static_cast<std::size_t>(std::count_if(manifests.begin(), manifests.end(), [&](const icd_manifest& m) { return m.kind == kind; }));
	}

	// -------------------------------------------------------------------------
	// C6 — loader requirement / portability gate
	// -------------------------------------------------------------------------

	// KosmicKrisp is a pure ICD and does not export the public vk* API, so shipping it
	// forces a genuine loader to be bundled. MoltenVK also exports vk* (legacy symlink).
	[[nodiscard]] constexpr bool icd_exports_public_vulkan_api(driver_kind kind) noexcept
	{
		return kind == driver_kind::moltenvk;
	}

	[[nodiscard]] constexpr bool needs_separate_loader(driver_kind kind) noexcept
	{
		return kind == driver_kind::kosmickrisp;
	}

	[[nodiscard]] inline bool bundle_needs_separate_loader(const std::vector<driver_kind>& bundled)
	{
		return std::any_of(bundled.begin(), bundled.end(), [](driver_kind k) { return needs_separate_loader(k); });
	}

	// Portability gate: MVK only appears with the portability enumeration extension +
	// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR. Keeping it ON is mandatory for
	// the dual-driver setup (LDP_LOADER_9 makes it safe for KK). Never gate it off.
	[[nodiscard]] constexpr bool must_enable_portability_enumeration() noexcept
	{
		return true;
	}

	// With both ICDs installed, the loader's visible physical devices are:
	//   portability off => { KosmicKrisp }   (MVK hidden)
	//   portability on  => { MoltenVK, KosmicKrisp }
	[[nodiscard]] inline std::vector<driver_kind> enumerated_drivers(bool portability_enabled)
	{
		if (portability_enabled)
		{
			return { driver_kind::moltenvk, driver_kind::kosmickrisp };
		}

		return { driver_kind::kosmickrisp };
	}

	// -------------------------------------------------------------------------
	// C8/C10 — host gating
	// -------------------------------------------------------------------------

	enum class host_arch
	{
		unknown,
		x86_64,
		arm64,
	};

	inline constexpr int kosmickrisp_min_macos_major = 26;

	[[nodiscard]] constexpr bool host_supports_kosmickrisp(int macos_major, host_arch arch) noexcept
	{
		return arch == host_arch::arm64 && macos_major >= kosmickrisp_min_macos_major;
	}

	// C10: the arm64 bundle carries both ICDs; the x86_64 bundle keeps MoltenVK only.
	[[nodiscard]] constexpr bool bundle_should_include(driver_kind kind, host_arch arch) noexcept
	{
		if (kind == driver_kind::kosmickrisp)
		{
			return arch == host_arch::arm64;
		}

		if (kind == driver_kind::moltenvk)
		{
			return arch != host_arch::unknown;
		}

		return false;
	}

	// -------------------------------------------------------------------------
	// C5 — bundle-only ICD discovery
	// -------------------------------------------------------------------------

	// The bundled loader searches `<bundle>/Contents/Resources/vulkan/icd.d` first
	// but then also scans the system ICD paths, so a system MoltenVK/KosmicKrisp
	// install duplicates every physical device. Setting this variable (loader
	// >= 1.4.359, macOS/iOS only) restricts discovery to the bundle. It must only
	// be set when the bundle actually ships an ICD manifest, otherwise the system
	// drivers have to stay reachable.
	inline constexpr const char* bundle_only_search_env_var = "VK_LOADER_SEARCH_ONLY_IN_BUNDLE";

	// `<bundle>/Contents/Resources/vulkan/icd.d` from an executable at
	// `<bundle>/Contents/MacOS/rpcs3`. Empty when the path is not bundle-shaped.
	[[nodiscard]] inline std::string bundle_icd_dir_from_executable(std::string_view executable_path)
	{
		const std::size_t macos_slash = executable_path.find_last_of('/');

		if (macos_slash == std::string_view::npos)
		{
			return {};
		}

		const std::string_view macos_dir = executable_path.substr(0, macos_slash);
		const std::size_t contents_slash = macos_dir.find_last_of('/');

		if (contents_slash == std::string_view::npos)
		{
			return {};
		}

		std::string out(macos_dir.substr(0, contents_slash));
		out.append("/Resources/vulkan/icd.d");
		return out;
	}

	[[nodiscard]] inline bool is_icd_manifest_filename(std::string_view filename) noexcept
	{
		constexpr std::string_view extension = ".json";

		if (filename.size() < extension.size())
		{
			return false;
		}

		const std::string_view tail = filename.substr(filename.size() - extension.size());

		return std::equal(extension.begin(), extension.end(), tail.begin(), [](char a, char b)
		{
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
	}

	// An explicit user setting always wins (setenv(..., overwrite = 0)).
	[[nodiscard]] constexpr bool should_enable_bundle_only_search(bool bundle_has_icd_manifests, bool user_override_present) noexcept
	{
		return bundle_has_icd_manifests && !user_override_present;
	}

	// C9 — RPCS3 requests VK_API_VERSION_1_2; a higher driver version is fine.
	// Vulkan compares major.minor for instance compatibility and ignores the patch
	// field (low 12 bits), so a driver exposing 1.4 satisfies a 1.2 request.
	[[nodiscard]] constexpr bool driver_api_version_compatible(std::uint32_t requested, std::uint32_t exposed) noexcept
	{
		return (exposed >> 12) >= (requested >> 12);
	}

	// -------------------------------------------------------------------------
	// Selection policy (doc 04)
	// -------------------------------------------------------------------------

	enum class driver_preference
	{
		auto_,
		moltenvk,
		kosmickrisp,
	};

	[[nodiscard]] constexpr const char* preference_name(driver_preference pref) noexcept
	{
		switch (pref)
		{
		case driver_preference::auto_:        return "Auto";
		case driver_preference::moltenvk:     return "MoltenVK";
		case driver_preference::kosmickrisp:  return "KosmicKrisp";
		}
		return "Auto";
	}

	[[nodiscard]] constexpr driver_kind preference_driver_kind(driver_preference pref) noexcept
	{
		switch (pref)
		{
		case driver_preference::moltenvk:     return driver_kind::moltenvk;
		case driver_preference::kosmickrisp:  return driver_kind::kosmickrisp;
		case driver_preference::auto_:        break;
		}
		return driver_kind::unknown;
	}

	struct host_info
	{
		int macos_major = 0; // 0 == unknown
		host_arch arch = host_arch::unknown;
	};

	struct device_info
	{
		driver_kind kind = driver_kind::unknown;
		std::string name;
		std::uint32_t driver_id = 0;
		int index = -1; // loader enumeration order; informational ONLY, never used to select
	};

	struct selection_result
	{
		bool found = false;
		int chosen_index = -1;
		driver_kind chosen = driver_kind::unknown;
		std::string chosen_name;
		bool fell_back = false;
		std::string diagnostic;
	};

	namespace detail
	{
		// A driver is "available" iff a device of that kind enumerated AND (for KK) the
		// host can actually run it. This is what makes Auto skip KK on <27/x86_64 even
		// if a bogus device is present.
		[[nodiscard]] inline bool device_present(const std::vector<device_info>& devices, driver_kind kind)
		{
			return std::any_of(devices.begin(), devices.end(), [&](const device_info& d) { return d.kind == kind; });
		}

		[[nodiscard]] inline bool kind_available(driver_kind kind, const host_info& host, const std::vector<device_info>& devices)
		{
			switch (kind)
			{
			case driver_kind::moltenvk:
				return device_present(devices, driver_kind::moltenvk);
			case driver_kind::kosmickrisp:
				return device_present(devices, driver_kind::kosmickrisp) && host_supports_kosmickrisp(host.macos_major, host.arch);
			default:
				return false;
			}
		}

		// Canonical, order-independent pick among devices of `kind`: smallest (name,
		// driver_id). Duplicates with identical name+id are interchangeable.
		[[nodiscard]] inline int pick_device(const std::vector<device_info>& devices, driver_kind kind, const host_info& host)
		{
			if (kind == driver_kind::kosmickrisp && !host_supports_kosmickrisp(host.macos_major, host.arch))
			{
				return -1;
			}

			int best = -1;

			for (int i = 0; i < static_cast<int>(devices.size()); ++i)
			{
				const device_info& d = devices[static_cast<std::size_t>(i)];

				if (d.kind != kind)
				{
					continue;
				}

				if (best < 0)
				{
					best = i;
					continue;
				}

				const device_info& b = devices[static_cast<std::size_t>(best)];

				if (d.name < b.name || (d.name == b.name && d.driver_id < b.driver_id))
				{
					best = i;
				}
			}

			return best;
		}
	} // namespace detail

	// Explicit preference, resolved with documented fallback. Never throws.
	[[nodiscard]] inline selection_result select_device(driver_preference pref, const std::vector<device_info>& devices, const host_info& host)
	{
		selection_result result;

		auto choose = [&](int idx, bool fell_back, std::string diagnostic)
		{
			result.found = true;
			result.chosen_index = idx;
			result.chosen = devices[static_cast<std::size_t>(idx)].kind;
			result.chosen_name = devices[static_cast<std::size_t>(idx)].name;
			result.fell_back = fell_back;
			result.diagnostic = std::move(diagnostic);
		};

		const bool mvk_ok = detail::kind_available(driver_kind::moltenvk, host, devices);
		const bool kk_ok = detail::kind_available(driver_kind::kosmickrisp, host, devices);

		driver_kind primary = driver_kind::unknown;
		driver_kind secondary = driver_kind::unknown;

		switch (pref)
		{
		case driver_preference::auto_:
			// MoltenVK is the default on every host; KosmicKrisp is opt-in.
			primary = mvk_ok ? driver_kind::moltenvk : (kk_ok ? driver_kind::kosmickrisp : driver_kind::unknown);
			break;
		case driver_preference::moltenvk:
			primary = driver_kind::moltenvk;
			secondary = driver_kind::kosmickrisp;
			break;
		case driver_preference::kosmickrisp:
			primary = driver_kind::kosmickrisp;
			secondary = driver_kind::moltenvk;
			break;
		}

		if (primary != driver_kind::unknown && detail::kind_available(primary, host, devices))
		{
			choose(detail::pick_device(devices, primary, host), false, {});
			return result;
		}

		if (secondary != driver_kind::unknown && detail::kind_available(secondary, host, devices))
		{
			choose(detail::pick_device(devices, secondary, host), true,
				std::string(preference_name(pref)) + " requested but unavailable; falling back to " + driver_display_name(secondary));
			return result;
		}

		result.diagnostic = std::string(preference_name(pref)) + ": no compatible Vulkan driver found";
		return result;
	}

	// C1 — selection must never match on deviceName alone. Returns the index of the
	// first device matching (kind, name), or -1.
	[[nodiscard]] inline int find_adapter_index(const std::vector<device_info>& devices, driver_kind kind, std::string_view name)
	{
		for (int i = 0; i < static_cast<int>(devices.size()); ++i)
		{
			const device_info& d = devices[static_cast<std::size_t>(i)];

			if (d.kind == kind && d.name == name)
			{
				return i;
			}
		}

		return -1;
	}

	// C5 — collapse devices that are indistinguishable to the user. Two physical
	// devices enumerated under the same (driver_kind, name) are one GPU seen through
	// two ICD manifests (bundled + system), so keep the first occurrence.
	[[nodiscard]] inline std::vector<device_info> dedup_devices(const std::vector<device_info>& devices)
	{
		std::vector<device_info> out;
		out.reserve(devices.size());

		for (const device_info& d : devices)
		{
			const bool dup = std::any_of(out.begin(), out.end(), [&](const device_info& e) { return e.kind == d.kind && e.name == d.name; });

			if (!dup)
			{
				out.push_back(d);
			}
		}

		return out;
	}

	// C1 — the UI adapter list: exactly one label per distinct (driver_kind, name).
	// A name owned by more than one driver gets the vendor suffix so the entries stay
	// unambiguous; a name owned by a single driver keeps its plain name.
	[[nodiscard]] inline std::vector<std::string> build_adapter_labels(const std::vector<device_info>& devices)
	{
		const std::vector<device_info> unique = dedup_devices(devices);

		std::map<std::string, int> name_counts;

		for (const device_info& d : unique)
		{
			name_counts[d.name]++;
		}

		std::vector<std::string> labels;
		labels.reserve(unique.size());

		for (const device_info& d : unique)
		{
			labels.push_back(name_counts[d.name] > 1 ? make_adapter_label(d.name, d.kind) : d.name);
		}

		return labels;
	}

	// C6 — the Apple adapter list: the driver is chosen by its own setting, so the list
	// names devices only. Every distinct deviceName yields exactly one plain label,
	// regardless of how many drivers expose it. Non-Apple keeps build_adapter_labels().
	[[nodiscard]] inline std::vector<std::string> build_device_labels(const std::vector<device_info>& devices)
	{
		std::vector<std::string> labels;
		labels.reserve(devices.size());

		for (const device_info& d : devices)
		{
			if (std::find(labels.begin(), labels.end(), d.name) == labels.end())
			{
				labels.push_back(d.name);
			}
		}

		return labels;
	}

} // namespace vk::driver_selection
