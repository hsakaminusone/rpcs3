#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLBarrier.h"
#include "Emu/RSX/MTL/MTLConstants.h"
#include "Emu/RSX/MTL/MTLDepthStencil.h"
#include "Emu/RSX/MTL/MTLFormats.h"
#include "Emu/RSX/MTL/MTLPassScheduler.h"
#include "Emu/RSX/MTL/MTLPipeline.h"
#include "Emu/RSX/MTL/MTLProgramPipeline.h"
#include "Emu/RSX/MTL/MTLRasterState.h"
#include "Emu/RSX/MTL/MTLSamplerState.h"
#include "Emu/RSX/MTL/MTLTextureCache.h"

// The Metal 4 backend state contract, GPU-free.
//
// Every test below exercises pure code: free functions, static functions, value
// types and POD structs.  No test opens an MTL::Device or an MTL4::CommandEncoder.
// The whole file is inert on a host that does not build the backend, because
// RSX_ENABLE_METAL4 gates it (MTLAvailability.h).
#ifdef RSX_ENABLE_METAL4

#include "Emu/Cell/Modules/cellVideoOut.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Program/program_util.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "Emu/RSX/rsx_methods.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>

namespace
{
	// The emulator's own power-on register state (rsx_methods.cpp:797).  A zeroed
	// file is not legal, because a getter that maps a word to a GCM enum verifies.
	void reset_registers()
	{
		rsx::method_registers.reset();
	}

	void set_register(u32 index, u32 value)
	{
		rsx::method_registers.registers[index] = value;
	}

	// The seven surface-access classes (TextureUtils.h:49-66).
	constexpr u32 access_domain[] = {
		rsx::surface_access::shader_read,
		rsx::surface_access::shader_write,
		rsx::surface_access::transfer_read,
		rsx::surface_access::transfer_write,
		rsx::surface_access::memory_read,
		rsx::surface_access::memory_write,
		rsx::surface_access::gpu_reference,
	};

	// One row of an RSX enum to Metal enum table.
	template <typename Rsx, typename Metal>
	struct mapping_row
	{
		Rsx guest;
		Metal host;
	};
}

// =============================================================================
// 1. The barrier stage masks - rsx::surface_access -> MTL::Stages
//     (MTLPassScheduler.cpp:7-27; HLD-03 §2.4)
// =============================================================================

TEST(MTLBarrierStageMasks, ShaderAccessSelectsTheVertexOrFragmentStage)
{
	// A shader read and a shader write carry the same mask; vertex_stage picks the
	// half.  These are the two rows that differ only by the stage bit.
	constexpr u32 shader[] = {
		rsx::surface_access::shader_read,
		rsx::surface_access::shader_write,
	};

	for (const u32 access : shader)
	{
		SCOPED_TRACE(access);
		EXPECT_EQ(mtl::access_stages(access, false), MTL::StageFragment);
		EXPECT_EQ(mtl::access_stages(access, true), MTL::StageVertex);
	}
}

TEST(MTLBarrierStageMasks, TransferAccessUsesBlitAndDispatch)
{
	constexpr u32 transfer[] = {
		rsx::surface_access::transfer_read,
		rsx::surface_access::transfer_write,
	};

	const MTL::Stages expected = static_cast<MTL::Stages>(MTL::StageBlit | MTL::StageDispatch);

	for (const u32 access : transfer)
	{
		SCOPED_TRACE(access);
		EXPECT_EQ(mtl::access_stages(access, false), expected);
		EXPECT_EQ(mtl::access_stages(access, true), expected);
	}
}

TEST(MTLBarrierStageMasks, MemoryAccessUsesAllStages)
{
	constexpr u32 memory[] = {
		rsx::surface_access::memory_read,
		rsx::surface_access::memory_write,
	};

	for (const u32 access : memory)
	{
		SCOPED_TRACE(access);
		EXPECT_EQ(mtl::access_stages(access, false), MTL::StageAll);
		EXPECT_EQ(mtl::access_stages(access, true), MTL::StageAll);
	}
}

TEST(MTLBarrierStageMasks, GpuReferenceOrdersNothing)
{
	// A reference orders no work, so its stage mask is empty (MTLPassScheduler.cpp:24-26).
	EXPECT_EQ(mtl::access_stages(rsx::surface_access::gpu_reference, false), static_cast<MTL::Stages>(0));
	EXPECT_EQ(mtl::access_stages(rsx::surface_access::gpu_reference, true), static_cast<MTL::Stages>(0));
}

TEST(MTLBarrierStageMasks, EveryAccessClassFollowsOneRule)
{
	// The mapping as a single expression.  A new class that no branch names falls
	// through to the empty mask, so a miss shows here.
	for (const u32 access : access_domain)
	{
		for (const bool vertex_stage : { false, true })
		{
			SCOPED_TRACE(access * 2 + (vertex_stage ? 1u : 0u));

			const MTL::Stages expected =
				(access == rsx::surface_access::shader_write || access == rsx::surface_access::shader_read)
					? (vertex_stage ? MTL::StageVertex : MTL::StageFragment)
					: (access == rsx::surface_access::transfer_write || access == rsx::surface_access::transfer_read)
						? static_cast<MTL::Stages>(MTL::StageBlit | MTL::StageDispatch)
						: (access == rsx::surface_access::memory_write || access == rsx::surface_access::memory_read)
							? MTL::StageAll
							: static_cast<MTL::Stages>(0);

			EXPECT_EQ(mtl::access_stages(access, vertex_stage), expected);
		}
	}
}

// =============================================================================
// 2. Intra-pass legality of a producer
//     (MTLBarrier.cpp:7-10; HLD-03 §3.4, invariant 4)
// =============================================================================

TEST(MTLBarrierLegality, OnlyAFragmentOrTileProducerForcesASplit)
{
	// A render encoder cannot order work that follows its own fragment or tile
	// stages.  The rule is one mask test over the low stage byte.
	for (u32 mask = 0; mask < 256; ++mask)
	{
		SCOPED_TRACE(mask);
		const bool expected = (mask & (MTL::StageFragment | MTL::StageTile)) == 0;
		EXPECT_EQ(mtl::stages_legal_intra_pass(static_cast<MTL::Stages>(mask)), expected);
	}
}

TEST(MTLBarrierLegality, HighStageBitsDoNotChangeTheVerdict)
{
	constexpr MTL::Stages others[] = {
		MTL::StageVertex,
		MTL::StageObject,
		MTL::StageMesh,
		MTL::StageDispatch,
		MTL::StageBlit,
		MTL::StageResourceState,
		MTL::StageAccelerationStructure,
		MTL::StageMachineLearning,
	};

	for (const MTL::Stages producer : others)
	{
		SCOPED_TRACE(static_cast<u64>(producer));
		EXPECT_TRUE(mtl::stages_legal_intra_pass(producer));
		EXPECT_FALSE(mtl::stages_legal_intra_pass(static_cast<MTL::Stages>(producer | MTL::StageFragment)));
		EXPECT_FALSE(mtl::stages_legal_intra_pass(static_cast<MTL::Stages>(producer | MTL::StageTile)));
	}

	EXPECT_TRUE(mtl::stages_legal_intra_pass(static_cast<MTL::Stages>(0)));
	EXPECT_FALSE(mtl::stages_legal_intra_pass(MTL::StageAll));
}

// =============================================================================
// 3. The barrier_spec value type
//     (MTLBarrier.h:14-24; HLD-03 §3.2)
// =============================================================================

TEST(MTLBarrierSpec, DefaultsToDeviceVisibilityAndNoSplit)
{
	const mtl::barrier_spec spec{};

	EXPECT_EQ(spec.producer, static_cast<MTL::Stages>(0));
	EXPECT_EQ(spec.consumer, static_cast<MTL::Stages>(0));
	EXPECT_EQ(spec.visibility, MTL4::VisibilityOptionDevice);
	EXPECT_FALSE(spec.cross_pass);
}

TEST(MTLBarrierSpec, VisibilityOptionsAreDistinct)
{
	// The scope is set once, at the chokepoint (HLD-03 §7 R4).
	EXPECT_NE(MTL4::VisibilityOptionNone, MTL4::VisibilityOptionDevice);
	EXPECT_NE(MTL4::VisibilityOptionDevice, MTL4::VisibilityOptionResourceAlias);
	EXPECT_NE(MTL4::VisibilityOptionNone, MTL4::VisibilityOptionResourceAlias);
}

TEST(MTLBarrierSpec, AggregateInitialisationPinsTheFieldOrder)
{
	const mtl::barrier_spec spec{ MTL::StageVertex, MTL::StageFragment, MTL4::VisibilityOptionResourceAlias, true };

	EXPECT_EQ(spec.producer, MTL::StageVertex);
	EXPECT_EQ(spec.consumer, MTL::StageFragment);
	EXPECT_EQ(spec.visibility, MTL4::VisibilityOptionResourceAlias);
	EXPECT_TRUE(spec.cross_pass);
}

// =============================================================================
// 4. The pass scheduler lifetime without an encoder
//     (MTLPassScheduler.h:22-50; MTLPassScheduler.cpp:29-104)
//
// The scheduler's queued edges are private and observe nothing until flush()
// runs on a real encoder.  These tests pin the call sequence and the two public
// invariants that do not need one: split_requested() stays false, and no call
// touches an encoder.
// =============================================================================

TEST(MTLPassSchedulerLifetime, SplitIsNeverRequestedWithoutAnEncoder)
{
	constexpr MTL::Stages stages[] = {
		MTL::StageVertex,
		MTL::StageFragment,
		MTL::StageTile,
		MTL::StageBlit,
		MTL::StageDispatch,
		MTL::StageAll,
		static_cast<MTL::Stages>(0),
	};

	for (u32 resource = 0; resource < 64; ++resource)
	{
		for (const MTL::Stages write_stages : stages)
		{
			for (const MTL::Stages read_stages : stages)
			{
				SCOPED_TRACE("resource " + std::to_string(resource));
				mtl::pass_scheduler scheduler;

				scheduler.begin_frame();
				EXPECT_FALSE(scheduler.split_requested());

				scheduler.begin_pass();
				scheduler.note_write(MTL::ResourceID{ static_cast<u64>(resource) }, write_stages);
				scheduler.note_read(MTL::ResourceID{ static_cast<u64>(resource) }, read_stages);
				EXPECT_FALSE(scheduler.split_requested());

				// The reverse order and a second pass; a same-pass read/write pair
				// is the TBDR feedback case that would split on a real encoder.
				scheduler.begin_pass();
				scheduler.note_read(MTL::ResourceID{ static_cast<u64>(resource) }, read_stages);
				scheduler.note_write(MTL::ResourceID{ static_cast<u64>(resource) }, write_stages);
				scheduler.flush(nullptr);

				EXPECT_FALSE(scheduler.split_requested());
			}
		}
	}
}

TEST(MTLPassSchedulerLifetime, FrameBoundaryRetiresEveryRecord)
{
	// begin_frame retires every record, so a note in one frame cannot leak its
	// edge into the next (MTLPassScheduler.cpp:29-36).
	mtl::pass_scheduler scheduler;

	scheduler.begin_frame();
	for (u32 i = 0; i < 256; ++i)
	{
		scheduler.note_write(MTL::ResourceID{ i }, MTL::StageVertex);
		scheduler.note_read(MTL::ResourceID{ i }, MTL::StageFragment);
	}

	scheduler.begin_frame();
	scheduler.begin_pass();
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLPassSchedulerLifetime, EmptyFlushIsSilent)
{
	mtl::pass_scheduler scheduler;
	scheduler.begin_frame();
	scheduler.begin_pass();

	// An encoder-less flush of an empty queue must not change the split flag.
	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.begin_pass();
	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());
}

TEST(MTLPassSchedulerLifetime, CyclicReferenceQueuesOneTextureBarrier)
{
	// I4: the cache flags a cyclic reference and the renderer turns the flag into one
	// queue-scope edge on the next pass.  The flag is single-read, so two samples of
	// the same surface in one clause emit one barrier.
	struct probe_cache : mtl::texture_cache
	{
		using mtl::texture_cache::insert_texture_barrier;
	};

	probe_cache cache;
	mtl::command_recorder recorder;

	EXPECT_FALSE(cache.consume_texture_barrier_request());

	cache.insert_texture_barrier(recorder, nullptr, true);
	cache.insert_texture_barrier(recorder, nullptr, false);

	EXPECT_EQ(cache.diagnostic_texture_barriers, 2u);
	EXPECT_TRUE(cache.consume_texture_barrier_request());
	EXPECT_FALSE(cache.consume_texture_barrier_request());

	// The request orders an already closed encoder, so it is never an illegal
	// intra-pass edge and it does not outlive its frame.
	mtl::pass_scheduler scheduler;

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.request_texture_barrier();
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());

	scheduler.begin_frame();
	scheduler.begin_pass();
	scheduler.flush(nullptr);
	EXPECT_FALSE(scheduler.split_requested());
}

// =============================================================================
// 5. Surface pixel formats (MTLFormats.cpp:32-84; HLD-02 §2.7)
// =============================================================================

TEST(MTLFormatsSurface, ColorFormatMatchesTheBackendTable)
{
	using row = mapping_row<rsx::surface_color_format, MTL::PixelFormat>;

	const row table[] = {
		{ rsx::surface_color_format::b8, MTL::PixelFormat::PixelFormatR8Unorm },
		{ rsx::surface_color_format::g8b8, MTL::PixelFormat::PixelFormatRG8Unorm },
		{ rsx::surface_color_format::r5g6b5, MTL::PixelFormat::PixelFormatB5G6R5Unorm },
		{ rsx::surface_color_format::x1r5g5b5_z1r5g5b5, MTL::PixelFormat::PixelFormatBGR5A1Unorm },
		{ rsx::surface_color_format::x1r5g5b5_o1r5g5b5, MTL::PixelFormat::PixelFormatBGR5A1Unorm },
		{ rsx::surface_color_format::x8r8g8b8_z8r8g8b8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ rsx::surface_color_format::x8r8g8b8_o8r8g8b8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ rsx::surface_color_format::a8r8g8b8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ rsx::surface_color_format::x8b8g8r8_z8b8g8r8, MTL::PixelFormat::PixelFormatRGBA8Unorm },
		{ rsx::surface_color_format::x8b8g8r8_o8b8g8r8, MTL::PixelFormat::PixelFormatRGBA8Unorm },
		{ rsx::surface_color_format::a8b8g8r8, MTL::PixelFormat::PixelFormatRGBA8Unorm },
		{ rsx::surface_color_format::w16z16y16x16, MTL::PixelFormat::PixelFormatRGBA16Float },
		{ rsx::surface_color_format::w32z32y32x32, MTL::PixelFormat::PixelFormatRGBA32Float },
		{ rsx::surface_color_format::x32, MTL::PixelFormat::PixelFormatR32Float },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 14u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_surface_color_format(entry.guest), entry.host);
	}
}

TEST(MTLFormatsSurface, DepthFormatMatchesTheBackendTable)
{
	using row = mapping_row<rsx::surface_depth_format2, MTL::PixelFormat>;

	const row table[] = {
		{ rsx::surface_depth_format2::z16_uint, MTL::PixelFormat::PixelFormatDepth16Unorm },
		{ rsx::surface_depth_format2::z16_float, MTL::PixelFormat::PixelFormatDepth32Float },
		{ rsx::surface_depth_format2::z24s8_uint, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 },
		{ rsx::surface_depth_format2::z24s8_float, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 4u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_surface_depth_format(entry.guest), entry.host);
	}
}

// =============================================================================
// 6. Guest texture formats (MTLFormats.cpp:86-186; HLD-02 §2.7; LLD-texture §4)
// =============================================================================

TEST(MTLFormatsTexture, TextureFormatMatchesTheBackendTable)
{
	using row = mapping_row<u32, MTL::PixelFormat>;

	const row table[] = {
		{ CELL_GCM_TEXTURE_B8, MTL::PixelFormat::PixelFormatR8Unorm },
		{ CELL_GCM_TEXTURE_A8R8G8B8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_D8R8G8B8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
#if !defined(__APPLE__) || !defined(ARCH_X64)
		// The uploader keeps a packed format packed here (TextureUtils.cpp:950), so the
		// host format holds the guest bits; the read permutes its field order.
		{ CELL_GCM_TEXTURE_R5G6B5, MTL::PixelFormat::PixelFormatB5G6R5Unorm },
		{ CELL_GCM_TEXTURE_A1R5G5B5, MTL::PixelFormat::PixelFormatA1BGR5Unorm },
		{ CELL_GCM_TEXTURE_A4R4G4B4, MTL::PixelFormat::PixelFormatABGR4Unorm },
		{ CELL_GCM_TEXTURE_R5G5B5A1, MTL::PixelFormat::PixelFormatBGR5A1Unorm },
		{ CELL_GCM_TEXTURE_D1R5G5B5, MTL::PixelFormat::PixelFormatA1BGR5Unorm },
		{ CELL_GCM_TEXTURE_R6G5B5, MTL::PixelFormat::PixelFormatB5G6R5Unorm },
#else
		// The Intel uploader expands every packed format to BGRA8 (TextureUtils.cpp:966-1015).
		{ CELL_GCM_TEXTURE_R5G6B5, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_A1R5G5B5, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_A4R4G4B4, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_R5G5B5A1, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_D1R5G5B5, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_R6G5B5, MTL::PixelFormat::PixelFormatBGRA8Unorm },
#endif
		{ CELL_GCM_TEXTURE_G8B8, MTL::PixelFormat::PixelFormatRG8Unorm },
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO8, MTL::PixelFormat::PixelFormatRG8Unorm },
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO_S8, MTL::PixelFormat::PixelFormatRG8Snorm },
		{ CELL_GCM_TEXTURE_Y16_X16, MTL::PixelFormat::PixelFormatRG16Unorm },
		{ CELL_GCM_TEXTURE_Y16_X16_FLOAT, MTL::PixelFormat::PixelFormatRG16Float },
		{ CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT, MTL::PixelFormat::PixelFormatRGBA16Float },
		{ CELL_GCM_TEXTURE_X16, MTL::PixelFormat::PixelFormatR16Unorm },
		{ CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT, MTL::PixelFormat::PixelFormatRGBA32Float },
		{ CELL_GCM_TEXTURE_X32_FLOAT, MTL::PixelFormat::PixelFormatR32Float },
		{ CELL_GCM_TEXTURE_DEPTH16, MTL::PixelFormat::PixelFormatDepth16Unorm },
		{ CELL_GCM_TEXTURE_DEPTH16_FLOAT, MTL::PixelFormat::PixelFormatDepth32Float },
		{ CELL_GCM_TEXTURE_DEPTH24_D8, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 },
		{ CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 },
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT1, MTL::PixelFormat::PixelFormatBC1_RGBA },
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT23, MTL::PixelFormat::PixelFormatBC2_RGBA },
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT45, MTL::PixelFormat::PixelFormatBC3_RGBA },
		{ CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
		{ CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8, MTL::PixelFormat::PixelFormatBGRA8Unorm },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 27u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(entry.guest);
		EXPECT_EQ(mtl::get_texture_format(entry.guest), entry.host);
	}
}

TEST(MTLFormatsLayout, ComponentLayoutMatchesTheBackendTable)
{
	struct row
	{
		u32 guest;
		std::array<MTL::TextureSwizzle, 4> host;
	};

	constexpr auto a = MTL::TextureSwizzleAlpha;
	constexpr auto r = MTL::TextureSwizzleRed;
	constexpr auto g = MTL::TextureSwizzleGreen;
	constexpr auto b = MTL::TextureSwizzleBlue;
	constexpr auto o = MTL::TextureSwizzleOne;

	const row table[] = {
#if !defined(__APPLE__) || !defined(ARCH_X64)
		// Metal names a packed format from its most significant field, so the read
		// restores the guest field order (REVIEW-RSX-NATIVE.md R1).  The guest
		// A4R4G4B4 is stored R4G4B4A4, so every field moves one place.
		{ CELL_GCM_TEXTURE_A1R5G5B5, { a, b, g, r } },
		{ CELL_GCM_TEXTURE_R5G5B5A1, { a, b, g, r } },
		{ CELL_GCM_TEXTURE_R6G5B5, { o, b, g, r } },
		{ CELL_GCM_TEXTURE_R5G6B5, { o, b, g, r } },
		{ CELL_GCM_TEXTURE_A4R4G4B4, { r, a, b, g } },
		{ CELL_GCM_TEXTURE_D1R5G5B5, { o, b, g, r } },
#else
		// The Intel uploader expands every packed format to BGRA8, so the guest
		// channels already sit in the A-R-G-B order.
		{ CELL_GCM_TEXTURE_A1R5G5B5, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_R5G5B5A1, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_R6G5B5, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_R5G6B5, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_A4R4G4B4, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_D1R5G5B5, { o, r, g, b } },
#endif
		{ CELL_GCM_TEXTURE_A8R8G8B8, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT, { a, r, g, b } },
		{ CELL_GCM_TEXTURE_DEPTH24_D8, { r, r, r, r } },
		{ CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT, { r, r, r, r } },
		{ CELL_GCM_TEXTURE_DEPTH16, { r, r, r, r } },
		{ CELL_GCM_TEXTURE_DEPTH16_FLOAT, { r, r, r, r } },
		{ CELL_GCM_TEXTURE_X32_FLOAT, { r, r, r, r } },
		{ CELL_GCM_TEXTURE_B8, { o, r, r, r } },
		{ CELL_GCM_TEXTURE_X16, { r, o, r, o } },
		{ CELL_GCM_TEXTURE_G8B8, { g, r, g, r } },
		{ CELL_GCM_TEXTURE_Y16_X16, { g, r, g, r } },
		{ CELL_GCM_TEXTURE_Y16_X16_FLOAT, { r, g, r, g } },
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO8, { r, g, r, g } },
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO_S8, { r, g, r, g } },
		{ CELL_GCM_TEXTURE_D8R8G8B8, { o, r, g, b } },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 22u);

	for (const row& entry : table)
	{
		const auto layout = mtl::get_texture_component_layout(entry.guest);
		for (usz i = 0; i < layout.size(); ++i)
		{
			SCOPED_TRACE(std::to_string(entry.guest) + "[" + std::to_string(i) + "]");
			EXPECT_EQ(layout[i], entry.host[i]);
		}
	}

	// An unknown format takes the A-R-G-B default.
	const auto fallback = mtl::get_texture_component_layout(0u);
	EXPECT_EQ(fallback[0], a);
	EXPECT_EQ(fallback[1], r);
	EXPECT_EQ(fallback[2], g);
	EXPECT_EQ(fallback[3], b);
}

// =============================================================================
// 7. The presentation pixel size and byte order
//     (MTLFormats.cpp:11-30)
// =============================================================================

TEST(MTLFormatsPixelSize, OnlyTheEightBitOutputFormatsHaveASize)
{
	struct row
	{
		MTL::PixelFormat format;
		u32 size;
		bool bgra;
	};

	const row table[] = {
		{ MTL::PixelFormat::PixelFormatBGRA8Unorm, 4, true },
		{ MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB, 4, true },
		{ MTL::PixelFormat::PixelFormatRGBA8Unorm, 4, false },
		{ MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB, 4, false },
		{ MTL::PixelFormat::PixelFormatRGBA16Float, 0, false },
		{ MTL::PixelFormat::PixelFormatR8Unorm, 0, false },
		{ MTL::PixelFormat::PixelFormatDepth32Float, 0, false },
		{ MTL::PixelFormat::PixelFormatRGBA32Float, 0, false },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u64>(entry.format));
		EXPECT_EQ(mtl::get_pixel_size(entry.format), entry.size);
		EXPECT_EQ(mtl::is_bgra_format(entry.format), entry.bgra);
	}
}

TEST(MTLFormatsPixelSize, EveryPixelFormatIsFourBytesOrZero)
{
	// The output path is total: it returns 4 or 0 for any enumerator, and is_bgra
	// is true for exactly the two BGRA8 formats.
	for (u32 raw = 0; raw < 256; ++raw)
	{
		SCOPED_TRACE(raw);
		const MTL::PixelFormat format = static_cast<MTL::PixelFormat>(raw);
		const u32 size = mtl::get_pixel_size(format);

		EXPECT_TRUE(size == 0 || size == 4);
		EXPECT_EQ(mtl::is_bgra_format(format),
			format == MTL::PixelFormat::PixelFormatBGRA8Unorm || format == MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
	}
}

// =============================================================================
// 8. The shader constant block layouts
//     (MTLConstants.h:18-102; RSXDefines2.glsl)
// =============================================================================

TEST(MTLConstantsLayout, StructSizesMatchTheShaderAbi)
{
	EXPECT_EQ(sizeof(mtl::vertex_context), 96u);
	EXPECT_EQ(sizeof(mtl::draw_parameters), 168u);
	EXPECT_EQ(sizeof(mtl::fragment_context), 32u);
	EXPECT_EQ(sizeof(mtl::sampler_info), 48u);
	EXPECT_EQ(sizeof(mtl::blend_values), 36u);
	EXPECT_EQ(sizeof(mtl::fill_values), 32u);
	EXPECT_EQ(sizeof(mtl::vec4_block), 16u);
}

TEST(MTLConstantsLayout, FieldOffsetsMatchTheShaderAbi)
{
	EXPECT_EQ(offsetof(mtl::vertex_context, scale_offset_mat), 0u);
	EXPECT_EQ(offsetof(mtl::vertex_context, user_clip_configuration_bits), 64u);
	EXPECT_EQ(offsetof(mtl::vertex_context, transform_branch_bits), 68u);
	EXPECT_EQ(offsetof(mtl::vertex_context, point_size), 72u);
	EXPECT_EQ(offsetof(mtl::vertex_context, z_near), 76u);
	EXPECT_EQ(offsetof(mtl::vertex_context, z_far), 80u);

	EXPECT_EQ(offsetof(mtl::draw_parameters, vertex_base_index), 0u);
	EXPECT_EQ(offsetof(mtl::draw_parameters, fs_texture_base_index), 28u);
	EXPECT_EQ(offsetof(mtl::draw_parameters, fs_stipple_pattern_offset), 32u);
	EXPECT_EQ(offsetof(mtl::draw_parameters, attrib_data), 40u);

	EXPECT_EQ(offsetof(mtl::fragment_context, fog_param0), 0u);
	EXPECT_EQ(offsetof(mtl::fragment_context, rop_control), 8u);
	EXPECT_EQ(offsetof(mtl::fragment_context, alpha_ref), 12u);
	EXPECT_EQ(offsetof(mtl::fragment_context, fog_mode), 16u);
	EXPECT_EQ(offsetof(mtl::fragment_context, wpos_scale), 20u);
	EXPECT_EQ(offsetof(mtl::fragment_context, wpos_bias), 24u);

	// Bytes 0..3 are the block's reserved word, so blend_eqn is at offset 4.
	EXPECT_EQ(offsetof(mtl::blend_values, reserved), 0u);
	EXPECT_EQ(offsetof(mtl::blend_values, blend_eqn), 4u);
	EXPECT_EQ(offsetof(mtl::blend_values, blend_sfactors), 8u);
	EXPECT_EQ(offsetof(mtl::blend_values, blend_dfactors), 12u);
	EXPECT_EQ(offsetof(mtl::blend_values, blend_constants), 16u);
	// The logic op travels in the same block, behind the blend constants
	// (REVIEW-FINDINGS.md C1), so a change of op does not recompile the shader.
	EXPECT_EQ(offsetof(mtl::blend_values, logic_op), 32u);

	// The built-in fill reads the colour at 0..15 and the depth at 16.
	EXPECT_EQ(offsetof(mtl::fill_values, color), 0u);
	EXPECT_EQ(offsetof(mtl::fill_values, depth), 16u);

	EXPECT_EQ(offsetof(mtl::vec4_block, v), 0u);
}

TEST(MTLConstantsLayout, SamplerInfoMatchesTheRsxSlot)
{
	// The shared framework fills rsx::fragment_program_texture_config::TIU_slot,
	// so the two layouts must be byte-identical (MTLConstants.h:58-68).
	using slot = rsx::fragment_program_texture_config::TIU_slot;

	EXPECT_EQ(sizeof(mtl::sampler_info), sizeof(slot));
	EXPECT_EQ(offsetof(mtl::sampler_info, scale), offsetof(slot, scale));
	EXPECT_EQ(offsetof(mtl::sampler_info, bias), offsetof(slot, bias));
	EXPECT_EQ(offsetof(mtl::sampler_info, clamp_min), offsetof(slot, clamp_min));
	EXPECT_EQ(offsetof(mtl::sampler_info, clamp_max), offsetof(slot, clamp_max));
	EXPECT_EQ(offsetof(mtl::sampler_info, remap), offsetof(slot, remap));
	EXPECT_EQ(offsetof(mtl::sampler_info, control), offsetof(slot, control));
}

TEST(MTLConstantsLayout, CapacityConstantsDescribeTheRings)
{
	EXPECT_EQ(mtl::vertex_constant_count, 468u);
	EXPECT_EQ(mtl::stipple_block_count, 8u);
	EXPECT_EQ(mtl::sampler_info_count, 16u);
	EXPECT_EQ(mtl::frame_slot_count, 3u);

	EXPECT_EQ(mtl::constants_heap::draw_capacity, 8192u);
	EXPECT_EQ(mtl::constants_heap::constant_capacity, 65536u);
	EXPECT_EQ(mtl::constants_heap::transform_capacity, 512u);
	EXPECT_EQ(mtl::constants_heap::fill_capacity, 1024u);

	EXPECT_EQ(mtl::constants_heap::stream_capacity, 8u * 1024u * 1024u);
	EXPECT_EQ(mtl::constants_heap::volatile_capacity, 2u * 1024u * 1024u);
	EXPECT_EQ(mtl::constants_heap::index_capacity, 2u * 1024u * 1024u);
	EXPECT_EQ(mtl::constants_heap::overlay_vertex_capacity, 4u * 1024u * 1024u);
	EXPECT_EQ(mtl::constants_heap::overlay_uniform_capacity, 128u * 1024u);
}

TEST(MTLConstantsRing, ABlockStartsAlignedAndAdvancesByItsAlignedSize)
{
	// A three-byte index block consumes four bytes, so the next block starts on a
	// four-byte boundary and the draw's byte address stays aligned.
	const mtl::ring_block first = mtl::allocate_ring_block(0, 3, 64, 4);
	EXPECT_FALSE(first.grow);
	EXPECT_EQ(first.offset, 0u);
	EXPECT_EQ(first.end, 4u);
	EXPECT_EQ(first.capacity, 64u);

	const mtl::ring_block second = mtl::allocate_ring_block(first.end, 5, first.capacity, 4);
	EXPECT_FALSE(second.grow);
	EXPECT_EQ(second.offset, 4u);
	EXPECT_EQ(second.end, 12u);
	EXPECT_EQ(second.capacity, 64u);
}

TEST(MTLConstantsRing, AFittingBlockEndsExactlyAtCapacity)
{
	const mtl::ring_block block = mtl::allocate_ring_block(0, 0x100, 0x100, 4);
	EXPECT_FALSE(block.grow);
	EXPECT_EQ(block.offset, 0u);
	EXPECT_EQ(block.end, 0x100u);
	EXPECT_EQ(block.capacity, 0x100u);
}

TEST(MTLConstantsRing, AnOversizedBlockGrowsTheBufferAndStartsAtZero)
{
	// The old ring allocated its small capacity and then copied the oversized
	// block past the end.  The reservation must instead ask for a buffer that
	// holds the block (MTLConstants.cpp:104-146, byte_ring at :187-213).
	const mtl::ring_block block = mtl::allocate_ring_block(32, 0x100, 64, 4);
	EXPECT_TRUE(block.grow);
	EXPECT_EQ(block.offset, 0u);
	EXPECT_EQ(block.end, 0x100u);
	EXPECT_EQ(block.capacity, 0x100u);
}

TEST(MTLConstantsRing, AnOverflowGrowsEvenWhenTheBlockItselfFits)
{
	const mtl::ring_block block = mtl::allocate_ring_block(60, 8, 64, 4);
	EXPECT_TRUE(block.grow);
	EXPECT_EQ(block.offset, 0u);
	EXPECT_EQ(block.end, 8u);
	EXPECT_EQ(block.capacity, 64u);
}

// =============================================================================
// 9. Depth bounds and the emulated depth encoding
//     (MTLDepthStencil.cpp:106-129; MTLDepthStencil.h:50-57;
//      TextureUtils.cpp:1517-1521, :1873-1887; REVIEW-FINDINGS E7)
// =============================================================================

TEST(MTLDepthBounds, DisabledRangeIsOpen)
{
	const mtl::depth_bounds defaults{};
	EXPECT_FLOAT_EQ(defaults.min, 0.0f);
	EXPECT_FLOAT_EQ(defaults.max, 1.0f);

	// The power-on state has the bounds test disabled (rsx_methods.cpp:353), so the
	// capture must collapse to [0,1] under both emulation settings.
	reset_registers();

	const mtl::depth_bounds plain = mtl::capture_depth_bounds(false);
	EXPECT_FLOAT_EQ(plain.min, 0.0f);
	EXPECT_FLOAT_EQ(plain.max, 1.0f);

	const mtl::depth_bounds emulated = mtl::capture_depth_bounds(true);
	EXPECT_FLOAT_EQ(emulated.min, 0.0f);
	EXPECT_FLOAT_EQ(emulated.max, 1.0f);
}

TEST(MTLDepthBounds, ClampUsesTheDepthFormatRange)
{
	constexpr f32 values[] = { -1000.0f, -1.0f, -0.0f, 0.0f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f, 1.5f, 1000.0f };

	for (const f32 value : values)
	{
		SCOPED_TRACE(value);

		// The integer formats saturate into [0,1].
		EXPECT_FLOAT_EQ(rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z16_uint, value), std::clamp(value, 0.0f, 1.0f));
		EXPECT_FLOAT_EQ(rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z24s8_uint, value), std::clamp(value, 0.0f, 1.0f));

		// z24s8_float clamps the low side only.
		EXPECT_FLOAT_EQ(rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z24s8_float, value), std::max(value, 0.0f));

		// z16_float saturates at the E4M12 top, which is just below 1, not at 1.
		const f32 f16 = rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z16_float, value);
		EXPECT_GE(f16, 0.0f);
		EXPECT_LE(f16, 1.0f);
		if (value >= 0.0f && value <= 0.5f)
		{
			EXPECT_FLOAT_EQ(f16, value);
		}
	}
}

TEST(MTLDepthBounds, Z16FloatSaturatesJustBelowOne)
{
	// decode_e4m12(0xFFFF) is 0.9998779..., not 1.0, so a z16_float bound that a
	// guest sets to 1 saturates below one.
	const f32 top = rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z16_float, 1.0f);
	EXPECT_LT(top, 1.0f);
	EXPECT_GT(top, 0.99f);
	EXPECT_FLOAT_EQ(top, rsx::clamp_depth_bounds_value(rsx::surface_depth_format2::z16_float, 1.0e30f));
}

TEST(MTLDepthBounds, EmulatedDepthHalvesTheBitPattern)
{
	// encode_emulated_depth clears the sign and shifts the magnitude right by one.
	// It returns 0 for every negative input, and it is monotonic on the rest.
	EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(0.0f)), 0u);
	EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(-0.0f)), 0u);
	EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(-1.0f)), 0u);
	EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(-1000.0f)), 0u);
	EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(1.0f)), 0x1fc00000u);

	// Infinity's bit pattern shifts to 0x3fc00000, which is 1.5.
	EXPECT_FLOAT_EQ(rsx::encode_emulated_depth(std::numeric_limits<f32>::infinity()), 1.5f);

	// The transform is exactly a right shift of the sign-cleared bit pattern, so
	// the result matches that shift for every finite non-negative input.
	for (u32 pattern = 0; pattern < 0x7f800000u; pattern += 0x00080000u)
	{
		SCOPED_TRACE(pattern);
		const u32 expected = (pattern & 0x7fffffffu) >> 1;
		EXPECT_EQ(std::bit_cast<u32>(rsx::encode_emulated_depth(std::bit_cast<f32>(pattern))), expected);
	}
}

TEST(MTLDepthBounds, EmulatedDepthIsMonotonic)
{
	f32 previous = -1.0f;
	for (u32 raw = 0; raw < 0x3f800001u; raw += 0x00100000u)
	{
		const f32 value = std::bit_cast<f32>(raw);
		const f32 encoded = rsx::encode_emulated_depth(value);
		SCOPED_TRACE(value);
		EXPECT_GE(encoded, previous);
		previous = encoded;
	}
}

TEST(MTLDepthBounds, CaptureAppliesTheClampAndTheEncoding)
{
	reset_registers();
	set_register(NV4097_SET_DEPTH_BOUNDS_TEST_ENABLE, 1);
	set_register(NV4097_SET_DEPTH_BOUNDS_MIN, std::bit_cast<u32>(0.25f));
	set_register(NV4097_SET_DEPTH_BOUNDS_MAX, std::bit_cast<u32>(0.75f));

	const auto format = rsx::method_registers.surface_depth_fmt();

	const mtl::depth_bounds plain = mtl::capture_depth_bounds(false);
	EXPECT_FLOAT_EQ(plain.min, std::clamp(rsx::clamp_depth_bounds_value(format, 0.25f), 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(plain.max, std::clamp(rsx::clamp_depth_bounds_value(format, 0.75f), 0.0f, 1.0f));

	const mtl::depth_bounds emulated = mtl::capture_depth_bounds(true);
	EXPECT_FLOAT_EQ(emulated.min, std::clamp(rsx::encode_emulated_depth(rsx::clamp_depth_bounds_value(format, 0.25f)), 0.0f, 1.0f));
	EXPECT_FLOAT_EQ(emulated.max, std::clamp(rsx::encode_emulated_depth(rsx::clamp_depth_bounds_value(format, 0.75f)), 0.0f, 1.0f));
}

// =============================================================================
// 10. Scissor and clip geometry
//      (MTLRasterState.cpp:97-105; rsx_utils.h:278-361; LLD-raster §3.1)
// =============================================================================

TEST(MTLScissorGeometry, ScissorRectFollowsTheClipSpaceFlip)
{
	constexpr u16 clip_height = 64;

	for (u32 x1 = 0; x1 < 16; ++x1)
	{
		for (u32 y1 = 0; y1 < 16; ++y1)
		{
			for (u32 width = 0; width < 8; ++width)
			{
				for (u32 height = 0; height < 8; ++height)
				{
					SCOPED_TRACE(std::to_string(x1) + "," + std::to_string(y1) + " " + std::to_string(width) + "x" + std::to_string(height));
					const MTL::ScissorRect rect = mtl::build_scissor_rect(x1, y1, x1 + width, y1 + height, clip_height);

					// The flip moves a guest region to the mirrored target row and
					// keeps its width and height (LLD-raster §3.1).
					EXPECT_EQ(rect.x, x1);
					EXPECT_EQ(rect.y, clip_height - (y1 + height));
					EXPECT_EQ(rect.width, width);
					EXPECT_EQ(rect.height, height);
				}
			}
		}
	}
}

TEST(MTLScissorGeometry, ViewportIsTheSurfaceClipWithOneYFlip)
{
	constexpr u16 widths[] = { 1, 2, 640, 1280, 1920 };
	constexpr u16 heights[] = { 1, 240, 720, 1080 };

	for (const u16 width : widths)
	{
		for (const u16 height : heights)
		{
			SCOPED_TRACE(std::to_string(width) + "x" + std::to_string(height));
			const MTL::Viewport viewport = mtl::build_viewport(width, height, 0.25, 0.75);

			// The rectangle is the surface clip, not the RSX viewport rect
			// (LLD-raster §3.1): the transform lives in `scale_offset_mat`.  The
			// height is negated because the shader clip space is y-down and
			// Metal's is y-up (invariant 2, kk_cmd_draw.c:785-793).
			EXPECT_DOUBLE_EQ(viewport.originX, 0.0);
			EXPECT_DOUBLE_EQ(viewport.originY, static_cast<f64>(height));
			EXPECT_DOUBLE_EQ(viewport.width, static_cast<f64>(width));
			EXPECT_DOUBLE_EQ(viewport.height, -static_cast<f64>(height));
			EXPECT_DOUBLE_EQ(viewport.znear, 0.25);
			EXPECT_DOUBLE_EQ(viewport.zfar, 0.75);
		}
	}
}

TEST(MTLScissorGeometry, ClipRegionWithFitStaysInsideTheParent)
{
	// attempt_fit = true resizes the region to the parent edge, so the result never
	// leaves the parent and never grows past the request.
	constexpr u32 parents[] = { 1, 8, 64, 1280 };

	for (const u32 parent_width : parents)
	{
		for (const u32 parent_height : parents)
		{
			for (u32 clip_x = 0; clip_x < 6; ++clip_x)
			{
				for (u32 clip_y = 0; clip_y < 6; ++clip_y)
				{
					for (u32 clip_width = 0; clip_width < 6; ++clip_width)
					{
						for (u32 clip_height = 0; clip_height < 6; ++clip_height)
						{
							SCOPED_TRACE(std::to_string(parent_width) + "x" + std::to_string(parent_height));
							auto [x, y, width, height] = rsx::clip_region(parent_width, parent_height, clip_x, clip_y, clip_width, clip_height, true);

							EXPECT_LE(x + width, parent_width);
							EXPECT_LE(y + height, parent_height);
							EXPECT_LE(width, clip_width);
							EXPECT_LE(height, clip_height);
						}
					}
				}
			}
		}
	}
}

TEST(MTLScissorGeometry, ClipRegionWithoutFitPinsTheOrigin)
{
	// attempt_fit = false: a region that starts at or beyond the parent edge is
	// pinned to the origin instead of being resized (rsx_utils.h:295, :315).
	for (u32 clip_x = 0; clip_x < 24; ++clip_x)
	{
		SCOPED_TRACE(clip_x);
		auto [x, y, width, height] = rsx::clip_region<u32>(16, 16, clip_x, clip_x, 4, 4, false);

		if (clip_x >= 16)
		{
			EXPECT_EQ(x, 0u);
			EXPECT_EQ(y, 0u);
		}
		else
		{
			EXPECT_EQ(x, clip_x);
			EXPECT_EQ(y, clip_x);
		}
	}
}

TEST(MTLScissorGeometry, IntersectRegionClampsToTheSmallerExtent)
{
	// Equal addresses: both origins are zero and the size is the smaller extent.
	{
		const auto [src, dst, size] = rsx::intersect_region(0x1000, 40, 30, 0x1000, 20, 10, 40);
		EXPECT_EQ(src, position2u(0, 0));
		EXPECT_EQ(dst, position2u(0, 0));
		EXPECT_EQ(size, size2u(20, 10));
	}

	// The child starts below the parent, so the parent is shifted down into the
	// child by the whole-row part of the address gap (0x100 bytes at 64-byte rows).
	{
		const auto [src, dst, size] = rsx::intersect_region(0x1000, 64, 8, 0x0F00, 64, 16, 64);
		EXPECT_EQ(src, position2u(0, 0));
		EXPECT_EQ(dst, position2u(0, 4));
		EXPECT_EQ(size, size2u(64, 8));
	}

	// A child above the parent: the retained extent never exceeds the parent.
	for (u32 offset = 0; offset < 64; offset += 4)
	{
		const auto [src, dst, size] = rsx::intersect_region(0x1000, 16, 16, 0x1000 + offset, 32, 32, 16);
		SCOPED_TRACE(offset);
		EXPECT_LE(size.width, 16u);
		EXPECT_LE(size.height, 16u);
	}
}

// =============================================================================
// 11. Sample-count folding and depth-format predicates
//      (TextureUtils.cpp:1477-1515, :1376-1474; REVIEW-FINDINGS E10)
// =============================================================================

TEST(MTLSampleCount, GuestAntialiasingFoldsToTheHostCount)
{
	using row = mapping_row<rsx::surface_antialiasing, u8>;

	const row table[] = {
		{ rsx::surface_antialiasing::center_1_sample, 1 },
		{ rsx::surface_antialiasing::diagonal_centered_2_samples, 2 },
		{ rsx::surface_antialiasing::square_centered_4_samples, 4 },
		{ rsx::surface_antialiasing::square_rotated_4_samples, 4 },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 4u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(rsx::get_format_sample_count(entry.guest), entry.host);
	}
}

TEST(MTLSampleCount, DepthFormatPredicatesFollowTheDepthFormat)
{
	constexpr rsx::surface_depth_format2 formats[] = {
		rsx::surface_depth_format2::z16_uint,
		rsx::surface_depth_format2::z16_float,
		rsx::surface_depth_format2::z24s8_uint,
		rsx::surface_depth_format2::z24s8_float,
	};

	for (const auto format : formats)
	{
		SCOPED_TRACE(static_cast<u32>(format));
		const bool stencil = format == rsx::surface_depth_format2::z24s8_uint || format == rsx::surface_depth_format2::z24s8_float;
		const bool floating = format == rsx::surface_depth_format2::z16_float || format == rsx::surface_depth_format2::z24s8_float;

		EXPECT_EQ(rsx::is_depth_stencil_format(format), stencil);
		EXPECT_EQ(rsx::is_float_depth_format(format), floating);
	}
}

TEST(MTLSampleCount, GuestBlockSizesMatchTheTexelPitch)
{
	using row = mapping_row<rsx::surface_color_format, u8>;

	const row color[] = {
		{ rsx::surface_color_format::b8, 1 },
		{ rsx::surface_color_format::g8b8, 2 },
		{ rsx::surface_color_format::r5g6b5, 2 },
		{ rsx::surface_color_format::x1r5g5b5_z1r5g5b5, 2 },
		{ rsx::surface_color_format::x1r5g5b5_o1r5g5b5, 2 },
		{ rsx::surface_color_format::x8r8g8b8_z8r8g8b8, 4 },
		{ rsx::surface_color_format::x8r8g8b8_o8r8g8b8, 4 },
		{ rsx::surface_color_format::a8r8g8b8, 4 },
		{ rsx::surface_color_format::x8b8g8r8_z8b8g8r8, 4 },
		{ rsx::surface_color_format::x8b8g8r8_o8b8g8r8, 4 },
		{ rsx::surface_color_format::a8b8g8r8, 4 },
		{ rsx::surface_color_format::x32, 4 },
		{ rsx::surface_color_format::w16z16y16x16, 8 },
		{ rsx::surface_color_format::w32z32y32x32, 16 },
	};

	EXPECT_EQ(sizeof(color) / sizeof(color[0]), 14u);

	for (const row& entry : color)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(rsx::get_format_block_size_in_bytes(entry.guest), entry.host);
	}

	// The guest depth texel is 2 bytes for z16 and 4 bytes for z24s8.
	EXPECT_EQ(rsx::get_format_block_size_in_bytes(rsx::surface_depth_format2::z16_uint), 2u);
	EXPECT_EQ(rsx::get_format_block_size_in_bytes(rsx::surface_depth_format2::z16_float), 2u);
	EXPECT_EQ(rsx::get_format_block_size_in_bytes(rsx::surface_depth_format2::z24s8_uint), 4u);
	EXPECT_EQ(rsx::get_format_block_size_in_bytes(rsx::surface_depth_format2::z24s8_float), 4u);
}

TEST(MTLSampleCount, CompressedTexelBlockSizes)
{
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_DXT1), 4);
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_DXT23), 4);
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_DXT45), 4);

	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_A8R8G8B8), 1);
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_HILO8), 1);
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8), 2);
	EXPECT_EQ(rsx::get_format_block_size_in_texel(CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8), 2);
}

// =============================================================================
// 12. The pure raster-state derivations
//      (MTLRasterState.cpp:39-95; LLD-raster §3)
// =============================================================================

TEST(MTLRasterDerivations, CullModeFollowsTheEnableAndFace)
{
	constexpr rsx::cull_face faces[] = {
		rsx::cull_face::front,
		rsx::cull_face::back,
		rsx::cull_face::front_and_back,
	};

	for (const bool enabled : { false, true })
	{
		for (const rsx::cull_face face : faces)
		{
			SCOPED_TRACE(static_cast<u32>(face) * 2 + (enabled ? 1u : 0u));

			mtl::raster_state state{};
			state.cull_enabled = enabled;
			state.cull_face = face;

			MTL::CullMode expected = MTL::CullModeNone;
			if (enabled && face == rsx::cull_face::front)
			{
				expected = MTL::CullModeFront;
			}
			else if (enabled && face == rsx::cull_face::back)
			{
				expected = MTL::CullModeBack;
			}

			EXPECT_EQ(mtl::capture_cull_mode(state), expected);
			EXPECT_EQ(mtl::culls_both_faces(state), enabled && face == rsx::cull_face::front_and_back);
		}
	}
}

TEST(MTLRasterDerivations, FrontFaceFollowsTheClipSpaceFlip)
{
	mtl::raster_state state{};

	// A Y mirror reverses the screen-space orientation, so the guest winding maps
	// to the opposite Metal winding.
	state.front_face = rsx::front_face::cw;
	EXPECT_EQ(mtl::capture_front_face(state), MTL::WindingCounterClockwise);

	state.front_face = rsx::front_face::ccw;
	EXPECT_EQ(mtl::capture_front_face(state), MTL::WindingClockwise);
}

TEST(MTLRasterDerivations, FillModeFollowsTheSurvivingFace)
{
	constexpr rsx::cull_face faces[] = {
		rsx::cull_face::front,
		rsx::cull_face::back,
		rsx::cull_face::front_and_back,
	};

	constexpr rsx::polygon_mode modes[] = {
		rsx::polygon_mode::fill,
		rsx::polygon_mode::line,
	};

	for (const bool enabled : { false, true })
	{
		for (const rsx::cull_face face : faces)
		{
			for (const rsx::polygon_mode front_mode : modes)
			{
				for (const rsx::polygon_mode back_mode : modes)
				{
					mtl::raster_state state{};
					state.cull_enabled = enabled;
					state.cull_face = face;
					state.front_polygon_mode = front_mode;
					state.back_polygon_mode = back_mode;

					// A culled front leaves the back mode; that is the only swap.
					const bool front_survives = !enabled || face != rsx::cull_face::front;
					const rsx::polygon_mode mode = front_survives ? front_mode : back_mode;
					const MTL::TriangleFillMode expected =
						mode == rsx::polygon_mode::line ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;

					SCOPED_TRACE(static_cast<u32>(face));
					EXPECT_EQ(mtl::capture_fill_mode(state), expected);
				}
			}
		}
	}
}

TEST(MTLRasterDerivations, DepthBiasNeedsTheFillEnable)
{
	constexpr f32 scales[] = { 0.0f, 1.0f, -2.5f, 0.125f };
	constexpr f32 biases[] = { 0.0f, -1.0f, 3.25f, -0.5f };

	for (const bool enabled : { false, true })
	{
		for (const f32 scale : scales)
		{
			for (const f32 bias : biases)
			{
				mtl::raster_state state{};
				state.poly_offset_fill = enabled;
				state.poly_offset_scale = scale;
				state.poly_offset_bias = bias;

				f32 out_bias = 0.0f;
				f32 out_scale = 0.0f;
				const bool used = mtl::capture_depth_bias(state, out_bias, out_scale);

				SCOPED_TRACE(scale);
				EXPECT_EQ(used, enabled);
				// The two values are copied even when the offset is off.
				EXPECT_FLOAT_EQ(out_bias, bias);
				EXPECT_FLOAT_EQ(out_scale, scale);
			}
		}
	}
}

TEST(MTLRasterDerivations, ClipRangeStaysInsideZeroToOne)
{
	constexpr f32 values[] = { -1.0f, -0.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.25f };

	for (const f32 clip_min : values)
	{
		for (const f32 clip_max : values)
		{
			mtl::raster_state state{};
			state.clip_min = clip_min;
			state.clip_max = clip_max;

			f64 znear = -1.0;
			f64 zfar = -1.0;
			const bool accepted = mtl::capture_clip_range(state, znear, zfar);
			const bool expected = !(clip_min < 0.0f || clip_max > 1.0f || clip_max <= clip_min);

			SCOPED_TRACE(std::to_string(clip_min) + ".." + std::to_string(clip_max));
			EXPECT_EQ(accepted, expected);

			if (accepted)
			{
				EXPECT_DOUBLE_EQ(znear, static_cast<f64>(clip_min));
				EXPECT_DOUBLE_EQ(zfar, static_cast<f64>(clip_max));
			}
		}
	}
}

// =============================================================================
// 13. The sampler latch, its equality and its hash
//      (MTLSamplerState.cpp:14-255; LLD-texture §3.2)
// =============================================================================

TEST(MTLSamplerMapping, AddressModeMatchesTheDomain)
{
	using row = mapping_row<rsx::texture_wrap_mode, MTL::SamplerAddressMode>;

	const row table[] = {
		{ rsx::texture_wrap_mode::wrap, MTL::SamplerAddressModeRepeat },
		{ rsx::texture_wrap_mode::mirror, MTL::SamplerAddressModeMirrorRepeat },
		{ rsx::texture_wrap_mode::clamp_to_edge, MTL::SamplerAddressModeClampToEdge },
		{ rsx::texture_wrap_mode::clamp, MTL::SamplerAddressModeClampToEdge },
		{ rsx::texture_wrap_mode::border, MTL::SamplerAddressModeClampToBorderColor },
		{ rsx::texture_wrap_mode::mirror_once_clamp_to_edge, MTL::SamplerAddressModeMirrorClampToEdge },
		{ rsx::texture_wrap_mode::mirror_once_border, MTL::SamplerAddressModeMirrorClampToEdge },
		{ rsx::texture_wrap_mode::mirror_once_clamp, MTL::SamplerAddressModeMirrorClampToEdge },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_address_mode(entry.guest), entry.host);
	}
}

TEST(MTLSamplerMapping, MinifyFilterNamesBothTheFilterAndTheMipMode)
{
	struct row
	{
		rsx::texture_minify_filter guest;
		MTL::SamplerMinMagFilter min_mag;
		MTL::SamplerMipFilter mip;
	};

	const row table[] = {
		{ rsx::texture_minify_filter::nearest, MTL::SamplerMinMagFilterNearest, MTL::SamplerMipFilterNotMipmapped },
		{ rsx::texture_minify_filter::linear, MTL::SamplerMinMagFilterLinear, MTL::SamplerMipFilterNotMipmapped },
		{ rsx::texture_minify_filter::nearest_nearest, MTL::SamplerMinMagFilterNearest, MTL::SamplerMipFilterNearest },
		{ rsx::texture_minify_filter::linear_nearest, MTL::SamplerMinMagFilterLinear, MTL::SamplerMipFilterNearest },
		{ rsx::texture_minify_filter::nearest_linear, MTL::SamplerMinMagFilterNearest, MTL::SamplerMipFilterLinear },
		{ rsx::texture_minify_filter::linear_linear, MTL::SamplerMinMagFilterLinear, MTL::SamplerMipFilterLinear },
		{ rsx::texture_minify_filter::convolution_min, MTL::SamplerMinMagFilterLinear, MTL::SamplerMipFilterNotMipmapped },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 7u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_min_mag_filter(entry.guest), entry.min_mag);
		EXPECT_EQ(mtl::get_mip_filter(entry.guest), entry.mip);
	}
}

TEST(MTLSamplerMapping, MagnifyFilterMatchesTheDomain)
{
	using row = mapping_row<rsx::texture_magnify_filter, MTL::SamplerMinMagFilter>;

	const row table[] = {
		{ rsx::texture_magnify_filter::nearest, MTL::SamplerMinMagFilterNearest },
		{ rsx::texture_magnify_filter::linear, MTL::SamplerMinMagFilterLinear },
		{ rsx::texture_magnify_filter::convolution_mag, MTL::SamplerMinMagFilterLinear },
	};

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_min_mag_filter(entry.guest), entry.host);
	}
}

TEST(MTLSamplerMapping, AnisotropyRoundsUpToTheMetalPowerOfTwo)
{
	using row = mapping_row<rsx::texture_max_anisotropy, u32>;

	// Metal accepts only 1, 2, 4, 8 and 16 (K1).  A guest value that is not a
	// power of two rounds up, never down.
	const row table[] = {
		{ rsx::texture_max_anisotropy::x1, 1 },
		{ rsx::texture_max_anisotropy::x2, 2 },
		{ rsx::texture_max_anisotropy::x4, 4 },
		{ rsx::texture_max_anisotropy::x6, 8 },
		{ rsx::texture_max_anisotropy::x8, 8 },
		{ rsx::texture_max_anisotropy::x10, 16 },
		{ rsx::texture_max_anisotropy::x12, 16 },
		{ rsx::texture_max_anisotropy::x16, 16 },
	};

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_max_anisotropy(entry.guest), entry.host);
	}
}

TEST(MTLSamplerMapping, LodBiasRoundsNearestMipsAndClampsToMetal)
{
	struct row
	{
		rsx::texture_minify_filter filter;
		f32 guest;
		f32 host;
	};

	const row table[] = {
		// A nearest mip filter quantizes to 0.5 steps (VKDraw.cpp:506-511).
		{ rsx::texture_minify_filter::nearest_nearest, 0.24f, 0.0f },
		{ rsx::texture_minify_filter::nearest_nearest, 0.26f, 0.5f },
		{ rsx::texture_minify_filter::linear_nearest, -0.26f, -0.5f },
		{ rsx::texture_minify_filter::linear_nearest, 1.74f, 1.5f },
		// A linear mip filter keeps the guest value.
		{ rsx::texture_minify_filter::nearest_linear, 0.26f, 0.26f },
		{ rsx::texture_minify_filter::linear_linear, -1.74f, -1.74f },
		// The Metal range is [-16.0, 15.999] (MTLSamplerDescriptor.lodBias).
		{ rsx::texture_minify_filter::linear_linear, 20.0f, 15.999f },
		{ rsx::texture_minify_filter::linear_linear, -20.0f, -16.0f },
		{ rsx::texture_minify_filter::nearest_nearest, 20.0f, 15.999f },
		{ rsx::texture_minify_filter::nearest_nearest, -20.0f, -16.0f },
	};

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.filter));
		EXPECT_FLOAT_EQ(mtl::get_sampler_lod_bias(entry.filter, entry.guest), entry.host);
	}
}

TEST(MTLSamplerMapping, BorderColorFallsBackToTheClosestOfThree)
{
	struct row
	{
		color4f guest;
		MTL::SamplerBorderColor host;
	};

	const row table[] = {
		{ color4f(0.0f, 0.0f, 0.0f, 0.0f), MTL::SamplerBorderColorTransparentBlack },
		{ color4f(0.0f, 0.0f, 0.0f, 1.0f), MTL::SamplerBorderColorOpaqueBlack },
		{ color4f(0.5f, 0.5f, 0.5f, 0.0f), MTL::SamplerBorderColorOpaqueWhite },
		{ color4f(1.0f, 1.0f, 1.0f, 0.0f), MTL::SamplerBorderColorOpaqueWhite },
		// A sum of exactly 1.35 is not bright, so the alpha decides.
		{ color4f(0.45f, 0.45f, 0.45f, 0.5f), MTL::SamplerBorderColorTransparentBlack },
		{ color4f(0.45f, 0.45f, 0.45f, 0.6f), MTL::SamplerBorderColorOpaqueBlack },
		{ color4f(0.2f, 0.2f, 0.2f, 1.0f), MTL::SamplerBorderColorOpaqueBlack },
		{ color4f(1.0f, 0.0f, 0.0f, 0.4f), MTL::SamplerBorderColorTransparentBlack },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		EXPECT_EQ(mtl::get_closest_border_color(entry.guest), entry.host);
	}
}

TEST(MTLSamplerMapping, LatchEqualityTracksEveryField)
{
	const mtl::sampler_state base{};
	EXPECT_TRUE(base == mtl::sampler_state{});
	EXPECT_FALSE(base == [&]
	{
		mtl::sampler_state other = base;
		other.wrap_s = rsx::texture_wrap_mode::clamp_to_edge;
		return other;
	}());

	// One modification per field; each must break equality.
	{
		mtl::sampler_state other = base;
		other.wrap_t = rsx::texture_wrap_mode::mirror;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.wrap_r = rsx::texture_wrap_mode::border;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.min_filter = rsx::texture_minify_filter::linear;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.mag_filter = rsx::texture_magnify_filter::linear;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.min_lod = 1.0f;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.max_lod = 2.0f;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.lod_bias = 0.5f;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.max_aniso = 8;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.border_color = MTL::SamplerBorderColorOpaqueWhite;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.depth_compare = true;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.compare = rsx::comparison_function::never;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::sampler_state other = base;
		other.force_nearest = true;
		EXPECT_FALSE(base == other);
	}
}

TEST(MTLSamplerMapping, LatchHashTracksEveryField)
{
	const mtl::sampler_state base{};
	const u64 base_hash = mtl::hash_sampler_state(base);
	EXPECT_EQ(base_hash, mtl::hash_sampler_state(mtl::sampler_state{}));

	struct probe
	{
		const char* name;
		mtl::sampler_state state;
	};

	probe probes[] = {
		{ "wrap_s", [&] { mtl::sampler_state s = base; s.wrap_s = rsx::texture_wrap_mode::clamp_to_edge; return s; }() },
		{ "wrap_t", [&] { mtl::sampler_state s = base; s.wrap_t = rsx::texture_wrap_mode::mirror; return s; }() },
		{ "wrap_r", [&] { mtl::sampler_state s = base; s.wrap_r = rsx::texture_wrap_mode::border; return s; }() },
		{ "min_filter", [&] { mtl::sampler_state s = base; s.min_filter = rsx::texture_minify_filter::linear; return s; }() },
		{ "mag_filter", [&] { mtl::sampler_state s = base; s.mag_filter = rsx::texture_magnify_filter::linear; return s; }() },
		{ "min_lod", [&] { mtl::sampler_state s = base; s.min_lod = 1.0f; return s; }() },
		{ "max_lod", [&] { mtl::sampler_state s = base; s.max_lod = 2.0f; return s; }() },
		{ "lod_bias", [&] { mtl::sampler_state s = base; s.lod_bias = 0.5f; return s; }() },
		{ "max_aniso", [&] { mtl::sampler_state s = base; s.max_aniso = 8; return s; }() },
		{ "border_color", [&] { mtl::sampler_state s = base; s.border_color = MTL::SamplerBorderColorOpaqueWhite; return s; }() },
		{ "depth_compare", [&] { mtl::sampler_state s = base; s.depth_compare = true; return s; }() },
		{ "compare", [&] { mtl::sampler_state s = base; s.compare = rsx::comparison_function::never; return s; }() },
		{ "force_nearest", [&] { mtl::sampler_state s = base; s.force_nearest = true; return s; }() },
	};

	for (const probe& entry : probes)
	{
		SCOPED_TRACE(entry.name);
		EXPECT_NE(mtl::hash_sampler_state(entry.state), base_hash);
	}
}

// =============================================================================
// 14. The depth-stencil latch, its equality and its hash
//      (MTLDepthStencil.cpp:15-259; LLD-depth-rop §3.1)
// =============================================================================

TEST(MTLDepthStencilMapping, CompareFunctionMatchesTheDomain)
{
	using row = mapping_row<rsx::comparison_function, MTL::CompareFunction>;

	const row table[] = {
		{ rsx::comparison_function::never, MTL::CompareFunctionNever },
		{ rsx::comparison_function::less, MTL::CompareFunctionLess },
		{ rsx::comparison_function::equal, MTL::CompareFunctionEqual },
		{ rsx::comparison_function::less_or_equal, MTL::CompareFunctionLessEqual },
		{ rsx::comparison_function::greater, MTL::CompareFunctionGreater },
		{ rsx::comparison_function::not_equal, MTL::CompareFunctionNotEqual },
		{ rsx::comparison_function::greater_or_equal, MTL::CompareFunctionGreaterEqual },
		{ rsx::comparison_function::always, MTL::CompareFunctionAlways },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_compare_function(entry.guest), entry.host);
	}
}

TEST(MTLDepthStencilMapping, StencilOperationMatchesTheDomain)
{
	using row = mapping_row<rsx::stencil_op, MTL::StencilOperation>;

	const row table[] = {
		{ rsx::stencil_op::keep, MTL::StencilOperationKeep },
		{ rsx::stencil_op::zero, MTL::StencilOperationZero },
		{ rsx::stencil_op::replace, MTL::StencilOperationReplace },
		{ rsx::stencil_op::incr, MTL::StencilOperationIncrementClamp },
		{ rsx::stencil_op::decr, MTL::StencilOperationDecrementClamp },
		{ rsx::stencil_op::invert, MTL::StencilOperationInvert },
		{ rsx::stencil_op::incr_wrap, MTL::StencilOperationIncrementWrap },
		{ rsx::stencil_op::decr_wrap, MTL::StencilOperationDecrementWrap },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_stencil_operation(entry.guest), entry.host);
	}
}

TEST(MTLDepthStencilMapping, LatchEqualityTracksEveryField)
{
	const mtl::depth_stencil_state base{};
	EXPECT_TRUE(base == mtl::depth_stencil_state{});

	{
		mtl::depth_stencil_state other = base;
		other.depth_compare = rsx::comparison_function::never;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.depth_write = true;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.fail = rsx::stencil_op::zero;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.zfail = rsx::stencil_op::replace;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.zpass = rsx::stencil_op::invert;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.read_mask = 0x0f;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.write_mask = 0xf0;
		EXPECT_FALSE(base == other);
	}
	{
		mtl::depth_stencil_state other = base;
		other.back.compare = rsx::comparison_function::less;
		EXPECT_FALSE(base == other);
	}

	// The face comparison is a separate operator; it tracks all six fields.
	const mtl::stencil_face face{};
	EXPECT_TRUE(face == mtl::stencil_face{});
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.compare = rsx::comparison_function::less; return f; }());
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.fail = rsx::stencil_op::zero; return f; }());
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.zfail = rsx::stencil_op::zero; return f; }());
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.zpass = rsx::stencil_op::zero; return f; }());
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.read_mask = 0; return f; }());
	EXPECT_FALSE(face == [&] { mtl::stencil_face f = face; f.write_mask = 0; return f; }());
}

TEST(MTLDepthStencilMapping, LatchHashTracksEveryField)
{
	const mtl::depth_stencil_state base{};
	const u64 base_hash = mtl::hash_depth_stencil_state(base);
	EXPECT_EQ(base_hash, mtl::hash_depth_stencil_state(mtl::depth_stencil_state{}));

	{
		mtl::depth_stencil_state other = base;
		other.depth_compare = rsx::comparison_function::never;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.depth_write = true;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.compare = rsx::comparison_function::never;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.fail = rsx::stencil_op::zero;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.zfail = rsx::stencil_op::zero;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.zpass = rsx::stencil_op::zero;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.read_mask = 0x0f;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.front.write_mask = 0x0f;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
	{
		mtl::depth_stencil_state other = base;
		other.back.compare = rsx::comparison_function::less;
		EXPECT_NE(mtl::hash_depth_stencil_state(other), base_hash);
	}
}

// =============================================================================
// 15. The pipeline key: topology class, equality and hash
//      (MTLPipeline.cpp:32-65; MTLPipeline.h:73-105; HLD-05 §3.5)
// =============================================================================

TEST(MTLPipelineKey, PrimitiveTopologyClassCoversTheDomain)
{
	using row = mapping_row<rsx::primitive_type, MTL::PrimitiveTopologyClass>;

	const row table[] = {
		{ rsx::primitive_type::points, MTL::PrimitiveTopologyClassPoint },
		{ rsx::primitive_type::lines, MTL::PrimitiveTopologyClassLine },
		{ rsx::primitive_type::line_loop, MTL::PrimitiveTopologyClassLine },
		{ rsx::primitive_type::line_strip, MTL::PrimitiveTopologyClassLine },
		{ rsx::primitive_type::triangles, MTL::PrimitiveTopologyClassTriangle },
		{ rsx::primitive_type::triangle_strip, MTL::PrimitiveTopologyClassTriangle },
		{ rsx::primitive_type::triangle_fan, MTL::PrimitiveTopologyClassTriangle },
		{ rsx::primitive_type::quads, MTL::PrimitiveTopologyClassTriangle },
		{ rsx::primitive_type::quad_strip, MTL::PrimitiveTopologyClassTriangle },
		{ rsx::primitive_type::polygon, MTL::PrimitiveTopologyClassTriangle },
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 10u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_primitive_topology_class(entry.guest), entry.host);
	}
}

TEST(MTLPipelineKey, KeyEqualityTracksEveryField)
{
	const mtl::pipeline_properties base{};
	EXPECT_TRUE(base == mtl::pipeline_properties{});

	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.color_format = rsx::surface_color_format::a8r8g8b8; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.attachment_count = 2; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.sample_count = 4; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.alpha_to_coverage = true; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.alpha_to_one = true; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.raster_enabled = false; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.enable_mask = 1; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.source_rgb = rsx::blend_factor::src_alpha; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.source_a = rsx::blend_factor::src_alpha; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.destination_rgb = rsx::blend_factor::dst_color; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.destination_a = rsx::blend_factor::dst_color; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.equation_rgb = rsx::blend_equation::min; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.equation_a = rsx::blend_equation::max; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.alpha_test_enabled = true; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.logic_op_enabled = true; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.logic_operation = rsx::logic_op::logic_xor; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.blend.shade_mode = rsx::shading_mode::flat; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.write_mask[0] = MTL::ColorWriteMaskRed; return p; }());
	EXPECT_FALSE(base == [&] { mtl::pipeline_properties p = base; p.input_primitive_topology = MTL::PrimitiveTopologyClassTriangle; return p; }());
}

TEST(MTLPipelineKey, KeyHashTracksEveryField)
{
	const mtl::pipeline_properties base{};
	const usz base_hash = rpcs3::hash_struct(base);
	EXPECT_EQ(base_hash, rpcs3::hash_struct(mtl::pipeline_properties{}));

	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.color_format = rsx::surface_color_format::a8r8g8b8; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.attachment_count = 2; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.sample_count = 4; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.alpha_to_coverage = true; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.alpha_to_one = true; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.raster_enabled = false; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.enable_mask = 1; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.source_rgb = rsx::blend_factor::src_alpha; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.source_a = rsx::blend_factor::src_alpha; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.destination_rgb = rsx::blend_factor::dst_color; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.destination_a = rsx::blend_factor::dst_color; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.equation_rgb = rsx::blend_equation::min; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.equation_a = rsx::blend_equation::max; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.alpha_test_enabled = true; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.logic_op_enabled = true; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.logic_operation = rsx::logic_op::logic_xor; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.blend.shade_mode = rsx::shading_mode::flat; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.write_mask[0] = MTL::ColorWriteMaskRed; return p; }()));
	EXPECT_NE(base_hash, rpcs3::hash_struct([&] { mtl::pipeline_properties p = base; p.input_primitive_topology = MTL::PrimitiveTopologyClassTriangle; return p; }()));
}

TEST(MTLPipelineKey, KeyHashIsOrderSensitive)
{
	// The key folds its fields through FNV, not XOR, so two fields that swap a
	// value must not collide (REVIEW-FINDINGS E11).
	mtl::pipeline_properties a{};
	a.blend.enable_mask = 0x12;
	a.blend.source_rgb = rsx::blend_factor::one;

	mtl::pipeline_properties b{};
	b.blend.enable_mask = 0x21;
	b.blend.source_rgb = rsx::blend_factor::zero;

	EXPECT_FALSE(a == b);
	EXPECT_NE(rpcs3::hash_struct(a), rpcs3::hash_struct(b));
}

// =============================================================================
// 16. The avconf aspect conversion
//      (rsx_utils.cpp:137-235; rsx_utils.h:144-173)
// =============================================================================

namespace
{
	void configure_aspect(rsx::avconf& conf, u8 aspect, u32 resolution_x, u32 resolution_y, bool stereo, u32 state)
	{
		conf.aspect = aspect;
		conf.resolution_x = resolution_x;
		conf.resolution_y = resolution_y;
		conf.stereo_enabled = stereo;
		conf.state = state;
	}
}

TEST(MTLAvconfAspect, VideoFrameSizeIsTheResolutionUnlessStereoRuns)
{
	constexpr u32 resolutions_x[] = { 320, 640, 1280, 1920 };
	constexpr u32 resolutions_y[] = { 240, 480, 720, 1080 };

	for (const u32 resolution_x : resolutions_x)
	{
		for (const u32 resolution_y : resolutions_y)
		{
			for (const u32 state : { 0u, 1u })
			{
				for (const bool stereo : { false, true })
				{
					SCOPED_TRACE(std::to_string(resolution_x) + "x" + std::to_string(resolution_y));
					rsx::avconf conf;
					configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, resolution_x, resolution_y, stereo, state);

					const bool split = state != 0 && stereo;
					const size2u expected = split
						? size2u{ resolution_x, (resolution_y - 30) / 2 }
						: size2u{ resolution_x, resolution_y };

					EXPECT_EQ(conf.video_frame_size(), expected);
				}
			}
		}
	}
}

TEST(MTLAvconfAspect, StereoHalvesTheHeightBelowTheThirtyLineBands)
{
	// The stereo split removes two 15-line bands before halving (rsx_utils.cpp:195).
	rsx::avconf conf;
	configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, 1280, 720, true, 1);
	EXPECT_EQ(conf.video_frame_size(), size2u(1280, 345));

	configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, 1280, 721, true, 1);
	EXPECT_EQ(conf.video_frame_size(), size2u(1280, 345));
}

TEST(MTLAvconfAspect, DimensionsKeepTheHeightAndScaleTheWidth)
{
	for (const u8 aspect : { CELL_VIDEO_OUT_ASPECT_16_9, CELL_VIDEO_OUT_ASPECT_4_3 })
	{
		const f64 ratio = aspect == CELL_VIDEO_OUT_ASPECT_16_9 ? 16.0 / 9.0 : 4.0 / 3.0;

		for (u32 height = 1; height <= 64; ++height)
		{
			rsx::avconf conf;
			configure_aspect(conf, aspect, 1280, 720, false, 0);

			const size2u result = conf.aspect_convert_dimensions(size2u(64, height));
			SCOPED_TRACE(height);
			EXPECT_EQ(result.width, static_cast<u32>(height * ratio));
			EXPECT_EQ(result.height, height);
		}
	}
}

TEST(MTLAvconfAspect, ZeroInputCollapsesToAnEmptyRegion)
{
	rsx::avconf conf;
	configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, 1280, 720, false, 0);

	EXPECT_EQ(conf.aspect_convert_dimensions(size2u(0, 480)), size2u(0, 0));
	EXPECT_EQ(conf.aspect_convert_dimensions(size2u(640, 0)), size2u(0, 0));
	EXPECT_EQ(conf.aspect_convert_region(size2u(0, 480), size2u(1280, 720)), areau());

	// A zero output dimension also collapses.
	EXPECT_EQ(conf.aspect_convert_region(size2u(640, 480), size2u(0, 720)), areau());
	EXPECT_EQ(conf.aspect_convert_region(size2u(640, 480), size2u(1280, 0)), areau());
}

TEST(MTLAvconfAspect, RegionFitsInsideTheOutput)
{
	// For any non-empty input and output the merged region stays inside the output
	// rectangle.  This is the load-bearing invariant of the window converter.
	constexpr size2u inputs[] = {
		{ 1, 1 }, { 4, 3 }, { 16, 9 }, { 640, 480 }, { 1920, 1080 }, { 1280, 720 }, { 720, 1280 }, { 480, 640 },
	};

	for (const u8 aspect : { CELL_VIDEO_OUT_ASPECT_16_9, CELL_VIDEO_OUT_ASPECT_4_3 })
	{
		for (const size2u& input : inputs)
		{
			for (const size2u& output : inputs)
			{
				for (const bool stereo : { false, true })
				{
					SCOPED_TRACE(std::to_string(input.width) + "x" + std::to_string(input.height));

					rsx::avconf conf;
					configure_aspect(conf, aspect, 1280, 720, stereo, 1);

					const areau region = conf.aspect_convert_region(input, output);
					EXPECT_LE(region.x1, region.x2);
					EXPECT_LE(region.y1, region.y2);
					EXPECT_LE(region.x2, output.width);
					EXPECT_LE(region.y2, output.height);
				}
			}
		}
	}
}

TEST(MTLAvconfAspect, MatchingAspectsFillTheOutput)
{
	// When the source aspect, the virtual frame and the physical output all agree,
	// the converter returns the whole output rectangle.
	rsx::avconf conf;
	configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, 1280, 720, false, 0);

	const areau region = conf.aspect_convert_region(size2u(1920, 1080), size2u(1280, 720));
	EXPECT_EQ(region, areau(0, 0, 1280, 720));
}

TEST(MTLAvconfAspect, LetterboxingPreservesTheSourceAspect)
{
	// A 4:3 image inside a 16:9 output is pillarboxed: the height fills and the
	// width shrinks by three quarters, centred.
	rsx::avconf conf;
	configure_aspect(conf, CELL_VIDEO_OUT_ASPECT_16_9, 1280, 720, false, 0);

	const areau region = conf.aspect_convert_region(size2u(640, 480), size2u(1280, 720));
	EXPECT_EQ(region, areau(160, 0, 1120, 720));
}

// =============================================================================
// 17. The guest depth pack decode
//      (MTLFormats.cpp decode_depth_plane; REVIEW-FINDINGS B8)
// =============================================================================

TEST(MTLFormatsDepthPlane, DecodesTheGuestDepthPack)
{
	struct row
	{
		rsx::surface_depth_format2 format;
		u32 raw;
		f32 expected;
	};

	const row table[] = {
		// The stencil byte of a z24s8 word is not depth.
		{ rsx::surface_depth_format2::z24s8_uint, 0xFFFFFF00, 1.0f },
		{ rsx::surface_depth_format2::z24s8_uint, 0x000000AB, 0.0f },
		{ rsx::surface_depth_format2::z16_uint, 0xFFFF, 1.0f },
		{ rsx::surface_depth_format2::z16_float, 0xF000, rsx::get_depth_clear_value(rsx::surface_depth_format2::z16_float, 0xF000) },
		{ rsx::surface_depth_format2::z24s8_float, (std::bit_cast<u32>(0.5f) >> 7) << 8, 0.5f },
	};

	for (const row& entry : table)
	{
		const u32 bpp = rsx::get_format_block_size_in_bytes(entry.format);
		u8 src[8]{};
		std::memcpy(src, &entry.raw, bpp);

		u8 dst[4]{};
		mtl::decode_depth_plane(dst, src, 1, 1, bpp, bpp, entry.format);

		f32 value = 0.0f;
		std::memcpy(&value, dst, sizeof(value));

		SCOPED_TRACE(static_cast<u32>(entry.format));
		EXPECT_FLOAT_EQ(value, entry.expected);
	}
}

// =============================================================================
// 17b. The guest depth pack encode: the read-back's inverse
//      (MTLFormats.cpp encode_depth_plane; REVIEW-FINDINGS B16)
// =============================================================================

TEST(MTLFormatsDepthPlane, EncodesTheGuestDepthPack)
{
	// The read-back writes the guest pack, not the host depth plane, so the encode
	// must invert the upload's decode for every word a format can represent.  A z24s8
	// word also carries the stencil byte in bits 7:0, which the depth plane does not
	// hold, so the stencil plane is named separately.
	struct row
	{
		rsx::surface_depth_format2 format;
		u32 raw;
		u32 stencil;
	};

	const row table[] = {
		{ rsx::surface_depth_format2::z16_uint, 0x0000, 0 },
		{ rsx::surface_depth_format2::z16_uint, 0x8000, 0 },
		{ rsx::surface_depth_format2::z16_uint, 0xFFFF, 0 },
		{ rsx::surface_depth_format2::z24s8_uint, 0x000000, 0x00 },
		{ rsx::surface_depth_format2::z24s8_uint, 0x800000, 0x5C },
		{ rsx::surface_depth_format2::z24s8_uint, 0xFFFFFF, 0xAB },
		{ rsx::surface_depth_format2::z16_float, 0x0000, 0 },
		{ rsx::surface_depth_format2::z16_float, 0xF000, 0 },
		{ rsx::surface_depth_format2::z16_float, 0xFFFF, 0 },
		{ rsx::surface_depth_format2::z24s8_float, (std::bit_cast<u32>(0.5f) >> 7) & 0xFFFFFF, 0x11 },
	};

	for (const row& entry : table)
	{
		const u32 bpp = rsx::get_format_block_size_in_bytes(entry.format);

		// The guest word the hardware holds.
		u8 guest[8]{};
		if (bpp == 4)
		{
			const u32 word = (entry.raw << 8) | entry.stencil;
			std::memcpy(guest, &word, 4);
		}
		else
		{
			const u16 word = static_cast<u16>(entry.raw);
			std::memcpy(guest, &word, 2);
		}

		// The upload's decode, which is what the read-back has to invert.
		u8 plane[4]{};
		mtl::decode_depth_plane(plane, guest, 1, 1, bpp, bpp, entry.format);

		// The read-back's encode, with the stencil byte taken from its own plane.
		const u8 stencil = static_cast<u8>(entry.stencil);
		u8 packed[8]{};
		mtl::encode_depth_plane(packed, plane, 1, 1, bpp, bpp, entry.format,
			bpp == 4 ? &stencil : nullptr, bpp == 4 ? 1u : 0u);

		SCOPED_TRACE(static_cast<u32>(entry.format));
		EXPECT_EQ(std::memcmp(packed, guest, bpp), 0);
	}
}

TEST(MTLFormatsDepthPlane, EncodesZeroWhenNoStencilPlaneIsNamed)
{
	// A depth-only read-back names no stencil plane, and the stencil byte of a z24s8
	// word must then be zero rather than whatever the buffer already held
	// (REVIEW-FINDINGS.md B16: the stencil plane is not written today).  The guest
	// word holds the stencil in bits 7:0 and the depth in bits 31:8
	// (rsx_decode.h:3404, clear_z24 is bf_decoder<8, 24>), so the zero byte is first.
	const f32 depth = 1.0f;
	u8 packed[4] = { 0xEE, 0xEE, 0xEE, 0xEE };

	mtl::encode_depth_plane(packed, &depth, 1, 1, 4, 4, rsx::surface_depth_format2::z24s8_uint);

	EXPECT_EQ(packed[0], 0x00u);
	EXPECT_EQ(packed[1], 0xFFu);
	EXPECT_EQ(packed[2], 0xFFu);
	EXPECT_EQ(packed[3], 0xFFu);
}

// =============================================================================
// Surface component layout
//      (MTLFormats.cpp get_surface_component_layout; DEVIATIONS-SURFACE.md S7)
// =============================================================================

namespace
{
	using layout4 = std::array<MTL::TextureSwizzle, 4>;

	layout4 identity_layout()
	{
		return { MTL::TextureSwizzleAlpha, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
	}
}

TEST(MTLFormatsSurfaceLayout, SingleChannelFormatsForceTheGuestChannels)
{
	// R8Unorm stores one channel; the guest reads it in every colour and an opaque
	// alpha.  R32Float repeats one value.  RG8Unorm puts guest blue in host red.
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::b8),
		(layout4{ MTL::TextureSwizzleOne, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed }));
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x32),
		(layout4{ MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed, MTL::TextureSwizzleRed }));
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::g8b8),
		(layout4{ MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleRed }));
}

TEST(MTLFormatsSurfaceLayout, TheXByteStatesItsAlpha)
{
	// A z variant stores no alpha, so a guest alpha read is zero; an o variant
	// reads as opaque.  The colour channels keep the host order.
	const layout4 zero = { MTL::TextureSwizzleZero, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };
	const layout4 one = { MTL::TextureSwizzleOne, MTL::TextureSwizzleRed, MTL::TextureSwizzleGreen, MTL::TextureSwizzleBlue };

	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x1r5g5b5_z1r5g5b5), zero);
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x8r8g8b8_z8r8g8b8), zero);
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x8b8g8r8_z8b8g8r8), zero);
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x1r5g5b5_o1r5g5b5), one);
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x8r8g8b8_o8r8g8b8), one);
	EXPECT_EQ(mtl::get_surface_component_layout(rsx::surface_color_format::x8b8g8r8_o8b8g8r8), one);
}

TEST(MTLFormatsSurfaceLayout, PackedFormatsKeepTheGuestOrder)
{
	// The host format already carries the guest channels in the guest bit order, so
	// no reordering is wanted (the texture path states the same map).
	for (const rsx::surface_color_format format : {
		rsx::surface_color_format::r5g6b5,
		rsx::surface_color_format::a8r8g8b8,
		rsx::surface_color_format::a8b8g8r8,
		rsx::surface_color_format::w16z16y16x16,
		rsx::surface_color_format::w32z32y32x32 })
	{
		SCOPED_TRACE(static_cast<u32>(format));
		EXPECT_EQ(mtl::get_surface_component_layout(format), identity_layout());
	}
}

// =============================================================================
// Format-aware clear colour and write mask
//      (MTLFormats.cpp resolve_surface_clear; DEVIATIONS-SURFACE.md S3, S4)
// =============================================================================

TEST(MTLFormatsClear, FloatSurfacesTakeNoColourClear)
{
	// A float surface has no hardware colour clear, so Vulkan and OpenGL drop the
	// colour aspect (VKGSRender.cpp:1528-1535).
	for (const rsx::surface_color_format format : {
		rsx::surface_color_format::x32,
		rsx::surface_color_format::w16z16y16x16,
		rsx::surface_color_format::w32z32y32x32 })
	{
		const mtl::resolved_clear clear = mtl::resolve_surface_clear(format, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_COLOR_RGBA_MASK);

		SCOPED_TRACE(static_cast<u32>(format));
		EXPECT_EQ(clear.mask, 0u);
		EXPECT_FALSE(clear.fast);
	}
}

TEST(MTLFormatsClear, TheClearWordFollowsTheSurfaceStorage)
{
	// The clear registers are one A8R8G8B8 word, so a reordered format swaps the
	// bytes before the value reaches Metal.  A packed format renormalises them.
	{
		const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::b8, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_COLOR_RGBA_MASK);
		EXPECT_EQ(clear.mask, static_cast<u32>(RSX_GCM_CLEAR_RED_BIT));
		EXPECT_TRUE(clear.fast);
		EXPECT_FLOAT_EQ(clear.color.red, 0x33 / 255.0);
		EXPECT_FLOAT_EQ(clear.color.blue, 0x11 / 255.0);
	}

	{
		const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::g8b8, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_COLOR_RGBA_MASK);
		EXPECT_EQ(clear.mask, static_cast<u32>(RSX_GCM_CLEAR_COLOR_RG_MASK));
		EXPECT_TRUE(clear.fast);
		EXPECT_FLOAT_EQ(clear.color.red, 0x33 / 255.0);
	}

	{
		const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::a8b8g8r8, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_COLOR_RGBA_MASK);
		EXPECT_EQ(clear.mask, static_cast<u32>(RSX_GCM_CLEAR_COLOR_RGBA_MASK));
		EXPECT_TRUE(clear.fast);
		EXPECT_FLOAT_EQ(clear.color.red, 0x33 / 255.0);
		EXPECT_FLOAT_EQ(clear.color.blue, 0x11 / 255.0);
	}

	{
		// B5G6R5: the 16-bit guest word is the two middle bytes of the clear colour.
		const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::r5g6b5, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_COLOR_RGBA_MASK);
		const u16 raw = 0x2233;
		EXPECT_EQ(clear.mask, static_cast<u32>(RSX_GCM_CLEAR_COLOR_RGBA_MASK));
		EXPECT_TRUE(clear.fast);
		EXPECT_FLOAT_EQ(clear.color.red, rsx::renormalize_color8(raw >> 11, 0x1f) / 255.0);
		EXPECT_FLOAT_EQ(clear.color.green, rsx::renormalize_color8(raw >> 5, 0x3f) / 255.0);
		EXPECT_FLOAT_EQ(clear.color.blue, rsx::renormalize_color8(raw, 0x1f) / 255.0);
	}
}

TEST(MTLFormatsClear, AChannelTheFormatDoesNotStoreIsNoOp)
{
	// b8 stores the blue byte alone, so a red-only clear writes nothing.
	const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::b8, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_RED_BIT);
	EXPECT_EQ(clear.mask, 0u);
	EXPECT_FALSE(clear.fast);
}

TEST(MTLFormatsClear, APartialMaskIsNotAFastClear)
{
	const mtl::resolved_clear clear = mtl::resolve_surface_clear(rsx::surface_color_format::a8r8g8b8, 0x11, 0x22, 0x33, 0x44, RSX_GCM_CLEAR_RED_BIT);
	EXPECT_EQ(clear.mask, static_cast<u32>(RSX_GCM_CLEAR_RED_BIT));
	EXPECT_FALSE(clear.fast);
}

TEST(MTLFormatsClear, TheRSXMaskMapsToTheMetalWriteMask)
{
	EXPECT_EQ(mtl::to_color_write_mask(0), MTL::ColorWriteMaskNone);
	EXPECT_EQ(mtl::to_color_write_mask(RSX_GCM_CLEAR_COLOR_RGBA_MASK), MTL::ColorWriteMaskAll);
	EXPECT_EQ(mtl::to_color_write_mask(RSX_GCM_CLEAR_RED_BIT | RSX_GCM_CLEAR_GREEN_BIT),
		MTL::ColorWriteMaskRed | MTL::ColorWriteMaskGreen);
	EXPECT_EQ(mtl::to_color_write_mask(RSX_GCM_CLEAR_BLUE_BIT), MTL::ColorWriteMaskBlue);
	EXPECT_EQ(mtl::to_color_write_mask(RSX_GCM_CLEAR_ALPHA_BIT), MTL::ColorWriteMaskAlpha);
}

TEST(MTLFormatsTexelSize, AnUncompressedFormatNamesItsTexel)
{
	const struct
	{
		MTL::PixelFormat format;
		u32 size;
	} table[] = {
		{ MTL::PixelFormat::PixelFormatR8Unorm, 1 },
		{ MTL::PixelFormat::PixelFormatB5G6R5Unorm, 2 },
		{ MTL::PixelFormat::PixelFormatDepth16Unorm, 2 },
		{ MTL::PixelFormat::PixelFormatRGBA8Unorm, 4 },
		{ MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB, 4 },
		{ MTL::PixelFormat::PixelFormatDepth32Float, 4 },
		{ MTL::PixelFormat::PixelFormatRGBA16Float, 8 },
		{ MTL::PixelFormat::PixelFormatDepth32Float_Stencil8, 8 },
		{ MTL::PixelFormat::PixelFormatRGBA32Float, 16 },
	};

	for (const auto& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.format));
		EXPECT_EQ(mtl::get_texel_size(entry.format), entry.size);
	}
}

TEST(MTLFormatsTexelSize, ACompressedBlockIsNotATexel)
{
	EXPECT_EQ(mtl::get_texel_size(MTL::PixelFormat::PixelFormatBC1_RGBA), 0u);
	EXPECT_EQ(mtl::get_texel_size(MTL::PixelFormat::PixelFormatBC3_RGBA), 0u);
}

TEST(MTLTextureExtent, ZeroFieldsBecomeTheSmallestLegalTexture)
{
	const mtl::texture_extent extent = mtl::normalize_texture_extent(MTL::TextureType2D, 0, 0, 0, 0);
	EXPECT_EQ(extent.width, 1);
	EXPECT_EQ(extent.height, 1);
	EXPECT_EQ(extent.depth, 1);
	EXPECT_EQ(extent.mipmaps, 1);
}

TEST(MTLTextureExtent, ACubeFaceIsSquareAndUsesTheWidth)
{
	const mtl::texture_extent extent = mtl::normalize_texture_extent(MTL::TextureTypeCube, 64, 32, 0, 3);
	EXPECT_EQ(extent.width, 64);
	EXPECT_EQ(extent.height, 64);
	EXPECT_EQ(extent.depth, 1);
	EXPECT_EQ(extent.mipmaps, 3);
}

TEST(MTLTextureExtent, ACubeDescriptorHasOneArraySlice)
{
	// L10: Metal rejects arrayLength 6 for a cube; the six RSX faces are the
	// upload slice argument, not the descriptor's array length.
	MTL::TextureDescriptor* descriptor = mtl::texture_cache::make_descriptor(
		MTL::PixelFormat::PixelFormatBC3_RGBA, MTL::TextureTypeCube, 256, 256, 6, 7);
	ASSERT_NE(descriptor, nullptr);
	EXPECT_EQ(descriptor->textureType(), MTL::TextureTypeCube);
	EXPECT_EQ(descriptor->arrayLength(), 1u);
	EXPECT_EQ(descriptor->depth(), 1u);
	EXPECT_EQ(descriptor->mipmapLevelCount(), 7u);
	descriptor->release();
}

TEST(MTLTextureExtent, TheMipChainIsCappedAtTheExtent)
{
	// 4x4 holds 1 + log2(4) = 3 levels.
	const mtl::texture_extent over = mtl::normalize_texture_extent(MTL::TextureType2D, 4, 4, 1, 9);
	EXPECT_EQ(over.mipmaps, 3);

	const mtl::texture_extent legal = mtl::normalize_texture_extent(MTL::TextureType2D, 64, 32, 1, 6);
	EXPECT_EQ(legal.width, 64);
	EXPECT_EQ(legal.height, 32);
	EXPECT_EQ(legal.depth, 1);
	EXPECT_EQ(legal.mipmaps, 6);
}

TEST(MTLTextureExtent, OnlyAVolumeKeepsADepth)
{
	const mtl::texture_extent flat = mtl::normalize_texture_extent(MTL::TextureType2D, 8, 8, 5, 1);
	EXPECT_EQ(flat.depth, 1);

	// An 8x4x0 volume holds 1 + log2(8) = 4 levels.
	const mtl::texture_extent volume = mtl::normalize_texture_extent(MTL::TextureType3D, 8, 4, 0, 5);
	EXPECT_EQ(volume.depth, 1);
	EXPECT_EQ(volume.mipmaps, 4);
}

TEST(MTLPipelineJobs, EqualKeysShareOneJobAndFailedKeysStayFailed)
{
	mtl::pipeline_jobs jobs;

	mtl::vertex_program vertex;
	vertex.id = 101;
	mtl::fragment_program fragment;
	fragment.id = 202;
	mtl::pipeline_properties properties{};

	mtl::shader_context context{};
	context.jobs = &jobs;

	auto callback = [](std::unique_ptr<mtl::pipeline>&) -> mtl::pipeline* { return nullptr; };

	// The registry key must deduplicate exactly like the shared pipeline key.
	EXPECT_TRUE(jobs.submit(vertex, fragment, properties, context, callback));
	EXPECT_FALSE(jobs.submit(vertex, fragment, properties, context, callback));

	mtl::pipeline_properties other = properties;
	other.write_mask[0] = static_cast<MTL::ColorWriteMask>(1);
	EXPECT_TRUE(jobs.submit(vertex, fragment, other, context, callback));
	EXPECT_FALSE(jobs.submit(vertex, fragment, other, context, callback));

	mtl::vertex_program second_vertex;
	second_vertex.id = 103;
	EXPECT_TRUE(jobs.submit(second_vertex, fragment, properties, context, callback));

	// The empty MSL fails the library stage.  The jobs must leave the registry,
	// record permanent failure and release their tasks without a GPU.
	EXPECT_TRUE(jobs.wait_for_idle(std::chrono::seconds(2), {}, [] { return false; }));
	EXPECT_TRUE(jobs.permanently_failed(vertex.id, fragment.id, properties));
	EXPECT_TRUE(jobs.permanently_failed(vertex.id, fragment.id, other));
	EXPECT_TRUE(jobs.permanently_failed(second_vertex.id, fragment.id, properties));
	EXPECT_TRUE(jobs.shutdown());
}

#endif // RSX_ENABLE_METAL4
