#include "MTLCommandRecorder.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLInstrumentation.h"
#include "MTLBarrier.h"

#include "Emu/Memory/vm.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Host/MM.h"
#include "util/asm.hpp"
#include "util/logs.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach/mach_time.h>
#include <thread>

namespace mtl
{
	// The bound on the commit-feedback wait.  Vulkan bounds the same wait with
	// FRAME_PRESENT_TIMEOUT, ten seconds (VKPresent.cpp:250).
	static constexpr u32 recorder_feedback_timeout = 10;

#if MTL_DIAGNOSTICS
	// TODO(excise): level 1; the wall clock the commit feedback is measured against.
	// The level-1 feedback gauges need it, so it sits under the wider switch.
	static f64 debug_now_seconds()
	{
		static const mach_timebase_info_data_t timebase = [] { mach_timebase_info_data_t value{}; mach_timebase_info(&value); return value; }();

		return static_cast<f64>(mach_absolute_time()) * static_cast<f64>(timebase.numer) / static_cast<f64>(timebase.denom) * 1e-9;
	}
#endif // MTL_DIAGNOSTICS

	void command_recorder::init(device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_queue = dev.queue();
		m_compiler = dev.compiler();
		m_resources = &resources;
		m_lost = &dev.lost_flag();
		m_feedback = std::make_shared<feedback_owner>();
		m_feedback->recorder = this;

		// One timeline for every commit.  The queue signals it after the commit's
		// work, so the CPU can read completion without the feedback handler's lock
		// (SY14; metal-cpp MTL4CommandQueue.hpp:142).  A device that cannot create a
		// shared event cannot report completion at all, which is a backend fault.
		m_completion_event = m_device->newSharedEvent();
		if (!m_completion_event)
		{
			die("Metal 4: the completion event could not be created");
		}

		for (frame_slot& slot : m_slots)
		{
			allocate_slot(slot);
		}

		for (std::atomic<u64>& completed : m_completed)
		{
			completed.store(0, std::memory_order_relaxed);
		}

		m_label_staging = resources.new_buffer(static_cast<u64>(frame_slot_count) * label_staging_bytes, MTL::StorageModeShared);
		if (!m_label_staging)
		{
			die("Metal 4: the label staging buffer could not be allocated");
		}

		// EXCISE-BEGIN label-probe-buffer
		// TODO(excise): level 2; probe word per slot.
		m_label_probe = resources.new_buffer(static_cast<u64>(frame_slot_count) * sizeof(u32), MTL::StorageModeShared);
		if (!m_label_probe)
		{
			die("Metal 4: the label probe buffer could not be allocated");
		}
		// EXCISE-END label-probe-buffer

		build_swap_pipelines();
		build_convert_pipelines();
	}

	namespace
	{
		// One invocation per element, so `dispatchThreads` supplies the exact count
		// and the kernel needs no bound.  Both kernels read and write the buffer in
		// place; the element width is chosen by the pipeline that is bound (T2).
		constexpr const char* swap_msl = R"(#include <metal_stdlib>
using namespace metal;

kernel void rsx_bswap16(device ushort* data [[buffer(0)]], uint i [[thread_position_in_grid]])
{
	const ushort value = data[i];
	data[i] = (value >> 8) | (value << 8);
}

kernel void rsx_bswap32(device uint* data [[buffer(0)]], uint i [[thread_position_in_grid]])
{
	const uint value = data[i];
	data[i] = (value << 24) | ((value & 0x0000ff00u) << 8) | ((value & 0x00ff0000u) >> 8) | (value >> 24);
}
)";
	}

	void command_recorder::build_swap_pipelines()
	{
		if (!m_compiler)
		{
			return;
		}

		MTL4::LibraryDescriptor* library_descriptor = MTL4::LibraryDescriptor::alloc()->init();
		library_descriptor->setSource(NS::String::string(swap_msl, NS::UTF8StringEncoding));

		NS::Error* error = nullptr;
		m_swap_library = m_compiler->newLibrary(library_descriptor, &error);
		library_descriptor->release();

		if (!m_swap_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: byte-swap library creation failed: %s", message);
			return;
		}

		const auto make_pipeline = [this](const char* name) -> MTL::ComputePipelineState*
		{
			MTL4::LibraryFunctionDescriptor* function = MTL4::LibraryFunctionDescriptor::alloc()->init();
			function->setLibrary(m_swap_library);
			function->setName(NS::String::string(name, NS::UTF8StringEncoding));

			MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
			descriptor->setComputeFunctionDescriptor(function);

			NS::Error* pipeline_error = nullptr;
			MTL::ComputePipelineState* state = m_compiler->newComputePipelineState(descriptor, nullptr, &pipeline_error);

			function->release();
			descriptor->release();

			if (!state)
			{
				const char* message = (pipeline_error && pipeline_error->localizedDescription()) ? pipeline_error->localizedDescription()->utf8String() : "unknown";
				rsx_log.error("Metal: byte-swap pipeline creation failed for %s: %s", name, message);
			}

			return state;
		};

		m_bswap16 = make_pipeline("rsx_bswap16");
		m_bswap32 = make_pipeline("rsx_bswap32");
	}

	void command_recorder::swap_bytes(MTL4::ComputeCommandEncoder* encoder, MTL::Buffer* buffer, u64 offset, u64 size, u32 element_size)
	{
		if (!encoder || !buffer || !size || (element_size != 2 && element_size != 4))
		{
			return;
		}

		MTL::ComputePipelineState* state = (element_size == 2) ? m_bswap16 : m_bswap32;
		const u64 elements = size / element_size;

		if (!state || !elements)
		{
			return;
		}

		// The copy ahead of the swap ran as a blit in this compute encoder, and
		// Metal 4 tracks no hazards, so the dispatch needs an encoder-scope edge in
		// both directions (T2).
		emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice, false });

		blit_table()->setAddress(buffer->gpuAddress() + offset, 0);
		encoder->setComputePipelineState(state);
		encoder->setArgumentTable(blit_table());
		encoder->dispatchThreads(MTL::Size::Make(elements, 1, 1), MTL::Size::Make(64, 1, 1));

		emit_barrier(encoder, barrier_spec{ MTL::StageDispatch, MTL::StageBlit, MTL4::VisibilityOptionDevice, false });
	}

	namespace
	{
		// The depth planes and the packed guest words are all buffers, so the
		// conversion needs no texture binding and the same argument table slot
		// layout serves both directions.  `mode` names the guest depth encoding
		// (T6): 0 z24s8_uint, 1 z24s8_float, 2 z16_uint, 3 z16_float.
		struct convert_params
		{
			u32 width;
			u32 height;
			u32 depth_stride;
			u32 pack_stride;
			u32 stencil_stride;
			u32 mode;
			u32 reserved0;
			u32 reserved1;
		};
		static_assert(sizeof(convert_params) == 32);

		constexpr const char* convert_msl = R"(#include <metal_stdlib>
using namespace metal;

struct rsx_convert_params
{
	uint width;
	uint height;
	uint depth_stride;
	uint pack_stride;
	uint stencil_stride;
	uint mode;
	uint reserved0;
	uint reserved1;
};

kernel void rsx_depth_to_pack(device const uchar* depth [[buffer(0)]],
	device const uchar* stencil [[buffer(1)]],
	device uchar* packed [[buffer(2)]],
	constant rsx_convert_params& p [[buffer(3)]],
	uint2 gid [[thread_position_in_grid]])
{
	if (gid.x >= p.width || gid.y >= p.height)
	{
		return;
	}

	uint s = 0;
	if (p.stencil_stride != 0)
	{
		s = stencil[gid.y * p.stencil_stride + gid.x];
	}

	const uint depth_offset = gid.y * p.depth_stride;
	const uint pack_offset = gid.y * p.pack_stride;

	if (p.mode <= 1)
	{
		const device float* d = (const device float*)(depth + depth_offset);
		const float v = d[gid.x];
		uint depth24;

		if (p.mode == 0)
		{
			depth24 = uint(clamp(v, 0.0f, 1.0f) * 16777215.0f + 0.5f);
		}
		else
		{
			depth24 = (as_uint(v) >> 7) & 0x00FFFFFFu;
		}

		((device uint*)(packed + pack_offset))[gid.x] = (depth24 << 8) | s;
	}
	else if (p.mode == 2)
	{
		const device ushort* d = (const device ushort*)(depth + depth_offset);
		((device ushort*)(packed + pack_offset))[gid.x] = d[gid.x];
	}
	else
	{
		const device float* d = (const device float*)(depth + depth_offset);
		const float v = d[gid.x];
		const float scaled = clamp(v, 0.0f, as_float(0x3F7FF800u)) * as_float(0x08000000u);
		const ushort raw = ushort((as_uint(scaled) + 0x400u) >> 11);
		((device ushort*)(packed + pack_offset))[gid.x] = raw;
	}
}

kernel void rsx_pack_to_depth(device const uchar* packed [[buffer(0)]],
	device uchar* depth [[buffer(1)]],
	device uchar* stencil [[buffer(2)]],
	constant rsx_convert_params& p [[buffer(3)]],
	uint2 gid [[thread_position_in_grid]])
{
	if (gid.x >= p.width || gid.y >= p.height)
	{
		return;
	}

	const uint pack_offset = gid.y * p.pack_stride;
	const uint depth_offset = gid.y * p.depth_stride;

	if (p.mode <= 1)
	{
		const uint word = ((const device uint*)(packed + pack_offset))[gid.x];
		const uint depth24 = (word >> 8) & 0x00FFFFFFu;
		float v;

		if (p.mode == 0)
		{
			v = float(depth24) / 16777215.0f;
		}
		else
		{
			v = as_float(depth24 << 7);
		}

		((device float*)(depth + depth_offset))[gid.x] = v;

		if (p.stencil_stride != 0)
		{
			stencil[gid.y * p.stencil_stride + gid.x] = uchar(word & 0xFFu);
		}
	}
	else if (p.mode == 2)
	{
		((device ushort*)(depth + depth_offset))[gid.x] = ((const device ushort*)(packed + pack_offset))[gid.x];
	}
	else
	{
		const uint raw = ((const device ushort*)(packed + pack_offset))[gid.x];
		const float v = as_float((raw << 11) & 0x07FFF800u) * as_float(0x77000000u);
		((device float*)(depth + depth_offset))[gid.x] = v;
	}
}
)";
	}

	void command_recorder::build_convert_pipelines()
	{
		if (!m_compiler)
		{
			return;
		}

		MTL4::LibraryDescriptor* library_descriptor = MTL4::LibraryDescriptor::alloc()->init();
		library_descriptor->setSource(NS::String::string(convert_msl, NS::UTF8StringEncoding));

		NS::Error* error = nullptr;
		m_convert_library = m_compiler->newLibrary(library_descriptor, &error);
		library_descriptor->release();

		if (!m_convert_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: depth conversion library creation failed: %s", message);
			return;
		}

		const auto make_pipeline = [this](const char* name) -> MTL::ComputePipelineState*
		{
			MTL4::LibraryFunctionDescriptor* function = MTL4::LibraryFunctionDescriptor::alloc()->init();
			function->setLibrary(m_convert_library);
			function->setName(NS::String::string(name, NS::UTF8StringEncoding));

			MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
			descriptor->setComputeFunctionDescriptor(function);

			NS::Error* pipeline_error = nullptr;
			MTL::ComputePipelineState* state = m_compiler->newComputePipelineState(descriptor, nullptr, &pipeline_error);

			function->release();
			descriptor->release();

			if (!state)
			{
				const char* message = (pipeline_error && pipeline_error->localizedDescription()) ? pipeline_error->localizedDescription()->utf8String() : "unknown";
				rsx_log.error("Metal: depth conversion pipeline creation failed for %s: %s", name, message);
			}

			return state;
		};

		m_depth_to_pack = make_pipeline("rsx_depth_to_pack");
		m_pack_to_depth = make_pipeline("rsx_pack_to_depth");
	}

	bool command_recorder::copy_typeless(command_recorder& cmd, const typeless_region& src, const typeless_region& dst)
	{
		if (!src.image || !dst.image)
		{
			return false;
		}

		const s32 src_w = src.rect.x2 - src.rect.x1;
		const s32 src_h = src.rect.y2 - src.rect.y1;
		const s32 dst_w = dst.rect.x2 - dst.rect.x1;
		const s32 dst_h = dst.rect.y2 - dst.rect.y1;

		if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		{
			rsx_log.error("Metal 4: a typeless copy was asked for an empty region");
			return false;
		}

		MTL4::ComputeCommandEncoder* encoder = cmd.open_compute();
		if (!encoder)
		{
			return false;
		}

		const MTL::PixelFormat src_format = src.image->pixelFormat();
		const MTL::PixelFormat dst_format = dst.image->pixelFormat();
		const MTL::Origin src_origin = MTL::Origin::Make(src.rect.x1, src.rect.y1, 0);
		const MTL::Origin dst_origin = MTL::Origin::Make(dst.rect.x1, dst.rect.y1, 0);
		const MTL::Size extent = MTL::Size::Make(src_w, src_h, 1);

		if (src_format == dst_format)
		{
			if (src_w != dst_w || src_h != dst_h)
			{
				rsx_log.error("Metal 4: a same-format copy between different-sized regions is not a copy");
				return false;
			}

			encoder->copyFromTexture(src.image, src.slice, src.level, src_origin, extent,
				dst.image, dst.slice, dst.level, dst_origin);
			return true;
		}

		const bool src_is_depth = is_depth_format(src_format);
		const bool dst_is_depth = is_depth_format(dst_format);

		if (!src_is_depth && !dst_is_depth)
		{
			const u32 src_texel = get_texel_size(src_format);
			const u32 dst_texel = get_texel_size(dst_format);

			if (!src_texel || !dst_texel || src_texel != dst_texel || src_w != dst_w || src_h != dst_h)
			{
				rsx_log.error("Metal 4: typeless copy between colour formats %u and %u has no bit-preserving route",
					static_cast<u32>(src_format), static_cast<u32>(dst_format));
				return false;
			}

			const u32 row_bytes = utils::align(static_cast<u32>(src_w) * src_texel, 256u);
			const u64 image_bytes = static_cast<u64>(row_bytes) * src_h;
			MTL::Buffer* staging = cmd.resources().new_buffer(image_bytes, MTL::StorageModeShared);

			if (!staging)
			{
				rsx_log.error("Metal 4: a typeless colour copy staging allocation failed");
				return false;
			}

			encoder->copyFromTexture(src.image, src.slice, src.level, src_origin, extent,
				staging, 0, row_bytes, image_bytes);

			if (!emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice, false }))
			{
				rsx_log.error("Metal 4: a typeless colour copy barrier was refused");
				cmd.resources().defer_release(staging);
				return false;
			}

			encoder->copyFromBuffer(staging, 0, row_bytes, image_bytes, extent,
				dst.image, dst.slice, dst.level, dst_origin);

			cmd.resources().defer_release(staging);
			return true;
		}

		if (src_is_depth && dst_is_depth)
		{
			rsx_log.error("Metal 4: typeless copy between depth formats %u and %u has no bit-preserving route",
				static_cast<u32>(src_format), static_cast<u32>(dst_format));
			return false;
		}

		const typeless_region& depth_side = src_is_depth ? src : dst;
		const typeless_region& colour_side = src_is_depth ? dst : src;

		u32 mode = 0;
		u32 guest_texel = 0;

		switch (depth_side.depth_format)
		{
		case rsx::surface_depth_format2::z24s8_uint:
			mode = 0;
			guest_texel = 4;
			break;
		case rsx::surface_depth_format2::z24s8_float:
			mode = 1;
			guest_texel = 4;
			break;
		case rsx::surface_depth_format2::z16_uint:
			mode = 2;
			guest_texel = 2;
			break;
		case rsx::surface_depth_format2::z16_float:
			mode = 3;
			guest_texel = 2;
			break;
		default:
			rsx_log.error("Metal 4: typeless depth conversion has an unknown guest depth format");
			return false;
		}

		if (src_w != dst_w || src_h != dst_h || get_texel_size(colour_side.image->pixelFormat()) != guest_texel)
		{
			rsx_log.error("Metal 4: a depth/colour typeless copy does not describe the same guest bytes");
			return false;
		}

		MTL::ComputePipelineState* pipeline = src_is_depth ? m_depth_to_pack : m_pack_to_depth;

		if (!pipeline)
		{
			rsx_log.error("Metal 4: the depth conversion pipeline is unavailable");
			return false;
		}

		const u32 width = static_cast<u32>(src_w);
		const u32 height = static_cast<u32>(src_h);
		const u32 depth_row = utils::align(width * depth_plane_texel_size(depth_side.image->pixelFormat()), 256u);
		const u32 pack_row = utils::align(width * guest_texel, 256u);
		const bool stencil = rsx::is_depth_stencil_format(depth_side.depth_format);
		const u32 stencil_row = utils::align(width, 256u);

		const u64 depth_bytes = static_cast<u64>(depth_row) * height;
		const u64 pack_bytes = static_cast<u64>(pack_row) * height;
		const u64 stencil_bytes = stencil ? static_cast<u64>(stencil_row) * height : 0;

		MTL::Buffer* depth_buffer = cmd.resources().new_buffer(depth_bytes, MTL::StorageModeShared);
		MTL::Buffer* pack_buffer = cmd.resources().new_buffer(pack_bytes, MTL::StorageModeShared);
		MTL::Buffer* stencil_buffer = stencil ? cmd.resources().new_buffer(stencil_bytes, MTL::StorageModeShared) : nullptr;
		MTL::Buffer* params_buffer = cmd.resources().new_buffer(sizeof(convert_params), MTL::StorageModeShared);

		if (!depth_buffer || !pack_buffer || !params_buffer || (stencil && !stencil_buffer))
		{
			rsx_log.error("Metal 4: a depth conversion staging allocation failed");
			cmd.resources().defer_release(depth_buffer);
			cmd.resources().defer_release(pack_buffer);
			cmd.resources().defer_release(stencil_buffer);
			cmd.resources().defer_release(params_buffer);
			return false;
		}

		auto* params = static_cast<convert_params*>(params_buffer->contents());
		params->width = width;
		params->height = height;
		params->depth_stride = depth_row;
		params->pack_stride = pack_row;
		params->stencil_stride = stencil ? stencil_row : 0;
		params->mode = mode;
		params->reserved0 = 0;
		params->reserved1 = 0;

		const MTL::BlitOption depth_option = get_depth_plane_option(depth_side.image->pixelFormat(), false);
		const MTL::BlitOption stencil_option = get_depth_plane_option(depth_side.image->pixelFormat(), true);
		const MTL::Size dispatch_size = MTL::Size::Make(width, height, 1);

		if (src_is_depth)
		{
			encoder->copyFromTexture(depth_side.image, depth_side.slice, depth_side.level, src_origin, extent,
				depth_buffer, 0, depth_row, depth_bytes, depth_option);

			if (stencil)
			{
				if (!emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice, false }))
				{
					rsx_log.error("Metal 4: a depth conversion barrier was refused");
					cmd.resources().defer_release(depth_buffer);
					cmd.resources().defer_release(pack_buffer);
					cmd.resources().defer_release(stencil_buffer);
					cmd.resources().defer_release(params_buffer);
					return false;
				}

				encoder->copyFromTexture(depth_side.image, depth_side.slice, depth_side.level, src_origin, extent,
					stencil_buffer, 0, stencil_row, stencil_bytes, stencil_option);
			}

			emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice, false });

			blit_table()->setAddress(depth_buffer->gpuAddress(), 0);
			blit_table()->setAddress(stencil_buffer ? stencil_buffer->gpuAddress() : depth_buffer->gpuAddress(), 1);
			blit_table()->setAddress(pack_buffer->gpuAddress(), 2);
			blit_table()->setAddress(params_buffer->gpuAddress(), 3);
			encoder->setComputePipelineState(pipeline);
			encoder->setArgumentTable(blit_table());
			encoder->dispatchThreads(dispatch_size, MTL::Size::Make(16, 16, 1));

			emit_barrier(encoder, barrier_spec{ MTL::StageDispatch, MTL::StageBlit, MTL4::VisibilityOptionDevice, false });

			encoder->copyFromBuffer(pack_buffer, 0, pack_row, pack_bytes, extent,
				dst.image, dst.slice, dst.level, dst_origin);
		}
		else
		{
			encoder->copyFromTexture(colour_side.image, colour_side.slice, colour_side.level, src_origin, extent,
				pack_buffer, 0, pack_row, pack_bytes);

			emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageDispatch, MTL4::VisibilityOptionDevice, false });

			blit_table()->setAddress(pack_buffer->gpuAddress(), 0);
			blit_table()->setAddress(depth_buffer->gpuAddress(), 1);
			blit_table()->setAddress(stencil_buffer ? stencil_buffer->gpuAddress() : depth_buffer->gpuAddress(), 2);
			blit_table()->setAddress(params_buffer->gpuAddress(), 3);
			encoder->setComputePipelineState(pipeline);
			encoder->setArgumentTable(blit_table());
			encoder->dispatchThreads(dispatch_size, MTL::Size::Make(16, 16, 1));

			emit_barrier(encoder, barrier_spec{ MTL::StageDispatch, MTL::StageBlit, MTL4::VisibilityOptionDevice, false });

			encoder->copyFromBuffer(depth_buffer, 0, depth_row, depth_bytes, extent,
				dst.image, dst.slice, dst.level, dst_origin, depth_option);

			if (stencil)
			{
				if (!emit_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice, false }))
				{
					rsx_log.error("Metal 4: a depth conversion barrier was refused");
					cmd.resources().defer_release(depth_buffer);
					cmd.resources().defer_release(pack_buffer);
					cmd.resources().defer_release(stencil_buffer);
					cmd.resources().defer_release(params_buffer);
					return false;
				}

				encoder->copyFromBuffer(stencil_buffer, 0, stencil_row, stencil_bytes, extent,
					dst.image, dst.slice, dst.level, dst_origin, stencil_option);
			}
		}

		cmd.resources().defer_release(depth_buffer);
		cmd.resources().defer_release(pack_buffer);
		cmd.resources().defer_release(stencil_buffer);
		cmd.resources().defer_release(params_buffer);
		return true;
	}

	u64 command_recorder::completed_commits(u32 slot) const
	{
		if (slot >= frame_slot_count)
		{
			return 0;
		}

		return m_completed[slot].load(std::memory_order_acquire);
	}

	// EXCISE-BEGIN encoder-provenance-impl
	// TODO(excise): level 1; see encoder_stamp().
	const char* command_recorder::encoder_event_name(encoder_event event)
	{
		switch (event)
		{
		case encoder_event::begin_frame: return "begin-frame";
		case encoder_event::open_pass: return "open-pass";
		case encoder_event::open_compute: return "open-compute";
		case encoder_event::close_encoder: return "close-encoder";
		case encoder_event::reenter_slot: return "reenter-slot";
		case encoder_event::allocate_slot: return "allocate-slot";
		case encoder_event::release_slot: return "release-slot";
		case encoder_event::submit: return "submit";
		case encoder_event::readback: return "readback";
		}

		return "?";
	}

	void command_recorder::note_encoder_event(encoder_event event, MTL4::CommandEncoder* encoder, void* caller)
	{
		++m_encoder_stamp;

		if (!probe::at_least(1))
		{
			return;
		}

		encoder_event_record& record = m_encoder_events[m_encoder_event_count % encoder_event_capacity];
		++m_encoder_event_count;
		record.stamp = m_encoder_stamp;
		record.event = event;
		record.encoder = encoder;
		record.caller = caller;
		record.slot = m_slot;
	}

	void command_recorder::dump_encoder_events(const char* why) const
	{
		const u32 count = std::min<u32>(m_encoder_event_count, encoder_event_capacity);

		rsx_log.error("Metal 4: encoder history (%s): stamp %u, slot %u, %u event(s) kept",
			why, m_encoder_stamp, m_slot, count);

		for (u32 index = 0; index < count; ++index)
		{
			const encoder_event_record& record =
				m_encoder_events[(m_encoder_event_count - count + index) % encoder_event_capacity];

			rsx_log.error("  stamp %u: %s encoder %p slot %u caller %p",
				record.stamp, encoder_event_name(record.event),
				static_cast<void*>(record.encoder), record.slot, record.caller);
		}
	}
	// EXCISE-END encoder-provenance-impl

	// EXCISE-BEGIN encoder-watch-impl
	// TODO(excise): level 1; see watch_encoder().
	void command_recorder::watch_encoder(MTL4::CommandEncoder* encoder, u64 draw)
	{
		if (!probe::at_least(1))
		{
			return;
		}

		m_watched_encoder = encoder;
		m_watched_draw = draw;
		m_watch_depth = 0;
	}

	void command_recorder::dump_stack(const char* why, void* const* frames, u32 count) const
	{
		rsx_log.error("Metal 4: %s: %u frame(s), innermost first", why, count);

		for (u32 index = 0; index < count; ++index)
		{
			Dl_info info{};
			dladdr(frames[index], &info);

			rsx_log.error("  #%u pc %p base %p image %s", index, frames[index],
				info.dli_fbase, info.dli_fname ? info.dli_fname : "?");
		}
	}
	// EXCISE-END encoder-watch-impl

	// Drop the Metal objects of one slot.  A committed command buffer and its
	// allocator are retained by the queue until their work completes, so releasing
	// this reference is safe even when the commit never reported back; resetting
	// the allocator is not (DEVIATIONS-SYNC.md, SY3).
	void command_recorder::release_slot(frame_slot& slot)
	{
		// EXCISE-BEGIN encoder-provenance-release-slot
		note_encoder_event(encoder_event::release_slot, m_encoder, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-release-slot

		// The slot is being abandoned or rebuilt, so its label addresses no longer
		// describe anything the GPU can still write.
		slot.labels.clear();

		if (slot.interpreter_fragment_table)
		{
			slot.interpreter_fragment_table->release();
			slot.interpreter_fragment_table = nullptr;
		}

		if (slot.blit_table)
		{
			slot.blit_table->release();
			slot.blit_table = nullptr;
		}

		if (slot.interpreter_vertex_table)
		{
			slot.interpreter_vertex_table->release();
			slot.interpreter_vertex_table = nullptr;
		}

		if (slot.fragment_table)
		{
			slot.fragment_table->release();
			slot.fragment_table = nullptr;
		}

		if (slot.vertex_table)
		{
			slot.vertex_table->release();
			slot.vertex_table = nullptr;
		}

		for (MTL4::CommandBuffer*& buffer : slot.buffers)
		{
			if (buffer)
			{
				buffer->release();
				buffer = nullptr;
			}
		}

		slot.buffer_index = 0;
		slot.buffer_open = false;
		slot.committed_label_count = 0;

		if (slot.allocator)
		{
			slot.allocator->release();
			slot.allocator = nullptr;
		}
	}

	// Build the Metal objects of one slot.  The slots are built here and rebuilt
	// when a commit never reports its feedback (HLD-04 §3.1).
	void command_recorder::allocate_slot(frame_slot& slot)
	{
		release_slot(slot);

		// The slot now holds fresh table objects, so every writer that caches what
		// it wrote must start over (see table_generation()).
		++m_table_generation;

		MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
		descriptor->setMaxBufferBindCount(argument_buffer_count);
		descriptor->setMaxTextureBindCount(argument_texture_count);
		descriptor->setMaxSamplerStateBindCount(argument_sampler_count);
		// An unbound slot reads a deterministic null (HLD-05 §4.10).
		descriptor->setInitializeBindings(true);

		slot.allocator = m_device->newCommandAllocator();
		slot.vertex_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.fragment_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.interpreter_vertex_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.interpreter_fragment_table = m_device->newArgumentTable(descriptor, nullptr);
		slot.blit_table = m_device->newArgumentTable(descriptor, nullptr);

		descriptor->release();

		if (!slot.allocator || !slot.vertex_table || !slot.fragment_table ||
			!slot.interpreter_vertex_table || !slot.interpreter_fragment_table || !slot.blit_table)
		{
			die("command recorder allocation failed");
		}

		// EXCISE-BEGIN encoder-provenance-allocate-slot
		note_encoder_event(encoder_event::allocate_slot, nullptr, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-allocate-slot
	}

	bool command_recorder::slot_idle(const frame_slot& slot) const
	{
		return !slot.completion_value || !m_completion_event ||
			slot.completion_value <= m_completion_event->signaledValue();
	}

	bool command_recorder::await_value(u64 value, const char* what)
	{
		if (!value || !m_completion_event || m_completion_event->signaledValue() >= value)
		{
			return true;
		}

		// The wait is bounded, because a signal that never arrives must not stop
		// the RSX thread (VKPresent.cpp:250 bounds the same wait with
		// FRAME_PRESENT_TIMEOUT).  A commit finishes in microseconds to
		// milliseconds, so a short pause spin covers the common case and lets the
		// thread sleep only when the GPU is genuinely behind.  The clock is read
		// on the sleep path only, so the spin costs nothing but the event read.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(recorder_feedback_timeout);
		u32 spins = 0;

		while (m_completion_event->signaledValue() < value)
		{
			if (++spins < 256)
			{
				utils::pause();
				continue;
			}

			if (std::chrono::steady_clock::now() >= deadline)
			{
				rsx_log.error("Metal 4: %s waited %u s for completion value %llu, the event is at %llu",
					what, recorder_feedback_timeout, value, m_completion_event->signaledValue());
				return false;
			}

			std::this_thread::sleep_for(std::chrono::microseconds(50));
		}

		return true;
	}

	bool command_recorder::await_slot(const frame_slot& slot, const char* what)
	{
		return await_value(slot.completion_value, what);
	}

	bool command_recorder::wait_idle()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/wait idle");

		u64 target = 0;

		for (const frame_slot& slot : m_slots)
		{
			target = std::max(target, slot.completion_value);
		}

		return await_value(target, "wait idle");
	}

	bool command_recorder::shutdown()
	{
		// Drain the ring before the objects go away (HLD-04 §2.4).  The wait is
		// bounded: a commit whose signal never arrives must not hold the shutdown
		// for ever, because the window close that owns this call would never return.
		u64 target = 0;

		for (const frame_slot& slot : m_slots)
		{
			target = std::max(target, slot.completion_value);
		}

		const bool idle = await_value(target, "shutdown");

		if (!idle)
		{
			u32 in_flight = 0;

			for (const frame_slot& slot : m_slots)
			{
				in_flight += slot_idle(slot) ? 0u : 1u;
			}

			rsx_log.error("Metal 4: the frame ring did not drain in %u s, %u slot(s) still in flight; their objects are leaked rather than released",
				recorder_feedback_timeout, in_flight);
		}

		// Releasing a slot whose GPU work is unfinished frees memory the queue may
		// still read (DEVIATIONS-SYNC.md, SY3).  The process is closing, so leaking
		// the objects is the safe choice.
		if (idle)
		{
			for (frame_slot& slot : m_slots)
			{
				release_slot(slot);
			}

			// The staging buffer is read by the GPU, so it goes with the slots.
			if (m_label_staging)
			{
				m_resources->defer_release(m_label_staging);
				m_label_staging = nullptr;
			}

			// The queue would signal a released event on a late commit.
			if (m_completion_event)
			{
				m_completion_event->release();
				m_completion_event = nullptr;
			}
		}

		if (m_bswap16)
		{
			m_bswap16->release();
			m_bswap16 = nullptr;
		}

		if (m_bswap32)
		{
			m_bswap32->release();
			m_bswap32 = nullptr;
		}

		if (m_swap_library)
		{
			m_swap_library->release();
			m_swap_library = nullptr;
		}

		if (m_depth_to_pack)
		{
			m_depth_to_pack->release();
			m_depth_to_pack = nullptr;
		}

		if (m_pack_to_depth)
		{
			m_pack_to_depth->release();
			m_pack_to_depth = nullptr;
		}

		if (m_convert_library)
		{
			m_convert_library->release();
			m_convert_library = nullptr;
		}

		// Feedback blocks can arrive after this point.  Wait for the ones already
		// running, then null the recorder so a late block skips it instead of
		// dereferencing freed state (L3).
		if (m_feedback)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(recorder_feedback_timeout);
			u32 spins = 0;

			while (m_feedback->in_flight.load(std::memory_order_acquire))
			{
				if (++spins < 256)
				{
					utils::pause();
					continue;
				}

				if (std::chrono::steady_clock::now() >= deadline)
				{
					rsx_log.error("Metal 4: commit feedback blocks did not drain; the recorder stays reachable");
					break;
				}

				std::this_thread::sleep_for(std::chrono::microseconds(50));
			}

			std::lock_guard lock(m_feedback->lock);
			m_feedback->recorder = nullptr;
			m_feedback.reset();
		}

		m_device = nullptr;
		m_queue = nullptr;
		m_compiler = nullptr;
		m_resources = nullptr;
		m_lost = nullptr;
		return idle;
	}

	bool command_recorder::begin_frame()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/begin frame");
		frame_slot& slot = m_slots[m_slot];

		// The slot returns only after its last commit (HLD-04 §3.2).  The wait is
		// bounded, because a feedback that never arrives must not stop the RSX
		// thread (VKPresent.cpp:250 bounds the same wait with
		// FRAME_PRESENT_TIMEOUT).
		bool reported = true;
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/wait feedback");

			if (!slot_idle(slot))
			{
				// TODO(excise): level 4; deep-probe line.
				MTL_NOTE(4, "Metal 4: slot %u is in flight, begin_frame waits for its completion", m_slot);
				reported = await_slot(slot, "begin frame");
			}
		}

		if (!reported)
		{
			// L1: a slot whose commit never reported is not recycled.  Its
			// allocator, the command buffers it handed out and every ring they read
			// may still be in use, so resetting or freeing them is undefined.  The
			// only safe action is to stop recording and report the loss; the core
			// then stops issuing draws.
			mark_device_lost("a frame slot did not complete");
			return false;
		}

		// The frame clock must observe the completion before it is advanced to the
		// new frame: the surface reuse test needs to know which frame the slot has
		// just finished, not the one it is about to record.
		m_resources->on_commit_complete(m_slot);
		m_resources->on_frame_begin(m_slot);

		// The ring is idle, so nothing can reference a deferred object (HLD-02 §4).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/gc");

			if (ring_idle())
			{
				m_resources->garbage_collect();
				m_resources->drain_quarantine();
			}
		}

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/begin frame/allocator");

			// Every command buffer of the slot has completed, so the allocator can
			// be reset and its buffers reused.  A slot is recycled once per frame;
			// a mid-frame commit reuses the allocator without this reset (A1/R1).
			if (!recycle_slot("begin frame"))
			{
				return false;
			}

			if (!next_buffer())
			{
				return false;
			}

			m_pending_count = 0;
			m_encoder = nullptr;
			m_compute_encoder = nullptr;
			// EXCISE-BEGIN encoder-provenance-begin-frame
			note_encoder_event(encoder_event::begin_frame, nullptr, __builtin_return_address(0));
			// EXCISE-END encoder-provenance-begin-frame
		}

#if MTL_PERF_ENABLED
		u32 in_flight = 0;

		for (const frame_slot& other : m_slots)
		{
			in_flight += slot_idle(other) ? 0u : 1u;
		}

		// TODO(excise): level 1; instantaneous value.
		MTL_PERF_GAUGE(1, "recorder/ring in flight", in_flight);
#endif // MTL_PERF_ENABLED

		return true;
	}

	void command_recorder::mark_device_lost(const char* why)
	{
		if (!m_device_lost.exchange(true, std::memory_order_acq_rel))
		{
			rsx_log.fatal("Metal 4: %s; the device is treated as lost", why);
		}

		if (m_lost)
		{
			m_lost->store(true, std::memory_order_release);
		}
	}

	bool command_recorder::next_buffer()
	{
		frame_slot& slot = m_slots[m_slot];

		if (m_device_lost)
		{
			return false;
		}

		if (slot.buffer_open)
		{
			return true;
		}

		// The pool is exhausted only after every buffer it holds has been
		// committed.  The queue runs them in order, so the slot's last commit
		// proves the whole pool done and the allocator can be reset (A1/R1).
		if (slot.buffer_index >= frame_slot::buffer_capacity)
		{
			if (!await_slot(slot, "buffer pool"))
			{
				mark_device_lost("the command-buffer pool did not complete");
				return false;
			}

			slot.allocator->reset();
			slot.buffer_index = 0;
		}

		const u32 index = slot.buffer_index;

		if (!slot.buffers[index])
		{
			slot.buffers[index] = m_device->newCommandBuffer();

			if (!slot.buffers[index])
			{
				die("command buffer allocation failed");
			}
		}

		slot.buffers[index]->beginCommandBuffer(slot.allocator);
		++slot.buffer_index;
		slot.buffer_open = true;
		return true;
	}

	bool command_recorder::recycle_slot(const char* what)
	{
		frame_slot& slot = m_slots[m_slot];

		if (!slot_idle(slot) && !await_slot(slot, what))
		{
			mark_device_lost("a frame slot did not complete");
			return false;
		}

		slot.allocator->reset();
		slot.buffer_index = 0;
		slot.buffer_open = false;
		slot.label_staging_offset = 0;
		slot.labels.clear();
		slot.committed_label_count = 0;
		return true;
	}

	u64 command_recorder::commit_open_buffer(const char* what)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/commit buffer");

		if (m_device_lost)
		{
			return 0;
		}

		frame_slot& slot = m_slots[m_slot];

		// Whatever the caller deferred is carried by this commit.
		m_label_commit_due = false;

		// A label recorded after the last pass belongs to this commit, and a copy
		// opened for it must be closed before the buffer ends.
		flush_label_writes();
		close_encoder();

		if (!slot.buffer_open)
		{
			return 0;
		}

		// The GPU is about to read guest memory that the CPU may still have a
		// deferred mprotect for, so apply the queue before the commit.  It is a
		// no-op when the queue is empty, and Vulkan does the same before it submits
		// (VKGSRender.cpp:2536; C25).
		rsx::mm_flush();
		publish_residency();

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/commit buffer/end");
			slot.buffers[slot.buffer_index - 1]->endCommandBuffer();
			slot.buffer_open = false;
		}

		const u32 generation = slot.generation.fetch_add(1, std::memory_order_relaxed) + 1;
		const u64 value = commit_slot(slot, m_slot, generation, slot.buffers[slot.buffer_index - 1]);

		// Every label staged so far is carried by this commit, so a later acquire
		// on one of them needs this value rather than the open buffer.
		slot.committed_label_count = static_cast<u32>(slot.labels.size());

		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: %s committed slot %u at value %llu", what, m_slot, value);
		return value;
	}

	void command_recorder::submit()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/submit");

		const u32 index = m_slot;
		frame_slot& slot = m_slots[index];

		// A label recorded after the last pass belongs to this commit, and the
		// commit's value covers every chunk committed earlier in the frame.
		commit_open_buffer("submit");

#if MTL_DIAGNOSTICS
		// TODO(excise): level 1; the commit stamp the feedback lag is measured from.
		slot.debug_commit_time.store(debug_now_seconds(), std::memory_order_relaxed);
#endif
		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: slot %u submitted at %.3f, %s %p", m_slot, slot.debug_commit_time.load(std::memory_order_relaxed), m_drawable ? "with drawable" : "no drawable", static_cast<const void*>(m_drawable));

		// The drawable is signalled only after the commit that fills it, so the
		// present is ordered behind the frame (C7, HLD-03 §4.9).
		if (m_drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/submit/signal drawable");
			m_queue->signalDrawable(m_drawable);
			m_drawable = nullptr;
		}

		m_slot = (m_slot + 1) % frame_slot_count;

		// EXCISE-BEGIN encoder-provenance-submit
		note_encoder_event(encoder_event::submit, nullptr, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-submit
	}

	void command_recorder::flush_for_readback()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/readback wait");

		// A readback must observe the copy it encoded.  The commit ends the open
		// buffer and returns the value the queue will signal; earlier chunks of
		// the slot are ordered before it.  When nothing is open (a local task
		// outside a frame) the last commit is the one to await.
		const u64 value = commit_open_buffer("readback");
		frame_slot& slot = m_slots[m_slot];
		const u64 target = value ? value : slot.completion_value;

		// EXCISE-BEGIN encoder-provenance-readback
		note_encoder_event(encoder_event::readback, nullptr, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-readback

		if (!await_value(target, "read-back"))
		{
			mark_device_lost("a read-back commit did not complete");
			return;
		}

		// The completion proves every commit that referenced the slot's label
		// staging region done, so the region can be reused and its addresses
		// retired (A1/R1).
		slot.label_staging_offset = 0;
		slot.labels.clear();
		slot.committed_label_count = 0;

		// TODO(excise): level 2; the GPU write must be visible in the guest window.
		if (m_diag_label_address)
		{
			MTL::Buffer* guest = m_resources->guest_buffer();
			u32 actual = 0;

			if (guest && m_diag_label_address <= guest->length() && guest->length() - m_diag_label_address >= sizeof(u32))
			{
				actual = *reinterpret_cast<const u32*>(static_cast<const u8*>(guest->contents()) + m_diag_label_address);
			}

			MTL_NOTE(2, "sync/label verify addr=0x%x expected=0x%x sudo=0x%x guest=0x%x gpu=0x%x\n",
				m_diag_label_address, m_diag_label_value, actual, vm::_ref<u32>(m_diag_label_address),
				(m_label_probe && m_label_probe->length() >= (m_slot + 1) * sizeof(u32))
					? *reinterpret_cast<const u32*>(static_cast<const u8*>(m_label_probe->contents()) + m_slot * sizeof(u32))
					: 0);
			m_diag_label_address = 0;
		}

		// The next encoder begins the slot's next buffer lazily; a readback does
		// not reset the allocator or reopen one eagerly (A1/R1).
		m_encoder = nullptr;
		m_compute_encoder = nullptr;
	}

	void command_recorder::set_present_drawable(MTL::Drawable* drawable)
	{
		// The wait must precede the encoding, because the drawable's texture is the
		// copy destination.
		if (drawable)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/present drawable wait");
			m_queue->wait(drawable);
		}

		m_drawable = drawable;
	}

	MTL4::RenderCommandEncoder* command_recorder::open_pass(const MTL4::RenderPassDescriptor* descriptor, MTL4::RenderEncoderOptions options)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/open pass");

		// The pass boundary is the natural place for a label batch: no open pass is
		// split, and the writes are ordered before the pass's load actions.
		flush_label_writes();
		close_encoder();

		// A back-end release asked for its label at the next boundary instead of
		// splitting the pass it arrived in (A2).  The pass is closed now, so this
		// commit costs nothing extra.
		if (m_label_commit_due)
		{
			commit_open_buffer("label boundary");
		}

		if (!next_buffer())
		{
			return nullptr;
		}

		MTL4::RenderCommandEncoder* encoder = m_slots[m_slot].buffers[m_slots[m_slot].buffer_index - 1]->renderCommandEncoder(descriptor, options);
		m_encoder = encoder;
		// EXCISE-BEGIN encoder-provenance-open-pass
		note_encoder_event(encoder_event::open_pass, encoder, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-open-pass
		return encoder;
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_compute()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/open compute");

		flush_label_writes();
		return open_copy_encoder();
	}

	MTL4::ComputeCommandEncoder* command_recorder::open_copy_encoder()
	{
		// A compute encoder is reused for consecutive copies, so a texture upload
		// of N units costs one encoder rather than N (HLD-04 §3.1).
		if (m_compute_encoder)
		{
			// Metal 4 rule: commands inside one compute encoder may run
			// concurrently.  barrierAfterQueueStages only waits for earlier
			// encoders, and barrierAfterStages only blocks later ones, so neither
			// orders two copies that share this encoder.  A clear followed by an
			// upload would otherwise race and publish a partially written image
			// that changes from frame to frame (S1).
			m_compute_encoder->barrierAfterEncoderStages(
				MTL::StageBlit | MTL::StageDispatch,
				MTL::StageBlit | MTL::StageDispatch,
				MTL4::VisibilityOptionDevice);
			return m_compute_encoder;
		}

		if (!next_buffer())
		{
			return nullptr;
		}

		close_encoder();

		MTL4::ComputeCommandEncoder* encoder = m_slots[m_slot].buffers[m_slots[m_slot].buffer_index - 1]->computeCommandEncoder();
		m_encoder = encoder;
		m_compute_encoder = encoder;
		// EXCISE-BEGIN encoder-provenance-open-compute
		note_encoder_event(encoder_event::open_compute, encoder, __builtin_return_address(0));
		// EXCISE-END encoder-provenance-open-compute
		return encoder;
	}

	bool command_recorder::write_label(u32 address, u32 value)
	{
		MTL::Buffer* guest = m_resources->guest_buffer();

		// The guest window is one buffer and a guest address is its offset
		// (HLD-02 §3.5).  A misaligned or out-of-window label cannot be written by
		// the GPU, so the caller stores it on the CPU instead.
		if (!guest || (address & 3) || address > guest->length() || guest->length() - address < sizeof(u32))
		{
			return false;
		}

		if (!next_buffer())
		{
			return false;
		}

		frame_slot& slot = m_slots[m_slot];

		if (slot.label_staging_offset + sizeof(u32) > label_staging_bytes)
		{
			// The region is rewritten only once every commit that reads it has
			// completed, so the slot commits and waits once.  The read-back resets
			// the staging offset and the label list.
			// TODO(excise): level 2; a frame that spends its whole label region.
			MTL_PROBE_LATCH(2, "sync/label write/region wrap");

			flush_for_readback();

			if (m_device_lost)
			{
				return false;
			}
		}

		const u32 offset = m_slot * label_staging_bytes + slot.label_staging_offset;
		std::memcpy(static_cast<u8*>(m_label_staging->contents()) + offset, &value, sizeof(u32));
		m_pending_labels[m_pending_count++] = address;
		slot.labels.push_back(address);
		slot.label_staging_offset += sizeof(u32);

		// EXCISE-BEGIN label-verify-write
		// TODO(excise): level 2; the last label is verified in guest memory after feedback.
		m_diag_label_address = address;
		m_diag_label_value = value;
		// EXCISE-END label-verify-write

		// TODO(excise): level 2; the labels the GPU writes instead of the CPU.
		MTL_PROBE_LATCH(2, "sync/label write/gpu");
		return true;
	}

	void command_recorder::commit_pending_labels()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/commit labels");

		// The GPU writes the labels when it runs this commit.  The guest is the one
		// waiting, so the RSX thread does not: the next encoder begins a fresh
		// buffer from the same allocator, and `sync_label` awaits the value this
		// commit returns (A1/R1).
		commit_open_buffer("label commit");
	}

	bool command_recorder::sync_label(u32 address)
	{
		bool waited = false;

		for (u32 index = 0; index < frame_slot_count; ++index)
		{
			frame_slot& slot = m_slots[index];

			bool has_address = false;
			bool uncommitted = false;

			for (u32 i = 0; i < slot.labels.size(); ++i)
			{
				if (slot.labels[i] != address)
				{
					continue;
				}

				has_address = true;
				uncommitted |= (i >= slot.committed_label_count);
			}

			if (!has_address)
			{
				continue;
			}

			if (index == m_slot && uncommitted)
			{
				// The label is in the open buffer: still staged, or flushed at a
				// pass boundary.  It has not reached the GPU, so commit the buffer
				// and await the value this commit returns.
				// TODO(excise): level 2; a guest acquire waited on a GPU label.
				MTL_PROBE_LATCH(2, "sync/label wait");

				const u64 value = commit_open_buffer("label wait");
				const u64 target = value ? value : slot.completion_value;

				if (!await_value(target, "label wait"))
				{
					mark_device_lost("a label commit did not complete");
				}
				else
				{
					waited = true;
				}
			}
			else if (!slot_idle(slot))
			{
				// The label is in a committed buffer of this slot (or an earlier
				// slot).  A later commit of the slot proves it visible.
				// TODO(excise): level 2; a guest acquire waited on an earlier slot.
				MTL_PROBE_LATCH(2, "sync/label wait");

				if (!await_slot(slot, "label wait"))
				{
					mark_device_lost("a label commit did not complete");
				}
				else
				{
					waited = true;
				}
			}

			// Every label of the slot is now visible: the wait above was for the
			// slot's last commit, which the serialized queue orders after them all.
			slot.labels.clear();
			slot.committed_label_count = 0;
		}

		return waited;
	}

	void command_recorder::publish_residency()
	{
		// A buffer that names an allocation the set has not published reads zeros in
		// silence (HLD-02 F1), so the set is committed before the buffer is.
		if (m_resources)
		{
			m_resources->flush_residency();
		}
	}

	void command_recorder::flush_label_writes()
	{
		if (!m_pending_count)
		{
			return;
		}

		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/flush labels");

		// The encoder opens first: opening it can begin a new command buffer, and
		// the batch that survives that is the one this copy must encode.
		MTL4::ComputeCommandEncoder* encoder = open_copy_encoder();

		if (!encoder)
		{
			// The device is lost; no label can reach the guest from here.
			m_pending_count = 0;
			return;
		}

		frame_slot& slot = m_slots[m_slot];
		const u32 first = slot.label_staging_offset - m_pending_count * sizeof(u32);

		// TODO(excise): level 2; the staged source must hold the guest word.
		MTL_NOTE(2, "sync/label source addr=0x%x value=0x%x\n", m_pending_labels[0],
			*reinterpret_cast<const u32*>(static_cast<const u8*>(m_label_staging->contents()) + m_slot * label_staging_bytes + first));

		// A label is a release, so the write must not run before the work encoded
		// ahead of it.  One queue-scope barrier covers the whole batch (HLD-03 §2.1).
		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });

		for (u32 i = 0; i < m_pending_count; ++i)
		{
			// A label page has its own import, made after the CPU mapped the page,
			// because the application-lifetime window keeps the GPU on the physical
			// page that existed when it was created (L136).  An address outside the
			// window falls back to the window itself.
			u32 dst_offset = 0;
			MTL::Buffer* dst = m_resources->label_buffer(m_pending_labels[i], dst_offset);

			if (!dst)
			{
				dst = m_resources->guest_buffer();
				dst_offset = m_pending_labels[i];
			}

			encoder->copyFromBuffer(m_label_staging, m_slot * label_staging_bytes + first + i * sizeof(u32),
				dst, dst_offset, sizeof(u32));
		}

		// TODO(excise): level 2; read the last label back inside the same command
		// buffer, so the word is what the GPU saw rather than what the guest left.
		if (m_diag_label_address)
		{
			// TODO(excise): level 2; proves the readback was encoded at all.
			MTL_PROBE_LATCH(2, "sync/label probe copy");
			u32 probe_offset = 0;
			MTL::Buffer* probe_source = m_resources->label_buffer(m_diag_label_address, probe_offset);

			if (!probe_source)
			{
				probe_source = m_resources->guest_buffer();
				probe_offset = m_diag_label_address;
			}

			emit_queue_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
			encoder->copyFromBuffer(probe_source, probe_offset,
				m_label_probe, m_slot * sizeof(u32), sizeof(u32));
		}

		// TODO(excise): level 2; the labels in one batch.
		MTL_PROBE_TOTAL(2, "sync/label batch", m_pending_count);

		m_pending_count = 0;
	}

	void command_recorder::close_encoder()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/close encoder");
		if (m_encoder)
		{
			// EXCISE-BEGIN encoder-provenance-close-encoder
			note_encoder_event(encoder_event::close_encoder, m_encoder, __builtin_return_address(0));
			// TODO(excise): level 3; one line per close, so the log carries the same
			// pointer the draw trace does and the two can be joined without the ring.
			MTL_NOTE(3, "Metal 4: close encoder %p slot %u stamp %u in flight %u",
				static_cast<void*>(m_encoder), m_slot, m_encoder_stamp,
				m_slots[m_slot].buffer_open ? 1u : 0u);
			// EXCISE-END encoder-provenance-close-encoder

			// EXCISE-BEGIN encoder-watch-trap
			// TODO(excise): level 1; the draw still holds this encoder, so ending it
			// here is the fault the run reproduces.  The stack is taken now, while
			// the culprit is on it.  Bounded to four reports.
			if (m_watched_encoder && m_encoder == m_watched_encoder)
			{
				m_watch_depth = static_cast<u32>(backtrace(m_watch_stack, static_cast<int>(encoder_watch_depth)));
				++m_watch_reports;

				if (m_watch_reports <= 4)
				{
					rsx_log.error("Metal 4: the draw's encoder %p was closed while the draw was open (draw %llu, slot %u, stamp %u, reports %u)",
						static_cast<void*>(m_encoder), m_watched_draw, m_slot, m_encoder_stamp, m_watch_reports);
					dump_encoder_events("encoder closed inside the draw");
					dump_stack("close inside the draw", m_watch_stack, m_watch_depth);
				}
			}
			// EXCISE-END encoder-watch-trap

			// The encoder boundary is the backstop the rest of the backend relies on:
			// Metal 4 tracks no hazards, so once this encoder ends, nothing else in the
			// backend is obliged to reconstruct what it wrote.  StageAll includes the
			// tile stage, so a stored attachment is ordered even when the producer was
			// a render pass's tile flush (L202 question (a); KK does the same in
			// end_encoder, kk_cmd_buffer.c:266).
			emit_producer_barrier(m_encoder,
				barrier_spec{ MTL::StageAll, MTL::StageAll, MTL4::VisibilityOptionDevice, true });

			m_encoder->endEncoding();
			m_encoder = nullptr;
		}

		m_compute_encoder = nullptr;
	}

	u64 command_recorder::commit_slot(frame_slot& slot, u32 index, u32 generation, MTL4::CommandBuffer* buffer)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/commit slot");
		// A commit takes its own options, and Metal keeps them until the feedback is
		// delivered, so one instance serves one commit (Apple, MTL4CommitOptions and
		// MTL4CommandQueue::commit; KK builds one per submit in kk_queue_commit).  A
		// reused instance delivers feedback for its first commit alone, which leaves
		// the slot in flight and stops the frame ring (REVIEW-FINDINGS.md, F17).
		MTL4::CommitOptions* options = nullptr;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/commit slot/options");
			options = MTL4::CommitOptions::alloc()->init();

			// The block captures the owner, not the recorder, so it can outlive
			// shutdown without dereferencing freed state (L3).
			std::shared_ptr<feedback_owner> owner = m_feedback;

			if (owner)
			{
				owner->in_flight.fetch_add(1, std::memory_order_acq_rel);
				options->addFeedbackHandler([owner, index, generation](MTL4::CommitFeedback* feedback)
				{
					{
						std::lock_guard lock(owner->lock);

						if (owner->recorder)
						{
							owner->recorder->on_feedback(index, generation, feedback);
						}
					}

					owner->in_flight.fetch_sub(1, std::memory_order_acq_rel);
				});
			}
		}

		const MTL4::CommandBuffer* buffers[] = { buffer };

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "recorder/commit slot/queue commit");
			m_queue->commit(buffers, 1, options);
		}

		options->release();

		// The queue signals the event after this commit's work, so one monotone
		// value per commit tells the CPU when the slot has run.  This is the only
		// completion point the waits use; the feedback handler stays for the
		// commit-diagnostic counters and the query gate (SY14).
		const u64 completion = ++m_completion_sequence;
		slot.completion_value = completion;
		m_queue->signalEvent(m_completion_event, completion);

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "recorder/commits", 1);
		return completion;
	}

	void command_recorder::on_feedback(u32 index, u32 generation, MTL4::CommitFeedback* feedback)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "recorder/feedback");
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "recorder/feedback events", 1);
		// TODO(excise): level 2; the commit feedback arrived (area M session classifier).
		MTL_PROBE_LATCH(2, "frame/feedback delivered");

		if (NS::Error* error = feedback->error())
		{
			// TODO(excise): level 2; the commit failed (area F, Metal faults).
			MTL_PROBE_LATCH(2, "fault/commit failed");

			const char* message = (error->localizedDescription())
				? error->localizedDescription()->utf8String()
				: "no description";
			rsx_log.error("Metal 4: commit failed: %s", message);

			// L3: a GPU error is a lost device, not a completed commit.  The
			// completion counter must not advance, or the query gate would read a
			// result the GPU never produced.
			mark_device_lost("a commit reported a GPU error");
			return;
		}

		// EXCISE-BEGIN feedback-lag
		if (feedback->GPUEndTime() > 0.0)
		{
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "recorder/feedback lag us", (debug_now_seconds() - feedback->GPUEndTime()) * 1e6);
		}
		// EXCISE-END feedback-lag

		// TODO(excise): temporary diagnostic; the frame's GPU execution and the time
		// it queued behind earlier frames tell a GPU-bound frame from a display-bound
		// one, which the present rows alone cannot.
		if (feedback->GPUEndTime() > feedback->GPUStartTime() && feedback->GPUStartTime() > 0.0)
		{
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "frame/gpu us", (feedback->GPUEndTime() - feedback->GPUStartTime()) * 1e6);
			// The commit stamp is written on the RSX thread, so it is read once here
			// rather than taken as a plain value on this callback thread.
			const f64 committed = m_slots[index].debug_commit_time.load(std::memory_order_relaxed);
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "frame/gpu wait us", (feedback->GPUStartTime() - committed) * 1e6);
		}

		// TODO(excise): level 4; deep-probe line.
		MTL_NOTE(4, "Metal 4: feedback for slot %u: gpu %.3f..%.3f, commit %.3f, start lag %.3f, delivery lag %.3f",
			index, feedback->GPUStartTime(), feedback->GPUEndTime(),
			m_slots[index].debug_commit_time.load(std::memory_order_relaxed),
			feedback->GPUStartTime() - m_slots[index].debug_commit_time.load(std::memory_order_relaxed),
			debug_now_seconds() - feedback->GPUEndTime());

		// A feedback of a commit the ring already abandoned carries an older
		// generation, so it must not count against the slot a newer commit now owns.
		if (m_slots[index].generation.load(std::memory_order_relaxed) != generation)
		{
			return;
		}

		// The commit is done.  A later commit of this slot proves every earlier
		// commit of the slot done as well, because the queue is serialized (Q4).
		m_completed[index].fetch_add(1, std::memory_order_release);
	}

	bool command_recorder::ring_idle() const
	{
		for (const frame_slot& slot : m_slots)
		{
			if (!slot_idle(slot))
			{
				return false;
			}
		}

		return true;
	}
}

#endif // RSX_ENABLE_METAL4
