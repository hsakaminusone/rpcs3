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

		// The guest publishes with a single PUT register but alternates between
		// command streams (Ridge Racer 7 uses two buffers ~4 MB apart). While the
		// parser is inside one stream, PUT may already name the end of the other,
		// so PUT is not a usable end marker for the stream being read. Track the
		// published end of every stream separately and never read at or past it:
		// a data word sitting exactly on the end has not been written for this
		// pass yet. Flow control (jump/call/return) is the only way across.
		//
		// A jump carries a strict frontier only when the producer has published the
		// target: PUT inside it (a new command buffer) or below every stream (static
		// code). A target above every stream that PUT does not name is a park area
		// the guest patches later, and is read unbounded, exactly as the hardware
		// reads it.
		class stream_tracker
		{
		public:
			static constexpr u32 max_put_gap = 0x100000;
			static constexpr u32 max_streams = 4;

			void reset(u32 base);
			// A resync keeps the streams that are already known: a recovery point
			// in static code must not turn the command buffers into fresh streams
			// with a frontier on their base, which parks the parser for ever.
			void reset_at(u32 base);
			void note_put(u32 put, u32 pos);

			// The word at a stream's published end is the link that carries the
			// parser to the next stream. It is consumed only when the parser stands
			// on it, and GET does not acknowledge it until then, so the producer
			// cannot have recycled it yet.
			static bool is_link(u32 word);

			u32 frontier(u32 put, u32 addr) const;
			void on_jump(u32 target, u32 put);
			void on_call(u32 target, u32 pos);
			void on_return();

			// The parser is standing on the published end of a stream and the
			// word there is not a link. If the producer has meanwhile published
			// another stream, this chunk was abandoned mid-command and the link
			// that would carry the parser out of it will never be written. Move
			// the parser to the base of the stream the producer did publish.
			bool redirect(u32 put, u32 addr, u32& target);

			// TODO(excise): level 1; the park line reports why a redirect could
			// not fire. A call owns the return address, and PUT must name a
			// stream the parser has already entered.
			bool in_call() const { return m_in_call; }
			bool is_known(u32 addr) const;

		private:
			struct entry
			{
				u32 base = 0;
				u32 frontier = 0;
				bool linear = false;
			};

			entry m_entries[max_streams];
			u32 m_count = 0;
			u32 m_current = 0;
			u32 m_pending[max_streams] = {};
			u32 m_pending_next = 0;
			u32 m_pending_count = 0;
			u32 m_saved = 0;
			bool m_in_call = false;
			bool m_call_linear = false;

			bool find(u32 addr, u32& index) const;
			u32 min_base() const;
			void add(u32 base, u32 frontier, bool linear);
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

			u32 m_cache_addr = 0;
			u32 m_cache_size = 0;
			alignas(64) std::byte m_cache[8][128];

			// Published end of the stream the parser is currently in, kept as the
			// hard upper bound for the cache and for whole-packet dispatch.
			stream_tracker m_streams;
			u32 m_published_end = 0;

			// Set when the parser has to leave a stream the producer abandoned.
			// Consumed at the start of read(), where no packet is in flight.
			u32 m_redirect = 0;

			// TODO(excise): level 1; FIFO desync instrumentation.  The desync only
			// shows up downstream, as a data word read as a command header.  These
			// record the producer frontier and the parser's own boundary so the
			// first overrun / phase slip is in the log instead of just its symptom.
			u32 m_diag_max_put = 0;
			u32 m_diag_prev_put = 0;
			u32 m_diag_max_jump = 0;
			u64 m_diag_last_log = 0;
			u32 m_diag_suppressed = 0;
			bool m_diag_retry = false;

			// TODO(excise): level 1; a caught-up parser waits on "put == position",
			// and that is the only stop condition it has.  These record what happens
			// when the producer then moves that frontier: whether the parser resumes
			// at a word the producer has not written yet, and how far it walks before
			// a non-NOP word appears.
			u32 m_diag_parked_put = umax;
			u32 m_diag_park_word = 0;

			// TODO(excise): level 1; the last few producer moves.  A park is
			// explained by the move that preceded it, so keep the old and the new
			// PUT plus the word that was at the old frontier when it moved.
			struct diag_put_entry { u32 prev, put, old_word, get; };
			diag_put_entry m_diag_puts[8] = {};
			u32 m_diag_put_count = 0;

			u32 m_diag_nop_run = 0;
			u32 m_diag_nop_start = 0;
			u64 m_diag_nop_log = 0;
			u32 m_diag_nop_suppressed = 0;

			// TODO(excise): level 1; the jump chain that led to a desync is gone by
			// the time the bad word is read, so keep the last few in memory and dump
			// them with the failing word.  Independent of MTL_PROBE: this is shared
			// RSX code and must also work with the Metal probes off.
			struct diag_jump_entry { u32 from, target, put, cmd; };
			diag_jump_entry m_diag_jumps[32] = {};
			u32 m_diag_jump_next = 0;
			u32 m_diag_jump_count = 0;
			u32 m_diag_last_jump_from = 0;
			u32 m_diag_last_jump_target = 0;
			u32 m_diag_cross_logged = 0;
			u32 m_diag_slab[2] = {};
			u64 m_diag_dump_time = 0;
			u32 m_diag_dump_pos = 0;

		public:
			FIFO_control(rsx::thread* pctrl);
			~FIFO_control() = default;

			u32 translate_address(u32 addr) const;

			std::pair<bool, u32> fetch_u32(u32 addr);
			void invalidate_cache() { m_cache_size = 0; m_published_end = 0; }

			// Stream bookkeeping for the redirect commands. reset_streams() starts
			// a linear stream at the boot position; strict streams are created when
			// a jump enters a new command buffer. A resync preserves the streams
			// that are already known.
			void reset_streams(u32 base, bool preserve = false);
			void on_jump(u32 target);
			void on_call(u32 target);
			void on_return();

			u32 get_pos() const { return m_fifo_pos; }
			u32 last_cmd() const { return m_cmd; }
			// TODO(excise): level 1; a desync capture compares the cache snapshot
			// with the ring, which is how a stale read is told from a mis-parse.
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

			// A word of the command ring, read through the iomap. Only the
			// diagnostics use it; the parser goes through fetch_u32.
			u32 peek_word(u32 addr) const;

			// TODO(excise): level 1; FIFO desync instrumentation, see the members.
			void diag_put_sample(u32 put, u32 addr);
			void diag_read_ahead(u32 addr, u32 put);
			void diag_phase(u32 pos);
			void diag_jump(u32 target);
			void diag_park(u32 addr, u32 frontier, u32 put, u32 word);
			void diag_resume(u32 addr, u32 put);
			void diag_nop(u32 pos);
			void diag_desync(u32 pos, u32 word);
		};
	}
}
