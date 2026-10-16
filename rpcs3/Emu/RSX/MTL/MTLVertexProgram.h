#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

// The Program headers assume the precompiled header supplies these.
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

#include "Utilities/StrFmt.h"

#include "../Program/VertexProgramDecompiler.h"

namespace mtl
{
	// The varying-register location table (HLD-05 §3.2).  The vertex and fragment
	// stages must agree on the location of every varying, so the table is shared
	// and the fragment unit reuses it.
	int varying_location(std::string_view name);

	// The texture unit index encoded in an RSX texture variable name ("tex3").
	int texture_index(std::string_view name);

	// RSX vertex ucode -> Vulkan-rule GLSL 450 (HLD-05 §2.3 route A).  The seam
	// mtl::decompile_vertex_program (MTLProgramPipeline.cpp) calls this and feeds
	// the text to spirv::compile_glsl_to_spv.
	std::string decompile_vertex_glsl(const RSXVertexProgram& prog);

	// The subclass the base-class contract names (VertexProgramDecompiler.h:9-18).
	// It emits GLSL text only; the SPIR-V, MSL and library steps stay in
	// MTLProgramPipeline.
	struct MTLVertexDecompilerThread : public VertexProgramDecompiler
	{
		MTLVertexDecompilerThread(const RSXVertexProgram& prog, std::string& shader);

		void Task();

	protected:
		std::string getFloatTypeName(usz elementCount) override;
		std::string getIntTypeName(usz elementCount) override;
		std::string getFunction(FUNCTION f) override;
		std::string compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1, bool scalar) override;

		void insertHeader(std::stringstream& OS) override;
		void insertInputs(std::stringstream& OS, const std::vector<ParamType>& inputs) override;
		void insertConstants(std::stringstream& OS, const std::vector<ParamType>& constants) override;
		void insertOutputs(std::stringstream& OS, const std::vector<ParamType>& outputs) override;
		void insertMainStart(std::stringstream& OS) override;
		void insertMainEnd(std::stringstream& OS) override;

	private:
		void insertFSExport(std::stringstream& OS);

		std::string& m_shader;
	};
}

#endif // RSX_ENABLE_METAL4
