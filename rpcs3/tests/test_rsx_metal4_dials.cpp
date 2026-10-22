#include <gtest/gtest.h>

#include "Emu/RSX/MTL/MTLArgumentBinds.h"
#include "Emu/RSX/MTL/MTLBlendState.h"
#include "Emu/RSX/MTL/MTLDepthStencil.h"
#include "Emu/RSX/MTL/MTLDrawState.h"
#include "Emu/RSX/MTL/MTLRasterState.h"
#include "Emu/RSX/MTL/MTLVertexState.h"

// The Metal 4 dial contract (docs/research/metal4/inventory/out).
//
// The curated state space is the authority on which dials are live.  These tests
// never restate it: they read it, and they fail when a test claims a dial the
// spine does not have.  Tier 1 proves the RSX-to-Metal enum mappers are total and
// collide only where the design records a collision.  Tier 2 proves one live dial
// reaches one latched field.  The ratchet is the floor of Tier 2 coverage; it
// never regresses, and it prints what is still missing.
#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Program/RSXFragmentProgram.h"
#include "Emu/RSX/Program/RSXVertexProgram.h"

// The exhaustive dial list, one row per dial of the inventory
// (docs/research/metal4/inventory/tools/gen_dial_list.py).
#include "rsx_metal4_dial_list.inc"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef RPCS3_SOURCE_DIR
#error "The dial tests need RPCS3_SOURCE_DIR to find the inventory."
#endif

namespace
{
	// ---------------------------------------------------------------- the spine

	// One row of an inventory CSV, split on commas.  Every file's first fields hold
	// no comma; the fields that do hold one are never read.
	using csv_row = std::vector<std::string>;

	// The inventory stores plain decimal; parse without touching the locale or
	// throwing, because the emulator build has no exceptions.
	u32 to_u32(const std::string& text)
	{
		u32 value = 0;

		for (const char c : text)
		{
			if (c >= '0' && c <= '9')
			{
				value = value * 10 + static_cast<u32>(c - '0');
			}
		}

		return value;
	}

	std::vector<csv_row> read_csv(const std::string& name)
	{
		std::vector<csv_row> rows;
		std::ifstream file(std::string(RPCS3_SOURCE_DIR) + "/docs/research/metal4/inventory/out/" + name);
		std::string line;

		if (!std::getline(file, line))
		{
			return rows;
		}

		while (std::getline(file, line))
		{
			std::stringstream stream(line);
			csv_row row;
			std::string cell;

			while (std::getline(stream, cell, ','))
			{
				row.push_back(cell);
			}

			rows.push_back(std::move(row));
		}

		return rows;
	}

	// `id,name,group,cluster,addr,addr_dec,word,has_decoder,n_fields,binding_kind,
	//  handler,step,count,source,disposition,metal4_target,...`
	const std::vector<csv_row>& spine()
	{
		static const std::vector<csv_row> rows = read_csv("rsx_state_space_curated.csv");
		return rows;
	}

	constexpr std::size_t spine_id = 0;
	constexpr std::size_t spine_cluster = 3;
	constexpr std::size_t spine_word = 6;
	constexpr std::size_t spine_disposition = 14;

	bool is_live(const std::string& disposition)
	{
		return disposition == "reshape" || disposition == "emulate" || disposition == "direct";
	}

	std::set<std::string> live_dials(const std::string& cluster)
	{
		std::set<std::string> result;

		for (const auto& row : spine())
		{
			if (row.size() > spine_disposition && row[spine_cluster] == cluster && is_live(row[spine_disposition]))
			{
				result.insert(row[spine_id]);
			}
		}

		return result;
	}

	// --------------------------------------------------------- the register file

	// The direct write path (rsx_methods.cpp:145-171).  The register index is the
	// word, so the `NV4097_*` constants are already divided by four.
	void reset_registers()
	{
		// The emulator's own power-on state (rsx_methods.cpp:797).  An all-zero
		// register file is not a legal RSX state: a getter that maps a word to a
		// GCM enum goes through `gcm_enum_cast`, which returns an `expected`, and
		// the conversion to the enum verifies (gcm_enums.h:1188).  `FRONT_FACE`
		// zero is one such word, so a zeroed fixture aborts on a valid dial.
		rsx::method_registers.reset();
	}

	void set_register(u32 index, u32 value)
	{
		rsx::method_registers.registers[index] = value;
	}

	// ------------------------------------------------------------ the registry

	// One entry per Tier 2 test.  A test registers itself as it is defined, so the
	// registry and the test set cannot drift.
	struct dial_registration
	{
		dial_registration(std::string cluster, std::string id)
		{
			covered().emplace_back(std::move(cluster), std::move(id));
		}

		static std::vector<std::pair<std::string, std::string>>& covered()
		{
			static std::vector<std::pair<std::string, std::string>> rows;
			return rows;
		}
	};

	// The spine spells a cluster with a hyphen and a C++ identifier cannot, so the
	// suite token carries the hyphen.
	std::string cluster_of(std::string name)
	{
		for (char& c : name)
		{
			if (c == '_')
			{
				c = '-';
			}
		}

		return name;
	}

	// Every Tier 2 test starts from an empty register file, so no test can depend
	// on the one before it.
	struct dial_test : ::testing::Test
	{
		void SetUp() override
		{
			reset_registers();
		}
	};

	// A Tier 2 latch test.  The name carries the cluster and the dial, so a failure
	// names both.
#define MTL_DIAL_TEST(cluster, reg)                                                      \
	static const ::dial_registration dial_##reg{ ::cluster_of(#cluster), "M_" #reg };    \
	TEST_F(dial_test, MTLDial_##cluster##_##reg)

	// ------------------------------------------------------------- the ratchet

	void check_ratchet(const std::string& cluster, std::size_t floor)
	{
		const std::set<std::string> expected = live_dials(cluster);
		ASSERT_FALSE(expected.empty()) << "the spine did not load, or " << cluster << " has no live dial";

		std::set<std::string> covered;
		for (const auto& [entry_cluster, id] : dial_registration::covered())
		{
			if (entry_cluster == cluster)
			{
				covered.insert(id);
			}
		}

		for (const auto& id : covered)
		{
			EXPECT_EQ(expected.count(id), 1u) << id << " is covered but is not a live dial of " << cluster;
		}

		for (const auto& id : expected)
		{
			if (!covered.count(id))
			{
				std::cout << "  " << cluster << " uncovered: " << id << '\n';
			}
		}

		EXPECT_GE(covered.size(), floor) << cluster << " coverage regressed";
	}

	// ------------------------------------------------------- the enum domains

	constexpr rsx::comparison_function compare_domain[] = {
		rsx::comparison_function::never,
		rsx::comparison_function::less,
		rsx::comparison_function::equal,
		rsx::comparison_function::less_or_equal,
		rsx::comparison_function::greater,
		rsx::comparison_function::not_equal,
		rsx::comparison_function::greater_or_equal,
		rsx::comparison_function::always,
	};

	constexpr rsx::stencil_op stencil_domain[] = {
		rsx::stencil_op::keep,
		rsx::stencil_op::zero,
		rsx::stencil_op::replace,
		rsx::stencil_op::incr,
		rsx::stencil_op::decr,
		rsx::stencil_op::invert,
		rsx::stencil_op::incr_wrap,
		rsx::stencil_op::decr_wrap,
	};

	constexpr rsx::blend_factor factor_domain[] = {
		rsx::blend_factor::zero,
		rsx::blend_factor::one,
		rsx::blend_factor::src_color,
		rsx::blend_factor::one_minus_src_color,
		rsx::blend_factor::dst_color,
		rsx::blend_factor::one_minus_dst_color,
		rsx::blend_factor::src_alpha,
		rsx::blend_factor::one_minus_src_alpha,
		rsx::blend_factor::dst_alpha,
		rsx::blend_factor::one_minus_dst_alpha,
		rsx::blend_factor::src_alpha_saturate,
		rsx::blend_factor::constant_color,
		rsx::blend_factor::one_minus_constant_color,
		rsx::blend_factor::constant_alpha,
		rsx::blend_factor::one_minus_constant_alpha,
	};

	constexpr rsx::blend_equation equation_domain[] = {
		rsx::blend_equation::add,
		rsx::blend_equation::min,
		rsx::blend_equation::max,
		rsx::blend_equation::subtract,
		rsx::blend_equation::reverse_subtract,
		rsx::blend_equation::reverse_subtract_signed,
		rsx::blend_equation::add_signed,
		rsx::blend_equation::reverse_add_signed,
	};

	constexpr rsx::primitive_type primitive_domain[] = {
		rsx::primitive_type::points,
		rsx::primitive_type::lines,
		rsx::primitive_type::line_loop,
		rsx::primitive_type::line_strip,
		rsx::primitive_type::triangles,
		rsx::primitive_type::triangle_strip,
		rsx::primitive_type::triangle_fan,
		rsx::primitive_type::quads,
		rsx::primitive_type::quad_strip,
		rsx::primitive_type::polygon,
	};

	constexpr rsx::index_array_type index_domain[] = {
		rsx::index_array_type::u16,
		rsx::index_array_type::u32,
	};

	// The size of the image of a mapper.  A mapper is injective exactly when this
	// equals the domain size.
	template <typename RsxEnum, std::size_t N, typename Mapper>
	std::size_t image_size(const RsxEnum (&domain)[N], Mapper mapper)
	{
		std::set<long long> image;

		for (const RsxEnum value : domain)
		{
			image.insert(static_cast<long long>(mapper(value)));
		}

		return image.size();
	}
}

// =============================================================================
// The spine
// =============================================================================

TEST(MTLDialSpine, ReaderSeesTheInventory)
{
	ASSERT_FALSE(spine().empty()) << "RPCS3_SOURCE_DIR=" RPCS3_SOURCE_DIR;

	std::size_t live = 0;
	for (const auto& row : spine())
	{
		if (is_live(row[spine_disposition]))
		{
			++live;
		}
	}

	EXPECT_EQ(spine().size(), 282u);
	EXPECT_EQ(live, 203u);
}

// =============================================================================
// Tier 1 - the RSX-to-Metal enum mappers
// =============================================================================

TEST(MTLDialMapper, CompareFunctionIsInjective)
{
	// The sampler path adds a second argument, so the mapper is wrapped to keep the
	// injectivity assertion about the one-argument mapping.
	EXPECT_EQ(image_size(compare_domain, [](rsx::comparison_function op) { return mtl::get_compare_function(op); }), 8u);
	EXPECT_EQ(image_size(compare_domain, [](rsx::comparison_function op) { return mtl::get_compare_function(op, true); }), 8u);
}

TEST(MTLDialMapper, StencilOperationIsInjective)
{
	EXPECT_EQ(image_size(stencil_domain, mtl::get_stencil_operation), 8u);
}

TEST(MTLDialMapper, BlendFactorIsInjective)
{
	EXPECT_EQ(image_size(factor_domain, mtl::get_blend_factor), 15u);
}

TEST(MTLDialMapper, IndexTypeIsInjective)
{
	EXPECT_EQ(image_size(index_domain, mtl::get_index_type), 2u);
}

TEST(MTLDialMapper, BlendOperationCollapsesTheSignedEquations)
{
	// Only the three signed equations lose their meaning; they fall back to the
	// unsigned operation (MTLBlendState.cpp:81-89).
	EXPECT_EQ(image_size(equation_domain, mtl::get_blend_operation), 5u);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::add), MTL::BlendOperationAdd);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::subtract), MTL::BlendOperationSubtract);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::reverse_subtract), MTL::BlendOperationReverseSubtract);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::min), MTL::BlendOperationMin);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::max), MTL::BlendOperationMax);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::add_signed), MTL::BlendOperationAdd);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::reverse_add_signed), MTL::BlendOperationAdd);
	EXPECT_EQ(mtl::get_blend_operation(rsx::blend_equation::reverse_subtract_signed), MTL::BlendOperationReverseSubtract);
}

TEST(MTLDialMapper, PrimitiveTypeCollapsesToNativeTopologies)
{
	// Metal has no line loop, no quad list and no polygon; the shared expander
	// rewrites them (Common/BufferUtils.cpp:942-957; CONFLICTS.md C5).
	EXPECT_EQ(image_size(primitive_domain, mtl::get_primitive_type), 5u);
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::points), MTL::PrimitiveTypePoint);
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::lines), MTL::PrimitiveTypeLine);
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::line_loop), mtl::get_primitive_type(rsx::primitive_type::line_strip));
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::triangle_fan), mtl::get_primitive_type(rsx::primitive_type::triangles));
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::quads), mtl::get_primitive_type(rsx::primitive_type::triangles));
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::polygon), mtl::get_primitive_type(rsx::primitive_type::triangles));
	EXPECT_EQ(mtl::get_primitive_type(rsx::primitive_type::quad_strip), mtl::get_primitive_type(rsx::primitive_type::triangle_strip));
}

// =============================================================================
// Tier 2 - the latched dials
// =============================================================================

MTL_DIAL_TEST(depth_rop, NV4097_SET_DEPTH_TEST_ENABLE)
{
	// Metal has no "depth test off": the reshape is compare Always plus no write
	// (CONFLICTS.md C2).
	set_register(NV4097_SET_DEPTH_FUNC, static_cast<u32>(rsx::comparison_function::never));
	set_register(NV4097_SET_DEPTH_MASK, 1);
	set_register(NV4097_SET_DEPTH_TEST_ENABLE, 0);

	const mtl::depth_stencil_state disabled = mtl::capture_depth_stencil_state();
	EXPECT_EQ(disabled.depth_compare, rsx::comparison_function::always);
	EXPECT_FALSE(disabled.depth_write);

	set_register(NV4097_SET_DEPTH_TEST_ENABLE, 1);

	const mtl::depth_stencil_state enabled = mtl::capture_depth_stencil_state();
	EXPECT_EQ(enabled.depth_compare, rsx::comparison_function::never);
	EXPECT_TRUE(enabled.depth_write);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_DEPTH_FUNC)
{
	set_register(NV4097_SET_DEPTH_TEST_ENABLE, 1);
	set_register(NV4097_SET_DEPTH_FUNC, static_cast<u32>(rsx::comparison_function::greater_or_equal));

	EXPECT_EQ(mtl::capture_depth_stencil_state().depth_compare, rsx::comparison_function::greater_or_equal);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_DEPTH_MASK)
{
	set_register(NV4097_SET_DEPTH_TEST_ENABLE, 1);

	set_register(NV4097_SET_DEPTH_MASK, 1);
	EXPECT_TRUE(mtl::capture_depth_stencil_state().depth_write);

	set_register(NV4097_SET_DEPTH_MASK, 0);
	EXPECT_FALSE(mtl::capture_depth_stencil_state().depth_write);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_TEST_ENABLE)
{
	set_register(NV4097_SET_STENCIL_FUNC, static_cast<u32>(rsx::comparison_function::never));

	const mtl::depth_stencil_state disabled = mtl::capture_depth_stencil_state();
	EXPECT_EQ(disabled.front.compare, rsx::comparison_function::always);
	EXPECT_EQ(disabled.back.compare, rsx::comparison_function::always);

	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);

	const mtl::depth_stencil_state enabled = mtl::capture_depth_stencil_state();
	EXPECT_EQ(enabled.front.compare, rsx::comparison_function::never);
	EXPECT_EQ(enabled.back.compare, rsx::comparison_function::never);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_FUNC)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_FUNC, static_cast<u32>(rsx::comparison_function::less_or_equal));

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.compare, rsx::comparison_function::less_or_equal);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_FUNC_REF)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_FUNC_REF, 0x5a);
	set_register(NV4097_SET_BACK_STENCIL_FUNC_REF, 0xc3);

	EXPECT_EQ(mtl::capture_stencil_references().front, 0x5a);
	// With the two-sided test off, the back face takes the front reference.
	EXPECT_EQ(mtl::capture_stencil_references().back, 0x5a);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_FUNC_MASK)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_FUNC_MASK, 0x3c);

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.read_mask, 0x3c);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_MASK)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_MASK, 0xa5);

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.write_mask, 0xa5);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_OP_FAIL)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_OP_FAIL, static_cast<u32>(rsx::stencil_op::replace));

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.fail, rsx::stencil_op::replace);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_OP_ZFAIL)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_OP_ZFAIL, static_cast<u32>(rsx::stencil_op::invert));

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.zfail, rsx::stencil_op::invert);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_STENCIL_OP_ZPASS)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_OP_ZPASS, static_cast<u32>(rsx::stencil_op::decr_wrap));

	EXPECT_EQ(mtl::capture_depth_stencil_state().front.zpass, rsx::stencil_op::decr_wrap);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE)
{
	// Metal always carries both faces; the one-sided case mirrors the front
	// (LLD-depth-rop §2.3).
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_STENCIL_FUNC, static_cast<u32>(rsx::comparison_function::less));
	set_register(NV4097_SET_BACK_STENCIL_FUNC, static_cast<u32>(rsx::comparison_function::greater));

	const mtl::depth_stencil_state mirrored = mtl::capture_depth_stencil_state();
	EXPECT_EQ(mirrored.back.compare, rsx::comparison_function::less);

	set_register(NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, 1);

	const mtl::depth_stencil_state sided = mtl::capture_depth_stencil_state();
	EXPECT_EQ(sided.front.compare, rsx::comparison_function::less);
	EXPECT_EQ(sided.back.compare, rsx::comparison_function::greater);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_BACK_STENCIL_FUNC)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_BACK_STENCIL_FUNC, static_cast<u32>(rsx::comparison_function::not_equal));

	EXPECT_EQ(mtl::capture_depth_stencil_state().back.compare, rsx::comparison_function::not_equal);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_BACK_STENCIL_MASK)
{
	set_register(NV4097_SET_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_BACK_STENCIL_MASK, 0x0f);

	EXPECT_EQ(mtl::capture_depth_stencil_state().back.write_mask, 0x0f);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_BACK_STENCIL_FUNC_REF)
{
	// The back reference is live only under a two-sided stencil test; otherwise
	// the front reference serves both faces (GLDraw.cpp:148-156).
	set_register(NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE, 1);
	set_register(NV4097_SET_BACK_STENCIL_FUNC_REF, 0xc3);

	EXPECT_EQ(mtl::capture_stencil_references().back, 0xc3);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_ZMIN_MAX_CONTROL)
{
	// SET_ZMIN_MAX_CONTROL carries clip (b0) and clamp (b4) independently, and
	// Metal carries one bit (LLD-depth-rop §3.3).
	set_register(NV4097_SET_ZMIN_MAX_CONTROL, 1);
	EXPECT_EQ(mtl::capture_depth_clip_mode(), MTL::DepthClipModeClip);

	set_register(NV4097_SET_ZMIN_MAX_CONTROL, 1 << 4);
	EXPECT_EQ(mtl::capture_depth_clip_mode(), MTL::DepthClipModeClamp);
}

MTL_DIAL_TEST(depth_rop, NV4097_SET_ANTI_ALIASING_CONTROL)
{
	// Alpha-to-coverage and alpha-to-one are descriptor state on Metal 4, so they
	// leave the encoder and enter the pipeline key (CONFLICTS.md C7).
	// Bit 0 is the MSAA enable, bit 4 alpha-to-coverage and bit 8 alpha-to-one
	// (rsx_decode.h:4010-4028).
	set_register(NV4097_SET_ANTI_ALIASING_CONTROL, 1 << 4);

	const mtl::anti_aliasing_state coverage = mtl::capture_anti_aliasing_state();
	EXPECT_TRUE(coverage.alpha_to_coverage);
	EXPECT_FALSE(coverage.alpha_to_one);

	set_register(NV4097_SET_ANTI_ALIASING_CONTROL, (1 << 4) | (1 << 8));

	const mtl::anti_aliasing_state both = mtl::capture_anti_aliasing_state();
	EXPECT_TRUE(both.alpha_to_coverage);
	EXPECT_TRUE(both.alpha_to_one);
}

MTL_DIAL_TEST(raster, NV4097_SET_CULL_FACE_ENABLE)
{
	set_register(NV4097_SET_CULL_FACE, static_cast<u32>(rsx::cull_face::back));

	set_register(NV4097_SET_CULL_FACE_ENABLE, 0);
	EXPECT_EQ(mtl::capture_cull_mode(mtl::capture_raster_state()), MTL::CullModeNone);

	set_register(NV4097_SET_CULL_FACE_ENABLE, 1);
	EXPECT_EQ(mtl::capture_cull_mode(mtl::capture_raster_state()), MTL::CullModeBack);
}

MTL_DIAL_TEST(raster, NV4097_SET_CULL_FACE)
{
	set_register(NV4097_SET_CULL_FACE_ENABLE, 1);

	set_register(NV4097_SET_CULL_FACE, static_cast<u32>(rsx::cull_face::front));
	EXPECT_EQ(mtl::capture_cull_mode(mtl::capture_raster_state()), MTL::CullModeFront);

	// Metal has no cull-both mode, so the pipeline key drops the rasterisation
	// (LLD-raster Q6).
	set_register(NV4097_SET_CULL_FACE, static_cast<u32>(rsx::cull_face::front_and_back));

	const mtl::raster_state both = mtl::capture_raster_state();
	EXPECT_TRUE(mtl::culls_both_faces(both));
	EXPECT_EQ(mtl::capture_cull_mode(both), MTL::CullModeNone);
}

MTL_DIAL_TEST(raster, NV4097_SET_FRONT_FACE)
{
	// The draw viewport negates its height, so the guest winding maps to the
	// opposite Metal winding (invariant 2).
	set_register(NV4097_SET_FRONT_FACE, static_cast<u32>(rsx::front_face::cw));
	EXPECT_EQ(mtl::capture_front_face(mtl::capture_raster_state()), MTL::WindingCounterClockwise);

	set_register(NV4097_SET_FRONT_FACE, static_cast<u32>(rsx::front_face::ccw));
	EXPECT_EQ(mtl::capture_front_face(mtl::capture_raster_state()), MTL::WindingClockwise);
}

MTL_DIAL_TEST(raster, NV4097_SET_FRONT_POLYGON_MODE)
{
	set_register(NV4097_SET_FRONT_POLYGON_MODE, static_cast<u32>(rsx::polygon_mode::line));

	EXPECT_EQ(mtl::capture_fill_mode(mtl::capture_raster_state()), MTL::TriangleFillModeLines);
}

MTL_DIAL_TEST(raster, NV4097_SET_BACK_POLYGON_MODE)
{
	// Metal has one fill mode per encoder, so the pair collapses to the face that
	// survives culling.
	set_register(NV4097_SET_FRONT_POLYGON_MODE, static_cast<u32>(rsx::polygon_mode::fill));
	set_register(NV4097_SET_BACK_POLYGON_MODE, static_cast<u32>(rsx::polygon_mode::line));
	set_register(NV4097_SET_CULL_FACE, static_cast<u32>(rsx::cull_face::front));
	set_register(NV4097_SET_CULL_FACE_ENABLE, 1);

	EXPECT_EQ(mtl::capture_fill_mode(mtl::capture_raster_state()), MTL::TriangleFillModeLines);
}

MTL_DIAL_TEST(raster, NV4097_SET_POLY_OFFSET_FILL_ENABLE)
{
	set_register(NV4097_SET_POLY_OFFSET_FILL_ENABLE, 0);

	f32 bias = 0.0f;
	f32 slope_scale = 0.0f;
	EXPECT_FALSE(mtl::capture_depth_bias(mtl::capture_raster_state(), bias, slope_scale));

	set_register(NV4097_SET_POLY_OFFSET_FILL_ENABLE, 1);
	EXPECT_TRUE(mtl::capture_depth_bias(mtl::capture_raster_state(), bias, slope_scale));
}

MTL_DIAL_TEST(raster, NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR)
{
	set_register(NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, std::bit_cast<u32>(1.5f));

	f32 bias = 0.0f;
	f32 slope_scale = 0.0f;
	mtl::capture_depth_bias(mtl::capture_raster_state(), bias, slope_scale);

	EXPECT_FLOAT_EQ(slope_scale, 1.5f);
}

MTL_DIAL_TEST(raster, NV4097_SET_POLYGON_OFFSET_BIAS)
{
	set_register(NV4097_SET_POLYGON_OFFSET_BIAS, std::bit_cast<u32>(-0.25f));

	f32 bias = 0.0f;
	f32 slope_scale = 0.0f;
	mtl::capture_depth_bias(mtl::capture_raster_state(), bias, slope_scale);

	EXPECT_FLOAT_EQ(bias, -0.25f);
}

MTL_DIAL_TEST(raster, NV4097_SET_CLIP_MIN)
{
	// The viewport depth range is clamped to [0,1], so an extended range is the
	// vertex shader's (LLD-raster invariant 4).
	set_register(NV4097_SET_CLIP_MIN, std::bit_cast<u32>(0.25f));
	set_register(NV4097_SET_CLIP_MAX, std::bit_cast<u32>(0.75f));

	f64 znear = 0.0;
	f64 zfar = 0.0;
	EXPECT_TRUE(mtl::capture_clip_range(mtl::capture_raster_state(), znear, zfar));
	EXPECT_DOUBLE_EQ(znear, 0.25);
	EXPECT_DOUBLE_EQ(zfar, 0.75);
}

MTL_DIAL_TEST(raster, NV4097_SET_CLIP_MAX)
{
	set_register(NV4097_SET_CLIP_MIN, std::bit_cast<u32>(-0.5f));
	set_register(NV4097_SET_CLIP_MAX, std::bit_cast<u32>(0.5f));

	f64 znear = 0.0;
	f64 zfar = 0.0;
	EXPECT_FALSE(mtl::capture_clip_range(mtl::capture_raster_state(), znear, zfar));
}

MTL_DIAL_TEST(raster, NV4097_SET_POINT_SIZE)
{
	set_register(NV4097_SET_POINT_SIZE, std::bit_cast<u32>(2.5f));

	EXPECT_FLOAT_EQ(mtl::capture_raster_state().point_size, 2.5f);
}

MTL_DIAL_TEST(raster, NV4097_SET_LINE_WIDTH)
{
	// The RSX width is fixed point: 8 is one pixel (rsx_decode.h:3261).
	set_register(NV4097_SET_LINE_WIDTH, 8);

	EXPECT_FLOAT_EQ(mtl::capture_raster_state().line_width, 1.0f);
}

MTL_DIAL_TEST(raster, NV4097_SET_LINE_SMOOTH_ENABLE)
{
	set_register(NV4097_SET_LINE_SMOOTH_ENABLE, 1);

	EXPECT_TRUE(mtl::capture_raster_state().line_smooth);
}

MTL_DIAL_TEST(raster, NV4097_SET_USER_CLIP_PLANE_CONTROL)
{
	set_register(NV4097_SET_USER_CLIP_PLANE_CONTROL, 0x0000'0033);

	EXPECT_EQ(mtl::capture_raster_state().user_clip_control, 0x0000'0033u);
}

MTL_DIAL_TEST(raster, NV4097_SET_POLYGON_STIPPLE)
{
	set_register(NV4097_SET_POLYGON_STIPPLE, 1);

	EXPECT_TRUE(mtl::capture_raster_state().stipple_enabled);
}

MTL_DIAL_TEST(raster, NV4097_SET_POLYGON_STIPPLE_PATTERN)
{
	// The latch copies the 32-word pattern instead of holding a guest pointer into
	// register space, so a consumer that holds the latch across a register write
	// cannot read retired data (E13).  The row asserts the copy.
	set_register(NV4097_SET_POLYGON_STIPPLE_PATTERN, 0x0000'0001);
	set_register(NV4097_SET_POLYGON_STIPPLE_PATTERN + 1, 0xffff'0000);

	const auto pattern = mtl::capture_raster_state().stipple_pattern;
	EXPECT_EQ(pattern[0], 0x0000'0001u);
	EXPECT_EQ(pattern[1], 0xffff'0000u);

	// A later write to the register file must not change a latch already taken.
	set_register(NV4097_SET_POLYGON_STIPPLE_PATTERN, 0x0000'0002);
	EXPECT_EQ(pattern[0], 0x0000'0001u);
}

MTL_DIAL_TEST(raster, NV4097_SET_POINT_SPRITE_CONTROL)
{
	set_register(NV4097_SET_POINT_SPRITE_CONTROL, 1);

	EXPECT_TRUE(mtl::capture_raster_state().point_sprite_enabled);
}

MTL_DIAL_TEST(blend, NV4097_SET_BLEND_ENABLE)
{
	set_register(NV4097_SET_BLEND_ENABLE, 1);

	EXPECT_EQ(mtl::capture_blend_state().enable_mask, 1u);
	EXPECT_TRUE(mtl::capture_blend_state().blend_enabled(0));
}

MTL_DIAL_TEST(blend, NV4097_SET_BLEND_ENABLE_MRT)
{
	// The MRT register holds the per-target bits; the base register holds target 0
	// (rsx_methods.h:493-497).
	set_register(NV4097_SET_BLEND_ENABLE, 0);
	set_register(NV4097_SET_BLEND_ENABLE_MRT, 0b1010);

	const mtl::blend_state state = mtl::capture_blend_state();
	EXPECT_EQ(state.enable_mask, 0b1010u);
	EXPECT_FALSE(state.blend_enabled(0));
	EXPECT_TRUE(state.blend_enabled(1));
}

MTL_DIAL_TEST(blend, NV4097_SET_BLEND_FUNC_SFACTOR)
{
	set_register(NV4097_SET_BLEND_FUNC_SFACTOR, static_cast<u32>(rsx::blend_factor::src_alpha));

	const mtl::blend_state state = mtl::capture_blend_state();
	EXPECT_EQ(state.source_rgb, rsx::blend_factor::src_alpha);
	EXPECT_EQ(mtl::get_blend_factor(state.source_rgb), MTL::BlendFactorSourceAlpha);
}

MTL_DIAL_TEST(blend, NV4097_SET_BLEND_FUNC_DFACTOR)
{
	set_register(NV4097_SET_BLEND_FUNC_DFACTOR, static_cast<u32>(rsx::blend_factor::one_minus_src_alpha));

	EXPECT_EQ(mtl::capture_blend_state().destination_rgb, rsx::blend_factor::one_minus_src_alpha);
}

MTL_DIAL_TEST(blend, NV4097_SET_BLEND_EQUATION)
{
	// The word holds the RGB equation in the low half and the alpha equation in
	// the high half (rsx_decode.h:2898), and both halves are decoded as enums.
	const u32 equation = static_cast<u32>(rsx::blend_equation::reverse_subtract);
	set_register(NV4097_SET_BLEND_EQUATION, equation | (equation << 16));

	EXPECT_EQ(mtl::capture_blend_state().equation_rgb, rsx::blend_equation::reverse_subtract);
}

MTL_DIAL_TEST(blend, NV4097_SET_ALPHA_TEST_ENABLE)
{
	set_register(NV4097_SET_ALPHA_TEST_ENABLE, 1);

	EXPECT_TRUE(mtl::capture_blend_state().alpha_test_enabled);
}

MTL_DIAL_TEST(blend, NV4097_SET_ALPHA_FUNC)
{
	set_register(NV4097_SET_ALPHA_FUNC, static_cast<u32>(rsx::comparison_function::greater));

	EXPECT_EQ(mtl::capture_blend_state().alpha_func, rsx::comparison_function::greater);
}

MTL_DIAL_TEST(blend, NV4097_SET_ALPHA_REF)
{
	// The default surface format is 8-bit, so the reference is a unorm8
	// (rsx_methods.h:749-762).
	set_register(NV4097_SET_ALPHA_REF, 0x80);

	EXPECT_FLOAT_EQ(mtl::capture_blend_state().alpha_ref, 128.0f / 255.0f);
}

MTL_DIAL_TEST(blend, NV4097_SET_LOGIC_OP_ENABLE)
{
	set_register(NV4097_SET_LOGIC_OP_ENABLE, 1);

	EXPECT_TRUE(mtl::capture_blend_state().logic_op_enabled);
}

MTL_DIAL_TEST(blend, NV4097_SET_LOGIC_OP)
{
	set_register(NV4097_SET_LOGIC_OP, static_cast<u32>(rsx::logic_op::logic_xor));

	EXPECT_EQ(mtl::capture_blend_state().logic_operation, rsx::logic_op::logic_xor);
}

MTL_DIAL_TEST(blend, NV4097_SET_SHADE_MODE)
{
	set_register(NV4097_SET_SHADE_MODE, static_cast<u32>(rsx::shading_mode::flat));

	EXPECT_EQ(mtl::capture_blend_state().shade_mode, rsx::shading_mode::flat);
}

MTL_DIAL_TEST(draw, NV4097_SET_VERTEX_DATA_BASE_OFFSET)
{
	set_register(NV4097_SET_VERTEX_DATA_BASE_OFFSET, 0x1234'0000);

	EXPECT_EQ(mtl::capture_draw_state().base_offset, 0x1234'0000u);
}

MTL_DIAL_TEST(draw, NV4097_SET_VERTEX_DATA_BASE_INDEX)
{
	set_register(NV4097_SET_VERTEX_DATA_BASE_INDEX, 0x0001'0000);

	EXPECT_EQ(mtl::capture_draw_state().base_index, 0x0001'0000u);
}

MTL_DIAL_TEST(draw, NV4097_SET_INDEX_ARRAY_ADDRESS)
{
	set_register(NV4097_SET_INDEX_ARRAY_ADDRESS, 0x00ab'cdef);

	EXPECT_EQ(mtl::capture_draw_state().index_address, 0x00ab'cdefu);
}

MTL_DIAL_TEST(draw, NV4097_SET_INDEX_ARRAY_DMA)
{
	// One register carries the location (b0-3) and the index type (b4-11)
	// (rsx_decode.h:3437-3450).
	set_register(NV4097_SET_INDEX_ARRAY_DMA,
		(static_cast<u32>(rsx::index_array_type::u32) << 4) | 3);

	const mtl::draw_state state = mtl::capture_draw_state();
	EXPECT_EQ(state.index_location, 3u);
	EXPECT_EQ(state.index_type, rsx::index_array_type::u32);
	EXPECT_EQ(mtl::get_index_type(state.index_type), MTL::IndexTypeUInt32);
}

MTL_DIAL_TEST(draw, NV4097_SET_RESTART_INDEX)
{
	set_register(NV4097_SET_RESTART_INDEX, 0xffff'ffff);

	EXPECT_EQ(mtl::capture_draw_state().restart_index, 0xffff'ffffu);
}

MTL_DIAL_TEST(draw, NV4097_SET_RESTART_INDEX_ENABLE)
{
	// The enable is derived: a 16-bit index array cannot carry a restart index
	// above 0xffff (rsx_methods.h:311-326).
	set_register(NV4097_SET_INDEX_ARRAY_DMA, static_cast<u32>(rsx::index_array_type::u16) << 4);
	set_register(NV4097_SET_RESTART_INDEX, 0x0001'0000);

	EXPECT_FALSE(mtl::capture_draw_state().restart_enabled);

	set_register(NV4097_SET_RESTART_INDEX_ENABLE, 1);
	EXPECT_FALSE(mtl::capture_draw_state().restart_enabled);

	set_register(NV4097_SET_RESTART_INDEX, 0x00ff);
	EXPECT_TRUE(mtl::capture_draw_state().restart_enabled);
}

// =============================================================================
// The exhaustive layer - every dial of the inventory, not just the latched ones
// =============================================================================

TEST(MTLDialRegisters, EveryCuratedRegisterMatchesTheHeader)
{
	std::map<std::string, u32> expected;
	for (const auto& row : spine())
	{
		if (row.size() > spine_word)
		{
			expected.emplace(row[spine_id], to_u32(row[spine_word]));
		}
	}

	ASSERT_EQ(expected.size(), 282u);

	std::size_t checked = 0;
#define X(id, symbol, word)                                                    \
	{                                                                          \
		++checked;                                                             \
		const auto found = expected.find(#id);                                 \
		ASSERT_NE(found, expected.end()) << #id;                               \
		EXPECT_EQ(static_cast<u32>(symbol), found->second) << #id;             \
	}
	RSX_METAL4_REGISTERS(X)
#undef X

	EXPECT_EQ(checked, expected.size());
}

TEST(MTLDialValues, EveryValueConstantMatchesTheHeader)
{
	std::size_t checked = 0;
#define X(symbol, name, value)                                                 \
	{                                                                          \
		++checked;                                                             \
		EXPECT_EQ(static_cast<u32>(symbol), static_cast<u32>(value)) << name;   \
	}
	RSX_METAL4_VALUES(X)
#undef X

	EXPECT_EQ(checked, 768u);
}

TEST(MTLDialValues, ListCoversTheInventory)
{
	const auto rows = read_csv("rsx_value_space.csv");
	ASSERT_EQ(rows.size(), 768u);

	std::set<std::string> listed;
#define X(symbol, name, value) listed.insert(name);
	RSX_METAL4_VALUES(X)
#undef X

	for (const auto& row : rows)
	{
		EXPECT_EQ(listed.count(row[1]), 1u) << row[1];
	}

	EXPECT_EQ(listed.size(), 768u);
}

TEST(MTLDialFacets, EveryShaderEnumMatchesTheHeader)
{
	std::size_t checked = 0;
#define X(symbol, name, value)                                                 \
	{                                                                          \
		++checked;                                                             \
		EXPECT_EQ(static_cast<u32>(symbol), static_cast<u32>(value)) << name;   \
	}
	RSX_METAL4_FACET_ENUMS(X)
#undef X

	EXPECT_EQ(checked, 58u);
}

TEST(MTLDialFacets, EveryFragmentOpcodeNameMatchesTheTable)
{
	std::size_t checked = 0;
#define X(index, text)                                                         \
	{                                                                          \
		++checked;                                                             \
		EXPECT_EQ(rsx_fp_op_names[index], text);                                \
	}
	RSX_METAL4_FACET_OPCODES(X)
#undef X

	EXPECT_EQ(checked, 70u);
}

TEST(MTLDialFacets, EveryShaderBitfieldHasTheRecordedPlace)
{
	// The width is read back from an all-ones word, and the shift from a word that
	// holds only the field at its maximum.  Together they pin both.
	std::size_t checked = 0;
#define X(owner, member, shift, width)                                                        \
	{                                                                                         \
		++checked;                                                                            \
		owner probe{};                                                                        \
		probe.HEX = ~0u;                                                                      \
		EXPECT_EQ(static_cast<u32>(probe.member), (1u << (width)) - 1u) << #owner "::" #member " width"; \
		probe.HEX = 0;                                                                        \
		probe.member = (1u << (width)) - 1u;                                                  \
		EXPECT_EQ(probe.HEX, ((1u << (width)) - 1u) << (shift)) << #owner "::" #member " shift"; \
	}
	RSX_METAL4_FACET_BITFIELDS(X)
#undef X

	EXPECT_EQ(checked, 129u);
}

TEST(MTLDialFacets, EveryShaderFieldIsARealMember)
{
	std::size_t checked = 0;
#define X(owner, member)                                                                      \
	{                                                                                         \
		++checked;                                                                            \
		static_assert(std::is_member_object_pointer_v<decltype(&owner::member)>);             \
	}
	RSX_METAL4_FACET_FIELDS(X)
#undef X

	EXPECT_EQ(checked, 18u);
}

TEST(MTLDialFacets, ListsCoverTheInventory)
{
	const auto rows = read_csv("rsx_shader_facets.csv");
	ASSERT_EQ(rows.size(), 275u);

	std::set<std::string> listed;
#define X(symbol, name, value) listed.insert(name);
	RSX_METAL4_FACET_ENUMS(X)
#undef X
#define X(index, text) listed.insert("rsx_fp_op_names[" #index "]");
	RSX_METAL4_FACET_OPCODES(X)
#undef X
#define X(owner, member, shift, width) listed.insert(#owner "::" #member);
	RSX_METAL4_FACET_BITFIELDS(X)
#undef X
#define X(owner, member) listed.insert(#owner "::" #member);
	RSX_METAL4_FACET_FIELDS(X)
#undef X

	for (const auto& row : rows)
	{
		EXPECT_EQ(listed.count(row[2]), 1u) << row[2];
	}

	EXPECT_EQ(listed.size(), 275u);
}

TEST(MTLDialBindings, EverySlotMatchesTheRsxAbi)
{
	// The Metal index map renames the slots but never renumbers them, so each Metal
	// index is the RSX ABI number (MTLVertexState.h, MTLArgumentBinds.h).
	struct slot
	{
		const char* name;
		u32 abi;
		u32 metal;
	};

	const slot map[] = {
		{ "vertex_params_bind_slot",             0, mtl::vertex_context_slot },
		{ "vertex_constant_buffers_bind_slot",   1, mtl::vertex_constants_slot },
		{ "fragment_constant_buffers_bind_slot", 2, mtl::fragment_constants_slot },
		{ "fragment_state_bind_slot",            3, mtl::fragment_state_slot },
		{ "fragment_texture_params_bind_slot",   4, mtl::fragment_texture_params_slot },
		{ "vertex_buffers_first_bind_slot",      5, mtl::vertex_buffers_slot },
		{ "conditional_render_predicate_slot",   8, mtl::conditional_render_predicate_slot },
		{ "rasterizer_env_bind_slot",            9, mtl::fragment_stipple_slot },
		{ "instancing_lookup_table_bind_slot",  10, mtl::instancing_lut_slot },
		{ "instancing_constants_buffer_slot",   11, mtl::instancing_constants_slot },
		{ "textures_first_bind_slot",           12, mtl::texture_slot_base },
		{ "vertex_textures_first_bind_slot",    12, mtl::vertex_textures_slot },
	};

	const auto rows = read_csv("rsx_binding_slots.csv");
	ASSERT_EQ(rows.size(), 12u);

	for (const auto& entry : map)
	{
		EXPECT_EQ(entry.abi, entry.metal) << entry.name;

		const auto row = std::find_if(rows.begin(), rows.end(),
			[&entry](const csv_row& candidate)
			{
				return candidate.size() > 1 && candidate[0] == entry.name;
			});

		ASSERT_NE(row, rows.end()) << entry.name;
		EXPECT_EQ(to_u32((*row)[1]), entry.abi) << entry.name;
	}
}

TEST(MTLDialBindings, TheMslIndexCapsHoldTheAbiMap)
{
	// MSL caps the shader-side index of each space: a [[buffer(n)]] index at 30, a
	// [[sampler(n)]] index at 15 and a [[texture(n)]] index at 127
	// (REVIEW-FINDINGS.md A9).  A declaration above its cap makes the library fail
	// to build, so every slot the map hands to a shader must stay inside its cap.
	// The header asserts this at compile time; this row restates the numbers so a
	// future edit fails a test instead of a shader library.
	constexpr u32 buffer_cap = 30;
	constexpr u32 sampler_cap = 15;
	constexpr u32 texture_cap = 127;

	// Every buffer slot of either stage, plus the fill's own slot.
	const u32 buffers[] = {
		mtl::vertex_context_slot,
		mtl::vertex_constants_slot,
		mtl::vertex_buffers_slot,
		mtl::vertex_volatile_slot,
		mtl::vertex_draw_parameters_slot,
		mtl::conditional_render_predicate_slot,
		mtl::instancing_lut_slot,
		mtl::instancing_constants_slot,
		mtl::vertex_draw_params_slot,
		mtl::fragment_constants_slot,
		mtl::fragment_state_slot,
		mtl::fragment_texture_params_slot,
		mtl::fragment_stipple_slot,
		mtl::fragment_blend_slot,
		mtl::fill_values_slot,
	};

	for (const u32 slot : buffers)
	{
		EXPECT_LE(slot, buffer_cap);
	}

	// Every texture slot of either stage.
	const u32 textures[] = {
		mtl::texture_slot_base,
		mtl::texture_slot_base + mtl::fragment_texture_count - 1,
		mtl::vertex_texture_slot(0),
		mtl::vertex_texture_slot(mtl::vertex_texture_count - 1),
		mtl::fragment_stencil_slot_base,
		mtl::fragment_stencil_slot_base + mtl::fragment_texture_count - 1,
		mtl::fragment_depth_input_slot,
	};

	for (const u32 slot : textures)
	{
		EXPECT_LE(slot, texture_cap);
	}

	// MSL has no combined image sampler, so a texture unit takes its position in
	// the stage's texture range as the sampler index instead of its texture index
	// (MTLShader.cpp remaps, MTLArgumentBinds.cpp writes).  The two must agree, the
	// index must stay inside the sampler cap, and no two units may share one index:
	// MSL rejects a duplicate sampler index outright.
	std::set<u32> unit_samplers;

	for (u32 index = 0; index < mtl::fragment_texture_count; ++index)
	{
		const u32 texture = mtl::texture_slot_base + index;
		EXPECT_EQ(mtl::texture_sampler_index(texture), index);
		EXPECT_LE(mtl::texture_sampler_index(texture), sampler_cap);
		EXPECT_TRUE(unit_samplers.insert(mtl::texture_sampler_index(texture)).second);
	}

	for (u32 index = 0; index < mtl::vertex_texture_count; ++index)
	{
		const u32 texture = mtl::vertex_texture_slot(index);
		EXPECT_EQ(mtl::texture_sampler_index(texture), index);
		EXPECT_LE(mtl::texture_sampler_index(texture), sampler_cap);
	}

	EXPECT_EQ(unit_samplers.size(), static_cast<std::size_t>(mtl::fragment_texture_count));

	// The argument table must be at least as wide as the highest index it receives.
	EXPECT_GT(mtl::argument_buffer_count, mtl::fill_values_slot);
	EXPECT_GT(mtl::argument_texture_count, mtl::fragment_depth_input_slot);

	// The table's sampler space has a cap of its own, the same value as the MSL
	// declaration cap (REVIEW-FINDINGS.md A9): the argument table descriptor must not
	// ask for more than 16 sampler entries.  It must still be wide enough for every
	// sampler a stage uses, which is one per texture unit.
	constexpr u32 table_sampler_cap = 16;
	EXPECT_LE(mtl::argument_sampler_count, table_sampler_cap);
	EXPECT_GE(mtl::argument_sampler_count, mtl::fragment_texture_count);
	EXPECT_GE(mtl::argument_sampler_count, mtl::vertex_texture_count);

	// Only a texture unit carries a sampler; the depth-as-texture inputs take one
	// index each with no sampler.  That rule is what keeps every sampler the writer
	// binds inside the table's sampler space: the validation layer aborts on a bind
	// past the declared count, so a slot that wrongly claimed a sampler would abort
	// the process on the validation layer rather than fail a draw.
	EXPECT_TRUE(mtl::slot_has_sampler(mtl::texture_slot_base));
	EXPECT_TRUE(mtl::slot_has_sampler(mtl::texture_slot_base + mtl::fragment_texture_count - 1));
	EXPECT_FALSE(mtl::slot_has_sampler(mtl::fragment_stencil_slot_base));
	EXPECT_FALSE(mtl::slot_has_sampler(mtl::fragment_stencil_slot_base + mtl::fragment_texture_count - 1));
	EXPECT_FALSE(mtl::slot_has_sampler(mtl::fragment_depth_input_slot));
}

TEST(MTLDialInventory, TheCountsAreExhaustive)
{
	std::size_t registers = 0;
	std::size_t values = 0;
	std::size_t facet_enums = 0;
	std::size_t facet_opcodes = 0;
	std::size_t facet_bitfields = 0;
	std::size_t facet_fields = 0;

#define X(id, symbol, word) ++registers;
	RSX_METAL4_REGISTERS(X)
#undef X
#define X(symbol, name, value) ++values;
	RSX_METAL4_VALUES(X)
#undef X
#define X(symbol, name, value) ++facet_enums;
	RSX_METAL4_FACET_ENUMS(X)
#undef X
#define X(index, text) ++facet_opcodes;
	RSX_METAL4_FACET_OPCODES(X)
#undef X
#define X(owner, member, shift, width) ++facet_bitfields;
	RSX_METAL4_FACET_BITFIELDS(X)
#undef X
#define X(owner, member) ++facet_fields;
	RSX_METAL4_FACET_FIELDS(X)
#undef X

	const std::size_t total = registers + values + facet_enums + facet_opcodes + facet_bitfields + facet_fields + 12u;
	std::cout << "  dials exercised: " << total << " (" << registers << " registers, "
		<< values << " values, " << facet_enums + facet_opcodes + facet_bitfields + facet_fields
		<< " shader facets, 12 binding slots)\n";

	EXPECT_EQ(registers, 282u);
	EXPECT_EQ(values, 768u);
	EXPECT_EQ(facet_enums, 58u);
	EXPECT_EQ(facet_opcodes, 70u);
	EXPECT_EQ(facet_bitfields, 129u);
	EXPECT_EQ(facet_fields, 18u);
	EXPECT_EQ(total, 1337u);
}

// =============================================================================
// The coverage ratchet.  A floor never falls.
// =============================================================================

TEST(MTLDialRatchet, depth_rop) { check_ratchet("depth-rop", 17); }
TEST(MTLDialRatchet, raster)    { check_ratchet("raster", 17); }
TEST(MTLDialRatchet, blend)     { check_ratchet("blend", 11); }
TEST(MTLDialRatchet, draw)      { check_ratchet("draw", 6); }

TEST(MTLDialRatchet, surface)       { check_ratchet("surface", 0); }
TEST(MTLDialRatchet, vertex)        { check_ratchet("vertex", 0); }
TEST(MTLDialRatchet, texture)       { check_ratchet("texture", 0); }
TEST(MTLDialRatchet, sync_flip)     { check_ratchet("sync-flip", 0); }
TEST(MTLDialRatchet, nv0039_dma)    { check_ratchet("nv0039-dma", 0); }
TEST(MTLDialRatchet, nv2d_surface)  { check_ratchet("nv2d-surface", 0); }
TEST(MTLDialRatchet, nv3089_image)  { check_ratchet("nv3089-image", 0); }
TEST(MTLDialRatchet, nv308a_image)  { check_ratchet("nv308a-image", 0); }
TEST(MTLDialRatchet, fixed_function){ check_ratchet("fixed-function", 0); }
TEST(MTLDialRatchet, misc)          { check_ratchet("misc", 0); }

TEST(MTLDialRatchet, Total)
{
	std::set<std::string> covered;
	for (const auto& [cluster, id] : dial_registration::covered())
	{
		covered.insert(id);
	}

	std::cout << "  dials covered: " << covered.size() << " of 203\n";
	EXPECT_GE(covered.size(), 51u) << "total coverage regressed";
}

#endif // RSX_ENABLE_METAL4
