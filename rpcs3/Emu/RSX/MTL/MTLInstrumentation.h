#pragma once

// TODO(excise): this file is temporary.  Removing the diagnostics means deleting
// it and every call site that names MTL_NOTE, MTL_WARN, MTL_ERROR, MTL_NOTE_DUMP,
// MTL_PERF_* or MTL_PROBE_*.  The units that carry call sites are:
// MTLCommandRecorder.cpp, MTLDevice.cpp, MTLFill.cpp, MTLFramebuffer.cpp,
// MTLGSRender.cpp, MTLMetalLayer.mm, MTLOverlays.cpp, MTLPassScheduler.cpp,
// MTLPresent.cpp, MTLPresenter.cpp, MTLProgramPipeline.cpp, MTLResourceManager.cpp,
// MTLSurface.cpp and MTLTextureCache.cpp.

/*
 * The graded diagnostic facility of the Metal 4 backend.  One runtime switch,
 * one experiment switch, and the rest are compile-time decisions.
 *
 * The runtime switches:
 *   MTL_PROBE            the level.  Unset, empty or "0": nothing runs.  "1":
 *                        the counters, the aggregate report and the mandatory
 *                        lines.  "2": adds the coverage latches, the histograms
 *                        and the coverage table.  "3": adds the per-event
 *                        verbose log and the trace level.  "4": adds the stall
 *                        probe, the layer walk, the heartbeat, the residency
 *                        churn and the display-latency stamps.  "5": every dump.
 *                        A number above 5 clamps to 5; a non-number turns the
 *                        facility off and prints one warning.
 *   MTL_PROBE_LOG        the probe's own file; everything except a warning and
 *                        an error goes there instead of the shared log.
 *   MTL_PROBE_DUMP_DIR   where every dump lands (default /tmp; a run that
 *                        matters names a path inside the repository).
 *   MTL_PROBE_FRAME_COUNT  frames an F12 capture window covers (default 1).
 *                        Rings for that many frames after the press are kept
 *                        too, so a race scene is dumped as a whole.
 *   MTL_PROBE_ONLY_DRAW  the bisection filter, <n> or <lo>-<hi> of the draw ids
 *                        printed by the per-draw trace; every other draw is
 *                        counted and dropped.
 *   MTL_PROBE_PIXELS     the fixed-patch readback after every draw of a
 *                        capture window.  The patch list is a semicolon list of
 *                        "x,y,w,h" (up to eight patches).
 *   MTL_PROBE_TAPE       the rolling pixel tape's depth in frames (default 0,
 *                        off).  The tape keeps the last N presented frames and
 *                        the images a feedback-loop sample read (up to 2048
 *                        square, so a full post-chain input fits), on the GPU,
 *                        and writes them when F12 freezes it.  N is raised to
 *                        at least 8 while enabled so a slot is never reused
 *                        before its frame has completed.  At the freeze the
 *                        present stream is read back as a timeline
 *                        (newly-black/newly-dark, bbox, worst 16x16 cell) and
 *                        each sample image gets a black/dark tally.
 *   MTL_PROBE_VARIANT    the experiment arms, semicolon separated.  State-only
 *                        arms never touch the shipping archive; the shader arms
 *                        build into their own archive suffix.  The arms:
 *                          replace-textures=red|uv|checker  (every fragment
 *                                                            unit samples the
 *                                                            diagnostic image)
 *                          replace-units=0,2,5              (...narrowed)
 *                          no-alpha-test                   (ROP alpha test out; own archive)
 *                          no-depth                        (depth test off)
 *                          no-cull                         (cull off)
 *                          sentinel                        (passes clear magenta)
 *                          poison                          (fresh allocations filled with a diagnostic word)
 *                          heaps                           (hazard-tracked argument heaps)
 *                          serialise                       (one command per encoder, wait-idle per draw)
 *                          force-fp=<ids>                  (colour output of the listed compiled fp ids is forced magenta; own archive)
 *                          force-fp-tex=<count>            (same, for fps enabling <count> textures; own archive)
 *                          lod0                            (force min LOD 0)
 *                          dump-tex=<hex>                  (every upload and every sample of that guest address dumps its bytes; bypasses the dedupe and the run budgets; 16 files, 64 MB per run)
 *                          dump-draw=<id>                  (the named draw's raw persistent and volatile vertex streams dump; 16 MB per run)
 *
 * With the level at 5 a per-address upload/sample census is kept for the whole
 * run and printed when a capture window closes (the Metal 4: census lines, plus
 * the Metal 4: census never-uploaded lines for allocations sampled with zero
 * uploads this run).
 *
 * Two compile switches remove the facility.  MTL_DEBUG_LOGS=0 removes the text
 * family.  MTL_PERF_ENABLED=0 removes the numeric family.  MTL_DIAGNOSTICS=0
 * forces both off in one place.  A build with both off carries no code, no
 * string and no cost.
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
#include <unordered_map>
#include <vector>
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

	// TODO(excise): level 5; the experiment arms of one run, one string.  Every
	// arm is a diagnostic; none changes gameplay when off, and the shader arms
	// build into their own archive suffix.  The arms, semicolon or comma
	// separated (see the file header):
	//   replace-textures=red|uv|checker   replace every fragment unit's image
	//   replace-units=0,2,5               ...narrowed to the listed units
	//   no-alpha-test no-depth no-cull sentinel poison heaps serialise lod0
	//   dump-tex=<hex> dump-draw=<n> force-fp=<ids> force-fp-tex=<count>
	// Read once.
	inline std::vector<std::string> variant_tokens()
	{
		static const std::vector<std::string> value = [] {
			std::vector<std::string> tokens;
			const char* text = std::getenv("MTL_PROBE_VARIANT");

			if (!text)
			{
				return tokens;
			}

			std::string token;

			for (const char c : std::string(text))
			{
				if (c == ',' || c == ';' || c == ' ' || c == '\t')
				{
					if (!token.empty())
					{
						tokens.push_back(std::move(token));
						token.clear();
					}

					continue;
				}

				token += c;
			}

			if (!token.empty())
			{
				tokens.push_back(std::move(token));
			}

			return tokens;
		}();

		return value;
	}

	inline bool variant(const char* name)
	{
		const std::string needle(name);

		for (const std::string& token : variant_tokens())
		{
			if (token == needle)
			{
				return true;
			}
		}

		return false;
	}

	inline std::string variant_arg(const char* name)
	{
		const std::string prefix = std::string(name) + "=";

		for (const std::string& token : variant_tokens())
		{
			if (token.rfind(prefix, 0) == 0)
			{
				return token.substr(prefix.size());
			}
		}

		return {};
	}

	// TODO(excise): level 5; MTL_PROBE_VARIANT=poison.  A texture created under this
	// switch is filled with a word that encodes its creating guest address; a
	// shared buffer is filled with a word derived from an allocation index, and the
	// log line carries it with the size and the format.  An unwritten resource
	// therefore samples as the word itself and the log names the allocation.
	// Read once per caller; state-only, the archive name does not change.
	// TODO(excise): level 5; the poison fill the two writers share.  A partial
	// trailing word is filled a byte at a time from the same word, so no byte of
	// a fresh allocation is left undefined.
	inline void fill_words(void* output, u64 size, u32 word)
	{
		u8* out = static_cast<u8*>(output);

		for (u64 index = 0; index < size;)
		{
			const u32 chunk = static_cast<u32>(std::min<u64>(size - index, sizeof(word)));
			std::memcpy(out + index, &word, chunk);
			index += chunk;
		}
	}

	inline u32 poison_word(u32 address)
	{
		return 0xff000000u | ((address * 2654435761u) & 0x00ffffffu);
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

	// Frames between two reports.  A compile constant: the report cadence is
	// not something a run tunes.
	inline u64 interval()
	{
		return static_cast<u64>(MTL_PROBE_INTERVAL);
	}

	// The acquire stall threshold in milliseconds.  A compile constant.
	inline f64 stall_ms()
	{
		return static_cast<f64>(MTL_PROBE_STALL_MS);
	}

	// The optional deep-capture trigger.  the time arm (removed) starts one capture
	// window after that many seconds, and MTL_PROBE_FRAME_COUNT is how many
	// consecutive frames the window covers (default one).  The same count is the
	// forward half of the ring dump, so a wide window keeps every frame's rings,
	// not only the six after the press.  An unset or negative value leaves the
	// window off.  The start is the first probe call, so the threshold is relative
	// to the backend coming up, not to process creation.
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
				// TODO(excise): level 3; MTL_PROBE_LOG names the file for this run.
				// The default truncates, which lost the level-5 capture of the car
				// twice, so a run that matters sets a unique path.
				if (const char* configured = std::getenv("MTL_PROBE_LOG"); configured && *configured)
				{
					return std::string(configured);
				}

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

	// TODO(excise): level 5; the directory every dump lands in.  The default is
	// the temporary directory, which the machine purges, and a capture that is
	// lost to the purge costs a whole run, so a run that matters names a path
	// inside the repository (MTL_PROBE_DUMP_DIR).
	inline const char* dump_directory()
	{
		static const std::string value = [] {
			if (const char* configured = std::getenv("MTL_PROBE_DUMP_DIR"); configured && *configured)
			{
				return std::string(configured);
			}

			return std::string("/tmp/mtl4-dump");
		}();

		return value.c_str();
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

	// TODO(excise): level 5; the forward half of a ring capture.  The history
	// before the key press is always written; this is how many frames after it
	// are kept.  It follows the capture window, so MTL_PROBE_FRAME_COUNT=150
	// captures a scene rather than one frame, with a floor of the six-frame burst
	// a single-frame defect needs.  The trigger frame is written as p1, so the
	// active window that starts on the next frame needs one more.
	inline u64 dump_frame_count()
	{
		const u64 window = capture_frame_count();
		return window + 1 < 6 ? 6 : window + 1;
	}

	// TODO(excise): level 5; a patch probe reads back after every draw, and a
	// race frame has more draws than the default allows, so the watch can be
	// widened for one run.  The cost is one queue round trip per probe.
	inline u32 pixel_probe_limit()
	{
		// The RCA run watches the whole capture window, not a prefix of one
		// frame.  A race frame reaches ~1100 draws and a 120-frame window is
		// ~130000 draws, so the limit covers the window with headroom.  The
		// cost is one queue round trip per probed draw, and a run that sets a
		// 120-frame window pays it deliberately.
		return 400000u;
	}

	// TODO(excise): level 5; why nothing is captured when the trigger frame is
	// already gone.  The user presses the key after seeing the defect, and the
	// capture window starts on the next frame, so the frame that showed the
	// defect is never the frame that is dumped.  The tape keeps the last few
	// presented frames and the small images a feedback-loop sample read, on the
	// GPU and without a readback, and F12 freezes it.  A depth below 8 is raised
	// to 8, because a slot must not be reused while its frame is still in flight.
	inline u32 tape_depth()
	{
		static const u32 cached = [] {
			const char* text = std::getenv("MTL_PROBE_TAPE");
			const u64 parsed = text ? std::strtoull(text, nullptr, 10) : 0;

			if (!parsed)
			{
				return 0u;
			}

			const u64 depth = parsed < 8 ? 8 : parsed;
			return static_cast<u32>(depth > 64 ? 64 : depth);
		}();

		return cached;
	}

	// TODO(excise): level 5; the RCA pack.  One host-side census of every texture
	// the backend uploads or samples in a run, so a frame's draws can be
	// attributed without a memory forensics session and a never-uploaded or
	// flat-zero allocation reads out of the capture instead of volunteering a
	// theory.  The upload path notes (bytes, BC-zero blocks), the draw path
	// notes (samples, unit-as-target hazards); the window-close site prints
	// the table.  Bounded at 384 addresses and hashed into the note budget, so a
	// hostile run reports the overflow as one count, never a flood.  The never-uploaded
	// list (sampled this run, zero uploads) prints first.
	struct census_entry
	{
		u64 first_frame = ~0ull;
		u64 last_frame = 0;
		u64 upload_bytes = 0;
		u64 samples = 0;
		u64 hazard_target = 0;
		u64 bc_blocks = 0;
		u64 bc_zero_blocks = 0;
		u32 uploads = 0;
		u32 bc_block_bytes = 0;
		u32 gcm_format = 0;
	};

	inline std::mutex& census_guard()
	{
		static std::mutex value;
		return value;
	}

	inline u64& census_frame()
	{
		static u64 value = 0;
		return value;
	}

	inline std::unordered_map<u32, census_entry>& census_map()
	{
		static std::unordered_map<u32, census_entry> value;
		return value;
	}

	inline u64& census_suppressed()
	{
		static u64 value = 0;
		return value;
	}

	inline void census_note_frame(u64 frame)
	{
		std::lock_guard<std::mutex> guard(census_guard());
		census_frame() = frame;
	}

	constexpr u32 census_address_limit = 384u;

	// The per-BC-block zero scan of one converted upload.  A flat-zero DXT payload
	// is what an allocation nobody uploaded looks like at the sample; counting the
	// blocks at the upload edge catches it before any screenshot shows black.
	// Only the three DXT formats have whole blocks; the scan walks 8 or 16 bytes
	// at a time and is bounded to the upload size.
	inline u64 bc_zero_block_scan(const u8* converted, u64 size, u32 block_bytes)
	{
		if (!converted || !block_bytes)
		{
			return 0;
		}

		u64 zero = 0;

		for (u64 offset = 0; offset + block_bytes <= size; offset += block_bytes)
		{
			u32 word = 0;
			for (u32 byte = 0; byte < block_bytes; ++byte)
			{
				word |= converted[offset + byte];
			}

			zero += (word == 0u) ? 1ull : 0ull;
		}

		return zero;
	}

	inline void census_note_upload(u32 address, u64 bytes, u32 gcm_format, u64 bc_blocks, u64 bc_zero_blocks, u32 bc_block_bytes)
	{
		if (!address)
		{
			return;
		}

		std::lock_guard<std::mutex> guard(census_guard());

		if (census_map().size() >= census_address_limit && !census_map().count(address))
		{
			++census_suppressed();
			return;
		}

		census_entry& entry = census_map()[address];
		const u64 frame = census_frame();
		entry.first_frame = std::min(entry.first_frame, frame);
		entry.last_frame = std::max(entry.last_frame, frame);
		++entry.uploads;
		entry.upload_bytes += bytes;
		entry.bc_blocks += bc_blocks;
		entry.bc_zero_blocks += bc_zero_blocks;
		entry.bc_block_bytes = bc_block_bytes;
		entry.gcm_format = gcm_format;
	}

	inline void census_note_sample(u32 address)
	{
		if (!address)
		{
			return;
		}

		std::lock_guard<std::mutex> guard(census_guard());

		if (census_map().size() >= census_address_limit && !census_map().count(address))
		{
			++census_suppressed();
			return;
		}

		census_entry& entry = census_map()[address];
		const u64 frame = census_frame();
		entry.first_frame = std::min(entry.first_frame, frame);
		entry.last_frame = std::max(entry.last_frame, frame);
		++entry.samples;
	}

	// The bound-unit-as-target flag: a unit that samples an address the same
	// draw is rendering into is the paint/reflection identity-split class.
	inline void census_note_hazard(u32 address)
	{
		if (!address)
		{
			return;
		}

		std::lock_guard<std::mutex> guard(census_guard());

		if (census_map().size() >= census_address_limit && !census_map().count(address))
		{
			++census_suppressed();
			return;
		}

		census_entry& entry = census_map()[address];
		const u64 frame = census_frame();
		entry.first_frame = std::min(entry.first_frame, frame);
		entry.last_frame = std::max(entry.last_frame, frame);
		++entry.hazard_target;
	}

	// The report a capture window prints when it closes: the never-uploaded list
	// first (the smoking gun), then every address with a zero BC block, then the
	// hazard flags, then the totals.  Fixed order, one line each, bounded.
	inline void census_print(const char* reason)
	{
		if (!at_least(5))
		{
			return;
		}

		std::lock_guard<std::mutex> guard(census_guard());

		u64 uploads = 0, upload_bytes = 0, samples = 0, hazards = 0, addresses = 0;
		u64 bc_blocks = 0, bc_zero = 0, never = 0;

		for (const auto& pair : census_map())
		{
			const census_entry& entry = pair.second;
			uploads += entry.uploads;
			upload_bytes += entry.upload_bytes;
			samples += entry.samples;
			hazards += entry.hazard_target;
			bc_blocks += entry.bc_blocks;
			bc_zero += entry.bc_zero_blocks;

			if (entry.samples && !entry.uploads)
			{
				++never;
				sink().write("Metal 4: census never-uploaded 0x%x, samples %llu, frames %llu..%llu",
					pair.first, static_cast<unsigned long long>(entry.samples),
					static_cast<unsigned long long>(entry.first_frame == ~0ull ? 0 : entry.first_frame),
					static_cast<unsigned long long>(entry.last_frame));
			}
		}

		sink().write("Metal 4: census %s: %zu entries, uploads %llu, bytes %llu, samples %llu, bc zero %llu/%llu blocks, hazards %llu, suppressed %llu",
			reason, census_map().size(),
			static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(upload_bytes),
			static_cast<unsigned long long>(samples),
			static_cast<unsigned long long>(bc_zero), static_cast<unsigned long long>(bc_blocks),
			static_cast<unsigned long long>(hazards), static_cast<unsigned long long>(census_suppressed()));

		std::vector<u32> keys;
		keys.reserve(census_map().size());

		for (const auto& pair : census_map())
		{
			keys.push_back(pair.first);
		}

		std::sort(keys.begin(), keys.end());

		for (const u32 key : keys)
		{
			const census_entry& entry = census_map()[key];

			if (!entry.samples && !entry.bc_zero_blocks && !entry.hazard_target)
			{
				continue;
			}

			sink().write("Metal 4: census 0x%x, uploads %u, bytes %llu, samples %llu, fmt 0x%x bc zero %llu/%llu (%uB), hazards %llu, frames %llu..%llu",
				key, entry.uploads, static_cast<unsigned long long>(entry.upload_bytes),
				static_cast<unsigned long long>(entry.samples), entry.gcm_format,
				static_cast<unsigned long long>(entry.bc_zero_blocks),
				static_cast<unsigned long long>(entry.bc_blocks), entry.bc_block_bytes,
				static_cast<unsigned long long>(entry.hazard_target),
				static_cast<unsigned long long>(entry.first_frame == ~0ull ? 0 : entry.first_frame),
				static_cast<unsigned long long>(entry.last_frame));
		}

		sink().write("Metal 4: census end %s", reason);
	}

	// TODO(excise): level 5; MTL_PROBE_VARIANT=dump-tex=<hex>.  The catch-all for the
	// one texture the other budgets can starve: every upload of that address and
	// every sample of it writes its bytes, bypassing the dedupe keys and the run
	// budgets, with its own budget (16 files, 64 MB per run).  Hex, with an optional
	// 0x.  A run that names nothing takes the fast path (one compare, zero).
	inline u32 dump_tex_address()
	{
		static const u32 value = [] {
			const std::string arg = variant_arg("dump-tex");

			if (arg.empty())
			{
				return 0u;
			}

			const char* text = arg.c_str();
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(text + (text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 2 : 0), &end, 16);
			return (end && *end == '\0' && end != text) ? static_cast<u32>(parsed) : 0u;
		}();

		return value;
	}

	inline std::atomic<u32>& dump_tex_file_count()
	{
		static std::atomic<u32> value{0};
		return value;
	}

	inline std::atomic<u64>& dump_tex_byte_count()
	{
		static std::atomic<u64> value{0ull};
		return value;
	}

	inline bool dump_tex_match(u32 address)
	{
		return address != 0 && address == dump_tex_address();
	}

	inline std::mutex& dump_tex_guard()
	{
		static std::mutex value;
		return value;
	}

	// Atomic claim: at most 16 files and 64 MB per run, however many sites match.
	inline bool dump_tex_take(u64 bytes)
	{
		if (!dump_tex_address())
		{
			return false;
		}

		std::lock_guard<std::mutex> guard(dump_tex_guard());

		if (dump_tex_file_count().load(std::memory_order_relaxed) >= 16u)
		{
			return false;
		}

		if (dump_tex_byte_count().load(std::memory_order_relaxed) + bytes > 64ull * 1024u * 1024u)
		{
			return false;
		}

		dump_tex_file_count().store(dump_tex_file_count().load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
		dump_tex_byte_count().store(dump_tex_byte_count().load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
		return true;
	}

	// TODO(excise): level 5; MTL_PROBE_VARIANT=dump-draw=<id>.  The isolated-draw
	// capture: when the per-draw trace finds that draw, its raw persistent and
	// volatile vertex streams are dumped (bypassing every other budget), the
	// record every isolated-draw analysis starts from.  Decimal or 0x hex.  One
	// draw, 16 MB per run.
	inline u64 dump_draw_id()
	{
		static const u64 value = [] {
			const std::string arg = variant_arg("dump-draw");

			if (arg.empty())
			{
				return ~0ull;
			}

			const char* text = arg.c_str();
			const bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');

			if (hex)
			{
				char* end = nullptr;
				const unsigned long long parsed = std::strtoull(text + 2, &end, 16);
				return (end && *end == '\0') ? static_cast<u64>(parsed) : ~0ull;
			}

			char* end = nullptr;
			const unsigned long long parsed = std::strtoull(text, &end, 10);
			return (end && *end == '\0') ? static_cast<u64>(parsed) : ~0ull;
		}();

		return value;
	}

	inline std::atomic<u64>& dump_draw_byte_count()
	{
		static std::atomic<u64> value{0ull};
		return value;
	}

	inline bool dump_draw_match(u64 draw)
	{
		return dump_draw_id() != ~0ull && draw == dump_draw_id();
	}

	inline bool dump_draw_take(u64 bytes)
	{
		if (!bytes || dump_draw_id() == ~0ull)
		{
			return false;
		}

		return dump_draw_byte_count().fetch_add(bytes, std::memory_order_relaxed) + bytes <= 16ull * 1024u * 1024u;
	}

	// TODO(excise): level 5; a deferred texture is materialised in the texture
	// cache but the host dump lives in the renderer, and the caller needs to read
	// the gathered image *before* the pass consumes it.  The bind leaves the
	// image here; the draw loop dumps and clears it.  One unit at a time, because
	// a bind is not re-entrant.
	struct deferred_dump_request
	{
		void* image = nullptr;
		u32 address = 0;
		u32 op = 0;
	};

	inline thread_local deferred_dump_request deferred_dump;

	// The anti-flood limits.  A text site prints at most `site_line_cap` lines by
	// itself in a run, every text site shares the interval budget, and the whole run
	// shares the run budget.  When a limit is reached the site stops printing and
	// counts instead, so no probe can flood a log.  All limits are compile constants,
	// not switches.
	// A capture window prints one block per draw, so five windows of five hundred
	// draws need more than the default per-site cap.  The override is diagnostics
	// only: it raises a limit, it never changes what is measured.
	inline u32 site_line_cap()
	{
		return 4096u;
	}

	inline s64 interval_line_budget()
	{
		return 40000ll;
	}

	inline s64 run_line_budget()
	{
		return 200000ll;
	}

	inline std::atomic<s64>& interval_budget()
	{
		static std::atomic<s64> value{interval_line_budget()};
		return value;
	}

	inline std::atomic<s64>& run_budget()
	{
		static std::atomic<s64> value{run_line_budget()};
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
		interval_budget().store(interval_line_budget(), std::memory_order_relaxed);
	}

	// True while this line is inside the per-site cap, the interval budget and the
	// run budget.  `site_counter` belongs to one call site.  The first line a site
	// refuses states the cap, and the site then counts only.
	inline bool allow_line(std::atomic<u32>& site_counter)
	{
		const u32 line = site_counter.fetch_add(1, std::memory_order_relaxed);

		if (line >= site_line_cap())
		{
			suppressed_lines().fetch_add(1, std::memory_order_relaxed);

			if (line == site_line_cap())
			{
				mtl_log.warning("Metal 4 probe: a site reached its cap of %u lines; later lines of that site are counted only", site_line_cap());
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
					static_cast<long long>(run_line_budget()));
			});

			return false;
		}

		return true;
	}

	// The phase witness is the device-verdict instrument, so it must still be
	// printing when the race starts.  The generic text budget is per call site
	// (4096 lines), and one witness line per frame crosses that in about two
	// minutes, which is before the game reaches the car: the budget would go
	// quiet exactly when the evidence appears.  A witness line therefore has its
	// own high cap and does not spend the generic site/interval budgets.
	inline u32 witness_line_cap()
	{
		return 400000u;
	}

	inline std::atomic<u32>& witness_lines()
	{
		static std::atomic<u32> value{0};
		return value;
	}

	inline bool allow_witness_line()
	{
		const u32 line = witness_lines().fetch_add(1, std::memory_order_relaxed);

		if (line >= witness_line_cap())
		{
			if (line == witness_line_cap())
			{
				mtl_log.warning("Metal 4 probe: the witness reached its cap of %u lines; later witness lines are dropped",
					witness_line_cap());
			}

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

		// TODO(excise): the switches that change what the backend emits, not only
		// what it logs.  A run's rendering is not the shipping rendering while one
		// is on, so the fact is stated once, before any frame, where the run's own
		// log carries it (the archive name alone is not read by a reader of the
		// probe log).
		if (variant("no-alpha-test"))
		{
			mtl_log.warning("Metal 4 probe: diagnostic, the alpha test is compiled out of every fragment "
				"program (MTL_PROBE_VARIANT=no-alpha-test); a fragment the guest would discard is drawn instead, and "
				"this run's pipeline archive is kept apart.");
		}

		if (variant("heaps"))
		{
			mtl_log.warning("Metal 4 probe: diagnostic, every heap tracks hazards (MTL_PROBE_VARIANT=heaps); the "
				"driver orders conflicting accesses, so a defect that goes is a missing edge and a defect that "
				"stays is content.");
		}

		if (variant("sentinel"))
		{
			mtl_log.warning("Metal 4 probe: diagnostic, every pass clears its colour attachments to the sentinel "
				"(MTL_PROBE_VARIANT=sentinel); a pass that does not write a pixel leaves it magenta, so a delivered "
				"image that keeps a magenta block is a surface no draw painted and a delivered image that keeps a "
				"non-magenta block in that position is one this run's draws painted.");
		}

		if (variant("poison"))
		{
			mtl_log.warning("Metal 4 probe: diagnostic, every freshly allocated texture and shared buffer is filled "
				"with a word (MTL_PROBE_VARIANT=poison); textures' words encode the creating guest address and buffers' "
				"words an allocation index, with a log line per allocation.  An unwritten resource samples as the "
				"word itself, and the log line with the same word names the allocation.");
		}

		if (variant("serialise"))
		{
			mtl_log.warning("Metal 4 probe: diagnostic, the GPU is serialised (MTL_PROBE_VARIANT=serialise); one "
				"command buffer per flush and wait-idle per draw, so a fault that goes is a missing GPU edge and a "
				"fault that stays is content.");
		}

		if (dump_tex_address() != 0)
		{
			mtl_log.warning("Metal 4 probe: diagnostic, every upload and every sample of 0x%x dumps its bytes to the "
				"dump directory (MTL_PROBE_VARIANT=dump-tex=0x%x); 16 files and 64 MB per run, bypassing the "
				"dedupe keys and the run budgets.", dump_tex_address(), dump_tex_address());
		}

		if (dump_draw_id() != ~0ull)
		{
			mtl_log.warning("Metal 4 probe: diagnostic, the draw named by MTL_PROBE_VARIANT=dump-draw dumps its raw "
				"persistent and volatile vertex streams to the dump directory (16 MB per run).");
		}

		if (const char* only = std::getenv("MTL_PROBE_ONLY_DRAW"); only && *only && *only != '0')
		{
			mtl_log.warning("Metal 4 probe: diagnostic, only the draws named by MTL_PROBE_ONLY_DRAW are encoded "
				"(<n> or <lo>-<hi>, draw ids are the 'draw <n>' ids in the log); every other draw is counted and "
				"dropped, so the isolated draw's contribution to the target can be read on its own.");
		}
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

// A witness line is the evidence the run exists to produce, so it is not bound
// by the per-site cap that silences an ordinary trace before the race.  It has
// its own large cap and prints at the level gate alone.
#define MTL_NOTE_WITNESS(level, ...) \
	do { \
		if (::mtl::probe::at_least(level) && ::mtl::probe::allow_witness_line()) \
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
#define MTL_NOTE_WITNESS(level, ...) do {} while (false)
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
