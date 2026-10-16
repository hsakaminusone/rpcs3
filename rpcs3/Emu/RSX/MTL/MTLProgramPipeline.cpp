#include "MTLProgramPipeline.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLShader.h"
#include "MTLFragmentProgram.h"
#include "MTLVertexProgram.h"

#include "../Program/GLSLTypes.h"
#include "../Program/SPIRVCommon.h"

#include "util/logs.hpp"

#include <utility>

namespace mtl
{
	// The RSX ucode -> GLSL hop.  The decompiler subclasses hold the GLSL dialect;
	// the SPIR-V, MSL and library hops stay in this unit (HLD-05 §2.1).
	std::string decompile_vertex_program(const RSXVertexProgram& program)
	{
		return decompile_vertex_glsl(program);
	}

	std::string decompile_fragment_program(const RSXFragmentProgram& program)
	{
		return decompile_fragment_glsl(program);
	}

	shader_program::~shader_program()
	{
		if (m_library)
		{
			m_library->release();
		}
	}

	shader_program::shader_program(shader_program&& other) noexcept
		: id(other.id)
		, spv(std::move(other.spv))
		, msl(std::move(other.msl))
		, color_output_mask(other.color_output_mask)
		, m_library(other.m_library)
	{
		other.m_library = nullptr;
	}

	shader_program& shader_program::operator=(shader_program&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		if (m_library)
		{
			m_library->release();
		}

		id = other.id;
		spv = std::move(other.spv);
		msl = std::move(other.msl);
		color_output_mask = other.color_output_mask;
		m_library = other.m_library;
		other.m_library = nullptr;
		return *this;
	}

	bool shader_program::ensure_library(MTL4::Compiler* compiler) const
	{
		if (m_library)
		{
			return true;
		}

		if (!compiler || msl.empty())
		{
			return false;
		}

		NS::Error* error = nullptr;
		m_library = compile_msl(compiler, msl, &error);
		if (!m_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: library creation failed: %s", message);
			return false;
		}

		return true;
	}

	pipeline::~pipeline()
	{
		if (state)
		{
			state->release();
		}
	}

	void MTLTraits::recompile_vertex_program(const RSXVertexProgram& program, vertex_program_type& data, usz id)
	{
		data.id = static_cast<u32>(id);
		data.spv.clear();
		data.msl.clear();
		data.color_output_mask = 0;

		// Vulkan-rule GLSL: glslang needs it to reach SPIR-V (HLD-05 §2.3).
		std::string glsl = decompile_vertex_program(program);
		if (glsl.empty())
		{
			rsx_log.error("Metal: the vertex decompile produced no GLSL (vp id %u)", data.id);
			return;
		}

		if (!spirv::compile_glsl_to_spv(data.spv, glsl, ::glsl::glsl_vertex_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: vertex SPIR-V compilation failed (vp id %u)", data.id);
			data.spv.clear();
			return;
		}

		if (!spv_to_msl(data.spv, true, data.msl, &data.color_output_mask))
		{
			rsx_log.error("Metal: vertex MSL translation failed (vp id %u)", data.id);
		}
	}

	void MTLTraits::recompile_fragment_program(const RSXFragmentProgram& program, fragment_program_type& data, usz id)
	{
		data.id = static_cast<u32>(id);
		data.spv.clear();
		data.msl.clear();
		data.color_output_mask = 0;

		std::string glsl = decompile_fragment_program(program);
		if (glsl.empty())
		{
			rsx_log.error("Metal: the fragment decompile produced no GLSL (fp id %u)", data.id);
			return;
		}

		if (!spirv::compile_glsl_to_spv(data.spv, glsl, ::glsl::glsl_fragment_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: fragment SPIR-V compilation failed (fp id %u)", data.id);
			data.spv.clear();
			return;
		}

		if (!spv_to_msl(data.spv, false, data.msl, &data.color_output_mask))
		{
			rsx_log.error("Metal: fragment MSL translation failed (fp id %u)", data.id);
		}
	}

	void MTLTraits::validate_pipeline_properties(const vertex_program_type&, const fragment_program_type& fragment, pipeline_properties& properties)
	{
		// A program whose MSL translation failed carries no reflection, so its
		// properties must be left alone.  A zero mask on a translated program is
		// not the same thing: it is a program with no colour output, and it must
		// clear every write mask so an unwritten attachment cannot be written.
		if (fragment.msl.empty())
		{
			return;
		}

		// Mask each write mask by the fragment program's live colour outputs (the
		// VKTraits intent, VKProgramBuffer.h:34-41).
		const u32 attachment_count = static_cast<u32>(properties.write_mask.size());
		for (u32 index = 0; index < attachment_count; ++index)
		{
			if ((fragment.color_output_mask & (1u << index)) == 0)
			{
				properties.write_mask[index] = MTL::ColorWriteMaskNone;
			}
		}
	}

	MTLTraits::pipeline_type* MTLTraits::build_pipeline(
		const vertex_program_type& vertex,
		const fragment_program_type& fragment,
		const pipeline_properties& properties,
		bool compile_async,
		std::function<pipeline_type*(pipeline_storage_type&)> callback,
		const ExtraData& extra)
	{
		// The MTL4 compiler is async by completion; the synchronous form is used
		// until the async task path lands (HLD-05 §3.6).
		(void)compile_async;

		if (!extra.compiler)
		{
			rsx_log.error("Metal: no MTL4 compiler for the pipeline build");
			return nullptr;
		}

		// The library is per program id and is created once (HLD-05 §3.6).
		if (!vertex.ensure_library(extra.compiler) || !fragment.ensure_library(extra.compiler))
		{
			rsx_log.error("Metal: shader library creation failed");
			return nullptr;
		}

		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(vertex.library());
		vertex_function->setName(NS::String::string(extra.vertex_entry.c_str(), NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(fragment.library());
		fragment_function->setName(NS::String::string(extra.fragment_entry.c_str(), NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);

		// The landed state applier fills the PSO-relevant subset (MTLPipeline.h).
		apply_pipeline_properties(descriptor, properties);

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = extra.compiler->newRenderPipelineState(descriptor, nullptr, &error);

		vertex_function->release();
		fragment_function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: render pipeline creation failed: %s", message);
			return nullptr;
		}

		pipeline_storage_type result = std::make_unique<pipeline>();
		result->state = state;
		return callback(result);
	}
}

#endif // RSX_ENABLE_METAL4
