#pragma once

// TODO(excise): this file is temporary.  Removing the diagnostics means deleting
// it and every call site that names MTL_NOTE, MTL_WARN, MTL_ERROR, MTL_NOTE_DUMP,
// MTL_PERF_* or MTL_PROBE_*.  The units that carry call sites are:
// MTLCommandRecorder.cpp, MTLDevice.cpp, MTLFill.cpp, MTLFramebuffer.cpp,
// MTLGSRender.cpp, MTLMetalLayer.mm, MTLOverlays.cpp, MTLPassScheduler.cpp,
// MTLPresent.cpp, MTLPresenter.cpp, MTLProgramPipeline.cpp, MTLResourceManager.cpp,
// MTLSurface.cpp and MTLTextureCache.cpp.

/*
 * The graded diagnostic facility of the Metal 4 backend.  One runtime switch and
 * two compile switches control every probe of the backend.
 *
 * One runtime switch.  MTL_PROBE is the level:
 *   - unset, empty or "0": nothing runs, nothing is formatted.
 *   - "1": the counters, the aggregate report and the mandatory lines.  Level 1
 *     is the performance level and it prints no per-event text.
 *   - "2": levels 1 and 2.  Level 2 adds the coverage latches, the histograms and
 *     the coverage table.
 *   - "3": levels 1 to 3.  Level 3 adds the per-event verbose log, and the MTL
 *     channel rises to the trace level.
 *   - "4": levels 1 to 4.  Level 4 adds the stall probe, the layer walk, the
 *     heartbeat, the residency churn and the display-latency stamps.
 *   - "5": every level.  Level 5 adds every dump, every shader source and every
 *     byte block.
 * A number above 5 clamps to 5.  A value that is not a number turns the facility
 * off and prints one warning.  MTL_PROBE_INTERVAL (frames between reports, default
 * 300) and MTL_PROBE_STALL_MS (acquire stall threshold in ms, default 16) tune two
 * probes; both are read once.  MTL_PROBE_AT_SEC (default off) arms one level-5
 * capture window after that many seconds, and MTL_PROBE_FRAME_COUNT (default 1)
 * is its length in frames.  The window is how a run reaches the draw state of a
 * specific menu, video or race without keeping a bounded prefix of the whole run.
 *
 * Two compile switches remove the facility.  MTL_DEBUG_LOGS=0 removes the text
 * family.  MTL_PERF_ENABLED=0 removes the numeric family.  MTL_DIAGNOSTICS=0 forces
 * both off in one place.  A build with both off carries no code, no string and no
 * cost.
 *
 * The macros.  The level is the first token and it is a literal.
 *   Text:     MTL_NOTE(level, ...), MTL_WARN(level, ...), MTL_ERROR(level, ...),
 *             MTL_NOTE_DUMP(level, expr).
 *   Numbers:  MTL_PERF_SCOPE(level, name), MTL_PERF_COUNT(level, name, value),
 *             MTL_PERF_GAUGE(level, name, value), MTL_PERF_FRAME(level),
 *             MTL_PERF_NOTE_ACQUIRED(level), MTL_PERF_NOTE_PRESENTED(level),
 *             MTL_PERF_REPORT(level).
 *   Coverage: MTL_PROBE_TOTAL(level, name, value), MTL_PROBE_LATCH(level, name),
 *             MTL_PROBE_HISTOGRAM(level, family, key, key_name),
 *             MTL_PROBE_DUMP(level).
 *
 * A block that does more than log is gated with ::mtl::probe::at_least(level).  The
 * coarse numbers RPCS3's own overlay shows come from m_frame_stats and stay outside
 * this facility.
 *
 * Where the text goes.  The bulk text (the perf report, the coverage table and the
 * verbose trace family) goes to the probe's own file, ::mtl::probe::sink(), so a
 * deep run can be read without the emulator's HLE channels on top; the banner names
 * the path.  A warning and an error stay in the shared MTL log, because each
 * carries a decision.
 */

#include "util/logs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

// The verbose text family.  A build that does not want it sets this to 0.
#ifndef MTL_DEBUG_LOGS
#define MTL_DEBUG_LOGS 1
#endif

// The numeric family.  A build that does not want it sets this to 0.
#ifndef MTL_PERF_ENABLED
#define MTL_PERF_ENABLED 1
#endif

// The umbrella.  A pull-request build sets MTL_DIAGNOSTICS=0, which forces both
// halves off in one place.
#ifndef MTL_DIAGNOSTICS
#define MTL_DIAGNOSTICS (MTL_DEBUG_LOGS || MTL_PERF_ENABLED)
#endif

// Frames between two performance reports, while the counters are on.
#ifndef MTL_PROBE_INTERVAL
#define MTL_PROBE_INTERVAL 300
#endif

// Acquire stall threshold in milliseconds, while the deep probe is on.
#ifndef MTL_PROBE_STALL_MS
#define MTL_PROBE_STALL_MS 16.0
#endif

// The one runtime switch.  Every probe of the backend reads it, so a run with
// MTL_PROBE unset formats nothing and pays one cached flag load per call site.
namespace mtl::probe
{
	// The raw text of the switch, empty when it is unset.
	inline const char* value()
	{
		static const char* const text = std::getenv("MTL_PROBE");
		return text ? text : "";
	}

	// The level: 0 when the switch is off, 1 through 5 when it is a level.  A junk
	// value is off and prints one warning.  A value above 5 clamps to 5.  The value
	// is read once, because the switch is an environment variable.
	inline int level()
	{
		static const int cached = [] {
			const char* text = value();

			if (!text[0] || std::strcmp(text, "0") == 0)
			{
				return 0;
			}

			char* end = nullptr;
			const long parsed = std::strtol(text, &end, 10);

			if (end == text || *end != '\0')
			{
				mtl_log.warning("Metal 4 probe: MTL_PROBE=%s is not a level; the probes stay off", text);
				return 0;
			}

			if (parsed < 1)
			{
				return 0;
			}

			return parsed > 5 ? 5 : static_cast<int>(parsed);
		}();

		return cached;
	}

	// The one level gate.  The level is cumulative, so `at_least(n)` is true when
	// the level is n or higher.
	inline bool at_least(int need)
	{
		return level() >= need;
	}

	// The clock the diagnostics stamp, in nanoseconds.  One home, so the text and
	// the numeric halves agree.
	inline u64 now_ns()
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// A 64-bit FNV-1a hash of a byte range, which the coverage histograms use as a
	// stable key.
	inline u64 fnv1a(const void* data, usz size)
	{
		const u8* bytes = static_cast<const u8*>(data);
		u64 hash = 0xcbf29ce484222325ull;

		for (usz index = 0; index < size; ++index)
		{
			hash = (hash ^ bytes[index]) * 0x100000001b3ull;
		}

		return hash;
	}

	// Frames between two reports.  The value is read once.
	inline u64 interval()
	{
		static const u64 cached = [] {
			const char* text = std::getenv("MTL_PROBE_INTERVAL");
			const u64 parsed = text ? std::strtoull(text, nullptr, 10) : 0;
			return parsed ? parsed : static_cast<u64>(MTL_PROBE_INTERVAL);
		}();

		return cached;
	}

	// The acquire stall threshold in milliseconds.  The value is read once.
	inline f64 stall_ms()
	{
		static const f64 cached = [] {
			const char* text = std::getenv("MTL_PROBE_STALL_MS");
			const f64 parsed = text ? std::strtod(text, nullptr) : 0.0;
			return parsed > 0.0 ? parsed : static_cast<f64>(MTL_PROBE_STALL_MS);
		}();

		return cached;
	}

	// The optional deep-capture trigger.  MTL_PROBE_AT_SEC starts one capture
	// window after that many seconds, and MTL_PROBE_FRAME_COUNT is how many
	// consecutive frames the window covers (default one).  An unset or negative
	// value leaves the window off.  The start is the first probe call, so the
	// threshold is relative to the backend coming up, not to process creation.
	inline f64 elapsed_seconds()
	{
		static const auto start = std::chrono::steady_clock::now();
		return std::chrono::duration<f64>(std::chrono::steady_clock::now() - start).count();
	}

	// The probe's own file.  The bulk output (the perf report, the coverage table
	// and the verbose trace family) goes here rather than into the shared log, so a
	// level-3 or level-5 run can be read without the emulator's HLE channels on
	// top.  A warning and an error stay in the shared log, because each carries a
	// decision.  The file opens on the first line, truncating, and every line is
	// stamped with the seconds since the probe came up.  This whole class belongs to
	// the temporary facility, so it goes when the file goes.
	class file_sink
	{
	public:
		static const char* path()
		{
			static const std::string value = [] {
				const char* tmp = std::getenv("TMPDIR");
				std::string dir = (tmp && *tmp) ? tmp : "/tmp";

				if (dir.empty() || dir.back() != '/')
				{
					dir += '/';
				}

				return dir + "rpcs3-mtl-probe.log";
			}();

			return value.c_str();
		}

		void write(const char* format, ...)
		{
			std::lock_guard lock(m_guard);
			std::FILE* file = ensure_open();

			if (!file)
			{
				return;
			}

			std::fprintf(file, "[%9.3f] ", elapsed_seconds());

			va_list args;
			va_start(args, format);
			std::vfprintf(file, format, args);
			va_end(args);

			std::fputc('\n', file);
			std::fflush(file);
		}

	private:
		// The caller holds the guard.
		std::FILE* ensure_open()
		{
			if (!m_opened)
			{
				m_opened = true;
				m_file = std::fopen(path(), "w");
			}

			return m_file;
		}

		std::mutex m_guard;
		std::FILE* m_file = nullptr;
		bool m_opened = false;
	};

	inline file_sink& sink()
	{
		static file_sink value;
		return value;
	}

	inline f64 capture_after_seconds()
	{
		static const f64 cached = [] {
			const char* text = std::getenv("MTL_PROBE_AT_SEC");
			const f64 parsed = text ? std::strtod(text, nullptr) : -1.0;
			return parsed >= 0.0 ? parsed : -1.0;
		}();

		return cached;
	}

	inline u64 capture_frame_count()
	{
		static const u64 cached = [] {
			const char* text = std::getenv("MTL_PROBE_FRAME_COUNT");
			const u64 parsed = text ? std::strtoull(text, nullptr, 10) : 1;
			return parsed ? parsed : 1;
		}();

		return cached;
	}

	// The anti-flood limits.  A text site prints at most `site_line_cap` lines by
	// itself in a run, every text site shares the interval budget, and the whole run
	// shares the run budget.  When a limit is reached the site stops printing and
	// counts instead, so no probe can flood a log.  All limits are compile constants,
	// not switches.
	constexpr u32 site_line_cap = 4096;
	constexpr s64 interval_line_budget = 40000;
	constexpr s64 run_line_budget = 200000;

	inline std::atomic<s64>& interval_budget()
	{
		static std::atomic<s64> value{interval_line_budget};
		return value;
	}

	inline std::atomic<s64>& run_budget()
	{
		static std::atomic<s64> value{run_line_budget};
		return value;
	}

	inline std::atomic<u64>& suppressed_lines()
	{
		static std::atomic<u64> value{0};
		return value;
	}

	// Refills the interval budget.  The report interval calls this.
	inline void refill_budget()
	{
		interval_budget().store(interval_line_budget, std::memory_order_relaxed);
	}

	// True while this line is inside the per-site cap, the interval budget and the
	// run budget.  `site_counter` belongs to one call site.  The first line a site
	// refuses states the cap, and the site then counts only.
	inline bool allow_line(std::atomic<u32>& site_counter)
	{
		const u32 line = site_counter.fetch_add(1, std::memory_order_relaxed);

		if (line >= site_line_cap)
		{
			suppressed_lines().fetch_add(1, std::memory_order_relaxed);

			if (line == site_line_cap)
			{
				mtl_log.warning("Metal 4 probe: a site reached its cap of %u lines; later lines of that site are counted only", site_line_cap);
			}

			return false;
		}

		if (interval_budget().fetch_sub(1, std::memory_order_relaxed) <= 0)
		{
			suppressed_lines().fetch_add(1, std::memory_order_relaxed);
			return false;
		}

		if (run_budget().fetch_sub(1, std::memory_order_relaxed) <= 0)
		{
			suppressed_lines().fetch_add(1, std::memory_order_relaxed);

			static std::once_flag once;
			std::call_once(once, []
			{
				mtl_log.warning("Metal 4 probe: the run reached its budget of %lld lines; later lines are counted only",
					static_cast<long long>(run_line_budget));
			});

			return false;
		}

		return true;
	}

	// Prints the boot banner for every level.  The bulk text needs no channel,
	// because it goes to ::mtl::probe::sink(); this reports the level and the path.
	// Called once, from the device the probes measure.
	inline void apply_log_level()
	{
		if (level() < 1)
		{
			return;
		}

		mtl_log.notice("Metal 4 probe level %d, interval %llu frames, stall threshold %.1f ms",
			level(), static_cast<unsigned long long>(interval()), stall_ms());
		mtl_log.notice("Metal 4 probe: the report, the coverage table and the verbose lines go to %s", sink().path());
	}
}

#if MTL_PERF_ENABLED

// Coverage.  A latch answers "did the guest ask for this at all": it counts every
// event, never resets, and the whole table prints every tenth interval and at the
// end of the renderer.  A histogram counts a bounded key space (a topology, a
// format, a sample count) and prints the same way.  The table is the answer to
// "which RSX features does this title use".  The counters sit under MTL_PERF_ENABLED
// and the printed table sits under MTL_DEBUG_LOGS.
namespace mtl::probe
{
	// A full table returns zero for a new name, so the call site then costs nothing.
	// The first overflow prints one warning, so a dropped category is not silent.
	inline void warn_overflow(const char* table)
	{
		static std::once_flag once;

		std::call_once(once, [table] {
			mtl_log.warning("Metal 4 coverage: the %s table is full; later keys are dropped", table);
		});
	}

	struct latch_entry
	{
		const char* name = nullptr;
		std::atomic<u64> count{0};
	};

	constexpr u32 latch_capacity = 512;

	inline std::array<latch_entry, latch_capacity>& latches()
	{
		static std::array<latch_entry, latch_capacity> value{};
		return value;
	}

	// The id of one latch, registered once per call site.
	inline u32 latch_id(const char* name)
	{
		static std::mutex guard;
		static u32 next = 0;

		std::lock_guard lock(guard);

		for (u32 i = 0; i < next; ++i)
		{
			if (latches()[i].name == name || std::strcmp(latches()[i].name, name) == 0)
			{
				return i + 1;
			}
		}

		if (next == latch_capacity)
		{
			warn_overflow("latch");
			return 0;
		}

		latches()[next].name = name;
		return ++next;
	}

	// `value` events on one latch.  The total is cumulative for the whole run.
	inline void note(u32 id, u64 value)
	{
		if (id)
		{
			latches()[id - 1].count.fetch_add(value, std::memory_order_relaxed);
		}
	}

	struct histogram_entry
	{
		const char* family = nullptr;
		const char* key_name = nullptr;
		u64 key = 0;
		std::atomic<u64> count{0};
	};

	constexpr u32 histogram_capacity = 512;

	inline std::array<histogram_entry, histogram_capacity>& histograms()
	{
		static std::array<histogram_entry, histogram_capacity> value{};
		return value;
	}

	// The id of one key of one family, registered on its first sight.  The caller
	// caches the id, so the search runs only when the key changes.
	inline u32 histogram_id(const char* family, u64 key, const char* key_name)
	{
		static std::mutex guard;
		static u32 next = 0;

		std::lock_guard lock(guard);

		for (u32 i = 0; i < next; ++i)
		{
			if (histograms()[i].key == key &&
				(histograms()[i].family == family || std::strcmp(histograms()[i].family, family) == 0))
			{
				return i + 1;
			}
		}

		if (next == histogram_capacity)
		{
			warn_overflow("histogram");
			return 0;
		}

		histograms()[next].family = family;
		histograms()[next].key = key;
		histograms()[next].key_name = key_name;
		return ++next;
	}

	inline void add_histogram(u32 id, u64 value)
	{
		if (id)
		{
			histograms()[id - 1].count.fetch_add(value, std::memory_order_relaxed);
		}
	}

	// Prints every latch that fired, by count, then every histogram family with its
	// keys, by count.  A row that never fired is not printed, so the table is the
	// answer to "what did this title use".
	inline void report_coverage(u64 frames_now)
	{
#if MTL_DEBUG_LOGS
		static std::mutex guard;
		std::lock_guard lock(guard);

		struct row
		{
			const char* name;
			u64 count;
			u64 key = 0;
		};

		std::vector<row> latch_rows;
		std::vector<const char*> families;
		u32 keys = 0;

		for (const latch_entry& value : latches())
		{
			const u64 count = value.name ? value.count.load(std::memory_order_relaxed) : 0;

			if (count)
			{
				latch_rows.push_back({value.name, count});
			}
		}

		for (const histogram_entry& value : histograms())
		{
			if (!value.family)
			{
				continue;
			}

			if (value.count.load(std::memory_order_relaxed))
			{
				keys++;
			}

			bool seen = false;

			for (const char* family : families)
			{
				if (family == value.family || std::strcmp(family, value.family) == 0)
				{
					seen = true;
					break;
				}
			}

			if (!seen)
			{
				families.emplace_back(value.family);
			}
		}

		std::sort(latch_rows.begin(), latch_rows.end(), [](const row& left, const row& right) { return left.count > right.count; });

		sink().write("Metal 4 coverage: begin, %zu latches and %u histogram keys fired over %llu frames",
			latch_rows.size(), keys, static_cast<unsigned long long>(frames_now));

		for (const row& value : latch_rows)
		{
			sink().write("Metal 4 coverage: latch %12llu  %s", static_cast<unsigned long long>(value.count), value.name);
		}

		for (const char* family : families)
		{
			std::vector<row> family_rows;

			for (const histogram_entry& value : histograms())
			{
				if (!value.family || !(value.family == family || std::strcmp(value.family, family) == 0))
				{
					continue;
				}

				const u64 count = value.count.load(std::memory_order_relaxed);

				if (count)
				{
					family_rows.push_back({value.key_name, count, value.key});
				}
			}

			std::sort(family_rows.begin(), family_rows.end(), [](const row& left, const row& right) { return left.count > right.count; });

			sink().write("Metal 4 coverage: family %s", family);

			for (const row& value : family_rows)
			{
				sink().write("Metal 4 coverage:   %12llu  %s (key %llu)", static_cast<unsigned long long>(value.count),
					value.name, static_cast<unsigned long long>(value.key));
			}
		}

		sink().write("Metal 4 coverage: end");
#else
		(void)frames_now;
#endif
	}
}

namespace mtl::perf
{
	// One row of the table.  The name and kind are fixed by the first call from a
	// call site, so the search for it runs once per site and not once per sample.
	struct slot
	{
		const char* name = nullptr;
		u8 kind = 0; // 0 time, 1 count, 2 gauge
		std::atomic<u64> samples{0};
		std::atomic<u64> total{0};
		std::atomic<u64> max{0};
		std::atomic<u64> min{std::numeric_limits<u64>::max()};
	};

	constexpr u32 slot_count = 2048;

	inline std::array<slot, slot_count>& slots()
	{
		static std::array<slot, slot_count> value{};
		return value;
	}

	// Ids are one-based, and 0 means "not measured".  The kind is fixed by the
	// first call from a call site.  A full table returns zero, and the site then
	// costs nothing.
	inline u32 slot_id(const char* name, u8 kind)
	{
		static std::mutex guard;
		static u32 next = 0;

		std::lock_guard lock(guard);

		for (u32 i = 0; i < next; ++i)
		{
			if (slots()[i].name == name || std::strcmp(slots()[i].name, name) == 0)
			{
				return i + 1;
			}
		}

		if (next == slot_count)
		{
			probe::warn_overflow("perf slot");
			return 0;
		}

		slots()[next].name = name;
		slots()[next].kind = kind;
		return ++next;
	}

	inline void update_max(std::atomic<u64>& target, u64 value)
	{
		u64 seen = target.load(std::memory_order_relaxed);
		while (value > seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
		{
		}
	}

	inline void update_min(std::atomic<u64>& target, u64 value)
	{
		u64 seen = target.load(std::memory_order_relaxed);
		while (value < seen && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
		{
		}
	}

	// A time sample, in nanoseconds.
	inline void add_time(u32 id, u64 duration_ns)
	{
		if (!id)
		{
			return;
		}

		slot& value = slots()[id - 1];
		value.samples.fetch_add(1, std::memory_order_relaxed);
		value.total.fetch_add(duration_ns, std::memory_order_relaxed);
		update_max(value.max, duration_ns);
	}

	// A count sample: `value_count` is added to the running total for the scope.
	inline void add_count(u32 id, u64 value_count)
	{
		if (!id)
		{
			return;
		}

		slot& value = slots()[id - 1];
		value.samples.fetch_add(1, std::memory_order_relaxed);
		value.total.fetch_add(value_count, std::memory_order_relaxed);
		update_max(value.max, value_count);
	}

	// A gauge sample: the instantaneous `value_now` is kept with its min and max.
	inline void add_gauge(u32 id, u64 value_now)
	{
		if (!id)
		{
			return;
		}

		slot& value = slots()[id - 1];
		value.samples.fetch_add(1, std::memory_order_relaxed);
		value.total.fetch_add(value_now, std::memory_order_relaxed);
		update_max(value.max, value_now);
		update_min(value.min, value_now);
	}

	// The scope of one measured block.  The macro resolves the id to zero below the
	// level, so a scope that is not measured reads no clock.
	struct scope
	{
		const u32 id;
		const u64 start;

		explicit scope(u32 value) : id(value), start(id ? probe::now_ns() : 0) {}

		~scope()
		{
			if (id)
			{
				add_time(id, probe::now_ns() - start);
			}
		}
	};

	inline std::atomic<u64>& frames()
	{
		static std::atomic<u64> value{0};
		return value;
	}

	inline std::atomic<u64>& frames_reported()
	{
		static std::atomic<u64> value{0};
		return value;
	}

	// The drawables the display has not yet shown, which is the depth of the pool
	// the acquire waits on.
	inline std::atomic<u64>& drawables_acquired()
	{
		static std::atomic<u64> value{0};
		return value;
	}

	inline std::atomic<u64>& drawables_presented()
	{
		static std::atomic<u64> value{0};
		return value;
	}

	inline void note_drawable_acquired()
	{
		if (probe::at_least(1))
		{
			drawables_acquired().fetch_add(1, std::memory_order_relaxed);
		}
	}

	inline void note_drawable_presented()
	{
		if (probe::at_least(1))
		{
			drawables_presented().fetch_add(1, std::memory_order_relaxed);
		}
	}

	inline u64 drawables_in_flight()
	{
		return drawables_acquired().load(std::memory_order_relaxed) - drawables_presented().load(std::memory_order_relaxed);
	}

	// Prints the three groups, sorted by total, and starts a new interval.
	inline void report()
	{
		if (!probe::at_least(1))
		{
			return;
		}

		static std::mutex guard;
		std::lock_guard lock(guard);

		const u64 total_frames = frames().load(std::memory_order_relaxed);
		const u64 interval_frames = total_frames - frames_reported().exchange(total_frames, std::memory_order_relaxed);

		struct row
		{
			const char* name;
			u64 samples;
			u64 total;
			u64 max;
			u64 min;
			u8 kind;
		};

		std::vector<row> rows;

		for (slot& value : slots())
		{
			if (!value.name)
			{
				continue;
			}

			const u64 samples = value.samples.exchange(0, std::memory_order_relaxed);
			const u64 total = value.total.exchange(0, std::memory_order_relaxed);
			const u64 max_value = value.max.exchange(0, std::memory_order_relaxed);
			const u64 min_value = value.min.exchange(std::numeric_limits<u64>::max(), std::memory_order_relaxed);

			if (!samples)
			{
				continue;
			}

			rows.push_back({value.name, samples, total, max_value, min_value, value.kind});
		}

		std::sort(rows.begin(), rows.end(), [](const row& left, const row& right) { return left.total > right.total; });

		const u64 suppressed = probe::suppressed_lines().exchange(0, std::memory_order_relaxed);

		probe::sink().write("Metal 4 perf: %zu rows over %llu frames, drawables in flight %llu, suppressed lines %llu",
			rows.size(), static_cast<unsigned long long>(interval_frames),
			static_cast<unsigned long long>(drawables_in_flight()),
			static_cast<unsigned long long>(suppressed));

		for (const row& value : rows)
		{
			const f64 avg_units = static_cast<f64>(value.total) / static_cast<f64>(value.samples);

			if (value.kind == 1)
			{
				probe::sink().write("Metal 4 perf: [C] %12llu total %9llu calls %12.2f avg %12llu max  %s",
					static_cast<unsigned long long>(value.total), static_cast<unsigned long long>(value.samples), avg_units,
					static_cast<unsigned long long>(value.max), value.name);
			}
			else if (value.kind == 2)
			{
				probe::sink().write("Metal 4 perf: [G] %12llu total %9llu samples %10.2f avg %10llu max %10llu min  %s",
					static_cast<unsigned long long>(value.total), static_cast<unsigned long long>(value.samples), avg_units,
					static_cast<unsigned long long>(value.max), static_cast<unsigned long long>(value.min), value.name);
			}
			else
			{
				probe::sink().write("Metal 4 perf: [T] %9.3f ms %9llu calls %8.2f us avg %8.2f us max  %s",
					static_cast<f64>(value.total) / 1e6, static_cast<unsigned long long>(value.samples), avg_units / 1e3,
					static_cast<f64>(value.max) / 1e3, value.name);
			}
		}
	}

	// One frame boundary of the renderer, which the report schedule follows.  A
	// level-2 run prints the coverage table at the tenth interval as well, so a run
	// that ends in a signal still leaves it behind.
	inline void tick_frame()
	{
		if (!probe::at_least(1))
		{
			return;
		}

		const u64 frames_now = frames().fetch_add(1, std::memory_order_relaxed) + 1;
		const u64 interval = probe::interval();

		if (frames_now % interval == 0)
		{
			// A new interval refills the text budget, so a flood in one interval
			// does not silence the rest of the run.
			probe::refill_budget();
			report();

			if ((frames_now / interval) % 10 == 0)
			{
				probe::report_coverage(frames_now);
			}
		}
	}
}

#endif // MTL_PERF_ENABLED

// Two-step paste, so __LINE__ expands before it joins the prefix.  One macro per
// line, because the site name is the line number.
#define MTL_PERF_CAT_(a, b) a##b
#define MTL_PERF_CAT(a, b) MTL_PERF_CAT_(a, b)

#if MTL_DEBUG_LOGS

// The verbose log of the backend.  The site gates on the level, and the MTL
// channel carries the trace level gate on top.  A trace site is also capped: one
// site prints at most `mtl::probe::site_line_cap` lines, and the run shares the
// interval budget.  A capped site counts instead of printing.
#define MTL_NOTE(level, ...) \
	do { \
		static std::atomic<u32> MTL_PERF_CAT(_mtl_note_site_, __LINE__){0}; \
		if (::mtl::probe::at_least(level) && ::mtl::probe::allow_line(MTL_PERF_CAT(_mtl_note_site_, __LINE__))) \
		{ \
			::mtl::probe::sink().write(__VA_ARGS__); \
		} \
	} while (false)

// One warning line at the given level.  A warning carries a decision, so it is
// not capped: it is gated by the level alone.
#define MTL_WARN(level, ...) do { if (::mtl::probe::at_least(level)) { mtl_log.warning(__VA_ARGS__); } } while (false)

// One error line at the given level.  Like a warning, an error is not capped.
#define MTL_ERROR(level, ...) do { if (::mtl::probe::at_least(level)) { mtl_log.error(__VA_ARGS__); } } while (false)

// One sampled dump of a pre-built string block.
#define MTL_NOTE_DUMP(level, expr) \
	do { \
		static std::atomic<u32> MTL_PERF_CAT(_mtl_note_dump_site_, __LINE__){0}; \
		if (::mtl::probe::at_least(level) && ::mtl::probe::allow_line(MTL_PERF_CAT(_mtl_note_dump_site_, __LINE__))) \
		{ \
			::mtl::probe::sink().write("%s", (expr)); \
		} \
	} while (false)

#else

#define MTL_NOTE(level, ...) do {} while (false)
#define MTL_WARN(level, ...) do {} while (false)
#define MTL_ERROR(level, ...) do {} while (false)
#define MTL_NOTE_DUMP(level, expr) do {} while (false)

#endif // MTL_DEBUG_LOGS

#if MTL_PERF_ENABLED

// Measures the time of the block it is placed in.  The slot is resolved once per
// call site, so a run below the level pays one cached flag load and two branches,
// and reads no clock.
#define MTL_PERF_SCOPE(level, name) \
	static const u32 MTL_PERF_CAT(_mtl_perf_scope_slot_, __LINE__) = ::mtl::probe::at_least(level) ? ::mtl::perf::slot_id(name, 0) : 0u; \
	const ::mtl::perf::scope MTL_PERF_CAT(_mtl_perf_scope_, __LINE__)(MTL_PERF_CAT(_mtl_perf_scope_slot_, __LINE__))

// Adds `value` to the running total of `name`, one row per call site.  The value
// is not evaluated below the level.
#define MTL_PERF_COUNT(level, name, value) \
	do { \
		static const u32 MTL_PERF_CAT(_mtl_perf_count_slot_, __LINE__) = ::mtl::probe::at_least(level) ? ::mtl::perf::slot_id(name, 1) : 0u; \
		const u32 _mtl_perf_count_id = MTL_PERF_CAT(_mtl_perf_count_slot_, __LINE__); \
		if (_mtl_perf_count_id) \
		{ \
			::mtl::perf::add_count(_mtl_perf_count_id, static_cast<u64>(value)); \
		} \
	} while (false)

// Samples the instantaneous `value` of `name`, keeping its min, average and max.
#define MTL_PERF_GAUGE(level, name, value) \
	do { \
		static const u32 MTL_PERF_CAT(_mtl_perf_gauge_slot_, __LINE__) = ::mtl::probe::at_least(level) ? ::mtl::perf::slot_id(name, 2) : 0u; \
		const u32 _mtl_perf_gauge_id = MTL_PERF_CAT(_mtl_perf_gauge_slot_, __LINE__); \
		if (_mtl_perf_gauge_id) \
		{ \
			::mtl::perf::add_gauge(_mtl_perf_gauge_id, static_cast<u64>(value)); \
		} \
	} while (false)

// One frame boundary for the report schedule.
#define MTL_PERF_FRAME(level) do { if (::mtl::probe::at_least(level)) { ::mtl::perf::tick_frame(); } } while (false)

// The drawable pool bookkeeping: one acquire and one display-shown callback.
#define MTL_PERF_NOTE_ACQUIRED(level) do { if (::mtl::probe::at_least(level)) { ::mtl::perf::note_drawable_acquired(); } } while (false)
#define MTL_PERF_NOTE_PRESENTED(level) do { if (::mtl::probe::at_least(level)) { ::mtl::perf::note_drawable_presented(); } } while (false)

// The aggregate report, which the renderer prints when it goes down.
#define MTL_PERF_REPORT(level) do { if (::mtl::probe::at_least(level)) { ::mtl::perf::report(); } } while (false)

// Counts `value` events on a named coverage latch, cumulatively.  The value is not
// evaluated below the level.
#define MTL_PROBE_TOTAL(level, name, value) \
	do { \
		static const u32 MTL_PERF_CAT(_mtl_probe_latch_, __LINE__) = ::mtl::probe::at_least(level) ? ::mtl::probe::latch_id(name) : 0u; \
		if (MTL_PERF_CAT(_mtl_probe_latch_, __LINE__)) \
		{ \
			::mtl::probe::note(MTL_PERF_CAT(_mtl_probe_latch_, __LINE__), static_cast<u64>(value)); \
		} \
	} while (false)

// One event on a named coverage latch.  The name binds to the call site on the first
// event, in a function-local static, so the macro must take a constant name.  A name
// that varies per call (a ternary, a computed string) registers only the variant that
// ran first and then attributes every later event to it; write one macro per variant
// in an if/else instead.
#define MTL_PROBE_LATCH(level, name) \
	do { \
		static const u32 MTL_PERF_CAT(_mtl_probe_latch_one_, __LINE__) = ::mtl::probe::at_least(level) ? ::mtl::probe::latch_id(name) : 0u; \
		if (MTL_PERF_CAT(_mtl_probe_latch_one_, __LINE__)) \
		{ \
			::mtl::probe::note(MTL_PERF_CAT(_mtl_probe_latch_one_, __LINE__), 1); \
		} \
	} while (false)

// One key of a named histogram.  The id and the last key are cached per call site,
// so a stable key costs one atomic add per sample.
#define MTL_PROBE_HISTOGRAM(level, family, key, key_name) \
	do { \
		static std::atomic<u64> MTL_PERF_CAT(_mtl_probe_hist_key_, __LINE__){std::numeric_limits<u64>::max()}; \
		static std::atomic<u32> MTL_PERF_CAT(_mtl_probe_hist_slot_, __LINE__){0u}; \
		if (::mtl::probe::at_least(level)) \
		{ \
			const u64 _mtl_probe_key = static_cast<u64>(key); \
			u32 _mtl_probe_slot = MTL_PERF_CAT(_mtl_probe_hist_slot_, __LINE__).load(std::memory_order_relaxed); \
			if (!_mtl_probe_slot || MTL_PERF_CAT(_mtl_probe_hist_key_, __LINE__).load(std::memory_order_relaxed) != _mtl_probe_key) \
			{ \
				_mtl_probe_slot = ::mtl::probe::histogram_id(family, _mtl_probe_key, key_name); \
				MTL_PERF_CAT(_mtl_probe_hist_key_, __LINE__).store(_mtl_probe_key, std::memory_order_relaxed); \
				MTL_PERF_CAT(_mtl_probe_hist_slot_, __LINE__).store(_mtl_probe_slot, std::memory_order_relaxed); \
			} \
			::mtl::probe::add_histogram(_mtl_probe_slot, 1); \
		} \
	} while (false)

// One coverage table, which the renderer prints when it goes down.
#define MTL_PROBE_DUMP(level) do { if (::mtl::probe::at_least(level)) { ::mtl::probe::report_coverage(::mtl::perf::frames().load(std::memory_order_relaxed)); } } while (false)

#else

#define MTL_PERF_SCOPE(level, name) do {} while (false)
#define MTL_PERF_COUNT(level, name, value) do {} while (false)
#define MTL_PERF_GAUGE(level, name, value) do {} while (false)
#define MTL_PERF_FRAME(level) do {} while (false)
#define MTL_PERF_NOTE_ACQUIRED(level) do {} while (false)
#define MTL_PERF_NOTE_PRESENTED(level) do {} while (false)
#define MTL_PERF_REPORT(level) do {} while (false)
#define MTL_PROBE_TOTAL(level, name, value) do {} while (false)
#define MTL_PROBE_LATCH(level, name) do {} while (false)
#define MTL_PROBE_HISTOGRAM(level, family, key, key_name) do {} while (false)
#define MTL_PROBE_DUMP(level) do {} while (false)

#endif // MTL_PERF_ENABLED
