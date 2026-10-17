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
	std::string decompile_fragment_glsl(const RSXFragmentProgram& prog, std::vector<u32>* constant_offsets = nullptr);

	// The subclass the base-class contract names (FragmentProgramDecompiler.h:8-21).
	// It emits GLSL text only; the SPIR-V, MSL and library steps stay in
	// MTLProgramPipeline.
	struct MTLFragmentDecompilerThread : public FragmentProgramDecompiler
	{
		MTLFragmentDecompilerThread(const RSXFragmentProgram& prog, u32& size, std::string& shader);

		void Task();

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
