#include "stdafx.h"

#include "Emu/System.h"
#include "RSXFIFO.h"
#include "RSXThread.h"
#include "Capture/rsx_capture.h"
#include "Core/RSXReservationLock.hpp"
#include "Emu/Memory/vm_reservation.h"
#include "Emu/Cell/lv2/sys_rsx.h"
#include "NV47/HW/context.h"

#include "util/asm.hpp"

#include <thread>

using spu_rdata_t = std::byte[128];

extern void mov_rdata(spu_rdata_t& _dst, const spu_rdata_t& _src);
extern bool cmp_rdata(const spu_rdata_t& _lhs, const spu_rdata_t& _rhs);

// EXCISE-BEGIN fifo-upstream-ab
// A/B switch for the whole-packet rule.  Upstream (8183f6e32) dispatches a
// packet as soon as its header is read; the production model waits until every
// argument of the packet is published.  RPCS3_FIFO_UPSTREAM=1 restores
// upstream's behaviour, so one build can show whether the gate is load-bearing
// for a given game.  The release tree has no switch: the production behaviour
// is compiled in.
#define RSX_FIFO_UPSTREAM_AB 1
// EXCISE-END fifo-upstream-ab

namespace rsx
{
	namespace FIFO
	{
		FIFO_control::FIFO_control(::rsx::thread* pctrl)
		{
			m_thread = pctrl;
			m_ctrl = pctrl->ctrl;
			m_iotable = &pctrl->iomap_table;

			// EXCISE-BEGIN fifo-upstream-ctor
			// The switch is dev-only; the release tree hard-codes production.
#if defined(RSX_FIFO_UPSTREAM_AB) && RSX_FIFO_UPSTREAM_AB
			if (const char* e = getenv("RPCS3_FIFO_UPSTREAM"); e && e[0] == '1')
			{
				m_upstream_semantics = true;
			}
#endif
			// EXCISE-END fifo-upstream-ctor
		}

		u32 FIFO_control::translate_address(u32 address) const
		{
			if (m_thread->get_context()->fw.is_fifo_rsx_local)
			{
				if (address >= m_thread->local_mem_size)
				{
					return umax;
				}

				return rsx::constants::local_mem_base + address;
			}

			return m_iotable->get_addr(address);
		}

		void FIFO_control::sync_get() const
		{
			// GET is the consumer's acknowledgement, not a read-ahead hint. The
			// guest uses it as the licence to recycle a command buffer, so it must
			// name only the position the parser has actually reached. Publishing
			// the end of the prefetched block told the guest that a chunk was free
			// while the parser was still inside it; the guest then reused the
			// chunk and overwrote the link the parser needed (L119, L126, L144).
			m_ctrl->get.release(m_fifo_pos);
		}

		void FIFO_control::restore_state(u32 cmd, u32 count, u32 position)
		{
			m_cmd = cmd;
			m_command_inc = ((m_cmd & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD) ? 0 : 4;
			m_remaining_commands = count;
			m_fifo_pos = position - (count ? 4 : 0);
			m_args_ptr = m_iotable->get_addr(m_fifo_pos);
			m_command_reg = (m_cmd & 0xffff) + m_command_inc * (((m_cmd >> 18) - count) & 0x7ff) - m_command_inc;
			// position is the next argument to read, so the packet ends count
			// words later. Using the rewound m_fifo_pos left the boundary one
			// word short and made the parser re-read the packet's last argument
			// as the next header.
			m_next_cmd_entry = position + m_remaining_commands * 4;
		}

		void FIFO_control::set_channel(RsxDmaControl* ctrl)
		{
			invalidate_cache();

			m_ctrl = ctrl;
			m_memwatch_addr = 0;
			m_memwatch_cmp = 0;
			m_remaining_commands = 0;
		}

		void FIFO_control::inc_get(bool wait)
		{
			m_fifo_pos += 4;

			if (!m_cache_size && wait && read_put<false>() == m_fifo_pos)
			{
				// NOTE: Only supposed to be invoked to wait for a single arg on command[0] (4 bytes)
				// Wait for put to allow us to proceed execution
				sync_get();
				invalidate_cache();

				// NOTE: lv2 waits for a freed context to go away, which only happens once this command returns
				while (read_put() == m_fifo_pos && !Emu.IsStopped() && !m_thread->get_context()->fw.free_requested)
				{
					m_thread->cpu_wait({});
				}
			}
		}

		template <bool Full>
		inline u32 FIFO_control::read_put() const
		{
			if constexpr (!Full)
			{
				return m_ctrl->put & ~3;
			}
			else
			{
				if (u32 put = m_ctrl->put; (put & 3) == 0) [[likely]]
				{
					return put;
				}

				return m_ctrl->put.and_fetch(~3);
			}
		}

		// A word of the command ring, read through the iomap. Called once per
		// producer move, so the cost does not matter.
		u32 FIFO_control::ring_word(u32 addr) const
		{
			const u32 host = m_iotable->get_addr(addr & -128);
			return host == umax ? 0 : +vm::read32(host | (addr & 127));
		}

		// EXCISE-BEGIN fifo-diag-impl
		// TODO(excise): level 1; FIFO desync instrumentation.  A desync only ever
		// shows up downstream, as a data word read as a command header; the state
		// that makes that possible (the parser at or past the producer frontier, or
		// a jump landing outside it) is gone by then.  These log that state when it
		// first happens.  Removed together with the probe work.
		void FIFO_control::diag_put_sample(u32 put, u32 addr)
		{
			if (put > m_diag_max_put)
			{
				m_diag_max_put = put;
			}

			// TODO(excise): level 1; record the frontier word at the moment the
			// producer moved away from it.  This is the only instant that separates
			// "the link was never written for this pass" from "it was written after
			// the move and the parser missed it".
			if (put != m_diag_prev_put && m_diag_prev_put)
			{
				diag_put_entry& entry = m_diag_puts[m_diag_put_count++ & 7u];
				entry.prev = m_diag_prev_put;
				entry.put = put;
				entry.old_word = ring_word(m_diag_prev_put);
				entry.new_word = ring_word(put);
				entry.get = m_fifo_pos;
				entry.get_reg = +m_ctrl->get;
			}

			// The producer pointer only moves backwards when the guest wraps its
			// command buffers, which is the one place the ring extent is observable.
			if (m_diag_prev_put > put && m_diag_prev_put - put > 0x10000)
			{
				rsx_log.warning("FIFO diag: PUT rewind prev=0x%x put=0x%x max=0x%x get=0x%x fetch=0x%x",
					m_diag_prev_put, put, m_diag_max_put, m_fifo_pos, addr);
			}
			else if (put != m_diag_prev_put && m_diag_prev_put)
			{
				// TODO(excise): level 1; the interesting case is not the rewind but
				// the move that ends a catch-up: the parser was stopped exactly on
				// the old frontier, so the next read resumes on a word the producer
				// may not have written yet.
				const bool parked = m_fifo_pos == m_diag_prev_put;
				const bool sideways = put > m_diag_prev_put && put - m_diag_prev_put > 0x10000;

				if (parked || sideways)
				{
					const u64 now = get_system_time();

					if (m_diag_last_log && now - m_diag_last_log < 20000)
					{
						m_diag_suppressed++;
					}
					else
					{
						m_diag_last_log = now;
						rsx_log.warning("FIFO diag: PUT move prev=0x%x put=0x%x parked=%u delta=%+d get=0x%x fetch=0x%x old_word=0x%x cache=0x%x+0x%x last_cmd=0x%x suppressed=%u",
							m_diag_prev_put, put, parked, static_cast<s32>(put - m_diag_prev_put), m_fifo_pos, addr,
							ring_word(m_diag_prev_put), m_cache_addr, m_cache_size, m_cmd, std::exchange(m_diag_suppressed, 0u));
					}
				}
			}

			// TODO(excise): level 1; learn the two command-buffer slabs from the
			// producer pointer.  This is what makes the cross-buffer filter generic:
			// only hops that land inside a buffer the guest actually publishes to
			// are reported, not loops that cross an arbitrary 64 KB line.
			{
				const u32 slab = put & ~0xffffu;

				if (slab != m_diag_slab[0] && slab != m_diag_slab[1])
				{
					if (!m_diag_slab[0]) m_diag_slab[0] = slab;
					else if (!m_diag_slab[1]) m_diag_slab[1] = slab;
				}
			}

			m_diag_prev_put = put;
		}

		void FIFO_control::diag_read_ahead(u32 addr, u32 put)
		{
			const u64 now = get_system_time();

			if (m_diag_last_log && now - m_diag_last_log < 20000)
			{
				m_diag_suppressed++;
				return;
			}

			m_diag_last_log = now;
			rsx_log.warning("FIFO diag: read ahead fetch=0x%x put=0x%x delta=0x%x cross=%u get=0x%x next=0x%x remaining=%u last_cmd=0x%x suppressed=%u",
				addr, put, addr - put, u32{!!((addr ^ put) & ~0xffffu)}, m_fifo_pos, m_next_cmd_entry, m_remaining_commands, m_cmd, std::exchange(m_diag_suppressed, 0u));
		}

		void FIFO_control::diag_phase(u32 pos)
		{
			if (m_diag_retry || m_next_cmd_entry == umax || pos == m_next_cmd_entry)
			{
				m_diag_retry = false;
				return;
			}

			const u64 now = get_system_time();

			if (m_diag_last_log && now - m_diag_last_log < 20000)
			{
				m_diag_suppressed++;
				return;
			}

			m_diag_last_log = now;
			rsx_log.warning("FIFO diag: header phase slip pos=0x%x expected=0x%x delta=%d last_cmd=0x%x put=0x%x suppressed=%u",
				pos, m_next_cmd_entry, static_cast<s32>(pos - m_next_cmd_entry), m_cmd, read_put(), std::exchange(m_diag_suppressed, 0u));
		}

		void FIFO_control::diag_jump(u32 target)
		{
			const u32 put = read_put();

			// TODO(excise): level 1; keep the chain in memory, the log is too slow to
			// hold every jump of a free-running parser.
			{
				auto& entry = m_diag_jumps[m_diag_jump_next];
				entry.from = m_fifo_pos;
				entry.target = target;
				entry.put = put;
				entry.cmd = m_cmd;
				m_diag_jump_next = (m_diag_jump_next + 1) & 31;
				m_diag_jump_count++;
			}

			// Remember the last jump for the NOP-run report: a free run that starts
			// right after a jump points at the jump as the thing that landed wrong.
			m_diag_last_jump_from = m_fifo_pos;
			m_diag_last_jump_target = target;

			// EXCISE-BEGIN fifo-chunk-tail
			// TODO(excise): level 1; capture the chunk before the producer
			// recycles it.  The parked tail is already zeroed, so the methods
			// that release the producer are only visible at entry.
			static std::atomic<u32> dump_count{0};

			if ((target == 0xcb0000 || target == 0x10a0000) && put > target && put - target <= 0x10000 &&
				dump_count.fetch_add(1, std::memory_order_relaxed) < 4)
			{
				const u32 start = put - std::min<u32>(put - target, 0x400);
				std::string dump;

				for (u32 addr = start; addr <= put; addr += 4)
				{
					const u32 host = m_iotable->get_addr(addr);
					dump += fmt::format(" %08x", host == umax ? 0 : vm::_ref<be_t<u32>>(host).value());
				}

				rsx_log.warning("FIFO diag: chunk tail %x..%x%s", start, put, dump.c_str());
			}
			// EXCISE-END fifo-chunk-tail

			// TODO(excise): level 1; a jump between the guest's two command
			// buffers is invisible to every numeric test in this file: the buffer
			// the parser is not filling has the lower address, so `target <= put`
			// holds and the jump reads as "inside the frontier" when it is really
			// entering the other buffer.  This is the only unconditional record of
			// those hops; a buffer is 64 KB and the hop happens once a chunk, so it
			// is not a flood.  Capped so a desync loop cannot fill the log.
			const u32 target_slab = target & ~0xffffu;

			if (target_slab != (m_fifo_pos & ~0xffffu) &&
				(target_slab == m_diag_slab[0] || target_slab == m_diag_slab[1]) &&
				m_diag_cross_logged < 8192)
			{
				m_diag_cross_logged++;
				rsx_log.warning("FIFO diag: jump cross-buffer from=0x%x target=0x%x put=0x%x cmd=0x%x count=%u",
					m_fifo_pos, target, put, m_cmd, m_diag_cross_logged);
			}

			if (target > m_diag_max_jump)
			{
				// New high-water mark: bounds the guest's command buffer ring.
				m_diag_max_jump = target;
				rsx_log.warning("FIFO diag: jump high-water target=0x%x from=0x%x put=0x%x", target, m_fifo_pos, put);
			}

			if (target <= put || target - put < 0x10000)
			{
				return;
			}

			const u64 now = get_system_time();

			if (m_diag_last_log && now - m_diag_last_log < 20000)
			{
				m_diag_suppressed++;
				return;
			}

			m_diag_last_log = now;
			rsx_log.warning("FIFO diag: jump past PUT target=0x%x put=0x%x delta=0x%x from=0x%x cmd=0x%x suppressed=%u",
				target, put, target - put, m_fifo_pos, m_cmd, std::exchange(m_diag_suppressed, 0u));
		}

		void FIFO_control::diag_park(u32 addr, u32 put, u32 word)
		{
			// TODO(excise): level 1; a park is the parser's only stop condition.
			// Record where it stopped and what the word there was. The producer
			// publishing the next chunk is what wakes it; the word changing under
			// the parser is the handshake. Do not let a jump logged in the same
			// millisecond hide the first stop.
			m_diag_last_log = get_system_time();
			rsx_log.warning("FIFO diag: parked addr=0x%x put=0x%x word=0x%x get=0x%x get_reg=0x%x cache=0x%x+0x%x next=0x%x last_cmd=0x%x suppressed=%u",
				addr, put, word, m_fifo_pos, +m_ctrl->get, m_cache_addr, m_cache_size, m_next_cmd_entry, m_cmd, std::exchange(m_diag_suppressed, 0u));

			// TODO(excise): level 1; the tail of the chunk that parked.  The
			// producer waits on the last methods of this chunk, not on the park word.
			if (addr >= 0x100)
			{
				std::string tail;

				for (u32 i = 64; i-- > 0;)
				{
					tail += fmt::format(" %08x", ring_word(addr - (i + 1) * 4));
				}

				rsx_log.warning("FIFO diag: park tail%s", tail.c_str());
			}

			// TODO(excise): level 1; the producer moves that led here, each with the
			// word that was at the frontier it left.  If none of them had a link at
			// the old frontier, the guest never published one for this pass.
			const u32 count = std::min<u32>(m_diag_put_count, 8);

			for (u32 i = 0; i < count; i++)
			{
				const diag_put_entry& entry = m_diag_puts[(m_diag_put_count - count + i) & 7u];
				rsx_log.warning("FIFO diag: park put[%u] prev=0x%x put=0x%x old_word=0x%x new_word=0x%x get=0x%x get_reg=0x%x",
					i, entry.prev, entry.put, entry.old_word, entry.new_word, entry.get, entry.get_reg);
			}
		}

		// TODO(excise): level 1; the producer's exit link. The cursor was on
		// the producer and the word there is a live link, so the producer's
		// own way out of the chunk is being consumed. The target is the
		// producer's next command region; for a ring it is the ring start.
		void FIFO_control::diag_link(u32 addr, u32 put, u32 word)
		{
			const u64 now = get_system_time();

			if (m_diag_last_log && now - m_diag_last_log < 20000)
			{
				m_diag_suppressed++;
				return;
			}

			m_diag_last_log = now;

			u32 target = 0;

			if ((word & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD)
			{
				target = word & RSX_METHOD_OLD_JUMP_OFFSET_MASK;
			}
			else if ((word & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD)
			{
				target = word & RSX_METHOD_CALL_OFFSET_MASK;
			}

			rsx_log.warning("FIFO diag: link at PUT addr=0x%x put=0x%x word=0x%x target=0x%x get=0x%x get_reg=0x%x next=0x%x last_cmd=0x%x suppressed=%u",
				addr, put, word, target, m_fifo_pos, +m_ctrl->get, m_next_cmd_entry, m_cmd,
				std::exchange(m_diag_suppressed, 0u));
		}

		void FIFO_control::diag_resume(u32 addr, u32 put)
		{
			// TODO(excise): level 1; the parser stopped here because it had reached
			// the producer, and is now reading here again.  A resume is only safe if
			// the producer moved forward by a sane amount; a producer behind the
			// resume point, or one that jumped to another region, both mean the word
			// at addr belongs to an older pass.
			if (put > addr && put - addr <= 0x10000)
			{
				return;
			}

			const u64 now = get_system_time();

			if (m_diag_last_log && now - m_diag_last_log < 20000)
			{
				m_diag_suppressed++;
				return;
			}

			m_diag_last_log = now;
			rsx_log.warning("FIFO diag: resume at former PUT addr=0x%x put=0x%x delta=%+d cache=0x%x+0x%x next=0x%x last_cmd=0x%x jump=0x%x->0x%x suppressed=%u",
				addr, put, static_cast<s32>(put - addr), m_cache_addr, m_cache_size, m_next_cmd_entry, m_cmd,
				m_diag_last_jump_from, m_diag_last_jump_target, std::exchange(m_diag_suppressed, 0u));
		}

		void FIFO_control::diag_nop(u32 pos)
		{
			if (!m_diag_nop_run)
			{
				m_diag_nop_start = pos;
			}

			if (++m_diag_nop_run != 256)
			{
				return;
			}

			// TODO(excise): level 1; 256 consecutive NOP headers is 1 KB of words
			// with a zero command count.  That is what unwritten IO memory parses
			// as, and the NOP path has no PUT check, so this is the shape of an
			// unbounded walk.  Report the start so the entry point is visible.
			const u64 now = get_system_time();

			if (m_diag_nop_log && now - m_diag_nop_log < 20000)
			{
				m_diag_nop_suppressed++;
				return;
			}

			m_diag_nop_log = now;
			const u32 put = read_put();
			rsx_log.warning("FIFO diag: NOP run start=0x%x cur=0x%x len=%u put=0x%x delta=%+d cache=0x%x+0x%x jump=0x%x->0x%x suppressed=%u",
				m_diag_nop_start, pos, m_diag_nop_run, put, static_cast<s32>(pos - put),
				m_cache_addr, m_cache_size, m_diag_last_jump_from, m_diag_last_jump_target,
				std::exchange(m_diag_nop_suppressed, 0u));
		}

		void FIFO_control::diag_desync(u32 pos, u32 word)
		{
			// TODO(excise): level 1; dump the jump chain once per failing word.
			const u64 now = get_system_time();

			if (m_diag_dump_pos == pos && now - m_diag_dump_time < 1000000)
			{
				return;
			}

			m_diag_dump_pos = pos;
			m_diag_dump_time = now;

			const u32 put = read_put();
			rsx_log.error("FIFO diag: desync word=0x%x pos=0x%x put=0x%x delta=%+d cache=0x%x+0x%x next=0x%x nops=%u",
				word, pos, put, static_cast<s32>(pos - put), m_cache_addr, m_cache_size, m_next_cmd_entry, m_diag_nop_run);

			const u32 count = std::min<u32>(m_diag_jump_count, 32);

			for (u32 i = 0; i < count; i++)
			{
				const auto& entry = m_diag_jumps[(m_diag_jump_next + 32 - count + i) & 31];
				rsx_log.error("FIFO diag: desync jump[%u] from=0x%x target=0x%x put=0x%x cmd=0x%x", i, entry.from, entry.target, entry.put, entry.cmd);
			}
		}
		// EXCISE-END fifo-diag-impl

		std::pair<bool, u32> FIFO_control::fetch_u32(u32 addr)
		{
			if (addr - m_cache_addr < m_cache_size)
			{
				// A word cached at or past the PUT that was current when the cache
				// was filled was read ahead of the producer. If PUT has since moved
				// past it the guest may have rewritten those bytes, so drop the
				// cache and refill instead of serving a stale copy. Words below that
				// PUT cannot have changed under GET.
				if (addr < m_cache_put || read_put() <= addr)
				{
					return {true, read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], addr - m_cache_addr)};
				}

				m_cache_size = 0;
			}

			const u32 put = read_put();

			// TODO(excise): level 1; sample the producer frontier.
			diag_put_sample(put, addr);

			// R1: the cursor is on the producer. There is nothing to read.
			if (put == addr)
			{
				return {false, FIFO_EMPTY};
			}

			m_cache_addr = addr & -128;

			const u32 addr1 = translate_address(m_cache_addr);

			if (addr1 == umax)
			{
				m_cache_size = 0;
				return {false, FIFO_ERROR};
			}

			if (addr == m_next_cmd_entry)
			{
				if (u32 cmd = vm::read32(addr1 | (addr & 127)); cmd & RSX_METHOD_NON_METHOD_CMD_MASK)
				{
					// FIFO control commands (including invalid)
					// No GET update here
					m_cache_addr = addr;
					m_cache_size = 4;
					m_cache_put = put;
					// The cache is now the source for this position, so it has to
					// hold the word the caller is about to receive.  Leaving the
					// stale bytes in place made a later read at the same position
					// (a retry, or the desync probe) return the previous command.
					const be_t<u32> raw = cmd;
					std::memcpy(+m_cache[0], &raw, sizeof(raw));
					return {true, cmd};
				}
			}

			// R2/R3: while the cursor is behind PUT only the published range
			// enters the cache. When it is ahead (a jump target the producer has
			// not reached), the region is walked exactly as hardware walks it;
			// dispatch is gated by the whole-packet rule in read(), not by a
			// per-stream frontier.
			const u32 cache_end = (addr < put) ? put : (addr + sizeof(m_cache));

			m_cache_size = std::min<u32>((cache_end | 0x7f) - m_cache_addr, u32{sizeof(m_cache)} - 1) + 1;

			if (0x100000 - (m_cache_addr & 0xfffff) < m_cache_size)
			{
				// Check if memory layout changes in the next 1MB page boundary
				if ((addr1 >> 20) + 1 != (translate_address(m_cache_addr + 0x100000) >> 20))
				{
					// Trim cache as needed if memory layout changes
					m_cache_size = 0x100000 - (m_cache_addr & 0xfffff);
				}
			}

			m_cache_put = put;

			// Make mask of cache lines to fetch
			u8 to_fetch = static_cast<u8>((1u << (m_cache_size / 128)) - 1);

			// Atomic FIFO debug options
			const bool force_cache_fill = g_cfg.core.rsx_fifo_accuracy == rsx_fifo_mode::atomic_ordered;
			const bool strict_fetch_ordering = g_cfg.core.rsx_fifo_accuracy >= rsx_fifo_mode::atomic_ordered;

			rsx::reservation_lock<true, 1> rsx_lock(addr1, m_cache_size, true);
			const auto src = vm::_ptr<spu_rdata_t>(addr1);

			u64 start_time = 0;
			// EXCISE-BEGIN fifo-stall-state
			u64 stall_start = 0;
			bool stall_logged = false;
			// EXCISE-END fifo-stall-state
			u32 bytes_read = 0;

			u32 fifo_code_path = m_next_cmd_entry;
			ensure(m_next_cmd_entry >= m_fifo_pos && m_next_cmd_entry < m_fifo_pos + 8192);

			// Find the next set bit after every iteration
			for (int i = 0;; i = (std::countr_zero<u32>(std::rotl<u8>(to_fetch, 0 - i - 1)) + i + 1) % 8)
			{
				// If a reservation is being updated, try to load another
				const auto& res = vm::reservation_acquire(addr1 + i * 128);
				const u64 time0 = res;

				if (!(time0 & 127))
				{
					mov_rdata(m_cache[i], src[i]);

					if (time0 == res && cmp_rdata(m_cache[i], src[i]))
					{
						// The fetch of the cache line content has been successful, unset its bit
						to_fetch &= ~(1u << i);

						if ((to_fetch & ((1u << i) - 1)) == 0)
						{
							const u32 highest_clear = std::min<u32>(m_cache_size, to_fetch ? std::bit_width<u8>(to_fetch & ~(to_fetch - 1)) * 128 - 128 : bytes_read + 128);

							ensure(highest_clear <= 1024);

							const u32 high_addr = m_cache_addr + highest_clear;

							while (fifo_code_path < high_addr)
							{
								const auto cmd = read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], fifo_code_path - m_cache_addr);

								if (cmd & RSX_METHOD_NON_METHOD_CMD_MASK) [[unlikely]]
								{
									// FIFO control commands (including invalid)
									fifo_code_path += 4;
									break;
								}

								const u32 count = (cmd >> 18) & 0x7ff;
								fifo_code_path += count * 4 + 4;
							}


							if (fifo_code_path < high_addr)
							{
								// Cache trimmed due to FIFO control command
								m_cache_size = std::max<u32>(fifo_code_path, addr + 4) - m_cache_addr;
								break;
							}
						}

						if (!to_fetch)
						{
							break;
						}

						bytes_read += 128;
						continue;
					}
				}

				if (!start_time)
				{
					if (bytes_read >= 256 && !force_cache_fill)
					{
						// Most of the data seems to be in cache already, don't over-wait
						break;
					}

					start_time = get_system_time();
					// EXCISE-BEGIN fifo-stall-start
					stall_start = start_time;
					// EXCISE-END fifo-stall-start
				}

				auto now = get_system_time();

				// EXCISE-BEGIN fifo-stall-log
				if (stall_start && !stall_logged && now - stall_start >= 1000000)
				{
					// TODO(excise): level 1; a fill that cannot commit for a
					// second is a parser stall, not a slow producer.
					stall_logged = true;
					rsx_log.warning("FIFO diag: fetch wait addr=0x%x put=0x%x cache=0x%x+0x%x to_fetch=0x%x bytes=%u next=0x%x",
						addr, put, m_cache_addr, m_cache_size, to_fetch, bytes_read, m_next_cmd_entry);
				}
				// EXCISE-END fifo-stall-log

				if (now - start_time >= 50u)
				{
					if (m_thread->is_stopped())
					{
						return {};
					}

					m_thread->cpu_wait({});

					const auto then = std::exchange(now, get_system_time());
					start_time = now;
					m_thread->performance_counters.idle_time += now - then;
				}
				else
				{
					busy_wait(200);
				}

				if (strict_fetch_ordering)
				{
					i = (i - 1) % 8;
				}
			}

			// Update FIFO GET
			sync_get();
			atomic_fence_seq_cst();

			ensure(addr < m_cache_addr + m_cache_size);
			const auto ret = read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], addr - m_cache_addr);
			return {true, ret};
		}

		bool FIFO_control::peek_cache(u32 pos, u32& word) const
		{
			if (pos - m_cache_addr >= m_cache_size)
			{
				return false;
			}

			word = read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], pos - m_cache_addr);
			return true;
		}

		void FIFO_control::set_get(u32 get, u32 spin_cmd)
		{
			invalidate_cache();

			m_thread->last_code_jump = m_fifo_pos;
			m_next_cmd_entry = get;

			if (spin_cmd && m_fifo_pos == get)
			{
				m_memwatch_addr = get;
				m_memwatch_cmp = spin_cmd;
				m_ctrl->get.release(get);
				return;
			}

			// Update ctrl registers
			m_ctrl->get.release(m_fifo_pos = get);
			m_remaining_commands = 0;
		}

		std::span<const u32> FIFO_control::get_current_arg_ptr(u32 length_in_words) const
		{
			if (!length_in_words)
			{
				// This means the caller is doing something stupid
				rsx_log.error("Invalid access to FIFO args data, requested length = 0");
				return {};
			}

			if (g_cfg.core.rsx_fifo_accuracy)
			{
				// Return a pointer to the cache storage with confined access
				const u32 cache_offset_in_words = (m_fifo_pos - m_cache_addr) / 4;
				const u32 cache_size_in_words = m_cache_size / 4;
				return {reinterpret_cast<const u32*>(&m_cache) + cache_offset_in_words, cache_size_in_words - cache_offset_in_words};
			}

			// Return a raw pointer to contiguous memory
			constexpr u32 _1M = 0x100000;
			const u32 size = length_in_words * sizeof(u32);
			const u32 from = translate_address(m_fifo_pos);

			for (u32 remaining = size, addr = m_fifo_pos, ptr = from; remaining > 0;)
			{
				const u32 next_block = utils::align(addr + 1, _1M);
				const u32 available = (next_block - addr);
				if (remaining <= available)
				{
					return { static_cast<const u32*>(vm::base(from)), length_in_words };
				}

				remaining -= available;
				const u32 next_ptr = translate_address(next_block);
				if (next_ptr != (ptr + available))
				{
					return { static_cast<const u32*>(vm::base(from)), (size - remaining) / sizeof(u32)};
				}

				ptr = next_ptr;
				addr = next_block;
			}

			fmt::throw_exception("Unreachable");
		}

		bool FIFO_control::read_unsafe(register_pair& data)
		{
			// Fast read with no processing, only safe inside a PACKET_BEGIN+count block
			if (m_remaining_commands)
			{
				bool ok{};
				u32 arg = 0;

				m_fifo_pos += 4;

				if (g_cfg.core.rsx_fifo_accuracy) [[ unlikely ]]
				{
					std::tie(ok, arg) = fetch_u32(m_fifo_pos);

					if (!ok)
					{
						if (arg == FIFO_ERROR)
						{
							m_thread->recover_fifo();
						}
						else
						{
							m_fifo_pos -= 4;
						}

						return false;
					}
				}
				else
				{
					if (m_fifo_pos == read_put<false>())
					{
						m_fifo_pos -= 4;
						return false;
					}

					m_args_ptr += 4;
					arg = vm::read32(m_args_ptr);
				}

				m_command_reg += m_command_inc;

				--m_remaining_commands;

				data.set(m_command_reg, arg);
				return true;
			}

			m_fifo_pos += 4;
			return false;
		}

		// Optimization for methods which can be batched together
		// Beware, can be easily misused
		bool FIFO_control::skip_methods(u32 count)
		{
			if (m_remaining_commands > count)
			{
				m_command_reg += m_command_inc * count;
				m_remaining_commands -= count;
				m_fifo_pos += 4 * count;
				m_args_ptr += 4 * count;
				return true;
			}

			m_fifo_pos += 4 * m_remaining_commands;
			m_remaining_commands = 0;
			return false;
		}

		void FIFO_control::abort()
		{
			m_remaining_commands = 0;
		}

		void FIFO_control::read(register_pair& data)
		{
			if (m_remaining_commands)
			{
				// Previous block aborted to wait for PUT pointer
				if (!read_unsafe(data))
				{
					// failed reads leave data unchanged
					data.reg = FIFO_EMPTY;
				}

				return;
			}

			if (m_memwatch_addr)
			{
				if (m_fifo_pos == m_memwatch_addr)
				{
					if (const u32 addr = translate_address(m_memwatch_addr); addr + 1)
					{
						if (vm::read32(addr) == m_memwatch_cmp)
						{
							// Still spinning in place
							data.reg = FIFO_EMPTY;
							return;
						}
					}
				}

				m_memwatch_addr = 0;
				m_memwatch_cmp = 0;
			}

			const u32 put = read_put();

			// R1: the cursor is on the producer. PUT is exclusive and there is no
			// exception: a word at that position is not published, so the parser
			// idles.
			//
			// A word there can *look* like a link - a stale wrap jump, or an
			// argument whose bits happen to match the jump encoding - and acting
			// on it is the 2026-11-11 regression: Demon's Souls parked at
			// 0x3f454 with 0x20040000 in the slot, the parser read it as
			// OLD_JUMP|0x40000, walked unwritten memory to 0x600000, read
			// 0x39300000 there and desynced at 0x19300000. The producer's exit
			// link is readable only once PUT has moved past it (or the producer
			// has rewound); the normal read path handles it then.
			if (put == m_fifo_pos)
			{
				// EXCISE-BEGIN fifo-park-word
				const u32 word = ring_word(m_fifo_pos);

				if (m_diag_parked_put != m_fifo_pos)
				{
					// TODO(excise): level 1; new park, remember the word.
					diag_park(m_fifo_pos, put, word);
					m_diag_park_word = word;
				}
				else if (word != m_diag_park_word)
				{
					// TODO(excise): level 1; the producer wrote the park word
					// while the parser waited. This is the healthy wake-up.
					rsx_log.warning("FIFO diag: park word change addr=0x%x old=0x%x new=0x%x put=0x%x",
						m_fifo_pos, m_diag_park_word, word, put);

					m_diag_park_word = word;
				}

				m_diag_parked_put = m_fifo_pos;
				// EXCISE-END fifo-park-word

				data.reg = FIFO_EMPTY;
				return;
			}

			// EXCISE-BEGIN fifo-resume
			m_diag_parked_put = umax;
			// EXCISE-END fifo-resume

			// R2/R3: read at the cursor whether it is behind PUT (published) or
			// ahead of it (a jump target the producer has not reached yet). There
			// is no per-stream frontier; PUT gates whole-packet dispatch below.
			if (!g_cfg.core.rsx_fifo_accuracy) [[ likely ]]
			{
				if (m_fifo_pos > put)
				{
					// EXCISE-BEGIN fifo-read-ahead
					diag_read_ahead(m_fifo_pos, put);
					// EXCISE-END fifo-read-ahead
				}

				if (const u32 addr = translate_address(m_fifo_pos); addr + 1)
				{
					m_cmd = vm::read32(addr);
				}
				else
				{
					data.reg = FIFO_ERROR;
					return;
				}
			}
			else
			{
				// TODO(excise): level 1; the parser is about to read a header, so its
				// position must match the boundary the previous command predicted.
				diag_phase(m_fifo_pos);

				if (auto [ok, arg] = fetch_u32(m_fifo_pos); ok)
				{
					m_cmd = arg;
				}
				else
				{
					data.reg = arg;
					return;
				}
			}

			if (m_cmd & RSX_METHOD_NON_METHOD_CMD_MASK) [[unlikely]]
			{
				m_next_cmd_entry = umax;

				if ((m_cmd & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD ||
					(m_cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_NEW_JUMP_CMD ||
					(m_cmd & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD ||
					(m_cmd & RSX_METHOD_RETURN_MASK) == RSX_METHOD_RETURN_CMD)
				{
					// TODO(excise): level 1; a jump ends any NOP run.
					m_diag_nop_run = 0;

					// Flow control, stop reading
					data.reg = m_cmd;
					return;
				}

				// TODO(excise): level 1; the data word that broke the parse.
				diag_desync(m_fifo_pos, m_cmd);

				// Malformed command, optional recovery
				data.reg = FIFO_ERROR;
				return;
			}

			ensure(!m_remaining_commands);
			const u32 count = (m_cmd >> 18) & 0x7ff;

			m_next_cmd_entry = m_fifo_pos + count * 4 + 4;

			// R2: dispatch a packet only when all of it is published. While the
			// cursor is behind PUT, a packet that ends past PUT is a mid-fill:
			// leave the header in place and wait. A cursor ahead of PUT is in a
			// region the guest jumped to before publishing it (R3); that region
			// is walked as hardware walks it and is not gated.
			if (!m_upstream_semantics && m_fifo_pos < put && m_next_cmd_entry > put)
			{
				// Nothing of the packet was consumed. Keep the boundary on the
				// header so the retry is not mistaken for a phase slip.
				m_next_cmd_entry = m_fifo_pos;
				data.reg = FIFO_EMPTY;
				return;
			}

			if (!count)
			{
				// TODO(excise): level 1; the count-0 path advances without any PUT
				// check, so this is the only place a walk through unwritten memory
				// is visible as such.
				diag_nop(m_fifo_pos);

				m_fifo_pos += 4;
				sync_get();
				data.reg = FIFO_NOP;
				return;
			}

			// TODO(excise): level 1; a real method ends any NOP run.
			m_diag_nop_run = 0;

			if (count > 1)
			{
				// Set up readback parameters
				m_command_reg = m_cmd & 0xfffc;
				m_command_inc = ((m_cmd & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD) ? 0 : 4;
				m_remaining_commands = count - 1;
			}

			if (g_cfg.core.rsx_fifo_accuracy)
			{
				m_fifo_pos += 4;

				auto [ok, arg] = fetch_u32(m_fifo_pos);

				if (!ok)
				{
					// Optional recovery
					if (arg == FIFO_ERROR)
					{
						data.reg = FIFO_ERROR;
					}
					else
					{
						// The first argument has not been published yet.  Nothing of
						// this command has been consumed, so put the position back on
						// the header and let the next read() parse the command again.
						// read_unsafe() advances before it fetches: leaving the
						// position on the argument and raising the remaining count
						// made the retry read the second argument first, shift the
						// whole argument block by one word and swallow the next
						// header.  The stream then parsed an argument as a command.
						m_fifo_pos -= 4;
						m_remaining_commands = 0;
						m_diag_retry = true; // TODO(excise): level 1; not a phase slip
						data.reg = FIFO_EMPTY;
					}

					return;
				}

				data.set(m_cmd & 0xfffc, arg);
				return;
			}

			inc_get(true); // Wait for data block to become available

			// Validate the args ptr if the command attempts to read from it
			m_args_ptr = translate_address(m_fifo_pos);
			if (m_args_ptr == umax) [[unlikely]]
			{
				// Optional recovery
				data.reg = FIFO_ERROR;
				return;
			}

			data.set(m_cmd & 0xfffc, vm::read32(m_args_ptr));
		}

		void flattening_helper::reset(bool _enabled)
		{
			enabled = _enabled;
			num_collapsed = 0;
			in_begin_end = false;
		}

		void flattening_helper::force_disable()
		{
			if (enabled)
			{
				rsx_log.warning("FIFO optimizations have been disabled as the application is not compatible with per-frame analysis");

				reset(false);
				fifo_hint = optimization_hint::application_not_compatible;
			}
		}

		void flattening_helper::evaluate_performance(u32 total_draw_count)
		{
			if (!enabled)
			{
				if (fifo_hint == optimization_hint::application_not_compatible)
				{
					// Not compatible, do nothing
					return;
				}

				if (total_draw_count <= 2000)
				{
					// Low draw call pressure
					fifo_hint = optimization_hint::load_low;
					return;
				}

				if (fifo_hint == optimization_hint::load_unoptimizable)
				{
					// Nope, wait for stats to change
					return;
				}
			}

			if (enabled)
			{
				// Currently activated. Check if there is any benefit
				if (num_collapsed < 500)
				{
					// Not worth it, disable
					enabled = false;
					fifo_hint = load_unoptimizable;
				}

				u32 real_total = total_draw_count + num_collapsed;
				if (real_total <= 2000)
				{
					// Low total number of draws submitted, no need to keep trying for now
					enabled = false;
					fifo_hint = load_low;
				}

				reset(enabled);
			}
			else
			{
				// Not enabled, check if we should try enabling
				ensure(total_draw_count > 2000);
				if (fifo_hint != load_unoptimizable)
				{
					// If its set to unoptimizable, we already tried and it did not work
					// If it resets to load low (usually after some kind of loading screen) we can try again
					ensure(in_begin_end == false); // "Incorrect initial state"
					ensure(num_collapsed == 0);
					enabled = true;
				}
			}
		}

		flatten_op flattening_helper::test(register_pair& command)
		{
			u32 flush_cmd = ~0u;
			switch (const u32 reg = (command.reg >> 2))
			{
			case NV4097_SET_BEGIN_END:
			{
				in_begin_end = !!command.value;

				if (command.value)
				{
					// This is a BEGIN call
					if (!deferred_primitive) [[likely]]
					{
						// New primitive block
						deferred_primitive = command.value;
					}
					else if (deferred_primitive == command.value)
					{
						// Same primitive can be chanined; do nothing
						command.reg = FIFO_DISABLED_COMMAND;
					}
					else
					{
						// Primitive command has changed!
						// Flush
						flush_cmd = command.value;
					}
				}
				else if (deferred_primitive)
				{
					command.reg = FIFO_DRAW_BARRIER;
					draw_count++;
				}
				else
				{
					rsx_log.error("Fifo flattener misalignment, disable FIFO reordering and report to developers");
					in_begin_end = false;
					flush_cmd = 0u;
				}

				break;
			}
			case NV4097_DRAW_ARRAYS:
			case NV4097_DRAW_INDEX_ARRAY:
			{
				// TODO: Check type
				break;
			}
			default:
			{
				if (draw_count) [[unlikely]]
				{
					if (m_register_properties[reg] & register_props::always_ignore) [[unlikely]]
					{
						// Always ignore
						command.reg = FIFO_DISABLED_COMMAND;
					}
					else
					{
						// Flush
						flush_cmd = (in_begin_end) ? deferred_primitive : 0u;
					}
				}
				else
				{
					// Nothing to do
					return NOTHING;
				}

				break;
			}
			}

			if (flush_cmd != ~0u)
			{
				num_collapsed += draw_count? (draw_count - 1) : 0;
				draw_count = 0;
				deferred_primitive = flush_cmd;

				return in_begin_end ? EMIT_BARRIER : EMIT_END;
			}

			return NOTHING;
		}
	}

	void thread::run_FIFO()
	{
		FIFO::register_pair command;
		fifo_ctrl->read(command);
		const auto cmd = command.reg;

		if (cmd & (0xffff0000 | RSX_METHOD_NON_METHOD_CMD_MASK)) [[unlikely]]
		{
			// Check for special FIFO commands
			switch (cmd)
			{
			case FIFO::FIFO_NOP:
			{
				if (performance_counters.state == FIFO::state::running)
				{
					performance_counters.FIFO_idle_timestamp = get_system_time();
					performance_counters.state = FIFO::state::nop;
				}

				return;
			}
			case FIFO::FIFO_EMPTY:
			{
				if (performance_counters.state == FIFO::state::running)
				{
					performance_counters.FIFO_idle_timestamp = get_system_time();
					performance_counters.state = FIFO::state::empty;
				}
				else
				{
					std::this_thread::yield();
				}

				return;
			}
			case FIFO::FIFO_BUSY:
			{
				// Do something else
				return;
			}
			case FIFO::FIFO_ERROR:
			{
				rsx_log.error("FIFO error: possible desync event (last cmd = 0x%x)", get_fifo_cmd());
				recover_fifo();
				return;
			}
			}

			// Check for flow control
			if (bit_set<2> jump_type; jump_type
				.set_unsafe(0, (cmd & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD)
				.set_unsafe(1, (cmd & RSX_METHOD_NEW_JUMP_CMD_MASK) == RSX_METHOD_NEW_JUMP_CMD)
				.any())
			{
				const u32 offs = cmd & (jump_type.test_unsafe(0) ? RSX_METHOD_OLD_JUMP_OFFSET_MASK : RSX_METHOD_NEW_JUMP_OFFSET_MASK);
				if (offs == fifo_ctrl->get_pos())
				{
					//Jump to self. Often preceded by NOP
					if (performance_counters.state == FIFO::state::running)
					{
						performance_counters.FIFO_idle_timestamp = get_system_time();
						sync_point_request.release(true);
					}

					performance_counters.state = FIFO::state::spinning;
				}
				else
				{
					last_known_code_start = offs;
				}

				// TODO(excise): level 1; a jump is the only way the parser reaches a
				// position the producer has not published, so its target is checked
				// against PUT here rather than after the next stale read.
				fifo_ctrl->diag_jump(offs);

				//rsx_log.warning("rsx jump(0x%x) #addr=0x%x, cmd=0x%x, get=0x%x, put=0x%x", offs, m_ioAddress + get, cmd, get, put);
				fifo_ctrl->set_get(offs, cmd);
				return;
			}
			if ((cmd & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD)
			{
				const u32 offs = cmd & RSX_METHOD_CALL_OFFSET_MASK;
				// Hardware has one return-address register, and every CALL
				// overwrites it: the outer caller's frame is lost, which is what
				// the guest's trampoline path relies on (the command chunk it
				// walks ends with a JUMP, not a RET, so the outer return is
				// never used). A nested CALL is therefore legal, not a fault
				// (L149).
				fifo_ret_addr = fifo_ctrl->get_pos() + 4;
				fifo_ctrl->diag_jump(offs); // TODO(excise): level 1
				fifo_ctrl->set_get(offs);
				last_known_code_start = offs;
				return;
			}
			if ((cmd & RSX_METHOD_RETURN_MASK) == RSX_METHOD_RETURN_CMD)
			{
				if (fifo_ret_addr == RSX_CALL_STACK_EMPTY)
				{
					rsx_log.error("FIFO: RET found without corresponding CALL (last cmd = 0x%x)", get_fifo_cmd());
					recover_fifo();
					return;
				}

				// Optimize returning to another CALL
				if ((ctrl->put & ~3) != fifo_ret_addr)
				{
					if (u32 addr = fifo_ctrl->translate_address(fifo_ret_addr); addr != umax)
					{
						const u32 cmd0 = vm::read32(addr);

						// Check for missing step flags, in case the user is single-stepping in the debugger
						if ((cmd0 & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD && cpu_flag::dbg_step - state)
						{
							fifo_ctrl->set_get(cmd0 & RSX_METHOD_CALL_OFFSET_MASK);
							last_known_code_start = ctrl->get;
							fifo_ret_addr += 4;
							return;
						}
					}
				}

				fifo_ctrl->set_get(std::exchange(fifo_ret_addr, RSX_CALL_STACK_EMPTY));
				last_known_code_start = fifo_ctrl->get_pos();
				return;
			}

			// If we reached here, this is likely an error
			fmt::throw_exception("Unexpected command 0x%x (last cmd: 0x%x)", cmd, fifo_ctrl->last_cmd());
		}

		if (const auto state = performance_counters.state;
			state != FIFO::state::running)
		{
			performance_counters.state = FIFO::state::running;

			// Hack: Delay FIFO wake-up according to setting
			// NOTE: The typical spin setup is a NOP followed by a jump-to-self
			// NOTE: There is a small delay when the jump address is dynamically edited by cell
			if (state != FIFO::state::nop)
			{
				fifo_wake_delay();
			}

			// Update performance counters with time spent in idle mode
			performance_counters.idle_time += (get_system_time() - performance_counters.FIFO_idle_timestamp);
		}

		do
		{
			if (capture_current_frame) [[unlikely]]
			{
				const u32 reg = (command.reg & 0xfffc) >> 2;
				const u32 value = command.value;

				frame_debug.command_queue.emplace_back(reg, value);

				if (!(reg == NV406E_SET_REFERENCE || reg == NV406E_SEMAPHORE_RELEASE || reg == NV406E_SEMAPHORE_ACQUIRE))
				{
					// todo: handle nv406e methods better?, do we care about call/jumps?
					rsx::frame_capture_data::replay_command replay_cmd;
					replay_cmd.rsx_command = std::make_pair((reg << 2) | (1u << 18), value);

					auto& commands = frame_capture.replay_commands;
					commands.push_back(replay_cmd);

					switch (reg)
					{
					case NV3089_IMAGE_IN:
						capture::capture_image_in(this, commands.back());
						break;
					case NV0039_BUFFER_NOTIFY:
						capture::capture_buffer_notify(this, commands.back());
						break;
					default:
					{
						static constexpr std::array<std::pair<u32, u32>, 3> ranges
						{{
							{NV308A_COLOR, 0x700},
							{NV4097_SET_TRANSFORM_PROGRAM, 32},
							{NV4097_SET_TRANSFORM_CONSTANT, 32}
						}};

						// Use legacy logic - enqueue leading command with count
						// Then enqueue each command arg alone with a no-op command
						for (const auto& range : ranges)
						{
							if (reg >= range.first && reg < range.first + range.second)
							{
								const u32 remaining = std::min<u32>(fifo_ctrl->get_remaining_args_count() + 1,
									(fifo_ctrl->last_cmd() & RSX_METHOD_NON_INCREMENT_CMD_MASK) ? -1 : (range.first + range.second) - reg);

								commands.back().rsx_command.first = (fifo_ctrl->last_cmd() & RSX_METHOD_NON_INCREMENT_CMD_MASK) | (reg << 2) | (remaining << 18);

								for (u32 i = 1; i < remaining && fifo_ctrl->get_pos() + i * 4 != (ctrl->put & ~3); i++)
								{
									replay_cmd.rsx_command = std::make_pair(0, vm::read32(fifo_ctrl->translate_address(fifo_ctrl->get_pos()) + (i * 4)));

									commands.push_back(replay_cmd);
								}

								break;
							}
						}

						break;
					}
					}
				}
			}

			if (m_flattener.is_enabled()) [[unlikely]]
			{
				switch(m_flattener.test(command))
				{
				case FIFO::NOTHING:
				{
					break;
				}
				case FIFO::EMIT_END:
				{
					// Emit end command to close existing scope
					AUDIT(m_ctx->in_begin_end);
					methods[NV4097_SET_BEGIN_END](m_ctx, NV4097_SET_BEGIN_END, 0);
					break;
				}
				case FIFO::EMIT_BARRIER:
				{
					AUDIT(m_ctx->in_begin_end);
					methods[NV4097_SET_BEGIN_END](m_ctx, NV4097_SET_BEGIN_END, 0);
					methods[NV4097_SET_BEGIN_END](m_ctx, NV4097_SET_BEGIN_END, m_flattener.get_primitive());
					break;
				}
				default:
				{
					fmt::throw_exception("Unreachable");
				}
				}

				if (command.reg == FIFO::FIFO_DISABLED_COMMAND)
				{
					// Optimized away
					continue;
				}
			}

			const u32 reg = (command.reg & 0xffff) >> 2;
			const u32 value = command.value;

			m_ctx->register_state->decode(reg, value);

			const bool value_changed = m_ctx->register_state->latch != value;
			if (value_changed)
			{
				// Mark the decoded draw state that depends on this register. Applies to method registers too.
				m_ctx->draw_state.pending |= draw_state_signals[reg];
			}

			// TODO(excise): level 1; the dispatch history is a diagnostic: a desync
			// capture prints it, so the command that precedes the corruption is
			// visible.  The ring is written on every command, so it must stay cheap.
			note_dispatch(reg, value, fifo_ctrl->get_pos(), fifo_ctrl->last_cmd());

			if (auto method = methods[reg])
			{
				method(m_ctx, reg, value);

				if (state & cpu_flag::again)
				{
					m_ctx->register_state->decode(reg, m_ctx->register_state->latch);
					break;
				}
			}
			else if (value_changed)
			{
				// Something changed, set signal flags if any specified
				m_ctx->graphics_state |= state_signals[reg];
			}
		}
		while (fifo_ctrl->read_unsafe(command));

		fifo_ctrl->sync_get();
	}
}
