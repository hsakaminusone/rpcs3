#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include <string>
#include <vector>

namespace mtl
{
	// The MSL entry point SPIRV-Cross emits for an RSX program.  The GLSL wrapper
	// names its entry `main`, and SPIRV-Cross renames it to avoid the MSL `main`.
	constexpr const char* msl_entry_point = "main0";

	// The sentinel that selects the RSX substituted push-constant slots.  A pipeline
	// that is not an RSX draw (an overlay or calibration pass, HLD-06 §2.6) passes
	// its own MSL buffer index instead, because the RSX slots carry RSX state that
	// the pass does not have.
	constexpr u32 rsx_push_slot = ~0u;

	// SPIR-V -> MSL (HLD-05 §2.3 route A).  `is_vertex` selects the stage.  The
	// fixed RSX slot map (HLD-05 §3.1-§3.2) is applied with
	// add_msl_resource_binding, so the emitted [[buffer(N)]]/[[texture(N)]]/
	// [[sampler(N)]] indices are the argument-table indices.  When
	// `color_output_mask` is not null it receives one bit per [[color(i)]] the
	// stage writes.  Returns false and leaves `msl` untouched on a translation
	// failure.
	bool spv_to_msl(const std::vector<u32>& spv, bool is_vertex, std::string& msl, u32* color_output_mask = nullptr, u32 push_slot = rsx_push_slot);

	// MSL -> MTL::Library (HLD-05 §3.4).  One MTL4::Compiler per device
	// (workaround W11).  Null on a compile error; `error` carries the diagnostic.
	MTL::Library* compile_msl(MTL4::Compiler* compiler, const std::string& msl, NS::Error** error);

	// The completion form used by the async pipeline registry.  Returns the task
	// that owns the in-flight compile; the handler runs on a Metal compiler thread
	// and must retain anything it stores.  Null when the submission itself failed.
	MTL4::CompilerTask* compile_msl_async(MTL4::Compiler* compiler, const std::string& msl,
		MTL::NewLibraryCompletionHandlerFunction handler);
}

#endif // RSX_ENABLE_METAL4
