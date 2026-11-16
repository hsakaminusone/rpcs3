#pragma once

#include "util/types.hpp"
#include "Emu/RSX/gcm_enums.h"

#include <span>

struct RsxDmaControl;

namespace rsx
{
	class thread;
	struct rsx_iomap_table;

	namespace FIFO
	{
		enum internal_commands : u32
		{
			FIFO_NOP = 0xBABEF1F4,
			FIFO_EMPTY = 0xDEADF1F0,
			FIFO_BUSY = 0xBABEF1F0,
			FIFO_ERROR = 0xDEADBEEF,
			FIFO_PACKET_BEGIN = 0xF1F0,
			FIFO_DISABLED_COMMAND = 0xF1F4,
			FIFO_DRAW_BARRIER = 0xF1F8,
		};

		enum flatten_op : u32
		{
			NOTHING = 0,
			EMIT_END = 1,
			EMIT_BARRIER = 2
		};

		enum class state : u8
		{
			running = 0,
			empty = 1,    // PUT == GET
			spinning = 2, // Puller continuously jumps to self addr (synchronization technique)
			nop = 3,      // Puller is processing a NOP command
			lock_wait = 4,// Puller is processing a lock acquire
			paused = 5,   // Puller is paused externallly
		};

		enum class interrupt_hint : u8
		{
			conditional_render_eval = 1,
			zcull_sync = 2
		};

		struct register_pair
		{
			u32 reg;
			u32 value;

			void set(u32 reg, u32 val)
			{
				this->reg = reg;
				this->value = val;
			}
		};

		class flattening_helper
		{
			enum register_props : u8
			{
				none = 0,
				skip_on_match = 1,
				always_ignore = 2
			};

			enum optimization_hint : u8
			{
				unknown,
				load_low,
				load_unoptimizable,
				application_not_compatible
			};

			// Workaround for MSVC, C2248
			static constexpr u8 register_props_always_ignore = register_props::always_ignore;

			static constexpr std::array<u8, 0x10000 / 4> m_register_properties = []
			{
				constexpr std::array<std::pair<u32, u32>, 4> ignorable_ranges =
				{{
					// General
					{ NV4097_INVALIDATE_VERTEX_FILE, 3 }, // PSLight clears VERTEX_FILE[0-2]
					{ NV4097_INVALIDATE_VERTEX_CACHE_FILE, 1 },
					{ NV4097_INVALIDATE_L2, 1 },
					{ NV4097_INVALIDATE_ZCULL, 1 }
				}};

				std::array<u8, 0x10000 / 4> register_properties{};

				for (const auto& method : ignorable_ranges)
				{
					for (u32 i = 0; i < method.second; ++i)
					{
						register_properties[method.first + i] |= register_props_always_ignore;
					}
				}

				return register_properties;
			}();

			u32 deferred_primitive = 0;
			u32 draw_count = 0;
			bool in_begin_end = false;

			bool enabled = false;
			u32  num_collapsed = 0;
			optimization_hint fifo_hint = unknown;

			void reset(bool _enabled);

		public:
			flattening_helper() = default;
			~flattening_helper() = default;

			u32 get_primitive() const { return deferred_primitive; }
			bool is_enabled() const { return enabled; }

			void force_disable();
			void evaluate_performance(u32 total_draw_count);
			inline flatten_op test(register_pair& command);
		};

		// A context's virtual FIFO channel while the puller runs another one.
		// Real hardware construct. PFIFO keeps these in RAMFC.
		struct channel_state
		{
			RsxDmaControl* ctrl = nullptr;
			u32 dma_address = 0;

			// Where the puller stopped. Channels only switch between packets.
			u32 get = 0;

			// Call stack and rollback point
			u32 ret_addr = RSX_CALL_STACK_EMPTY;
			u32 saved_ret_addr = RSX_CALL_STACK_EMPTY;
			u32 restore_point = 0;

			state puller_state = state::running;
			flattening_helper flattener;
		};

		// Pure decision table for the single-cursor parser (handoff §17, §18).
		//
		// Hardware has one read cursor and one producer position. PUT is the
		// producer's watermark and it is *exclusive*: the word at PUT is not
		// published, and there is no exception. A word there that looks like a
		// link is previous-lap data; consuming it jumps into unwritten memory
		// (the 2026-11-11 Demon's Souls regression, L214).
		//
		// Keeping the decision free of parser state lets the rule be tested
		// directly.
		struct cursor_rules
		{
			enum class action : u8
			{
				read,  // the cursor is not on the producer: read the command there
				idle,  // the cursor is on the producer: nothing is published yet
			};

			static action decide(u32 pos, u32 put)
			{
				return put == pos ? action::idle : action::read;
			}
		};

		// A packet start the parser executed, kept so that a recovery can roll
		// back to a boundary that is still the one that was parsed there.  The
		// position alone proves nothing: the guest recycles the ring, so the
		// header has to be compared as well.  RR7 re-entered 0x10a588c twenty
		// times in two seconds because recovery trusted the address and the word
		// there had become the next generation's data.
		struct packet_boundary
		{
			u32 pos;
			u32 header;
		};

		struct recovery_rules
		{
			// Newest first.  A boundary is usable when it is strictly below the
			// failure - re-entering the failing packet reproduces the failure,
			// because the parse is deterministic - and the word there is still
			// the header that was parsed.
			template <typename ReadWord>
			static u32 select(const packet_boundary* boundaries, u32 count, u32 failing, ReadWord&& read_word)
			{
				for (u32 i = 0; i < count; i++)
				{
					const packet_boundary& boundary = boundaries[i];

					if (boundary.pos < failing && read_word(boundary.pos) == boundary.header)
					{
						return boundary.pos;
					}
				}

				return umax;
			}
		};

		class FIFO_control
		{
		private:
			mutable rsx::thread* m_thread;
			RsxDmaControl* m_ctrl = nullptr;
			const rsx::rsx_iomap_table* m_iotable;
			u32 m_fifo_pos = 0;

			u32 m_memwatch_addr = 0;
			u32 m_memwatch_cmp = 0;

			u32 m_command_reg = 0;
			u32 m_command_inc = 0;
			u32 m_remaining_commands = 0;
			u32 m_args_ptr = 0;
			u32 m_cmd = ~0u;
			u32 m_next_cmd_entry = ~0u;

			// Last executed packet starts, used only by recovery.  Bounded: a
			// chunk is hundreds of packets, so no finite history spans one; the
			// point is the common case where the guest rewrote only the tail the
			// parser had not reached yet.
			static constexpr u32 boundary_capacity = 32;
			packet_boundary m_boundaries[boundary_capacity] = {};
			u32 m_boundary_head = 0;
			u32 m_boundary_total = 0;
			u32 m_recovery_pos = umax;
			u32 m_recovery_word = 0;

			void note_boundary(u32 pos, u32 header);

			u32 m_cache_addr = 0;
			u32 m_cache_size = 0;

			// The window inside which the cache snapshot may be served.  PUT is
			// the producer's watermark and GET is the consumer's acknowledgement;
			// the producer may rewrite anything below the GET it has seen, and the
			// cache line start is rounded down below the frontier, so a snapshot
			// holds words the guest is entitled to overwrite.  Only [get, put) as
			// they were when the block was filled is immutable: the guest cannot
			// have touched it and the producer has already written all of it.  A
			// hit outside that window re-reads the ring instead of stitching two
			// generations together (L145 and the cache rule are one invariant).
			u32 m_cache_get = 0;
			u32 m_cache_put = 0;
			alignas(64) std::byte m_cache[8][128];

			// One cursor, no stream table. PUT is the producer's watermark and
			// the producer's exit link is the only other way the cursor moves.
			// See the FIFO park handoff, §17. The decision itself is
			// cursor_rules::decide().

			// A word of the command ring, read through the iomap. Used at a park
			// (once per producer move) and by the diagnostics.
			u32 ring_word(u32 addr) const;


		public:
			FIFO_control(rsx::thread* pctrl);
			~FIFO_control() = default;

			u32 translate_address(u32 addr) const;

			std::pair<bool, u32> fetch_u32(u32 addr);
			void invalidate_cache() { m_cache_size = 0; }

			u32 get_pos() const { return m_fifo_pos; }
			u32 last_cmd() const { return m_cmd; }

			// Rollback target for recover_fifo().  Prefers the newest executed
			// packet start whose header is still intact; falls back to `fallback`
			// unless that is the same poisoned word the previous recovery already
			// tried, in which case there is no point in re-parsing it (returns
			// umax and lets the caller stop).
			u32 recovery_point(u32 failing, u32 fallback);
			// A desync capture compares the cache snapshot with the ring, which
			// is how a stale read is told from a mis-parse.  The accessor is
			// production code -- the definition lives in the .cpp -- so it stays
			// in the release tree; only the capture that calls it is excised.
			bool peek_cache(u32 pos, u32& word) const;
			u32 cache_addr() const { return m_cache_addr; }
			u32 cache_size() const { return m_cache_size; }
			u32 next_cmd_entry() const { return m_next_cmd_entry; }
			void sync_get() const;
			std::span<const u32> get_current_arg_ptr(u32 length_in_words) const;
			u32 get_remaining_args_count() const { return m_remaining_commands; }
			void restore_state(u32 cmd, u32 count, u32 position);

			// Channel switch. The caller is expected to manually set GET afterwards.
			void set_channel(RsxDmaControl* ctrl);

			void inc_get(bool wait);
			void set_get(u32 get, u32 spin_cmd = 0);
			void abort();

			template <bool = true>
			u32 read_put() const;

			void read(register_pair& data);
			inline bool read_unsafe(register_pair& data);
			bool skip_methods(u32 count);

		};
	}
}
