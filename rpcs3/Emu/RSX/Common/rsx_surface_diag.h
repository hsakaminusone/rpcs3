#pragma once

// TODO(excise): whole-unit; temporary surface-lock provenance instrument.  It
// exists to answer one question: when a framebuffer_storage section flushes, why
// is the surface it points at not locked?  Every surface keeps a small ring of
// the events that change its lock state, and a global ring keeps the same events
// across surfaces (a clone or a rebind moves the lock between two of them, so
// one surface's ring alone cannot show the hand-off).  Enabled only when
// RSX_SURFACE_DIAG is set; otherwise every entry point returns on one cached
// bool.  Delete this file and every EXCISE-BEGIN surface-diag range when the
// investigation closes.

#include "util/types.hpp"
#include "util/logs.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>

namespace rsx
{
	namespace surface_diag
	{
		enum class event_kind : u16
		{
			lock = 1,
			unlock,
			swap_out,
			swap_in,
			clone_from,
			reset,
			bind,
			create_section,
			destroy_section,
			flush_unlocked
		};

		inline const char* name_of(event_kind kind)
		{
			switch (kind)
			{
			case event_kind::lock: return "LOCK";
			case event_kind::unlock: return "UNLOCK";
			case event_kind::swap_out: return "SWAP-OUT";
			case event_kind::swap_in: return "SWAP-IN";
			case event_kind::clone_from: return "CLONE";
			case event_kind::reset: return "RESET";
			case event_kind::bind: return "BIND";
			case event_kind::create_section: return "SECTION-CREATE";
			case event_kind::destroy_section: return "SECTION-DESTROY";
			case event_kind::flush_unlocked: return "FLUSH-UNLOCKED";
			default: return "?";
			}
		}

		// One event.  `surface` is the surface that owns the ring the event was
		// written to; `peer` names the other surface of a hand-off (the clone
		// source, the outgoing attachment of a rebind).  `tag` is the surface's
		// texture_cache_metadata.timestamp at the event, so a lock and its later
		// unlock can be paired; it is read, never consumed.
		struct event
		{
			u64 time_ns = 0;
			u64 surface = 0;
			u64 tag = 0;
			u32 addr = 0;
			u32 peer = 0;
			u16 kind = 0;
			u8 locked = 0;
			u8 refs = 0;
		};

		inline bool enabled()
		{
			static const bool value = []
			{
				const char* text = std::getenv("RSX_SURFACE_DIAG");
				return text && text[0] && text[0] != '0';
			}();

			return value;
		}

		inline u64 now_ns()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// The cross-surface ring.  A relaxed index makes the write a single store;
		// a dump that races a write may read one torn slot, which is acceptable for
		// evidence and cheaper than a lock on the lock/unlock path.
		constexpr u32 global_capacity = 512;

		inline std::array<event, global_capacity>& global_ring()
		{
			static std::array<event, global_capacity> value{};
			return value;
		}

		inline std::atomic<u32>& global_index()
		{
			static std::atomic<u32> value{0};
			return value;
		}

		inline void record(event_kind kind, u64 surface, u32 addr, u64 tag, u32 peer, bool locked, bool refs)
		{
			if (!enabled())
			{
				return;
			}

			event entry{};
			entry.time_ns = now_ns();
			entry.surface = surface;
			entry.tag = tag;
			entry.addr = addr;
			entry.peer = peer;
			entry.kind = static_cast<u16>(kind);
			entry.locked = locked ? 1 : 0;
			entry.refs = refs ? 1 : 0;

			const u32 slot = global_index().fetch_add(1, std::memory_order_relaxed) % global_capacity;
			global_ring()[slot] = entry;
		}

		// The per-surface ring: small, so the last lock, the clone that moved the
		// lock away and the rebind that lost it are all still present at the abort.
		constexpr u32 surface_capacity = 16;

		struct surface_ring
		{
			std::array<event, surface_capacity> events{};
			u32 next = 0;
			u32 count = 0;
		};

		inline void note(surface_ring& ring, event_kind kind, u64 surface, u32 addr, u64 tag, u32 peer, bool locked, bool refs)
		{
			if (!enabled())
			{
				return;
			}

			event entry{};
			entry.time_ns = now_ns();
			entry.surface = surface;
			entry.tag = tag;
			entry.addr = addr;
			entry.peer = peer;
			entry.kind = static_cast<u16>(kind);
			entry.locked = locked ? 1 : 0;
			entry.refs = refs ? 1 : 0;

			ring.events[ring.next] = entry;
			ring.next = (ring.next + 1) % surface_capacity;

			if (ring.count < surface_capacity)
			{
				++ring.count;
			}

			record(kind, surface, addr, tag, peer, locked, refs);
		}

		inline void print_event(const char* where, const event& entry)
		{
			rsx_log.error("RSX surface diag: %s t=%llu %s surface=0x%llx addr=0x%x peer=0x%x tag=%llu locked=%u refs=%u",
				where, static_cast<unsigned long long>(entry.time_ns), name_of(static_cast<event_kind>(entry.kind)),
				static_cast<unsigned long long>(entry.surface), entry.addr, entry.peer,
				static_cast<unsigned long long>(entry.tag), entry.locked, entry.refs);
		}

		inline void dump_global(const char* reason)
		{
			if (!enabled())
			{
				return;
			}

			const u32 index = global_index().load(std::memory_order_relaxed);
			const u32 count = index < global_capacity ? index : global_capacity;

			rsx_log.error("RSX surface diag: global ring (%s): %u of %u events", reason, count, global_capacity);

			for (u32 n = 0; n < count; ++n)
			{
				const u32 slot = index >= global_capacity ? (index + n) % global_capacity : n;
				print_event("global", global_ring()[slot]);
			}
		}

		// Called from has_flushable_data() when its invariant is already broken.
		// Prints the surface's identity, its ring oldest-first, then the global ring
		// so the other half of a hand-off is visible.
		inline void dump_surface(const char* reason, const surface_ring& ring, u64 self, u32 addr, u64 tag,
			u64 last_use_tag, bool locked, bool refs, bool depth, u32 width, u32 height, u32 pitch)
		{
			if (!enabled())
			{
				return;
			}

			rsx_log.error("RSX surface diag: %s: surface=0x%llx addr=0x%x locked=%u refs=%u depth=%u extent=%ux%u pitch=%u tag=%llu last_use_tag=%llu",
				reason, static_cast<unsigned long long>(self), addr, locked ? 1 : 0, refs ? 1 : 0,
				depth ? 1 : 0, width, height, pitch,
				static_cast<unsigned long long>(tag), static_cast<unsigned long long>(last_use_tag));

			const u32 count = ring.count < surface_capacity ? ring.count : surface_capacity;
			const u32 first = (ring.next + surface_capacity - count) % surface_capacity;

			for (u32 n = 0; n < count; ++n)
			{
				print_event("surface", ring.events[(first + n) % surface_capacity]);
			}

			dump_global(reason);
		}

		// Called from should_flush() with the section in hand, because the surface's
		// own ring cannot name the section or its protection.
		inline void dump_section(const char* reason, u32 base, u32 length, u32 protection, u32 context,
			u64 surface, bool exists, bool surface_locked)
		{
			if (!enabled())
			{
				return;
			}

			rsx_log.error("RSX surface diag: %s: section base=0x%x len=0x%x protection=%u context=%u exists=%u surface=0x%llx surface_locked=%u",
				reason, base, length, protection, context, exists ? 1 : 0,
				static_cast<unsigned long long>(surface), surface_locked ? 1 : 0);
		}
	}
}
