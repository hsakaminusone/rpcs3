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

namespace rsx
{
	namespace FIFO
	{
		FIFO_control::FIFO_control(::rsx::thread* pctrl)
		{
			m_thread = pctrl;
			m_ctrl = pctrl->ctrl;
			m_iotable = &pctrl->iomap_table;
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

			// A serialized parser position is not tied to a stream we know, so
			// fall back to the linear rule until the next jump recreates them.
			reset_streams(m_fifo_pos);
		}

		void FIFO_control::set_channel(RsxDmaControl* ctrl)
		{
			invalidate_cache();

			m_ctrl = ctrl;
			m_memwatch_addr = 0;
			m_memwatch_cmp = 0;
			m_remaining_commands = 0;

			// Command streams belong to the channel that published them. Start
			// the new channel on the linear rule; its jumps recreate the rest.
			reset_streams(ctrl->get);
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
		u32 FIFO_control::peek_word(u32 addr) const
		{
			const u32 host = m_iotable->get_addr(addr & -128);
			return host == umax ? 0 : +vm::read32(host | (addr & 127));
		}

		bool FIFO_control::is_valid_link(u32 word) const
		{
			if (!stream_tracker::is_link(word))
			{
				return false;
			}

			// A data word that ends in 0b10 passes the CALL mask, so the mask
			// alone does not make a link.  The target must name mapped memory:
			// following one into unmapped memory turns a park into a desync storm
			// (the 0xbd192712 / 0x1d192710 capture).  A RETURN goes to the saved
			// call address, which the parser validates itself.
			if ((word & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD)
			{
				return translate_address(word & RSX_METHOD_CALL_OFFSET_MASK) != umax;
			}

			if ((word & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD)
			{
				return translate_address(word & RSX_METHOD_OLD_JUMP_OFFSET_MASK) != umax;
			}

			return true;
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
				entry.old_word = peek_word(m_diag_prev_put);
				entry.new_word = peek_word(put);
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
							peek_word(m_diag_prev_put), m_cache_addr, m_cache_size, m_cmd, std::exchange(m_diag_suppressed, 0u));
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
			rsx_log.warning("FIFO diag: fetch at/after PUT fetch=0x%x put=0x%x delta=0x%x cross=%u get=0x%x next=0x%x remaining=%u last_cmd=0x%x suppressed=%u",
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

		void FIFO_control::diag_park(u32 addr, u32 frontier, u32 put, u32 word)
		{
			// TODO(excise): level 1; a park is the parser's only stop condition.
			// Record where it stopped and what the word there was: a data word at
			// the frontier is normal, a missing link is not.  The caller already
			// guards on a new park address; do not let a jump logged in the same
			// millisecond hide the first stop.
			m_diag_last_log = get_system_time();
			rsx_log.warning("FIFO diag: parked addr=0x%x frontier=0x%x put=0x%x word=0x%x in_call=%d put_known=%d get=0x%x get_reg=0x%x cache=0x%x+0x%x next=0x%x last_cmd=0x%x suppressed=%u",
				addr, frontier, put, word, m_streams.in_call() ? 1 : 0, m_streams.is_known(put) ? 1 : 0,
				m_fifo_pos, +m_ctrl->get, m_cache_addr, m_cache_size, m_next_cmd_entry, m_cmd, std::exchange(m_diag_suppressed, 0u));

			// TODO(excise): level 1; the tail of the chunk that parked.  The
			// producer waits on the last methods of this chunk, not on the park word.
			if (addr >= 0x100)
			{
				std::string tail;

				for (u32 i = 64; i-- > 0;)
				{
					tail += fmt::format(" %08x", peek_word(addr - (i + 1) * 4));
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

		void stream_tracker::reset(u32 base)
		{
			m_count = 0;
			m_current = 0;
			m_pending_next = 0;
			m_pending_count = 0;
			m_saved = 0;
			m_in_call = false;
			m_call_linear = false;

			std::memset(m_pending, 0, sizeof(m_pending));

			add(base, base, true);
		}

		void stream_tracker::reset_at(u32 base)
		{
			m_pending_next = 0;
			m_pending_count = 0;
			m_saved = 0;
			m_in_call = false;
			m_call_linear = false;

			std::memset(m_pending, 0, sizeof(m_pending));

			u32 index;

			if (find(base, index))
			{
				// Static code and the boot stream are linear; anything already
				// known keeps the frontier the producer last published for it.
				m_current = index;
				return;
			}

			add(base, base, true);
		}

		bool stream_tracker::is_known(u32 addr) const
		{
			u32 index;
			return find(addr, index);
		}

		bool stream_tracker::find(u32 addr, u32& index) const
		{
			bool found = false;

			for (u32 i = 0; i < m_count; i++)
			{
				const entry& stream = m_entries[i];

				if (addr < stream.base || addr - stream.base > max_put_gap)
				{
					continue;
				}

				if (!found || stream.base > m_entries[index].base)
				{
					index = i;
					found = true;
				}
			}

			return found;
		}

		u32 stream_tracker::min_base() const
		{
			u32 base = umax;

			for (u32 i = 0; i < m_count; i++)
			{
				base = std::min(base, m_entries[i].base);
			}

			return base;
		}

		void stream_tracker::add(u32 base, u32 frontier, bool linear)
		{
			u32 index;

			if (m_count < max_streams)
			{
				index = m_count++;
			}
			else
			{
				// Recycle the lowest non-current stream. The boot stream is the
				// usual victim and it never comes back once the guest moves on.
				index = 0;

				for (u32 i = 1; i < max_streams; i++)
				{
					if (i == m_current)
					{
						continue;
					}

					if (index == m_current || m_entries[i].base < m_entries[index].base)
					{
						index = i;
					}
				}
			}

			m_entries[index].base = base;
			m_entries[index].frontier = frontier;
			m_entries[index].linear = linear;
			m_current = index;
		}

		bool stream_tracker::is_link(u32 word)
		{
			return (word & RSX_METHOD_NON_METHOD_CMD_MASK) &&
				((word & RSX_METHOD_OLD_JUMP_CMD_MASK) == RSX_METHOD_OLD_JUMP_CMD ||
				 (word & RSX_METHOD_CALL_CMD_MASK) == RSX_METHOD_CALL_CMD ||
				 (word & RSX_METHOD_RETURN_MASK) == RSX_METHOD_RETURN_CMD);
		}

		void stream_tracker::note_put(u32 put, u32 pos)
		{
			u32 index;

			if (find(put, index))
			{
				u32 current;

				// Never move a frontier behind a parser that is already past it:
				// the buffer was recycled under the parser and this write is not
				// for the pass it is reading.
				if (!find(pos, current) || index != current || put >= pos)
				{
					m_entries[index].frontier = put;
				}

				return;
			}

			// PUT names a stream the parser has not entered yet. Hold it until the
			// jump that enters that stream arrives.
			if (m_pending_count && m_pending[(m_pending_next + max_streams - 1) % max_streams] == put)
			{
				return;
			}

			m_pending[m_pending_next % max_streams] = put;
			m_pending_next++;
			m_pending_count = std::min<u32>(m_pending_count + 1, max_streams);
		}

		u32 stream_tracker::frontier(u32 put, u32 addr) const
		{
			if (m_call_linear)
			{
				// A subroutine in static memory has no stream of its own and is
				// fully published by the time the call executes.
				return put;
			}

			u32 index;

			if (find(addr, index))
			{
				// A linear stream follows PUT, but PUT can move backwards when
				// the guest wraps its ring. The parser must not rewind with it:
				// everything the stored frontier already covered is still
				// published, so keep the larger of the two.
				return m_entries[index].linear ? std::max(put, m_entries[index].frontier) : m_entries[index].frontier;
			}

			return put;
		}

		void stream_tracker::on_jump(u32 target, u32 put)
		{
			u32 index;

			if (find(target, index))
			{
				m_current = index;
				return;
			}

			u32 frontier = target;

			// A PUT for this buffer may have arrived before its first jump did.
			for (u32 i = m_pending_count; i-- > 0;)
			{
				const u32 slot = (m_pending_next - 1 - i) % max_streams;
				const u32 pending = m_pending[slot];

				if (pending && pending >= target && pending - target <= max_put_gap)
				{
					frontier = pending;
					m_pending[slot] = 0;
					break;
				}
			}

			if (frontier == target)
			{
				if (put >= target && put - target <= max_put_gap)
				{
					frontier = put;
				}
				else if (target < min_base())
				{
					// Static code below every command stream: it is not published
					// incrementally, so follow PUT exactly as the linear parser did.
					add(target, put, true);
					return;
				}
				else if (target > put && target - put > max_put_gap)
				{
					// A region above every command stream that the producer is not
					// filling: static code, or the zero-filled park the guest jumps
					// to while it prepares the next chunk (Ridge Racer 7 jumps to
					// 0x3020300 with a NOP there and PUT still in the other buffer).
					// Hardware follows the jump and reads whatever is at the target,
					// so no frontier applies; a strict one parks on the first word
					// and never sees the guest patch the park.
					add(target, umax, false);
					return;
				}
			}

			add(target, frontier, false);
		}

		void stream_tracker::on_call(u32 target, u32 pos)
		{
			u32 current;

			// Hardware overwrites its single return-address register on every
			// CALL, so a nested call loses the outer caller's frame for good.
			// Keep the same model: the newest call owns the saved stream, or
			// on_return would restore a stream the parser is no longer in and
			// add() could recycle the stream it actually occupies while the
			// producer still writes it (L149).
			m_saved = find(pos, current) ? current : m_current;
			m_in_call = true;

			u32 index;

			if (find(target, index))
			{
				m_current = index;
				m_call_linear = false;
			}
			else
			{
				// Static subroutine: follow PUT while it runs.
				m_call_linear = true;
			}
		}

		void stream_tracker::on_return()
		{
			m_in_call = false;
			m_call_linear = false;
			m_current = m_saved;
		}

		bool stream_tracker::redirect(u32 put, u32 addr, u32& target)
		{
			u32 current;

			if (!find(addr, current) || m_entries[current].linear)
			{
				return false;
			}

			// Only a parser standing exactly on the published end can be moved:
			// inside the stream the producer's words are still readable.
			if (m_entries[current].frontier != addr)
			{
				return false;
			}

			// A call owns the return address in this chunk; abandoning it would
			// leave the guest's call stack pointing into recycled memory.
			if (m_in_call)
			{
				return false;
			}

			u32 next;

			if (!find(put, next) || next == current || put <= m_entries[next].base)
			{
				return false;
			}

			// The producer moved on without writing the link that would carry the
			// parser out of this chunk. Mark the chunk abandoned so a jump back
			// into it does not resume on the stale tail, and enter the stream the
			// producer did publish.
			m_entries[current].frontier = m_entries[current].base;
			m_current = next;
			target = m_entries[next].base;
			return true;
		}

		void FIFO_control::reset_streams(u32 base, bool preserve)
		{
			if (preserve)
			{
				m_streams.reset_at(base);
			}
			else
			{
				m_streams.reset(base);
			}

			m_redirect = 0;
			m_published_end = 0;
			m_diag_parked_put = umax; // TODO(excise): level 1
			m_diag_park_word = 0; // TODO(excise): level 1
			m_diag_put_count = 0; // TODO(excise): level 1
		}

		void FIFO_control::on_jump(u32 target)
		{
			const u32 put = read_put();
			m_streams.on_jump(target, put);

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
		}

		void FIFO_control::on_call(u32 target)
		{
			m_streams.on_call(target, m_fifo_pos);
		}

		void FIFO_control::on_return()
		{
			m_streams.on_return();
		}

		std::pair<bool, u32> FIFO_control::fetch_u32(u32 addr)
		{
			if (addr - m_cache_addr >= m_cache_size)
			{
				const u32 put = read_put();

				// TODO(excise): level 1; sample the producer frontier.
				diag_put_sample(put, addr);
				m_streams.note_put(put, m_fifo_pos);

				// PUT is the position the producer has published up to, so the
				// word at PUT is normally not readable yet.  The one exception is
				// the link: the guest publishes a chunk by pointing PUT at its
				// final jump/call/return, which is written before the move.  The
				// frontier check below consumes that word alone; a data word at
				// the same position is a mid-fill and must wait.
				const u32 published_end = m_streams.frontier(put, addr);
				m_published_end = published_end;

				m_cache_addr = addr & -128;

				const u32 addr1 = translate_address(m_cache_addr);

				if (addr1 == umax)
				{
					m_cache_size = 0;
					return {false, FIFO_ERROR};
				}

				if (addr >= published_end)
				{
					// The parser is standing on the published end of its stream.
					// Only the link word that carries it to the next stream may be
					// consumed here. A data word at this position has not been
					// written for this pass, so wait for the producer instead of
					// parsing whatever is left over in the ring.
					//
					// The link is live: GET has not acknowledged this position, so
					// the producer cannot have recycled it. If the word is data the
					// producer is mid-fill, or it abandoned the chunk (redirect).
					u32 word = vm::read32(addr1 | (addr & 127));
					const bool link = addr == published_end && is_valid_link(word);

					if (!link)
					{
						if (addr > published_end)
						{
							// TODO(excise): level 1; the parser ran past the
							// published end of its stream, which should not happen.
							diag_read_ahead(addr, published_end);
						}

						u32 target;

						if (m_streams.redirect(put, addr, target))
						{
							// TODO(excise): level 1; the producer abandoned this
							// chunk mid-command and published the other buffer.
							rsx_log.warning("FIFO diag: redirect from=0x%x target=0x%x put=0x%x word=0x%x",
								addr, target, put, word);

							// The link that would carry the parser out of this
							// chunk will never be written: the guest moved on and
							// is now waiting for GET to leave the buffer. Follow
							// the publication instead of parking for ever.
							m_redirect = target;
							m_cache_size = 0;
							return {false, FIFO_EMPTY};
						}

						// EXCISE-BEGIN fifo-park-word
						if (m_diag_parked_put != addr)
						{
							// TODO(excise): level 1; new park, remember the word.
							diag_park(addr, published_end, put, word);
							m_diag_park_word = word;
						}
						else if (word != m_diag_park_word)
						{
							// TODO(excise): level 1; the producer wrote the frontier
							// while the parser waited.  A link here means the parser
							// missed a wake; a data word means the guest recycled the
							// chunk under the parser.
							rsx_log.warning("FIFO diag: park word change addr=0x%x old=0x%x new=0x%x put=0x%x",
								addr, m_diag_park_word, word, put);

							m_diag_park_word = word;
						}

						m_diag_parked_put = addr;
						// EXCISE-END fifo-park-word
						// Nothing of this position is in the cache.  Leaving the
						// old range live makes the retry address the start of a
						// line whose bytes were never copied, so it would read a
						// word from the previous line instead of re-evaluating
						// this one.
						m_cache_size = 0;
						return {false, FIFO_EMPTY};
					}

					// Consume the link alone. The cache must never span the end of
					// the stream, or a cached word at PUT would be served later.
					m_cache_addr = addr;
					m_cache_size = 4;
					const be_t<u32> raw = word;
					std::memcpy(+m_cache[0], &raw, sizeof(raw));
					return {true, word};
				}

				// EXCISE-BEGIN fifo-resume
				if (m_diag_parked_put == addr)
				{
					// TODO(excise): level 1; resumed on a frontier that was just read.
					diag_resume(addr, put);
				}

				m_diag_parked_put = umax;
				// EXCISE-END fifo-resume

				if (addr == m_next_cmd_entry)
				{
					if (u32 cmd = vm::read32(addr1 | (addr & 127)); cmd & RSX_METHOD_NON_METHOD_CMD_MASK)
					{
						// FIFO control commands (including invalid)
						// No GET update here
						m_cache_addr = addr;
						m_cache_size = 4;
						// The cache is now the source for this position, so it has to
						// hold the word the caller is about to receive.  Leaving the
						// stale bytes in place made a later read at the same position
						// (a retry, or the desync probe) return the previous command.
						const be_t<u32> raw = cmd;
						std::memcpy(+m_cache[0], &raw, sizeof(raw));
						return {true, cmd};
					}
				}

				m_cache_size = std::min<u32>((published_end | 0x7f) - m_cache_addr, u32{sizeof(m_cache)} - 1) + 1;

				if (0x100000 - (m_cache_addr & 0xfffff) < m_cache_size)
				{
					// Check if memory layout changes in the next 1MB page boundary
					if ((addr1 >> 20) + 1 != (translate_address(m_cache_addr + 0x100000) >> 20))
					{
						// Trim cache as needed if memory layout changes
						m_cache_size = 0x100000 - (m_cache_addr & 0xfffff);
					}
				}

				// Make mask of cache lines to fetch
				u8 to_fetch = static_cast<u8>((1u << (m_cache_size / 128)) - 1);

				if (published_end < m_cache_addr + m_cache_size)
				{
					// The cache holds only published data. The old code rounded
					// this up to a 128-byte line and then served the words at or
					// past PUT from it, which is how a mid-fill PUT was parsed.
					m_cache_size = published_end - m_cache_addr;
				}

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
							// Cut our losses if we have something to work with.
							// This is the first time falling out of the reservation loop above, so we have clean data with no holes.
							m_cache_size = bytes_read;
							break;
						}

						start_time = get_system_time();

						// EXCISE-BEGIN fifo-stall-start
						if (!stall_start)
						{
							stall_start = start_time;
						}
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

				// The fetch loop can grow the cache back to a line boundary (or cut
				// it at a control word past the frontier); keep the published end
				// as the hard upper bound no matter which path ran.
				if (m_cache_size > published_end - m_cache_addr)
				{
					m_cache_size = published_end - m_cache_addr;
				}

				// Update FIFO GET
				sync_get();
				atomic_fence_seq_cst();
			}

			ensure(addr < m_cache_addr + m_cache_size);
			const auto ret = read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], addr - m_cache_addr);
			return {true, ret};
		}

		// TODO(excise): whole-unit; level 1; the desync capture reads the cache snapshot so the
		// cached bytes can be compared with the ring at the same positions.
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
			m_redirect = 0;
		}

		void FIFO_control::read(register_pair& data)
		{
			if (m_redirect)
			{
				// A parked parser was moved to the base of the stream the producer
				// published. Nothing of the abandoned packet was consumed (the
				// park happens between packets), so drop the boundary state and
				// restart there.
				const u32 target = std::exchange(m_redirect, 0u);
				m_fifo_pos = target;
				m_next_cmd_entry = target;
				m_remaining_commands = 0;
				m_command_reg = 0;
				m_args_ptr = m_iotable->get_addr(target);
				m_memwatch_addr = 0;
				m_memwatch_cmp = 0;
				invalidate_cache();
				m_ctrl->get.release(target);
				data.reg = FIFO_EMPTY;
				return;
			}

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

			if (!g_cfg.core.rsx_fifo_accuracy) [[ likely ]]
			{
				const u32 put = read_put();
				m_streams.note_put(put, m_fifo_pos);
				m_published_end = m_streams.frontier(put, m_fifo_pos);

				if (m_fifo_pos >= m_published_end)
				{
					// The parser is standing on the end of its stream. Only the
					// link may be consumed; a data word is a mid-fill and must
					// wait. This used to be checked against the global PUT alone,
					// so once the producer moved PUT to its other buffer the
					// parser read the current stream's stale tail as commands.
					// The accuracy mode chooses the copy strategy, not whether
					// the stream boundary is real.
					if (auto [ok, word] = fetch_u32(m_fifo_pos); ok)
					{
						m_cmd = word;
					}
					else
					{
						data.reg = word;
						return;
					}
				}
				// The frontier branch above may have consumed this stream's link;
				// in that case do not read the same position again.  Keep the
				// upstream address translation so LOCAL FIFO mode stays valid.
				else if (const u32 addr = translate_address(m_fifo_pos); addr + 1)
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

			if (count && g_cfg.core.rsx_fifo_accuracy)
			{
				// Dispatch a packet only once all of it is published. The producer
				// may be mid-fill and publishing the other stream at this instant,
				// so a partially written method is not a method yet.
				const u64 packet_end = u64{m_fifo_pos} + (u64{count} + 1) * 4;

				if (packet_end > m_published_end)
				{
					const u32 put = read_put();
					m_streams.note_put(put, m_fifo_pos);
					m_published_end = m_streams.frontier(put, m_fifo_pos);
				}

				if (packet_end > m_published_end)
				{
					// Nothing of the packet was consumed. Keep the boundary on the
					// header so the retry is not mistaken for a phase slip.
					m_next_cmd_entry = m_fifo_pos;
					data.reg = FIFO_EMPTY;
					return;
				}
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
				fifo_ctrl->on_jump(offs);
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
				fifo_ctrl->on_call(offs);
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
							fifo_ctrl->on_call(cmd0 & RSX_METHOD_CALL_OFFSET_MASK);
							fifo_ctrl->set_get(cmd0 & RSX_METHOD_CALL_OFFSET_MASK);
							last_known_code_start = ctrl->get;
							fifo_ret_addr += 4;
							return;
						}
					}
				}

				fifo_ctrl->on_return();
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
