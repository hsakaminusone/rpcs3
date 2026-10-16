#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLPipeline.h"

#include "../Program/ProgramStateCache.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mtl
{
	// The guest ucode -> GLSL step.  The Metal decompiler subclass lands with the
	// shader front-end unit (mirrors VKVertexProgram; HLD-05 §2.1); the traits
	// call this seam so the SPIR-V -> MSL step, the library and the pipeline can
	// land first.  These are the only hop not implemented by this unit.
	std::string decompile_vertex_program(const RSXVertexProgram& program);
	std::string decompile_fragment_program(const RSXFragmentProgram& program);

	// One compiled guest shader: the SPIR-V, the MSL and the MTL::Library that
	// owns the entry.  The library is per program id, not per pipeline, because
	// SPIRV-Cross is not free (HLD-05 §3.6).  The library is created lazily, when
	// the MTL4::Compiler is first available.
	struct shader_program
	{
		u32 id = 0;
		std::vector<u32> spv;
		std::string msl;
		u32 color_output_mask = 0; // bit i set when the stage writes [[color(i)]]

		shader_program() = default;
		~shader_program();
		shader_program(const shader_program&) = delete;
		shader_program& operator=(const shader_program&) = delete;
		shader_program(shader_program&& other) noexcept;
		shader_program& operator=(shader_program&& other) noexcept;

		MTL::Library* library() const { return m_library; }

		// Create the library once.  One MTL4::Compiler per device (workaround
		// W11).  Returns false when there is no MSL or the compile fails.
		bool ensure_library(MTL4::Compiler* compiler) const;

	private:
		// Mutable: build_pipeline receives the program by const reference
		// (ProgramStateCache.h:438-444) and the library is created there.
		mutable MTL::Library* m_library = nullptr;
	};

	using vertex_program = shader_program;
	using fragment_program = shader_program;

	// The linked object: one MTL::RenderPipelineState.  The two shader_program
	// objects own their libraries, so the pipeline does not (HLD-05 §3.4).
	struct pipeline
	{
		MTL::RenderPipelineState* state = nullptr;

		pipeline() = default;
		~pipeline();
		pipeline(const pipeline&) = delete;
		pipeline& operator=(const pipeline&) = delete;
	};

	// The extra argument the shared cache forwards to build_pipeline.  The header
	// comment names it ExtraData (ProgramStateCache.h:153,157); the tree passes it
	// as the trailing Args... of get_graphics_pipeline (ProgramStateCache.h:438-444).
	struct shader_context
	{
		MTL4::Compiler* compiler = nullptr;
		std::string vertex_entry;
		std::string fragment_entry;
	};

	// The program_state_cache backend traits for Metal (HLD-01 §3.3; mirrors the
	// role of VKProgramBuffer.h:9, not its structure).
	struct MTLTraits
	{
		using vertex_program_type = vertex_program;
		using fragment_program_type = fragment_program;
		using pipeline_type = pipeline;
		using pipeline_storage_type = std::unique_ptr<pipeline_type>;
		using pipeline_properties = mtl::pipeline_properties;
		using ExtraData = shader_context;

		// RSX ucode -> GLSL -> glslang -> SPIR-V -> SPIRV-Cross -> MSL.  The
		// library is not built here: the device is not available to the cache.
		static void recompile_fragment_program(const RSXFragmentProgram& program, fragment_program_type& data, usz id);
		static void recompile_vertex_program(const RSXVertexProgram& program, vertex_program_type& data, usz id);

		// Mask the write mask by the fragment program's live colour outputs, so an
		// unwritten attachment cannot be written.
		static void validate_pipeline_properties(const vertex_program_type& vertex, const fragment_program_type& fragment, pipeline_properties& properties);

		// Assemble the MTL4::RenderPipelineDescriptor and link it.  The
		// `compile_async` flag and the callback match the shared cache contract
		// (ProgramStateCache.h:401-446).
		static pipeline_type* build_pipeline(
			const vertex_program_type& vertex,
			const fragment_program_type& fragment,
			const pipeline_properties& properties,
			bool compile_async,
			std::function<pipeline_type*(pipeline_storage_type&)> callback,
			const ExtraData& extra);
	};

	class program_cache : public program_state_cache<MTLTraits>
	{
	public:
		explicit program_cache(decompiler_callback_t callback)
		{
			notify_pipeline_compiled = callback;
		}
	};
}

#endif // RSX_ENABLE_METAL4
