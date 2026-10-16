#pragma once

/*
 * vk_driver_selection.h — dependency-free seams for MoltenVK / KosmicKrisp coexistence.
 *
 * This header deliberately contains NO Vulkan headers, NO GPU access and NO mutable
 * globals. It is pure logic over plain data ("seams") that the KosmicKrisp
 * integration in `VK/vkutils/{device,instance,swapchain,chip_class}.cpp`,
 * `VK/VKGSRender.cpp` and `rpcs3qt/render_creator.cpp` consumes.
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
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace vk::driver_selection
{
	// -------------------------------------------------------------------------
	// Driver identity
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
	inline constexpr std::uint32_t driver_id_moltenvk         = 14;
	inline constexpr std::uint32_t driver_id_mesa_honeykrisp  = 26;
	inline constexpr std::uint32_t driver_id_mesa_kosmickrisp = 28;

	// Replaces the hard-coded `#ifdef __APPLE__ return driver_vendor::MVK;`.
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

	// Stable human-readable vendor tag used inside adapter labels.
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

	// The two drivers report the same deviceName; the label must disambiguate.
	[[nodiscard]] inline std::string make_adapter_label(std::string_view device_name, driver_kind kind)
	{
		std::string out(device_name);
		out.append(" (");
		out.append(driver_display_name(kind));
		out.append(")");
		return out;
	}

	// Inverse of make_adapter_label(): recovers the raw deviceName from a label,
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
	// Driver-specific workaround profile (keyed on driver_kind, never __APPLE__)
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
			// Must mirror the Honeykrisp/Apple entries, NOT fall into `default`.
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
	// Host gating
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

	// -------------------------------------------------------------------------
	// Bundle-only ICD discovery
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

	// -------------------------------------------------------------------------
	// Selection policy
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
		// host can actually run it. This is what makes Auto skip KK on <26/x86_64 even
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

	// Selection must never match on deviceName alone. Returns the index of the
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

	// Collapse devices that are indistinguishable to the user. Two physical
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

	// The UI adapter list: exactly one label per distinct (driver_kind, name).
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

	// The Apple adapter list: the driver is chosen by its own setting, so the list
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
