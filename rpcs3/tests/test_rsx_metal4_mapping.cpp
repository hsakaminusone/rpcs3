#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLBarrier.h"
#include "Emu/RSX/MTL/MTLBlendState.h"
#include "Emu/RSX/MTL/MTLDepthStencil.h"
#include "Emu/RSX/MTL/MTLFormats.h"
#include "Emu/RSX/MTL/MTLPassScheduler.h"
#include "Emu/RSX/MTL/MTLPipeline.h"
#include "Emu/RSX/MTL/MTLRasterState.h"
#include "Emu/RSX/MTL/MTLSamplerState.h"

// The RSX-to-Metal 4 mapping tables, GPU-free.
//
// Every expectation comes from the RSX side, never from the Metal function.  The
// sources are the RSX enums and CELL_GCM_* definitions (gcm_enums.h), the state
// decoders (rsx_decode.h), and, as independent evidence about RSX behaviour, the
// Vulkan and GL backends.  A wrong table entry must fail one of these tests.
//
// The file is inert when the host does not build the backend, because
// RSX_ENABLE_METAL4 gates it (MTLAvailability.h).
#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/gcm_enums.h"
#include "Emu/RSX/rsx_methods.h"

#include <array>
#include <string>

namespace
{
	// The emulator's power-on register state (rsx_methods.cpp:797).  A zeroed file
	// is not legal, because a getter that maps a word to a GCM enum verifies.
	void reset_registers()
	{
		rsx::method_registers.reset();
	}

	void set_register(u32 index, u32 value)
	{
		rsx::method_registers.registers[index] = value;
	}

	// One row of an RSX enum to Metal enum table.
	template <typename Rsx, typename Metal>
	struct mapping_row
	{
		Rsx guest;
		Metal host;
	};
}

// =============================================================================
// 1. access_stages: rsx::surface_access -> MTL::Stages
//    (MTLPassScheduler.h:17; MTLPassScheduler.cpp:7-27)
// =============================================================================

TEST(MTLMapAccessStages, EveryClassSelectsItsStageMask)
{
	// The F2 scheduler's edge mask.  A shader access is a shader stage and
	// vertex_stage picks the half; a transfer access is a blit and a dispatch; a
	// memory access is every stage; a bare reference orders nothing.  The producer
	// and consumer columns are the same mask (MTLPassScheduler.h:14-17), so one
	// call carries both.
	struct row
	{
		u32 guest;
		MTL::Stages when_vertex;
		MTL::Stages when_fragment;
	};

	constexpr MTL::Stages blit = static_cast<MTL::Stages>(MTL::StageBlit | MTL::StageDispatch);

	const row table[] = {
		{ rsx::surface_access::shader_read,   MTL::StageVertex, MTL::StageFragment }, // TextureUtils.h:55
		{ rsx::surface_access::shader_write,  MTL::StageVertex, MTL::StageFragment }, // TextureUtils.h:56
		{ rsx::surface_access::transfer_read,  blit, blit },                          // TextureUtils.h:57
		{ rsx::surface_access::transfer_write, blit, blit },                          // TextureUtils.h:58
		{ rsx::surface_access::memory_read,    MTL::StageAll, MTL::StageAll },        // TextureUtils.h:62
		{ rsx::surface_access::memory_write,   MTL::StageAll, MTL::StageAll },        // TextureUtils.h:61
		{ rsx::surface_access::gpu_reference,  static_cast<MTL::Stages>(0), static_cast<MTL::Stages>(0) }, // TextureUtils.h:65
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 7u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(entry.guest);
		EXPECT_EQ(mtl::access_stages(entry.guest, true), entry.when_vertex);
		EXPECT_EQ(mtl::access_stages(entry.guest, false), entry.when_fragment);
	}
}

TEST(MTLMapAccessStages, OnlyTheShaderClassesDependOnTheStage)
{
	// vertex_stage selects the half only inside the shader stage class
	// (MTLPassScheduler.cpp:9-12).  Every other class gives one mask for both.
	constexpr u32 classes[] = {
		rsx::surface_access::shader_read,
		rsx::surface_access::shader_write,
		rsx::surface_access::transfer_read,
		rsx::surface_access::transfer_write,
		rsx::surface_access::memory_read,
		rsx::surface_access::memory_write,
		rsx::surface_access::gpu_reference,
	};

	for (const u32 access : classes)
	{
		SCOPED_TRACE(access);
		const bool shader = access == rsx::surface_access::shader_read || access == rsx::surface_access::shader_write;
		const bool differs = mtl::access_stages(access, true) != mtl::access_stages(access, false);
		EXPECT_EQ(differs, shader);
	}
}

// =============================================================================
// 2. get_address_mode: rsx::texture_wrap_mode -> MTL::SamplerAddressMode
//    (MTLSamplerState.h:49; MTLSamplerState.cpp:14-36)
// =============================================================================

TEST(MTLMapSamplerAddress, AddressModeFollowsTheGcmWrapMode)
{
	using row = mapping_row<rsx::texture_wrap_mode, MTL::SamplerAddressMode>;

	const row table[] = {
		{ rsx::texture_wrap_mode::wrap,          MTL::SamplerAddressModeRepeat },          // CELL_GCM_TEXTURE_WRAP (gcm_enums.h:953); VKFormats.cpp:91
		{ rsx::texture_wrap_mode::mirror,        MTL::SamplerAddressModeMirrorRepeat },     // CELL_GCM_TEXTURE_MIRROR (gcm_enums.h:954); VKFormats.cpp:92
		{ rsx::texture_wrap_mode::clamp_to_edge, MTL::SamplerAddressModeClampToEdge },      // CELL_GCM_TEXTURE_CLAMP_TO_EDGE (gcm_enums.h:955); VKFormats.cpp:93
		{ rsx::texture_wrap_mode::clamp,         MTL::SamplerAddressModeClampToEdge },      // CELL_GCM_TEXTURE_CLAMP (gcm_enums.h:957); VKFormats.cpp:95
		{ rsx::texture_wrap_mode::border,        MTL::SamplerAddressModeClampToBorderColor }, // CELL_GCM_TEXTURE_BORDER (gcm_enums.h:956); VKFormats.cpp:94
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 5u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_address_mode(entry.guest), entry.host);
	}
}

TEST(MTLMapSamplerAddress, MirrorOnceTakesTheDocumentedFallback)
{
	// Metal has no mirror-once form.  Vulkan keeps the mirror half and drops the
	// border with MIRROR_CLAMP_TO_EDGE (VKFormats.cpp:96-98).  Metal's closest
	// mode is MirrorClampToEdge (MTLSamplerState.cpp:27-32).  The three RSX forms
	// differ only in the dropped edge, so the fallback is one value.
	constexpr rsx::texture_wrap_mode once[] = {
		rsx::texture_wrap_mode::mirror_once_clamp_to_edge, // CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP_TO_EDGE (gcm_enums.h:958)
		rsx::texture_wrap_mode::mirror_once_border,        // CELL_GCM_TEXTURE_MIRROR_ONCE_BORDER (gcm_enums.h:959)
		rsx::texture_wrap_mode::mirror_once_clamp,         // CELL_GCM_TEXTURE_MIRROR_ONCE_CLAMP (gcm_enums.h:960)
	};

	for (const rsx::texture_wrap_mode mode : once)
	{
		SCOPED_TRACE(static_cast<u32>(mode));
		EXPECT_EQ(mtl::get_address_mode(mode), MTL::SamplerAddressModeMirrorClampToEdge);
	}
}

// =============================================================================
// 3. get_min_mag_filter: the two RSX filter domains -> MTL::SamplerMinMagFilter
//    (MTLSamplerState.h:50-51; MTLSamplerState.cpp:38-68)
// =============================================================================

TEST(MTLMapSamplerFilter, MinifyFilterLowersToTheMinMagFilter)
{
	// The leading word of the RSX minify name is the min filter; the trailing word
	// is the mip mode (gcm_enums.h:1524-1533).  Vulkan lowers the same way
	// (VKFormats.cpp:38-44).
	using row = mapping_row<rsx::texture_minify_filter, MTL::SamplerMinMagFilter>;

	const row table[] = {
		{ rsx::texture_minify_filter::nearest,         MTL::SamplerMinMagFilterNearest }, // CELL_GCM_TEXTURE_NEAREST (gcm_enums.h:973)
		{ rsx::texture_minify_filter::linear,          MTL::SamplerMinMagFilterLinear },  // CELL_GCM_TEXTURE_LINEAR (gcm_enums.h:974)
		{ rsx::texture_minify_filter::nearest_nearest, MTL::SamplerMinMagFilterNearest }, // CELL_GCM_TEXTURE_NEAREST_NEAREST (gcm_enums.h:975)
		{ rsx::texture_minify_filter::linear_nearest,  MTL::SamplerMinMagFilterLinear },  // CELL_GCM_TEXTURE_LINEAR_NEAREST (gcm_enums.h:976)
		{ rsx::texture_minify_filter::nearest_linear,  MTL::SamplerMinMagFilterNearest }, // CELL_GCM_TEXTURE_NEAREST_LINEAR (gcm_enums.h:977)
		{ rsx::texture_minify_filter::linear_linear,   MTL::SamplerMinMagFilterLinear },  // CELL_GCM_TEXTURE_LINEAR_LINEAR (gcm_enums.h:978)
		{ rsx::texture_minify_filter::convolution_min, MTL::SamplerMinMagFilterLinear },  // gcm_enums.h:979, looks close to linear_linear
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 7u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_min_mag_filter(entry.guest), entry.host);
	}
}

TEST(MTLMapSamplerFilter, MagnifyFilterLowersToTheMinMagFilter)
{
	using row = mapping_row<rsx::texture_magnify_filter, MTL::SamplerMinMagFilter>;

	const row table[] = {
		{ rsx::texture_magnify_filter::nearest,         MTL::SamplerMinMagFilterNearest }, // CELL_GCM_TEXTURE_NEAREST (gcm_enums.h:1545); VKFormats.cpp:54
		{ rsx::texture_magnify_filter::linear,          MTL::SamplerMinMagFilterLinear },  // CELL_GCM_TEXTURE_LINEAR (gcm_enums.h:1546); VKFormats.cpp:55
		{ rsx::texture_magnify_filter::convolution_mag, MTL::SamplerMinMagFilterLinear },  // CELL_GCM_TEXTURE_CONVOLUTION_MAG (gcm_enums.h:1547); VKFormats.cpp:56
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 3u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_min_mag_filter(entry.guest), entry.host);
	}
}

TEST(MTLMapSamplerFilter, TheTwoOverloadsAgreeOnTheSharedValues)
{
	// nearest and linear are the values the two RSX filter domains share
	// (gcm_enums.h:973-974, :1545-1546).  The overloads must not disagree on them.
	EXPECT_EQ(mtl::get_min_mag_filter(rsx::texture_minify_filter::nearest),
		mtl::get_min_mag_filter(rsx::texture_magnify_filter::nearest));
	EXPECT_EQ(mtl::get_min_mag_filter(rsx::texture_minify_filter::linear),
		mtl::get_min_mag_filter(rsx::texture_magnify_filter::linear));
}

// =============================================================================
// 4. get_mip_filter: the minify filter names the mip mode
//    (MTLSamplerState.h:55; MTLSamplerState.cpp:70-89)
// =============================================================================

TEST(MTLMapMipFilter, MinifyFilterNamesTheMipFilter)
{
	struct row
	{
		rsx::texture_minify_filter guest;
		MTL::SamplerMipFilter host;
	};

	const row table[] = {
		{ rsx::texture_minify_filter::nearest,         MTL::SamplerMipFilterNotMipmapped }, // base level only (gcm_enums.h:1526)
		{ rsx::texture_minify_filter::linear,          MTL::SamplerMipFilterNotMipmapped }, // base level only (gcm_enums.h:1527)
		{ rsx::texture_minify_filter::nearest_nearest, MTL::SamplerMipFilterNearest },      // closest level (gcm_enums.h:1528)
		{ rsx::texture_minify_filter::linear_nearest,  MTL::SamplerMipFilterNearest },      // closest level (gcm_enums.h:1529)
		{ rsx::texture_minify_filter::nearest_linear,  MTL::SamplerMipFilterLinear },       // linear mix (gcm_enums.h:1530)
		{ rsx::texture_minify_filter::linear_linear,   MTL::SamplerMipFilterLinear },       // linear mix (gcm_enums.h:1531)
		{ rsx::texture_minify_filter::convolution_min, MTL::SamplerMipFilterLinear },       // gcm_enums.h:1532, looks close to linear_linear
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 7u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_mip_filter(entry.guest), entry.host);
	}
}

TEST(MTLMapMipFilter, OnlyTheBaseLevelFiltersAreNotMipmapped)
{
	// The RSX minify name alone says whether the mip chain is sampled.  nearest
	// and linear name the base level (gcm_enums.h:1526-1527).  Every other name
	// encodes a mip mode, and convolution_min follows linear_linear
	// (gcm_enums.h:1532).  This ties the mip filter to the min filter.
	constexpr rsx::texture_minify_filter all[] = {
		rsx::texture_minify_filter::nearest,
		rsx::texture_minify_filter::linear,
		rsx::texture_minify_filter::nearest_nearest,
		rsx::texture_minify_filter::linear_nearest,
		rsx::texture_minify_filter::nearest_linear,
		rsx::texture_minify_filter::linear_linear,
		rsx::texture_minify_filter::convolution_min,
	};

	for (const rsx::texture_minify_filter filter : all)
	{
		SCOPED_TRACE(static_cast<u32>(filter));
		const bool base_level = filter == rsx::texture_minify_filter::nearest || filter == rsx::texture_minify_filter::linear;
		EXPECT_EQ(mtl::get_mip_filter(filter) == MTL::SamplerMipFilterNotMipmapped, base_level);
	}
}

// =============================================================================
// 5. get_compare_function: rsx::comparison_function -> MTL::CompareFunction
//    (MTLDepthStencil.h:116; MTLDepthStencil.cpp)
// =============================================================================

TEST(MTLMapCompare, CompareFunctionFollowsTheGcmOrder)
{
	using row = mapping_row<rsx::comparison_function, MTL::CompareFunction>;

	const row table[] = {
		{ rsx::comparison_function::never,           MTL::CompareFunctionNever },        // CELL_GCM_NEVER (gcm_enums.h:311); VKDraw.cpp:36
		{ rsx::comparison_function::less,            MTL::CompareFunctionLess },         // CELL_GCM_LESS (gcm_enums.h:312); VKDraw.cpp:38
		{ rsx::comparison_function::equal,           MTL::CompareFunctionEqual },        // CELL_GCM_EQUAL (gcm_enums.h:313); VKDraw.cpp:41
		{ rsx::comparison_function::less_or_equal,   MTL::CompareFunctionLessEqual },    // CELL_GCM_LEQUAL (gcm_enums.h:314); VKDraw.cpp:39
		{ rsx::comparison_function::greater,         MTL::CompareFunctionGreater },      // CELL_GCM_GREATER (gcm_enums.h:315); VKDraw.cpp:37
		{ rsx::comparison_function::not_equal,       MTL::CompareFunctionNotEqual },     // CELL_GCM_NOTEQUAL (gcm_enums.h:316); VKDraw.cpp:42
		{ rsx::comparison_function::greater_or_equal, MTL::CompareFunctionGreaterEqual }, // CELL_GCM_GEQUAL (gcm_enums.h:317); VKDraw.cpp:40
		{ rsx::comparison_function::always,          MTL::CompareFunctionAlways },       // CELL_GCM_ALWAYS (gcm_enums.h:318); VKDraw.cpp:43
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_compare_function(entry.guest), entry.host);
	}
}

TEST(MTLMapCompare, TheMetalValueFollowsTheGcmOrdinal)
{
	// gcm_enums.h:311-318 lays never..always out as 0x200..0x207.  The GL backend
	// casts the RSX value straight to GLenum (GLDraw.cpp:13-16), so the RSX word is
	// in the GL compare order.  Metal's CompareFunction is the same order 0..7
	// (MTLDepthStencil.hpp).  A swapped row breaks this index, not just the names.
	for (u32 index = 0; index < 8; ++index)
	{
		SCOPED_TRACE(index);
		const auto op = static_cast<rsx::comparison_function>(CELL_GCM_NEVER + index);
		EXPECT_EQ(static_cast<u32>(mtl::get_compare_function(op)), index);
	}
}

// =============================================================================
// 6. get_primitive_topology_class: rsx::primitive_type -> MTL::PrimitiveTopologyClass
//    (MTLPipeline.h:65; MTLPipeline.cpp:45-65)
// =============================================================================

TEST(MTLMapTopology, PrimitiveFamilyFollowsTheGcmClass)
{
	using row = mapping_row<rsx::primitive_type, MTL::PrimitiveTopologyClass>;

	const row table[] = {
		{ rsx::primitive_type::points,         MTL::PrimitiveTopologyClassPoint },    // CELL_GCM_PRIMITIVE_POINTS (gcm_enums.h:1255)
		{ rsx::primitive_type::lines,          MTL::PrimitiveTopologyClassLine },     // CELL_GCM_PRIMITIVE_LINES (gcm_enums.h:1256)
		{ rsx::primitive_type::line_loop,      MTL::PrimitiveTopologyClassLine },     // CELL_GCM_PRIMITIVE_LINE_LOOP (gcm_enums.h:1257)
		{ rsx::primitive_type::line_strip,     MTL::PrimitiveTopologyClassLine },     // CELL_GCM_PRIMITIVE_LINE_STRIP (gcm_enums.h:1258)
		{ rsx::primitive_type::triangles,      MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_TRIANGLES (gcm_enums.h:1259)
		{ rsx::primitive_type::triangle_strip, MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_TRIANGLE_STRIP (gcm_enums.h:1260)
		{ rsx::primitive_type::triangle_fan,   MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_TRIANGLE_FAN (gcm_enums.h:1261)
		{ rsx::primitive_type::quads,          MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_QUADS (gcm_enums.h:1262)
		{ rsx::primitive_type::quad_strip,     MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_QUAD_STRIP (gcm_enums.h:1263)
		{ rsx::primitive_type::polygon,        MTL::PrimitiveTopologyClassTriangle }, // CELL_GCM_PRIMITIVE_POLYGON (gcm_enums.h:1264)
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 10u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_primitive_topology_class(entry.guest), entry.host);
	}
}

TEST(MTLMapTopology, EveryPrimitiveTypeHasAFamily)
{
	// The three RSX primitive families cover every value (gcm_enums.h:1253-1265),
	// so the class is never Unspecified.  A default branch that returns a family
	// keeps this true.
	for (u32 raw = 0; raw < 256; ++raw)
	{
		SCOPED_TRACE(raw);
		EXPECT_NE(mtl::get_primitive_topology_class(static_cast<rsx::primitive_type>(raw)), MTL::PrimitiveTopologyClassUnspecified);
	}
}

// =============================================================================
// 7. get_blend_factor / get_blend_operation
//    (MTLBlendState.h:55,59; MTLBlendState.cpp:47-94)
// =============================================================================

TEST(MTLMapBlend, BlendFactorFollowsTheGcmFactor)
{
	// The fifteen RSX factors map by name (LLD-blend §3.2).  Vulkan maps the same
	// fifteen by name (VKGSRender.cpp:137-159).
	using row = mapping_row<rsx::blend_factor, MTL::BlendFactor>;

	const row table[] = {
		{ rsx::blend_factor::zero,                     MTL::BlendFactorZero },                  // CELL_GCM_ZERO (gcm_enums.h:1600); VKGSRender.cpp:142
		{ rsx::blend_factor::one,                      MTL::BlendFactorOne },                   // CELL_GCM_ONE (gcm_enums.h:1601); VKGSRender.cpp:141
		{ rsx::blend_factor::src_color,                MTL::BlendFactorSourceColor },           // CELL_GCM_SRC_COLOR (gcm_enums.h:1602); VKGSRender.cpp:145
		{ rsx::blend_factor::one_minus_src_color,      MTL::BlendFactorOneMinusSourceColor },   // CELL_GCM_ONE_MINUS_SRC_COLOR (gcm_enums.h:1603); VKGSRender.cpp:149
		{ rsx::blend_factor::dst_color,                MTL::BlendFactorDestinationColor },      // CELL_GCM_DST_COLOR (gcm_enums.h:1604); VKGSRender.cpp:146
		{ rsx::blend_factor::one_minus_dst_color,      MTL::BlendFactorOneMinusDestinationColor }, // CELL_GCM_ONE_MINUS_DST_COLOR (gcm_enums.h:1605); VKGSRender.cpp:150
		{ rsx::blend_factor::src_alpha,                MTL::BlendFactorSourceAlpha },           // CELL_GCM_SRC_ALPHA (gcm_enums.h:1606); VKGSRender.cpp:143
		{ rsx::blend_factor::one_minus_src_alpha,      MTL::BlendFactorOneMinusSourceAlpha },   // CELL_GCM_ONE_MINUS_SRC_ALPHA (gcm_enums.h:1607); VKGSRender.cpp:151
		{ rsx::blend_factor::dst_alpha,                MTL::BlendFactorDestinationAlpha },      // CELL_GCM_DST_ALPHA (gcm_enums.h:1608); VKGSRender.cpp:144
		{ rsx::blend_factor::one_minus_dst_alpha,      MTL::BlendFactorOneMinusDestinationAlpha }, // CELL_GCM_ONE_MINUS_DST_ALPHA (gcm_enums.h:1609); VKGSRender.cpp:152
		{ rsx::blend_factor::src_alpha_saturate,       MTL::BlendFactorSourceAlphaSaturated },  // CELL_GCM_SRC_ALPHA_SATURATE (gcm_enums.h:1610); VKGSRender.cpp:155
		{ rsx::blend_factor::constant_color,           MTL::BlendFactorBlendColor },            // CELL_GCM_CONSTANT_COLOR (gcm_enums.h:1611); VKGSRender.cpp:147
		{ rsx::blend_factor::one_minus_constant_color, MTL::BlendFactorOneMinusBlendColor },    // CELL_GCM_ONE_MINUS_CONSTANT_COLOR (gcm_enums.h:1612); VKGSRender.cpp:154
		{ rsx::blend_factor::constant_alpha,           MTL::BlendFactorBlendAlpha },            // CELL_GCM_CONSTANT_ALPHA (gcm_enums.h:1613); VKGSRender.cpp:148
		{ rsx::blend_factor::one_minus_constant_alpha, MTL::BlendFactorOneMinusBlendAlpha },    // CELL_GCM_ONE_MINUS_CONSTANT_ALPHA (gcm_enums.h:1614); VKGSRender.cpp:153
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 15u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_blend_factor(entry.guest), entry.host);
	}
}

TEST(MTLMapBlend, BlendEquationFollowsTheGcmEquationWithSignedFallbacks)
{
	// Metal has no signed blend equation.  The three RSX signed equations take the
	// unsigned fallback (MTLBlendState.cpp:81-89): add_signed and reverse_add_signed
	// become Add, reverse_subtract_signed becomes ReverseSubtract.  GL emulates the
	// same way (GLDraw.cpp:28-42) and Vulkan agrees (VKGSRender.cpp:165-176).
	using row = mapping_row<rsx::blend_equation, MTL::BlendOperation>;

	const row table[] = {
		{ rsx::blend_equation::add,                    MTL::BlendOperationAdd },             // CELL_GCM_FUNC_ADD (gcm_enums.h:1579); VKGSRender.cpp:168
		{ rsx::blend_equation::min,                    MTL::BlendOperationMin },             // CELL_GCM_MIN (gcm_enums.h:1580); VKGSRender.cpp:177
		{ rsx::blend_equation::max,                    MTL::BlendOperationMax },             // CELL_GCM_MAX (gcm_enums.h:1581); VKGSRender.cpp:178
		{ rsx::blend_equation::subtract,               MTL::BlendOperationSubtract },        // CELL_GCM_FUNC_SUBTRACT (gcm_enums.h:1582); VKGSRender.cpp:172
		{ rsx::blend_equation::reverse_subtract,       MTL::BlendOperationReverseSubtract }, // CELL_GCM_FUNC_REVERSE_SUBTRACT (gcm_enums.h:1583); VKGSRender.cpp:176
		{ rsx::blend_equation::reverse_subtract_signed, MTL::BlendOperationReverseSubtract }, // signed fallback (gcm_enums.h:1584); GLDraw.cpp:35-38
		{ rsx::blend_equation::add_signed,             MTL::BlendOperationAdd },             // signed fallback (gcm_enums.h:1585); GLDraw.cpp:28-31
		{ rsx::blend_equation::reverse_add_signed,     MTL::BlendOperationAdd },             // signed fallback (gcm_enums.h:1586); GLDraw.cpp:39-42
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 8u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_blend_operation(entry.guest), entry.host);
	}
}

// =============================================================================
// 8. get_surface_color_format / get_surface_depth_format / get_texture_format
//    (MTLFormats.h:19-28; MTLFormats.cpp:32-141)
// =============================================================================

TEST(MTLMapSurfaceFormat, ColorFormatFollowsTheGcmSurfaceFormat)
{
	// The GL render-target table picks the same internal family for every row
	// (GLRenderTargets.cpp:8-70); the R/B swap on the packed rows is the recorded
	// A1B5G5R5 border-colour hardware bug (HLD-02 §2.7).
	using row = mapping_row<rsx::surface_color_format, MTL::PixelFormat>;

	const row table[] = {
		{ rsx::surface_color_format::b8,                  MTL::PixelFormat::PixelFormatR8Unorm },       // CELL_GCM_SURFACE_B8 (gcm_enums.h:943); GLRenderTargets.cpp:52
		{ rsx::surface_color_format::g8b8,                MTL::PixelFormat::PixelFormatRG8Unorm },      // CELL_GCM_SURFACE_G8B8 (gcm_enums.h:944); GLRenderTargets.cpp:56
		{ rsx::surface_color_format::r5g6b5,              MTL::PixelFormat::PixelFormatB5G6R5Unorm },   // CELL_GCM_SURFACE_R5G6B5 (gcm_enums.h:939); GLRenderTargets.cpp:13
		{ rsx::surface_color_format::x1r5g5b5_z1r5g5b5,   MTL::PixelFormat::PixelFormatBGR5A1Unorm },   // CELL_GCM_SURFACE_X1R5G5B5_Z1R5G5B5 (gcm_enums.h:937); GLRenderTargets.cpp:26
		{ rsx::surface_color_format::x1r5g5b5_o1r5g5b5,   MTL::PixelFormat::PixelFormatBGR5A1Unorm },   // CELL_GCM_SURFACE_X1R5G5B5_O1R5G5B5 (gcm_enums.h:938); GLRenderTargets.cpp:22
		{ rsx::surface_color_format::x8r8g8b8_z8r8g8b8,   MTL::PixelFormat::PixelFormatBGRA8Unorm },    // CELL_GCM_SURFACE_X8R8G8B8_Z8R8G8B8 (gcm_enums.h:940); GLRenderTargets.cpp:30
		{ rsx::surface_color_format::x8r8g8b8_o8r8g8b8,   MTL::PixelFormat::PixelFormatBGRA8Unorm },    // CELL_GCM_SURFACE_X8R8G8B8_O8R8G8B8 (gcm_enums.h:941); GLRenderTargets.cpp:42
		{ rsx::surface_color_format::a8r8g8b8,            MTL::PixelFormat::PixelFormatBGRA8Unorm },    // CELL_GCM_SURFACE_A8R8G8B8 (gcm_enums.h:942); GLRenderTargets.cpp:16
		{ rsx::surface_color_format::x8b8g8r8_z8b8g8r8,   MTL::PixelFormat::PixelFormatRGBA8Unorm },    // CELL_GCM_SURFACE_X8B8G8R8_Z8B8G8R8 (gcm_enums.h:948); GLRenderTargets.cpp:38
		{ rsx::surface_color_format::x8b8g8r8_o8b8g8r8,   MTL::PixelFormat::PixelFormatRGBA8Unorm },    // CELL_GCM_SURFACE_X8B8G8R8_O8B8G8R8 (gcm_enums.h:949); GLRenderTargets.cpp:34
		{ rsx::surface_color_format::a8b8g8r8,            MTL::PixelFormat::PixelFormatRGBA8Unorm },    // CELL_GCM_SURFACE_A8B8G8R8 (gcm_enums.h:950); GLRenderTargets.cpp:64
		{ rsx::surface_color_format::w16z16y16x16,        MTL::PixelFormat::PixelFormatRGBA16Float },   // CELL_GCM_SURFACE_F_W16Z16Y16X16 (gcm_enums.h:945); GLRenderTargets.cpp:46
		{ rsx::surface_color_format::w32z32y32x32,        MTL::PixelFormat::PixelFormatRGBA32Float },   // CELL_GCM_SURFACE_F_W32Z32Y32X32 (gcm_enums.h:946); GLRenderTargets.cpp:49
		{ rsx::surface_color_format::x32,                 MTL::PixelFormat::PixelFormatR32Float },      // CELL_GCM_SURFACE_F_X32 (gcm_enums.h:947); GLRenderTargets.cpp:60
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 14u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_surface_color_format(entry.guest), entry.host);
	}
}

TEST(MTLMapSurfaceFormat, DepthFormatFollowsTheGcmSurfaceDepthFormat)
{
	// z16_float uses a float target, because a unorm target cannot hold the float
	// range; z24s8 has no 24-bit Metal depth format, so it widens to a 32-bit
	// depth plus stencil (VKFormats.cpp:9-32; GLRenderTargets.cpp:72-80).
	using row = mapping_row<rsx::surface_depth_format2, MTL::PixelFormat>;

	const row table[] = {
		{ rsx::surface_depth_format2::z16_uint,   MTL::PixelFormat::PixelFormatDepth16Unorm },        // VKFormats.cpp:13-14; MTLFormats.cpp:71-72
		{ rsx::surface_depth_format2::z16_float,  MTL::PixelFormat::PixelFormatDepth32Float },        // VKFormats.cpp:15-16; MTLFormats.cpp:73-76
		{ rsx::surface_depth_format2::z24s8_uint, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 }, // VKFormats.cpp:17-22; MTLFormats.cpp:77-79
		{ rsx::surface_depth_format2::z24s8_float, MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 }, // VKFormats.cpp:23-27; MTLFormats.cpp:77-79
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 4u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_surface_depth_format(entry.guest), entry.host);
	}
}

TEST(MTLMapTextureFormat, TextureFormatFollowsTheGcmTextureFormat)
{
	// Each row is checked against the Apple Vulkan format (VKFormats.cpp:195-241,
	// the `__APPLE__` branch at :207-215) and the RSX component map
	// (VKFormats.cpp:124-193; GLTexture.cpp:214-269).  The packed rows expand to
	// B8G8R8A8 because the Apple uploader expands them (TextureUtils.cpp:965-1014).
	using row = mapping_row<u32, MTL::PixelFormat>;

	const row table[] = {
		{ CELL_GCM_TEXTURE_B8,                        MTL::PixelFormat::PixelFormatR8Unorm },            // 0x81 (gcm_enums.h:134); VKFormats.cpp:216
		{ CELL_GCM_TEXTURE_A8R8G8B8,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x85 (gcm_enums.h:138); VKFormats.cpp:217
		{ CELL_GCM_TEXTURE_D8R8G8B8,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x9E (gcm_enums.h:159); VKFormats.cpp:232
		{ CELL_GCM_TEXTURE_R5G6B5,                    MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x84 (gcm_enums.h:137); VKFormats.cpp:210
		{ CELL_GCM_TEXTURE_A1R5G5B5,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x82 (gcm_enums.h:135); VKFormats.cpp:213
		{ CELL_GCM_TEXTURE_A4R4G4B4,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x83 (gcm_enums.h:136); VKFormats.cpp:214
		{ CELL_GCM_TEXTURE_R5G5B5A1,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x97 (gcm_enums.h:152); VKFormats.cpp:211
		{ CELL_GCM_TEXTURE_D1R5G5B5,                  MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x9D (gcm_enums.h:158); VKFormats.cpp:212
		{ CELL_GCM_TEXTURE_R6G5B5,                    MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x8F (gcm_enums.h:145); VKFormats.cpp:209
		{ CELL_GCM_TEXTURE_G8B8,                      MTL::PixelFormat::PixelFormatRG8Unorm },           // 0x8B (gcm_enums.h:142); VKFormats.cpp:221
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO8,          MTL::PixelFormat::PixelFormatRG8Unorm },           // 0x98 (gcm_enums.h:153); VKFormats.cpp:233
		{ CELL_GCM_TEXTURE_COMPRESSED_HILO_S8,        MTL::PixelFormat::PixelFormatRG8Snorm },           // 0x99 (gcm_enums.h:154); VKFormats.cpp:234
		{ CELL_GCM_TEXTURE_Y16_X16,                   MTL::PixelFormat::PixelFormatRG16Unorm },          // 0x95 (gcm_enums.h:151); VKFormats.cpp:227
		{ CELL_GCM_TEXTURE_Y16_X16_FLOAT,             MTL::PixelFormat::PixelFormatRG16Float },          // 0x9F (gcm_enums.h:160); VKFormats.cpp:228
		{ CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT,     MTL::PixelFormat::PixelFormatRGBA16Float },        // 0x9A (gcm_enums.h:155); VKFormats.cpp:229
		{ CELL_GCM_TEXTURE_X16,                       MTL::PixelFormat::PixelFormatR16Unorm },           // 0x94 (gcm_enums.h:150); VKFormats.cpp:226
		{ CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT,     MTL::PixelFormat::PixelFormatRGBA32Float },        // 0x9B (gcm_enums.h:156); VKFormats.cpp:230
		{ CELL_GCM_TEXTURE_X32_FLOAT,                 MTL::PixelFormat::PixelFormatR32Float },           // 0x9C (gcm_enums.h:157); VKFormats.cpp:231
		{ CELL_GCM_TEXTURE_DEPTH16,                   MTL::PixelFormat::PixelFormatDepth16Unorm },       // 0x92 (gcm_enums.h:148); VKFormats.cpp:224
		{ CELL_GCM_TEXTURE_DEPTH16_FLOAT,             MTL::PixelFormat::PixelFormatDepth32Float },       // 0x93 (gcm_enums.h:149); VKFormats.cpp:225
		{ CELL_GCM_TEXTURE_DEPTH24_D8,                MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 }, // 0x90 (gcm_enums.h:146); VKFormats.cpp:222
		{ CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT,          MTL::PixelFormat::PixelFormatDepth32Float_Stencil8 }, // 0x91 (gcm_enums.h:147); VKFormats.cpp:223
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT1,           MTL::PixelFormat::PixelFormatBC1_RGBA },           // 0x86 (gcm_enums.h:139); VKFormats.cpp:218
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT23,          MTL::PixelFormat::PixelFormatBC2_RGBA },           // 0x87 (gcm_enums.h:140); VKFormats.cpp:219
		{ CELL_GCM_TEXTURE_COMPRESSED_DXT45,          MTL::PixelFormat::PixelFormatBC3_RGBA },           // 0x88 (gcm_enums.h:141); VKFormats.cpp:220
		{ CELL_GCM_TEXTURE_COMPRESSED_B8R8_G8R8,      MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x8D (gcm_enums.h:143); VKFormats.cpp:235
		{ CELL_GCM_TEXTURE_COMPRESSED_R8B8_R8G8,      MTL::PixelFormat::PixelFormatBGRA8Unorm },         // 0x8E (gcm_enums.h:144); VKFormats.cpp:236
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 27u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(entry.guest);
		EXPECT_EQ(mtl::get_texture_format(entry.guest), entry.host);
	}
}

TEST(MTLMapTextureFormat, PackedFormatsExpandToBgraWithAnArgbRead)
{
	// The GCM packed names put R first and B last, with R in the high bits
	// (TextureUtils.cpp:45-56, :84-111).  The Apple uploader expands each to BGRA8
	// (TextureUtils.cpp:965-1014), and the unit reads it A-R-G-B, matching the GL
	// swizzle (GLTexture.cpp:219-230) and the Vulkan map (VKFormats.cpp:131-138).
	EXPECT_EQ(CELL_GCM_TEXTURE_R5G6B5, 0x84u);   // gcm_enums.h:137
	EXPECT_EQ(CELL_GCM_TEXTURE_R6G5B5, 0x8Fu);   // gcm_enums.h:145
	EXPECT_EQ(CELL_GCM_TEXTURE_R5G5B5A1, 0x97u); // gcm_enums.h:152

	constexpr u32 packed[] = {
		CELL_GCM_TEXTURE_R5G6B5,
		CELL_GCM_TEXTURE_R6G5B5,
		CELL_GCM_TEXTURE_R5G5B5A1,
	};

	constexpr std::array<MTL::TextureSwizzle, 4> argb = {
		MTL::TextureSwizzleAlpha,
		MTL::TextureSwizzleRed,
		MTL::TextureSwizzleGreen,
		MTL::TextureSwizzleBlue,
	};

	for (const u32 gcm : packed)
	{
		SCOPED_TRACE(gcm);
		EXPECT_EQ(mtl::get_texture_format(gcm), MTL::PixelFormat::PixelFormatBGRA8Unorm);

		const auto layout = mtl::get_texture_component_layout(gcm);
		for (usz i = 0; i < layout.size(); ++i)
		{
			SCOPED_TRACE(std::to_string(gcm) + "[" + std::to_string(i) + "]");
			EXPECT_EQ(layout[i], argb[i]);
		}
	}
}

TEST(MTLMapSurfaceFormat, PackedSurfaceFormatsUseTheSwappedChannelOrder)
{
	// GCM packs R in the high bits (TextureUtils.cpp:47-49, :86-89).  Metal names
	// its 16-bit format from the blue end, so r5g6b5 becomes B5G6R5Unorm, and the
	// A1R5G5B5 family becomes BGR5A1Unorm.  GL selects the same rgb565/bgr5a1
	// internals (GLRenderTargets.cpp:13, :22-28).
	EXPECT_EQ(mtl::get_surface_color_format(rsx::surface_color_format::r5g6b5), MTL::PixelFormat::PixelFormatB5G6R5Unorm);
	EXPECT_EQ(mtl::get_surface_color_format(rsx::surface_color_format::x1r5g5b5_z1r5g5b5), MTL::PixelFormat::PixelFormatBGR5A1Unorm);
	EXPECT_EQ(mtl::get_surface_color_format(rsx::surface_color_format::x1r5g5b5_o1r5g5b5), MTL::PixelFormat::PixelFormatBGR5A1Unorm);
}

TEST(MTLMapFormatAgreement, SharedGcmLayoutsMapToTheSamePixelFormat)
{
	// A surface colour format and a texture format that describe the same guest
	// layout must land on the same Metal format.  r5g6b5 is left out on purpose:
	// the texture path expands the packed format to BGRA8 with a component
	// layout, not to a packed Metal format (MTLFormats.h:25-28).
	struct row
	{
		rsx::surface_color_format surface;
		u32 texture;
	};

	const row table[] = {
		{ rsx::surface_color_format::b8,           CELL_GCM_TEXTURE_B8 },                    // R8Unorm
		{ rsx::surface_color_format::g8b8,         CELL_GCM_TEXTURE_G8B8 },                  // RG8Unorm
		{ rsx::surface_color_format::a8r8g8b8,     CELL_GCM_TEXTURE_A8R8G8B8 },              // BGRA8Unorm
		{ rsx::surface_color_format::w16z16y16x16, CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT }, // RGBA16Float
		{ rsx::surface_color_format::w32z32y32x32, CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT }, // RGBA32Float
		{ rsx::surface_color_format::x32,          CELL_GCM_TEXTURE_X32_FLOAT },             // R32Float
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 6u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(entry.texture);
		EXPECT_EQ(mtl::get_surface_color_format(entry.surface), mtl::get_texture_format(entry.texture));
	}
}

// =============================================================================
// 9. get_pixel_size / is_bgra_format
//    (MTLFormats.h:36,39; MTLFormats.cpp:9-30)
// =============================================================================

TEST(MTLMapPixelSize, TheEightBitColourFormatsReportFourBytes)
{
	constexpr MTL::PixelFormat eight_bit[] = {
		MTL::PixelFormat::PixelFormatBGRA8Unorm,
		MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB,
		MTL::PixelFormat::PixelFormatRGBA8Unorm,
		MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB,
	};

	for (const MTL::PixelFormat format : eight_bit)
	{
		SCOPED_TRACE(static_cast<u64>(format));
		EXPECT_EQ(mtl::get_pixel_size(format), 4u);
	}

	// A depth format and a 16-bit colour format are not presentable.
	EXPECT_EQ(mtl::get_pixel_size(MTL::PixelFormat::PixelFormatDepth16Unorm), 0u);
	EXPECT_EQ(mtl::get_pixel_size(MTL::PixelFormat::PixelFormatDepth32Float), 0u);
	EXPECT_EQ(mtl::get_pixel_size(MTL::PixelFormat::PixelFormatRGBA16Float), 0u);
}

TEST(MTLMapPixelSize, BgraImpliesFourBytesAndOnlyTwoFormatsAreBgra)
{
	// The output path returns 0 or 4 for any enumerator, and is_bgra is true for
	// exactly the two BGRA8 formats (MTLFormats.cpp:11-30).  The first rule is the
	// load-bearing one: a BGRA format that reports a size other than 4 breaks the
	// sink's stride.
	for (u32 raw = 0; raw < 512; ++raw)
	{
		SCOPED_TRACE(raw);
		const MTL::PixelFormat format = static_cast<MTL::PixelFormat>(raw);

		const u32 size = mtl::get_pixel_size(format);
		EXPECT_TRUE(size == 0u || size == 4u);

		if (mtl::is_bgra_format(format))
		{
			EXPECT_EQ(size, 4u);
		}

		EXPECT_EQ(mtl::is_bgra_format(format),
			format == MTL::PixelFormat::PixelFormatBGRA8Unorm || format == MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB);
	}
}

TEST(MTLMapPixelSize, EverySurfaceColourFormatIsZeroOrFourBytes)
{
	// The output path presents 8-bit colour; every other surface width reports 0
	// (MTLFormats.cpp:11-23).  The expected size comes from the RSX surface byte
	// width (gcm_enums.h:1364-1380), not from the Metal function.
	using row = mapping_row<rsx::surface_color_format, u32>;

	const row table[] = {
		{ rsx::surface_color_format::b8,                0 }, // 1 byte
		{ rsx::surface_color_format::g8b8,              0 }, // 2 bytes
		{ rsx::surface_color_format::r5g6b5,            0 }, // 2 bytes
		{ rsx::surface_color_format::x1r5g5b5_z1r5g5b5, 0 }, // 2 bytes
		{ rsx::surface_color_format::x1r5g5b5_o1r5g5b5, 0 }, // 2 bytes
		{ rsx::surface_color_format::x8r8g8b8_z8r8g8b8, 4 }, // 4 bytes
		{ rsx::surface_color_format::x8r8g8b8_o8r8g8b8, 4 }, // 4 bytes
		{ rsx::surface_color_format::a8r8g8b8,          4 }, // 4 bytes
		{ rsx::surface_color_format::x8b8g8r8_z8b8g8r8, 4 }, // 4 bytes
		{ rsx::surface_color_format::x8b8g8r8_o8b8g8r8, 4 }, // 4 bytes
		{ rsx::surface_color_format::a8b8g8r8,          4 }, // 4 bytes
		{ rsx::surface_color_format::w16z16y16x16,      0 }, // 8 bytes
		{ rsx::surface_color_format::w32z32y32x32,      0 }, // 16 bytes
		{ rsx::surface_color_format::x32,               0 }, // 4 bytes, float
	};

	EXPECT_EQ(sizeof(table) / sizeof(table[0]), 14u);

	for (const row& entry : table)
	{
		SCOPED_TRACE(static_cast<u32>(entry.guest));
		EXPECT_EQ(mtl::get_pixel_size(mtl::get_surface_color_format(entry.guest)), entry.host);
	}
}

// =============================================================================
// 10. capture_cull_mode / capture_front_face
//     (MTLRasterState.h:51-56; MTLRasterState.cpp:39-67)
// =============================================================================

TEST(MTLMapRaster, CullModeFollowsTheGcmCullFace)
{
	// Metal has no cull-both mode, so front_and_back culls nothing at the encoder
	// and culls_both_faces() tells the pipeline key to drop the raster
	// (MTLRasterState.h:49-50; MTLRasterState.cpp:39-62).
	constexpr rsx::cull_face faces[] = {
		rsx::cull_face::front,          // CELL_GCM_FRONT (gcm_enums.h:1670)
		rsx::cull_face::back,           // CELL_GCM_BACK (gcm_enums.h:1671)
		rsx::cull_face::front_and_back, // CELL_GCM_FRONT_AND_BACK (gcm_enums.h:1672)
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

TEST(MTLMapRaster, WindingFollowsTheGcmFrontFace)
{
	// The Metal viewport is not y-flipped, so the winding is direct
	// (MTLRasterState.h:54-55).  cw becomes Clockwise and ccw becomes
	// CounterClockwise (gcm_enums.h:1656-1657).
	mtl::raster_state state{};

	state.front_face = rsx::front_face::cw;
	SCOPED_TRACE("cw");
	EXPECT_EQ(mtl::capture_front_face(state), MTL::WindingClockwise);

	state.front_face = rsx::front_face::ccw;
	SCOPED_TRACE("ccw");
	EXPECT_EQ(mtl::capture_front_face(state), MTL::WindingCounterClockwise);
}

TEST(MTLMapRaster, TheLatchedRegistersProduceTheDerivedCullMode)
{
	// The latch reads SET_CULL_FACE_ENABLE, SET_CULL_FACE and SET_FRONT_FACE
	// (MTLRasterState.cpp:13-15; rsx_decode.h:920, :2702, :2724).  A power-on
	// register file plus the three writes is a legal capture.
	constexpr rsx::cull_face faces[] = {
		rsx::cull_face::front,
		rsx::cull_face::back,
		rsx::cull_face::front_and_back,
	};

	constexpr rsx::front_face windings[] = {
		rsx::front_face::cw,
		rsx::front_face::ccw,
	};

	for (const u32 enabled : { 0u, 1u })
	{
		for (const rsx::cull_face face : faces)
		{
			for (const rsx::front_face winding : windings)
			{
				SCOPED_TRACE(std::to_string(enabled) + "," + std::to_string(static_cast<u32>(face)) + "," + std::to_string(static_cast<u32>(winding)));

				reset_registers();
				set_register(NV4097_SET_CULL_FACE_ENABLE, enabled);
				set_register(NV4097_SET_CULL_FACE, static_cast<u32>(face));
				set_register(NV4097_SET_FRONT_FACE, static_cast<u32>(winding));

				const mtl::raster_state state = mtl::capture_raster_state();
				EXPECT_EQ(state.cull_enabled, enabled != 0);
				EXPECT_EQ(state.cull_face, face);
				EXPECT_EQ(state.front_face, winding);

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
				EXPECT_EQ(mtl::capture_front_face(state),
					winding == rsx::front_face::cw ? MTL::WindingClockwise : MTL::WindingCounterClockwise);
			}
		}
	}
}

#endif // RSX_ENABLE_METAL4
