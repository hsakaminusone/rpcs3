#include "stdafx.h"
#include "nv308a.h"

#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Core/RSXReservationLock.hpp"
// EXCISE-BEGIN fp-literal-write-watch-include
#include "Emu/RSX/Core/RSXProgramWriteWatch.h"
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
							std::snprintf(b, n, "x=%u y=%u pitch=%u fmt=%u count=%u", x, y, out_pitch,
								static_cast<u32>(*REGS(ctx)->blit_engine_nv3062_color_format()), count);
						});
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
						std::snprintf(b, n, "x=%u y=%u pitch=%u fmt=%u count=%u", x, y, out_pitch,
							static_cast<u32>(*REGS(ctx)->blit_engine_nv3062_color_format()), count);
					});
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
						std::snprintf(b, n, "x=%u y=%u pitch=%u count=%u", x, y, out_pitch, count);
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
