#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include <array>
#include <string>
#include <vector>

namespace mtl
{
	// The MSL entry point SPIRV-Cross emits for an RSX program.  The GLSL wrapper
	// names its entry `main`, and SPIRV-Cross renames it to avoid the MSL `main`.
	constexpr const char* msl_entry_point = "main0";

	// SPIR-V -> MSL (HLD-05 §2.3 route A).  `is_vertex` selects the stage.  The
	// fixed RSX slot map (HLD-05 §3.1-§3.2) is applied with
	// add_msl_resource_binding, so the emitted [[buffer(N)]]/[[texture(N)]]/
	// [[sampler(N)]] indices are the argument-table indices.  When
	// `color_output_mask` is not null it receives one bit per [[color(i)]] the
	// stage writes.  Returns false and leaves `msl` untouched on a translation
	// failure.
	bool spv_to_msl(const std::vector<u32>& spv, bool is_vertex, std::string& msl, u32* color_output_mask = nullptr);

	// MSL -> MTL::Library (HLD-05 §3.4).  One MTL4::Compiler per device
	// (workaround W11).  Null on a compile error; `error` carries the diagnostic.
	MTL::Library* compile_msl(MTL4::Compiler* compiler, const std::string& msl, NS::Error** error);

	// --- The three MSL post-passes (HLD-05 §2.4) ---
	// Pure text transforms, so they are unit-testable without a GPU.  Each takes
	// the whole MSL source and returns the rewritten source.

	// (1) Programmable blending.  SPIRV-Cross emits an input attachment as
	// `texture2d<float> NAME [[texture(N)]]` and reads it with
	// `NAME.read(uint2(gl_FragCoord.xy))`.  The tile framebuffer fetch is a
	// `[[color(color_index)]]` fragment input instead (HLD-05 §2.4, §3.2).
	std::string rewrite_programmable_blend(std::string msl, u32 color_index);

	// (2) Push-constant substitution.  SPIRV-Cross already emits a push-constant
	// block as `constant BLOCK& NAME [[buffer(N)]]`.  MSL has no push constants,
	// so the RSX substituted slot (HLD-05 §3.3, §3.2 †) must be the buffer index.
	// The caller passes the GLSL block name; an empty name is a no-op.
	std::string rewrite_push_constant_slot(std::string msl, const std::string& block_name, u32 slot);

	// (3) Border-colour / LOD-bias injection (04 R18/R19).  Neither is
	// expressible in MSL sampler state, so a helper is injected and the named
	// texture's two-argument sample call is routed through it.
	struct sampler_injection
	{
		std::string texture;          // the MSL texture variable
		std::string sampler;          // its MSL sampler variable
		f32 lod_bias = 0.0f;          // RSX mip LOD bias (04 R19)
		bool border_enabled = false;  // RSX border colour (04 R18)
		std::array<f32, 4> border{};  // the border RGBA
	};
	std::string inject_sampler_border_and_lod(std::string msl, const sampler_injection& spec);
}

#endif // RSX_ENABLE_METAL4
