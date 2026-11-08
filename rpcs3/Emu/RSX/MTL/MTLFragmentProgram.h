#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

// The Program headers assume the precompiled header supplies these.
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

#include "Utilities/StrFmt.h"

#include "../Program/FragmentProgramDecompiler.h"
#include "../Program/GLSLTypes.h"

namespace mtl
{
	// RSX fragment ucode -> Vulkan-rule GLSL 450 (HLD-05 §2.3 route A).  The seam
	// mtl::decompile_fragment_program (MTLProgramPipeline.cpp) calls this and feeds
	// the text to spirv::compile_glsl_to_spv.
	//
	// `constant_offsets` is the literal relocation table the decompiler decoded
	// (FragmentProgramDecompiler.h:189).  The draw path needs it to fill the
	// fragment constant file (ProgramStateCache.h:141), and the decompiler is the
	// only thing that can produce it.
	// `id` is the compiled-program id the cache assigned, used only by the
	// TODO(excise) MTL_PROBE_VARIANT=force-fp diagnostic to name the one program whose
	// output is forced.
	std::string decompile_fragment_glsl(const RSXFragmentProgram& prog, std::vector<u32>* constant_offsets = nullptr, u32 id = 0);

	// The subclass the base-class contract names (FragmentProgramDecompiler.h:8-21).
	// It emits GLSL text only; the SPIR-V, MSL and library steps stay in
	// MTLProgramPipeline.
	struct MTLFragmentDecompilerThread : public FragmentProgramDecompiler
	{
		MTLFragmentDecompilerThread(const RSXFragmentProgram& prog, u32& size, std::string& shader, u32 id = 0);

		// TODO(excise): MTL_PROBE_VARIANT=force-fp=<id>[,<id>...] forces the colour output
		// of the named fragment programs, and clears their discard, so the write
		// stage of one draw is observed on its own.  Read once, when the program
		// is built, so the run needs a cold archive.
		bool m_force_output = false;

		void Task();

		// The shader-interpreter source shares the decompiler's declarations but
		// not its generated main (LLD-shader-interpreter.md §6).
		void emitInterpreterPrelude(std::stringstream& OS);
		void emitInterpreterGlobals(std::stringstream& OS);

	protected:
		std::string getFloatTypeName(usz elementCount) override;
		std::string getHalfTypeName(usz elementCount) override;
		std::string getFunction(FUNCTION f) override;
		std::string compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1) override;

		void insertHeader(std::stringstream& OS) override;
		void insertInputs(std::stringstream& OS) override;
		void insertOutputs(std::stringstream& OS) override;
		void insertConstants(std::stringstream& OS) override;
		void insertGlobalFunctions(std::stringstream& OS) override;
		void insertMainStart(std::stringstream& OS) override;
		void insertMainEnd(std::stringstream& OS) override;

	private:
		std::string& m_shader;
		glsl::shader_properties m_shader_props{};
	};
}

#endif // RSX_ENABLE_METAL4
