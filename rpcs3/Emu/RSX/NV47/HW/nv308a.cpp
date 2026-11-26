#include "stdafx.h"
#include "nv308a.h"

#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Core/RSXReservationLock.hpp"
// EXCISE-BEGIN fp-literal-write-watch-include
#include "Emu/RSX/Core/RSXProgramWriteWatch.h"
#include <bit>
// EXCISE-END fp-literal-write-watch-include

#include "context_accessors.define.h"

namespace rsx
{
	namespace nv308a
	{
		void color::impl(context* ctx, u32 reg, u32)
		{
			const u32 out_x_max = REGS(ctx)->nv308a_size_out_x();
			const u32 index = reg - NV308A_COLOR;

			if (index >= out_x_max)
			{
				// Skip
				return;
			}

			// Get position of the current command arg
			[[maybe_unused]] const u32 src_offset = RSX(ctx)->fifo_ctrl->get_pos();

			// FIFO args count including this one
			const u32 fifo_args_cnt = RSX(ctx)->fifo_ctrl->get_remaining_args_count() + 1;

			// The range of methods this function resposible to
			const u32 method_range = std::min<u32>(0x700 - index, out_x_max - index);

			// Get limit imposed by FIFO PUT (if put is behind get it will result in a number ignored by min)
			const u32 fifo_read_limit = static_cast<u32>(((RSX(ctx)->ctrl->put & ~3ull) - (RSX(ctx)->fifo_ctrl->get_pos())) / 4);

			u32 count = std::min<u32>({ fifo_args_cnt, fifo_read_limit, method_range });

			if (!count)
			{
				rsx_log.error("nv308a::color - No data to read/write.");
				RSX(ctx)->fifo_ctrl->skip_methods(fifo_args_cnt - 1);
				return;
			}

			const u32 dst_dma = REGS(ctx)->blit_engine_output_location_nv3062();
			const u32 dst_offset = REGS(ctx)->blit_engine_output_offset_nv3062();
			const u32 out_pitch = REGS(ctx)->blit_engine_output_pitch_nv3062();

			const u32 x = REGS(ctx)->nv308a_x() + index;
			const u32 y = REGS(ctx)->nv308a_y();

			const auto fifo_span = RSX(ctx)->fifo_ctrl->get_current_arg_ptr(count);

			if (fifo_span.size() < count)
			{
				count = ::size32(fifo_span);
			}

			// Skip "handled methods"
			RSX(ctx)->fifo_ctrl->skip_methods(count - 1);

			// 308A::COLOR can be used to create custom sync primitives.
			// Hide this behind strict mode due to the potential performance implications.
			if (count == 1 && g_cfg.video.strict_rendering_mode && !g_cfg.video.relaxed_zcull_sync)
			{
				RSX(ctx)->sync();
			}

			switch (*REGS(ctx)->blit_engine_nv3062_color_format())
			{
			case blit_engine::transfer_destination_format::a8r8g8b8:
			case blit_engine::transfer_destination_format::y32:
			{
				// Bit cast - optimize to mem copy

				const u32 data_length = count * 4;
				// The blit writes at an element offset inside the destination;
				// invalidation must cover the bytes actually written.
				const u32 write_offset = dst_offset + (x * 4) + (out_pitch * y);

				const auto dst_address = get_address(ctx, write_offset, dst_dma, data_length);

				if (!dst_address)
				{
					RSX(ctx)->recover_fifo();
					return;
				}

				const auto dst = vm::_ptr<u8>(dst_address);
				const auto src = reinterpret_cast<const u8*>(fifo_span.data());

				rsx::reservation_lock<true> rsx_lock(dst_address, data_length);

				// EXCISE-BEGIN fp-stage-watch
				// TODO(excise): level 5; arm the store watch on the guest bytes
				// this command consumes, so the next frame's write to the same
				// staging bytes is attributed to its producer.  The copy itself
				// is faithful, so the source range is the only place the
				// transposed word can come from.  watch_range merges, so the
				// ring collapses to a few intervals.
				if (const u32 stage_addr = RSX(ctx)->fifo_ctrl->translate_address(src_offset); stage_addr != umax)
				{
					rsx::fp_stage_watch::watch_range(stage_addr, data_length);
				}
				// EXCISE-END fp-stage-watch

				// EXCISE-BEGIN fp-stage-watch
				// TODO(excise): level 5; the staging-range watch.  The copy below
				// is faithful (`src == after`), so the transposed word is already
				// in the bytes this command reads.  Those bytes come from guest
				// memory at the FIFO cursor (`src_offset`).  Keep the last 16
				// bytes of every distinct source address this path has copied
				// from, re-read them at each later command, and log the first time
				// each 4-byte lane differs.  A lane that changes between two
				// commands was written by a layer other than this copy, which is
				// the layer H9 is looking for; the FIFO put/get and byte delta
				// order that write against the command stream.  Bounded (32
				// addresses, 4096 lines) and inert unless RSX_FP_STAGE_LOG is set.
				if (const char* stage_path = std::getenv("RSX_FP_STAGE_LOG"))
				{
					struct stage_slot
					{
						u32 addr;
						u8 bytes[16];
					};

					static stage_slot s_slots[32] = {};
					static u32 s_slot_count = 0;
					static u32 s_stage_lines = 0;
					static u64 s_stage_seen = 0;
					static u32 s_next_slot = 0;
					++s_stage_seen;

					// RSX_FP_STAGE_BASE=0 disables the first-sight sample.  On by
					// default: a word already wrong when the range is first seen is
					// invisible to a change-only re-read, which is the same
					// "instrument cannot see the defect" failure as F43.
					static const bool base_enabled = [] {
						const char* text = std::getenv("RSX_FP_STAGE_BASE");
						return !text || !*text || std::strcmp(text, "0") != 0;
					}();

					const auto guest_read = [](u32 addr, u8 (&out)[16]) -> bool
					{
						if (!vm::check_addr(addr, vm::page_readable, 16))
						{
							std::memset(out, 0, 16);
							return false;
						}

						std::memcpy(out, vm::_ptr<u8>(addr), 16);
						return true;
					};

					// Re-read every address this path has seen; a change since the
					// last command is a write by some other layer.
					for (u32 i = 0; i < 32 && s_stage_lines < 4096; ++i)
					{
						stage_slot& slot = s_slots[i];

						if (!slot.addr)
						{
							continue;
						}

						u8 now[16] = {};

						if (!guest_read(slot.addr, now))
						{
							continue;
						}

						for (unsigned lane = 0; lane < 4 && s_stage_lines < 4096; ++lane)
						{
							u32 was = 0;
							u32 cur = 0;
							std::memcpy(&was, slot.bytes + lane * 4, 4);
							std::memcpy(&cur, now + lane * 4, 4);

							if (was == cur)
							{
								continue;
							}

							const u32 decoded = rsx::fp_stage_watch::decode_word(cur);

							if (std::FILE* out = std::fopen(stage_path, "a"))
							{
								std::fprintf(out,
									"stage addr %x lane %u was %08x now %08x slot %08x alt %08x band %u seq %llu put %llx get %u dst %x:%x count %u\n",
									slot.addr, lane, was, cur, decoded, std::byteswap(decoded),
									rsx::fp_stage_watch::suspicious_word(cur) ? 1u : 0u,
									static_cast<unsigned long long>(s_stage_seen),
									static_cast<unsigned long long>(RSX(ctx)->ctrl->put),
									src_offset, dst_dma, dst_offset, count);
								std::fclose(out);
							}

							++s_stage_lines;
						}

						std::memcpy(slot.bytes, now, 16);
					}

					// Insert the current copy source if it is new.  A full table
					// recycles the oldest slot round-robin.
					const u32 guest_addr = RSX(ctx)->fifo_ctrl->translate_address(src_offset);

					if (guest_addr != umax)
					{
						bool known = false;

						for (u32 i = 0; i < s_slot_count; ++i)
						{
							known |= (s_slots[i].addr == guest_addr);
						}

						if (!known)
						{
							const u32 index = (s_slot_count < 32) ? s_slot_count++ : (s_next_slot++ % 32);
							s_slots[index].addr = guest_addr;

							if (guest_read(guest_addr, s_slots[index].bytes) && base_enabled && s_stage_lines < 4096)
							{
								const u8* b = s_slots[index].bytes;
								u32 w[4] = {};
								u32 s[4] = {};
								std::memcpy(w, b, 16);

								for (u32 lane = 0; lane < 4; ++lane)
								{
									s[lane] = rsx::fp_stage_watch::decode_word(w[lane]);
								}

								if (std::FILE* out = std::fopen(stage_path, "a"))
								{
									std::fprintf(out,
										"stage-base addr %x bytes %08x,%08x,%08x,%08x mask %x slot %08x,%08x,%08x,%08x seq %llu put %llx get %u dst %x:%x count %u\n",
										guest_addr, w[0], w[1], w[2], w[3],
										rsx::fp_stage_watch::band_mask(b, 16),
										s[0], s[1], s[2], s[3],
										static_cast<unsigned long long>(s_stage_seen),
										static_cast<unsigned long long>(RSX(ctx)->ctrl->put),
										src_offset, dst_dma, dst_offset, count);
									std::fclose(out);
								}

								++s_stage_lines;
							}
						}
					}
				}
				// EXCISE-END fp-stage-watch

				if (RSX(ctx)->fifo_ctrl->last_cmd() & RSX_METHOD_NON_INCREMENT_CMD_MASK) [[unlikely]]
				{
					// Move last 32 bits
					reinterpret_cast<u32*>(dst)[0] = reinterpret_cast<const u32*>(src)[count - 1];

					// EXCISE-BEGIN fp-literal-write-watch
					// TODO(excise): level 5; the single-word form of the write
					// above, reported with the pointer it wrote through and the
					// FIFO word it consumed.
					rsx::program_write_watch::note("nv308a-noninc", reinterpret_cast<u64>(dst), 4,
						src + (count - 1) * 4, 4,
						[&](char* b, usz n)
						{
							// TODO(excise): level 5; the FIFO cursor this
							// command was decoded at, the FIFO PUT, and the
							// span's host address, so the producer can be
							// ordered against the guest's PUT writes and the
							// source words read in both byte orders.
							u32 word = 0;
							std::memcpy(&word, src + (count - 1) * 4, 4);

							std::snprintf(b, n, "x=%u y=%u pitch=%u fmt=%u count=%u fifo=%u put=%llx span=%llx dst=%x:%x mode=noninc srcbe=%08x",
								x, y, out_pitch,
								static_cast<u32>(*REGS(ctx)->blit_engine_nv3062_color_format()), count,
								src_offset,
								static_cast<unsigned long long>(RSX(ctx)->ctrl->put),
								static_cast<unsigned long long>(reinterpret_cast<u64>(fifo_span.data())),
								dst_dma, dst_offset, std::byteswap(word));
						});
					// EXCISE-END fp-literal-write-watch

					// EXCISE-BEGIN fp-literal-write-watch
					// TODO(excise): level 5; the single-word form gets the same
					// address-keyed stage score as the bulk form above.
					rsx::fp_stage_watch::note_mem(vm::get_addr(dst), 4, "nv308a-noninc-dst", 0, "-", "nv308a noninc", /*allow_ring=*/false);
					// EXCISE-END fp-literal-write-watch

					RSX(ctx)->invalidate_fragment_program(dst_dma, write_offset, 4, "nv308a");
					return;
				}

				if (dst_dma & CELL_GCM_LOCATION_MAIN)
				{
					// May overlap
					std::memmove(dst, src, data_length);
				}
				else
				{
					// Never overlaps
					std::memcpy(dst, src, data_length);
				}

				// EXCISE-BEGIN fp-literal-write-watch
				// TODO(excise): level 5; the bulk form, reported with the pointer
				// the copy wrote through, the FIFO bytes it consumed, and the
				// destination geometry, so a write that lands on a program's
				// literal block through an element offset is attributed.
				rsx::program_write_watch::note("nv308a", reinterpret_cast<u64>(dst), data_length, src, data_length,
					[&](char* b, usz n)
					{
						// TODO(excise): level 5; the FIFO cursor, PUT, span
						// address and copy path, plus the first four source
						// words in both byte orders.  `le` is what the ring
						// prints; `be` is the guest command-stream reading.
						u32 words[4] = {};
						std::memcpy(words, src, std::min<u32>(16, data_length));

						// TODO(excise): level 5; read the same words straight
						// from guest memory at the FIFO cursor and compare them
						// with what this command consumed.  `same=1` proves the
						// transposed bytes are in guest RAM and not produced by
						// the FIFO cache copy; `same=0` convicts the cache.
						const u32 guest_addr = RSX(ctx)->fifo_ctrl->translate_address(src_offset);
						u32 guest_words[4] = {};
						u32 guest_same = 0;

						if (guest_addr != umax)
						{
							const u8* guest_ptr = vm::_ptr<u8>(guest_addr);
							const u32 cmp = std::min<u32>(16, data_length);
							std::memcpy(guest_words, guest_ptr, cmp);
							guest_same = std::memcmp(guest_ptr, src, cmp) == 0 ? 1u : 0u;
						}

						std::snprintf(b, n, "x=%u y=%u pitch=%u fmt=%u count=%u fifo=%u put=%llx span=%llx dst=%x:%x mode=%s le=%08x,%08x,%08x,%08x be=%08x,%08x,%08x,%08x guest=%x same=%u g=%08x,%08x,%08x,%08x",
							x, y, out_pitch,
							static_cast<u32>(*REGS(ctx)->blit_engine_nv3062_color_format()), count,
							src_offset,
							static_cast<unsigned long long>(RSX(ctx)->ctrl->put),
							static_cast<unsigned long long>(reinterpret_cast<u64>(fifo_span.data())),
							dst_dma, dst_offset, (dst_dma & CELL_GCM_LOCATION_MAIN) ? "memmove" : "memcpy",
							words[0], words[1], words[2], words[3],
							std::byteswap(words[0]), std::byteswap(words[1]),
							std::byteswap(words[2]), std::byteswap(words[3]),
							guest_addr, guest_same,
							guest_words[0], guest_words[1], guest_words[2], guest_words[3]);
					});
				// EXCISE-END fp-literal-write-watch

				// EXCISE-BEGIN fp-literal-write-watch
				// TODO(excise): level 5; score the copy destination with the same
				// address-keyed stage watch as every other writer.  The RSX-side
				// copy does not go through a CPU store hook, so without this line
				// a copy into a program image cannot be joined to a fill defect
				// by address.  The source EA is carried in the detail so the join
				// also names the guest buffer the bytes came from.
				if (const u32 src_ea = RSX(ctx)->fifo_ctrl->translate_address(src_offset); src_ea != umax)
				{
					char stage_detail[80];
					std::snprintf(stage_detail, sizeof(stage_detail), "nv308a src_ea=%08x dst_ea=%08x len=%u", src_ea, dst_address, data_length);
					rsx::fp_stage_watch::note_mem(dst_address, data_length, "nv308a-dst", 0, "-", stage_detail, /*allow_ring=*/false);

					// EXCISE-BEGIN fp-producer-ledger
					// TODO(excise): level 5; the copy names both ends, so the
					// origin report can say the defect quad arrived through this
					// engine and from this guest address.
					rsx::producer_ledger::record_copy(dst_address, data_length, "nv308a", src_ea, data_length, false);
					// EXCISE-END fp-producer-ledger
				}
				// EXCISE-END fp-literal-write-watch

				RSX(ctx)->invalidate_fragment_program(dst_dma, write_offset, count * 4, "nv308a");
				break;
			}
			case blit_engine::transfer_destination_format::r5g6b5:
			{
				const auto data_length = count * 2;
				const u32 write_offset = dst_offset + (x * 2) + (y * out_pitch);

				const auto dst_address = get_address(ctx, write_offset, dst_dma, data_length);
				const auto dst = vm::_ptr<u16>(dst_address);
				const auto src = utils::bless<const be_t<u32>>(fifo_span.data());

				if (!dst_address)
				{
					RSX(ctx)->recover_fifo();
					return;
				}

				rsx::reservation_lock<true> rsx_lock(dst_address, data_length);

				// EXCISE-BEGIN fp-stage-watch
				// TODO(excise): level 5; the 16-bit form consumes a 32-bit
				// source word per element, so watch that many source bytes.
				if (const u32 stage_addr = RSX(ctx)->fifo_ctrl->translate_address(src_offset); stage_addr != umax)
				{
					rsx::fp_stage_watch::watch_range(stage_addr, count * 4);
				}
				// EXCISE-END fp-stage-watch

				auto convert = [](u32 input) -> u16
				{
					// Input is considered to be ARGB8
					u32 r = (input >> 16) & 0xFF;
					u32 g = (input >> 8) & 0xFF;
					u32 b = input & 0xFF;

					r = (r * 32) / 255;
					g = (g * 64) / 255;
					b = (b * 32) / 255;
					return static_cast<u16>((r << 11) | (g << 5) | b);
				};

				if (RSX(ctx)->fifo_ctrl->last_cmd() & RSX_METHOD_NON_INCREMENT_CMD_MASK) [[unlikely]]
				{
					// Move last 16 bits
					dst[0] = convert(src[count - 1]);
					RSX(ctx)->invalidate_fragment_program(dst_dma, write_offset, 2, "nv308a-r5g6b5");
					break;
				}

				for (u32 i = 0; i < count; i++)
				{
					dst[i] = convert(src[i]);
				}

				// EXCISE-BEGIN fp-literal-write-watch
				// TODO(excise): level 5; the 16-bit form, which converts rather
				// than copies, so a destination block that reads back as the
				// source word would convict the per-element conversion.
				rsx::program_write_watch::note("nv308a-r5g6b5", reinterpret_cast<u64>(dst), data_length,
					src, count * 4,
					[&](char* b, usz n)
					{
						// TODO(excise): level 5; the FIFO cursor and PUT for
						// the converted form, so a destination block that
						// reads back as the source word is distinguishable
						// from one that copied through this path.
						std::snprintf(b, n, "x=%u y=%u pitch=%u count=%u fifo=%u put=%llx span=%llx dst=%x:%x mode=convert",
							x, y, out_pitch, count,
							src_offset,
							static_cast<unsigned long long>(RSX(ctx)->ctrl->put),
							static_cast<unsigned long long>(reinterpret_cast<u64>(fifo_span.data())),
							dst_dma, dst_offset);
					});
				// EXCISE-END fp-literal-write-watch

				RSX(ctx)->invalidate_fragment_program(dst_dma, write_offset, count * 2, "nv308a-r5g6b5");
				break;
			}
			default:
			{
				fmt::throw_exception("Unreachable");
			}
			}
		}
	}
}
