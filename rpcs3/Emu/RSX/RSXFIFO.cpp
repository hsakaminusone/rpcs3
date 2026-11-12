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


		std::pair<bool, u32> FIFO_control::fetch_u32(u32 addr)
		{
			if (addr - m_cache_addr < m_cache_size &&
				static_cast<s32>(addr - m_cache_get) >= 0 && static_cast<s32>(addr - m_cache_put) < 0)
			{
				// Only the part of the snapshot the guest could not have touched is
				// served: it was at or above the GET published when the block was
				// filled (everything below that is licenced for reuse) and below the
				// PUT that was current then (everything at or above it was read
				// ahead of the producer).  Anything else drops to the slow path and
				// is re-read, which is the only way to avoid stitching two
				// generations of the same ring together.
				return {true, read_from_ptr_unsafe<be_t<u32>>(+m_cache[0], addr - m_cache_addr)};
			}

			const u32 put = read_put();


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
					// The cache is now the source for this position, so it has to
					// hold the word the caller is about to receive (a retry at the
					// same position must see the command, not the stale bytes), and
					// it is the only word of the window: nothing above it was
					// published or copied.
					m_cache_get = addr;
					m_cache_put = addr + 4;
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

			// Make mask of cache lines to fetch
			u8 to_fetch = static_cast<u8>((1u << (m_cache_size / 128)) - 1);

			// PUT is exclusive: the word at PUT is not published. The cache is
			// filled in 128-byte lines, so the line that contains PUT is read up
			// to 0x7f bytes past it; hide that read-ahead from hits. Otherwise a
			// word written by the producer after the line was fetched (the exit
			// link at the frontier) is served from the stale copy. The check has
			// to be positional, not a comparison of PUT against the address: the
			// producer re-points PUT to a numerically lower offset when it wraps
			// or reuses a command buffer (RR7: 0x10a0e20 -> 0xcb13bc after the
			// frontier link was written), and any "PUT moved past addr" test
			// fails open there. A frontier word is always the first byte past
			// the exposed range, so the retry re-reads it.
			if (addr < put)
			{
				m_cache_size = std::min<u32>(m_cache_size, put - m_cache_addr);
			}

			// Snapshot the window this block is valid in: from the GET that
			// sync_get() is about to publish (the frontier word the caller is
			// waiting on, so everything below it is licenced for reuse) up to the
			// PUT read above. A hit outside it re-reads rather than stitches.
			m_cache_get = addr;
			m_cache_put = put;

			// Atomic FIFO debug options
			const bool force_cache_fill = g_cfg.core.rsx_fifo_accuracy == rsx_fifo_mode::atomic_ordered;
			const bool strict_fetch_ordering = g_cfg.core.rsx_fifo_accuracy >= rsx_fifo_mode::atomic_ordered;

			rsx::reservation_lock<true, 1> rsx_lock(addr1, m_cache_size, true);
			const auto src = vm::_ptr<spu_rdata_t>(addr1);

			u64 start_time = 0;
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
				}

				auto now = get_system_time();


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

		void FIFO_control::note_boundary(u32 pos, u32 header)
		{
			m_boundaries[m_boundary_head] = {pos, header};

			if (++m_boundary_head == boundary_capacity)
			{
				m_boundary_head = 0;
			}

			m_boundary_total = std::min<u32>(m_boundary_total + 1, boundary_capacity);
		}

		u32 FIFO_control::recovery_point(u32 failing, u32 fallback)
		{
			packet_boundary ordered[boundary_capacity];

			for (u32 i = 0; i < m_boundary_total; i++)
			{
				ordered[i] = m_boundaries[(m_boundary_head + boundary_capacity - 1 - i) % boundary_capacity];
			}

			if (const u32 pos = recovery_rules::select(ordered, m_boundary_total, failing, [this](u32 addr) { return ring_word(addr); });
				pos != umax)
			{
				m_recovery_pos = pos;
				m_recovery_word = ring_word(pos);
				return pos;
			}

			// Nothing that was executed is still intact. Re-entering the position
			// the previous recovery already resumed at cannot make progress: the
			// word there is the one that failed and the parse is deterministic.
			// That is the RR7 loop; stop instead of spinning the watchdog.
			if (fallback == m_recovery_pos && ring_word(fallback) == m_recovery_word)
			{
				return umax;
			}

			m_recovery_pos = fallback;
			m_recovery_word = ring_word(fallback);
			return fallback;
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

				data.reg = FIFO_EMPTY;
				return;
			}


			// R2/R3: read at the cursor whether it is behind PUT (published) or
			// ahead of it (a jump target the producer has not reached yet). There
			// is no per-stream frontier; PUT gates whole-packet dispatch below.
			if (!g_cfg.core.rsx_fifo_accuracy) [[ likely ]]
			{

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

					// A flow-control command is a legal packet boundary too: if
					// the stream is broken after it, resuming here re-follows the
					// same link instead of re-entering a word that changed.
					note_boundary(m_fifo_pos, m_cmd);

					// Flow control, stop reading
					data.reg = m_cmd;
					return;
				}


				// Malformed command, optional recovery
				data.reg = FIFO_ERROR;
				return;
			}

			ensure(!m_remaining_commands);
			const u32 count = (m_cmd >> 18) & 0x7ff;

			m_next_cmd_entry = m_fifo_pos + count * 4 + 4;

			// R2: dispatch a packet only when all of it is published. While the
			// cursor is behind PUT, a packet that ends past PUT is a mid-fill:
			// leave the header in place and wait for the last argument, so no
			// method is ever entered with a partially published argument block.
			// Upstream dispatches on the header and parks on the first unpublished
			// argument instead; this is the one dispatch rule we keep locally
			// (handoff §19.4). A cursor ahead of PUT is in a region the guest
			// jumped to before publishing it (R3); that region is walked as
			// hardware walks it and is not gated.
			if (m_fifo_pos < put && m_next_cmd_entry > put)
			{
				// Nothing of the packet was consumed. Keep the boundary on the
				// header so the retry is not mistaken for a phase slip.
				m_next_cmd_entry = m_fifo_pos;
				data.reg = FIFO_EMPTY;
				return;
			}

			if (!count)
			{

				m_fifo_pos += 4;
				sync_get();
				data.reg = FIFO_NOP;
				return;
			}


			// This packet was accepted and is about to execute, so its start is a
			// valid rollback target while its header is still in the ring.
			note_boundary(m_fifo_pos, m_cmd);

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
