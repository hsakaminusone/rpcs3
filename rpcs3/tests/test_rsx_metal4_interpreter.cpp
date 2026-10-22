#include "stdafx.h"

#include "Emu/RSX/MTL/MTLAvailability.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/MTL/MTLShader.h"
#include "Emu/RSX/MTL/MTLShaderInterpreter.h"
#include "Emu/RSX/Program/GLSLCommon.h"
#include "Emu/RSX/Program/ShaderInterpreter.h"
#include "Emu/RSX/Program/SPIRVCommon.h"

#include <gtest/gtest.h>

namespace
{
	std::string compile_to_msl(const std::string& glsl, bool is_vertex)
	{
		static const bool initialized = []
		{
			spirv::initialize_compiler_context();
			return true;
		}();
		(void)initialized;

		std::vector<u32> spv;
		std::string source = glsl;

		EXPECT_TRUE(spirv::compile_glsl_to_spv(spv, source,
			is_vertex ? glsl::glsl_vertex_program : glsl::glsl_fragment_program, glsl::glsl_rules_vulkan));

		std::string msl;
		EXPECT_TRUE(mtl::spv_to_msl(spv, is_vertex, msl, nullptr, mtl::rsx_push_slot));
		return msl;
	}
}

TEST(MTLInterpreter, SingleTypeMappingCanonicalises)
{
	const mtl::interpreter_texture_mapping mapping = mtl::interpreter_texture_mapping::from_mask(0x0003, 0x5);

	// Two 2D units: the shader indexes a 16-element array by RSX unit, so the
	// mask and the dimensions must not enter the key.
	EXPECT_TRUE(mapping.is_single());
	EXPECT_EQ(mapping.single_type, 1u);
	EXPECT_EQ(mapping.total, 16u);
	EXPECT_EQ(mapping.referenced_mask, 0u);
	EXPECT_EQ(mapping.dimensions, 0u);
	EXPECT_EQ(mapping.array_size[1], 16u);
	EXPECT_EQ(mapping.unit_index[7], 7u);
}

TEST(MTLInterpreter, MixedTypeMappingPacksByDimension)
{
	// Units 0..3 are 1D, 2D, cube and 3D.
	const mtl::interpreter_texture_mapping mapping = mtl::interpreter_texture_mapping::from_mask(0x000F, 0xE4);

	EXPECT_FALSE(mapping.is_single());
	EXPECT_EQ(mapping.total, 4u);
	EXPECT_EQ(mapping.array_size[0], 1u);
	EXPECT_EQ(mapping.array_size[1], 1u);
	EXPECT_EQ(mapping.array_size[2], 1u);
	EXPECT_EQ(mapping.array_size[3], 1u);
	EXPECT_EQ(mapping.array_base[0], 0u);
	EXPECT_EQ(mapping.array_base[1], 1u);
	EXPECT_EQ(mapping.array_base[2], 2u);
	EXPECT_EQ(mapping.array_base[3], 3u);

	for (u32 unit = 0; unit < 4; ++unit)
	{
		EXPECT_EQ(mapping.global_index(unit), unit);
	}
}

TEST(MTLInterpreter, VertexSourceCompiles)
{
	const std::string msl = compile_to_msl(mtl::interpreter_cache::build_vertex_source(0), true);
	EXPECT_NE(msl.find("main0"), std::string::npos);
}

TEST(MTLInterpreter, SingleTypeFragmentSourceCompiles)
{
	mtl::interpreter_fragment_key key;
	key.options = program_common::interpreter::COMPILER_OPT_ENABLE_TEXTURES |
		program_common::interpreter::COMPILER_OPT_ENABLE_F32_EXPORT |
		program_common::interpreter::COMPILER_OPT_ENABLE_DEPTH_EXPORT |
		program_common::interpreter::COMPILER_OPT_ENABLE_FLOW_CTRL |
		program_common::interpreter::COMPILER_OPT_ENABLE_PACKING |
		program_common::interpreter::COMPILER_OPT_ENABLE_KIL |
		program_common::interpreter::COMPILER_OPT_ENABLE_STIPPLING |
		program_common::interpreter::COMPILER_OPT_ENABLE_ROP_REMAP;
	key.output_count = 4;
	key.mapping = mtl::interpreter_texture_mapping::from_mask(0x0003, 0x5);

	const std::string msl = compile_to_msl(mtl::interpreter_cache::build_fragment_source(key), false);

	// SPIRV-Cross splits a combined image sampler into a texture array and a
	// sampler array (REVIEW-FINDINGS.md A9); the canonical single-dimension
	// program indexes both by RSX unit over all 16 slots.
	EXPECT_NE(msl.find("array<texture2d<float>, 16> sampler2D_array"), std::string::npos);
	EXPECT_NE(msl.find("array<sampler, 16> sampler2D_arraySmplr"), std::string::npos);
}

TEST(MTLInterpreter, MixedTypeFragmentSourceCompiles)
{
	mtl::interpreter_fragment_key key;
	key.options = program_common::interpreter::COMPILER_OPT_ENABLE_TEXTURES |
		program_common::interpreter::COMPILER_OPT_ENABLE_F32_EXPORT |
		program_common::interpreter::COMPILER_OPT_ENABLE_DEPTH_EXPORT |
		program_common::interpreter::COMPILER_OPT_ENABLE_FLOW_CTRL |
		program_common::interpreter::COMPILER_OPT_ENABLE_PACKING |
		program_common::interpreter::COMPILER_OPT_ENABLE_KIL |
		program_common::interpreter::COMPILER_OPT_ENABLE_STIPPLING |
		program_common::interpreter::COMPILER_OPT_ENABLE_ROP_REMAP;
	key.output_count = 4;
	key.mapping = mtl::interpreter_texture_mapping::from_mask(0x000F, 0xE4);

	const std::string msl = compile_to_msl(mtl::interpreter_cache::build_fragment_source(key), false);

	// One compact array per referenced dimension, sized to the referenced units.
	EXPECT_NE(msl.find("array<texture1d<float>, 1> sampler1D_array"), std::string::npos);
	EXPECT_NE(msl.find("array<texture2d<float>, 1> sampler2D_array"), std::string::npos);
	EXPECT_NE(msl.find("array<texturecube<float>, 1> samplerCube_array"), std::string::npos);
	EXPECT_NE(msl.find("array<texture3d<float>, 1> sampler3D_array"), std::string::npos);
}

#endif // RSX_ENABLE_METAL4
