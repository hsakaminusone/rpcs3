#include "MTLShaderInterpreter.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLFragmentProgram.h"
#include "MTLInstrumentation.h"
#include "MTLProgramPipeline.h"
#include "MTLShader.h"
#include "MTLVertexProgram.h"

#include "../Program/GLSLCommon.h"
#include "../Program/SPIRVCommon.h"
#include "../Program/ShaderInterpreter.h"
#include "Emu/RSX/rsx_methods.h"

#include "Emu/system_config.h"

#include "util/logs.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace mtl
{
	using enum program_common::interpreter::compiler_option;

	namespace
	{
		constexpr u32 texture_type_count = 4;

		const char* const sampler_type_names[texture_type_count] =
		{
			"sampler1D",
			"sampler2D",
			"samplerCube",
			"sampler3D"
		};

		const char* const sampler_macro_names[texture_type_count] =
		{
			"SAMPLER1D",
			"SAMPLER2D",
			"SAMPLERCUBE",
			"SAMPLER3D"
		};

		const char* const sampler_no_macro_names[texture_type_count] =
		{
			"MTL_NO_TEX1D",
			"MTL_NO_TEX2D",
			"MTL_NO_TEXCUBE",
			"MTL_NO_TEX3D"
		};

		bool same_mapping(const interpreter_texture_mapping& a, const interpreter_texture_mapping& b)
		{
			return a.referenced_mask == b.referenced_mask && a.dimensions == b.dimensions &&
				a.unit_type == b.unit_type && a.unit_index == b.unit_index &&
				a.array_size == b.array_size && a.array_base == b.array_base &&
				a.total == b.total && a.single_type == b.single_type;
		}

		std::string build_vertex_glsl(u64 options)
		{
			RSXVertexProgram vertex{};
			if (options & COMPILER_OPT_ENABLE_INSTANCING)
			{
				vertex.ctrl |= RSX_SHADER_CONTROL_INSTANCED_CONSTANTS;
			}

			std::string shader;
			MTLVertexDecompilerThread decompiler(vertex, shader);
			decompiler.properties.has_indexed_constants = true;
			// The body emulates the LIT opcode, so the prologue must provide
			// _builtin_lit (VKShaderInterpreter.cpp:180).
			decompiler.properties.has_lit_op = true;

			std::stringstream out;
			decompiler.emitInterpreterPrelude(out);

			// The interpreter body writes this payload, and the fragment interpreter
			// reads it.  The RSX unit array occupies locations 0..15, so the payload
			// is location 16 (VKShaderInterpreter.cpp:196).
			out << "layout(location=16) out flat uvec4 draw_params_payload;\n\n";

			out <<
				"#define xform_constants_offset get_draw_params().xform_constants_offset\n"
				"#define scale_offset_mat get_vertex_context().scale_offset_mat\n"
				"#define transform_branch_bits get_vertex_context().transform_branch_bits\n"
				"#define point_size get_vertex_context().point_size\n"
				"#define z_near get_vertex_context().z_near\n"
				"#define z_far get_vertex_context().z_far\n\n";

			out <<
				"layout(std430, set=0, binding=12) readonly restrict buffer VertexInstructionBlock\n"
				"{\n"
				"	uint base_address;\n"
				"	uint entry;\n"
				"	uint output_mask;\n"
				"	uint control;\n"
				"	uvec4 vp_instructions[];\n"
				"};\n\n";

			if (options & COMPILER_OPT_ENABLE_INSTANCING)
			{
				out << "#define _ENABLE_INSTANCED_CONSTANTS\n\n";
			}

			// The shared body selects its VULKAN paths through #ifdef VULKAN, which
			// glslang supplies itself for the Vulkan rule set; defining it here would
			// be a preprocessor error.  Metal follows the same 0..1 clip-space
			// convention, so the Vulkan substitution is what this backend wants.

			decompiler.emitInterpreterFunctions(out);
			out << program_common::interpreter::get_vertex_interpreter();
			return out.str();
		}

		std::string build_fragment_glsl(const interpreter_fragment_key& key)
		{
			RSXFragmentProgram fragment{};
			fragment.ctrl = RSX_SHADER_CONTROL_INTERPRETER_MODEL;

			if (key.options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
			{
				fragment.ctrl |= CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT;
			}

			if (key.options & COMPILER_OPT_ENABLE_F32_EXPORT)
			{
				fragment.ctrl |= CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS;
			}

			if (key.options & COMPILER_OPT_ENABLE_KIL)
			{
				fragment.ctrl |= RSX_SHADER_CONTROL_USES_KIL;
			}

			if (key.options & COMPILER_OPT_ENABLE_ROP_REMAP)
			{
				fragment.ctrl |= RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP;
			}

			u32 length = 0;
			std::string shader;
			MTLFragmentDecompilerThread decompiler(fragment, length, shader);

			// The interpreter body emulates LIT and reads the window position and fog
			// coordinate straight from the fragment context, so the prologue only
			// has to provide the LIT helper and the context accessors.  The guest's
			// inputs arrive as the in_regs[] array, not as named varyings, so the
			// in_register_mask stays clear (VKShaderInterpreter.cpp:286).
			decompiler.properties.has_lit_op = true;

			std::stringstream out;
			decompiler.emitInterpreterPrelude(out);

			out << "layout(location=16) in flat uvec4 draw_params_payload;\n\n";

			// Same accessors the Vulkan interpreter defines by hand.  They replace the
			// prologue's fetch_fog_value()/get_wpos(), which reference the named
			// varying inputs the interpreter never declares.
			out <<
				"#define fog_param0 fs_contexts[_fs_context_offset].fog_param0\n"
				"#define fog_param1 fs_contexts[_fs_context_offset].fog_param1\n"
				"#define fog_mode fs_contexts[_fs_context_offset].fog_mode\n"
				"#define wpos_scale fs_contexts[_fs_context_offset].wpos_scale\n"
				"#define wpos_bias fs_contexts[_fs_context_offset].wpos_bias\n\n";

			const interpreter_texture_mapping& mapping = key.mapping;
			const bool textures = !!(key.options & COMPILER_OPT_ENABLE_TEXTURES) && !mapping.empty();

			if (textures)
			{
				if (mapping.is_single())
				{
					const u32 type = mapping.single_type;
					out << "layout(set=1, binding=12) uniform " << sampler_type_names[type] << " "
						<< sampler_type_names[type] << "_array[16];\n\n";
				}
				else
				{
					for (u32 type = 0; type < texture_type_count; ++type)
					{
						if (!mapping.array_size[type])
						{
							continue;
						}

						out << "layout(set=1, binding=" << (12u + mapping.array_base[type]) << ") uniform "
							<< sampler_type_names[type] << " " << sampler_type_names[type] << "_array["
							<< static_cast<u32>(mapping.array_size[type]) << "];\n";
					}

					out << "\n";
				}
			}

			// The instruction block lives in the buffer space at 12.  Its descriptor
			// set is 0 so it cannot collide with the set=1 sampler arrays in the
			// GLSL/SPIR-V descriptor space; MSL uses the binding alone (R8).
			out <<
				"layout(std430, set=0, binding=12) readonly restrict buffer FragmentInstructionBlock\n"
				"{\n"
				"	uint shader_control;\n"
				"	uint texture_control;\n"
				"	uint textures_resident;\n"
				"	uint reserved2;\n"
				"	uvec4 fp_instructions[];\n"
				"};\n\n";

			out << "uint rop_control = fs_contexts[_fs_context_offset].rop_control;\n"
				"float alpha_ref = fs_contexts[_fs_context_offset].alpha_ref;\n\n";

			if (textures)
			{
				out << "#define WITH_TEXTURES\n";

				if (mapping.is_single())
				{
					out << "#define " << sampler_macro_names[mapping.single_type] << "(index) "
						<< sampler_type_names[mapping.single_type] << "_array[index]\n";
				}
				else
				{
					for (u32 type = 0; type < texture_type_count; ++type)
					{
						if (!mapping.array_size[type])
						{
							continue;
						}

						out << "int mtl_sampler_" << type << "(uint unit)\n"
							"{\n"
							"	switch (unit)\n"
							"	{\n";

						for (u32 unit = 0; unit < 16; ++unit)
						{
							if ((mapping.referenced_mask & (1u << unit)) && mapping.unit_type[unit] == type)
							{
								out << "	case " << unit << ": return " << static_cast<u32>(mapping.unit_index[unit]) << ";\n";
							}
						}

						out << "	default: return 0;\n"
							"	}\n"
							"}\n\n";

						out << "#define " << sampler_macro_names[type] << "(index) "
							<< sampler_type_names[type] << "_array[mtl_sampler_" << type << "(index)]\n";
					}
				}

				out << "\n";
			}

			for (u32 type = 0; type < texture_type_count; ++type)
			{
				if (!textures || (mapping.is_single() ? mapping.single_type != type : !mapping.array_size[type]))
				{
					out << "#define " << sampler_no_macro_names[type] << "\n";
				}
			}

			if (key.options & COMPILER_OPT_ENABLE_DEPTH_EXPORT)
			{
				out << "#define WITH_DEPTH_EXPORT\n";
			}

			if (!(key.options & COMPILER_OPT_ENABLE_F32_EXPORT))
			{
				out << "#define WITH_HALF_OUTPUT_REGISTER\n";
			}

			if (key.options & COMPILER_OPT_ENABLE_FLOW_CTRL)
			{
				out << "#define WITH_FLOW_CTRL\n";
			}

			if (key.options & COMPILER_OPT_ENABLE_PACKING)
			{
				out << "#define WITH_PACKING\n";
			}

			if (key.options & COMPILER_OPT_ENABLE_KIL)
			{
				out << "#define WITH_KIL\n";
			}

			if (key.options & COMPILER_OPT_ENABLE_STIPPLING)
			{
				out << "#define WITH_STIPPLING\n";
			}

			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_GE) out << "#define ALPHA_TEST_GEQUAL\n";
			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_G) out << "#define ALPHA_TEST_GREATER\n";
			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_LE) out << "#define ALPHA_TEST_LEQUAL\n";
			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_L) out << "#define ALPHA_TEST_LESS\n";
			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_EQ) out << "#define ALPHA_TEST_EQUAL\n";
			if (key.options & COMPILER_OPT_ENABLE_ALPHA_TEST_NE) out << "#define ALPHA_TEST_NEQUAL\n";

			if (key.output_count < 2) out << "#define MTL_NO_OCOL1\n";
			if (key.output_count < 3) out << "#define MTL_NO_OCOL2\n";
			if (key.output_count < 4) out << "#define MTL_NO_OCOL3\n";

			// VULKAN is supplied by glslang for the Vulkan rule set (see the vertex
			// builder); the shared body's #ifdef VULKAN paths take the Vulkan route.

			decompiler.emitInterpreterGlobals(out);

			// The base decompiler's TEX_PARAM names the per-unit aliases of a real
			// program.  The interpreter indexes the whole per-unit array dynamically.
			out << "#undef TEX_PARAM\n"
				"#define TEX_PARAM(index) texture_parameters[index + texture_base_index]\n"
				"#define IS_TEXTURE_RESIDENT(index) ((textures_resident & (1u << (index))) != 0)\n\n";

			out << program_common::interpreter::get_fragment_interpreter();
			return out.str();
		}
	}

	std::string interpreter_cache::build_vertex_source(u64 options)
	{
		return build_vertex_glsl(options);
	}

	std::string interpreter_cache::build_fragment_source(const interpreter_fragment_key& key)
	{
		return build_fragment_glsl(key);
	}

	interpreter_texture_mapping interpreter_texture_mapping::from_mask(u16 mask, u32 dimensions)
	{
		interpreter_texture_mapping mapping;
		mapping.referenced_mask = mask;
		mapping.dimensions = dimensions;

		if (!mask)
		{
			return mapping;
		}

		u32 type_counts[4]{};

		for (u32 unit = 0; unit < 16; ++unit)
		{
			if (!(mask & (1u << unit)))
			{
				continue;
			}

			const u32 type = (dimensions >> (unit * 2)) & 3u;
			mapping.unit_type[unit] = static_cast<u8>(type);
			type_counts[type]++;
		}

		u32 referenced_types = 0;
		u32 single_type = 0;

		for (u32 type = 0; type < 4; ++type)
		{
			if (type_counts[type])
			{
				referenced_types++;
				single_type = type;
			}
		}

		if (referenced_types == 1)
		{
			mapping.single_type = static_cast<u8>(single_type);
			mapping.canonicalize();
			return mapping;
		}

		u32 base = 0;

		for (u32 type = 0; type < 4; ++type)
		{
			mapping.array_base[type] = static_cast<u8>(base);
			mapping.array_size[type] = static_cast<u8>(type_counts[type]);
			base += type_counts[type];
		}

		mapping.total = static_cast<u8>(base);

		u32 next_index[4]{};

		for (u32 unit = 0; unit < 16; ++unit)
		{
			if (!(mask & (1u << unit)))
			{
				continue;
			}

			const u32 type = mapping.unit_type[unit];
			mapping.unit_index[unit] = static_cast<u8>(next_index[type]++);
		}

		return mapping;
	}

	void interpreter_texture_mapping::canonicalize()
	{
		if (!is_single())
		{
			return;
		}

		// The single-type shader indexes the 16-element array by the RSX unit, so
		// neither the mask nor the dimensions are baked into its source.
		referenced_mask = 0;
		dimensions = 0;
		total = 16;
		array_size = {};
		array_base = {};
		array_size[single_type] = 16;

		for (u32 unit = 0; unit < 16; ++unit)
		{
			unit_type[unit] = single_type;
			unit_index[unit] = static_cast<u8>(unit);
		}
	}

	bool interpreter_texture_mapping::operator==(const interpreter_texture_mapping& other) const
	{
		return same_mapping(*this, other);
	}

	usz interpreter_fragment_key_hash::operator()(const interpreter_fragment_key& key) const
	{
		usz seed = rpcs3::hash_base<u64>(key.options);
		seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.output_count));
		seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.referenced_mask));
		seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.dimensions));
		seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.total));
		seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.single_type));

		for (u32 unit = 0; unit < 16; ++unit)
		{
			seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.unit_type[unit]));
			seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.unit_index[unit]));
		}

		for (u32 type = 0; type < 4; ++type)
		{
			seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.array_size[type]));
			seed = rpcs3::hash64(seed, rpcs3::hash_base<u32>(key.mapping.array_base[type]));
		}

		return seed;
	}

	usz interpreter_cache::pipeline_key_hash::operator()(const pipeline_key& key) const
	{
		usz seed = rpcs3::hash_base<unsigned long long>(reinterpret_cast<unsigned long long>(key.vertex));
		seed = rpcs3::hash64(seed, rpcs3::hash_base<unsigned long long>(reinterpret_cast<unsigned long long>(key.fragment)));
		seed = rpcs3::hash64(seed, rpcs3::hash_struct(key.properties));
		return seed;
	}

	interpreter_shader::~interpreter_shader()
	{
		// A live compile owns the library and the task.  The shutdown drain is
		// bounded; when it times out the completion handler still holds a
		// reference, so the destructor runs after the handler and releases both.
		if (m_state.load(std::memory_order_acquire) != interpreter_library_state::compiling)
		{
			if (library)
			{
				library->release();
			}

			if (MTL4::CompilerTask* task = m_task.load(std::memory_order_acquire))
			{
				task->release();
			}
		}
	}

	void interpreter_shader::finish(MTL::Library* value, NS::Error* error)
	{
		std::lock_guard lock(m_mutex);

		if (m_state.load(std::memory_order_acquire) != interpreter_library_state::compiling)
		{
			// A duplicate delivery must not replace a ready library.
			if (value)
			{
				value->release();
			}
			return;
		}

		if (value)
		{
			library = value;
		}
		else
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: interpreter library creation failed: %s", message);
		}

		m_state.store(value ? interpreter_library_state::ready : interpreter_library_state::failed, std::memory_order_release);
		m_cv.notify_all();
	}

	void interpreter_shader::attach(MTL4::CompilerTask* task)
	{
		if (!task)
		{
			// The submission itself failed.  Publish the failure so a waiter does
			// not block until its timeout.
			finish(nullptr, nullptr);
			return;
		}

		std::lock_guard lock(m_mutex);

		if (m_state.load(std::memory_order_acquire) == interpreter_library_state::compiling && !m_task.load(std::memory_order_relaxed))
		{
			m_task.store(task, std::memory_order_release);
			return;
		}

		// The handler already ran, or a task was already attached.  The task is
		// finished or redundant; release it on this thread.
		task->release();
	}

	void interpreter_shader::release_task()
	{
		if (!m_task.load(std::memory_order_acquire))
		{
			return;
		}

		std::lock_guard lock(m_mutex);

		if (MTL4::CompilerTask* task = m_task.load(std::memory_order_relaxed))
		{
			task->release();
			m_task.store(nullptr, std::memory_order_release);
		}
	}

	bool interpreter_shader::wait(std::chrono::milliseconds timeout)
	{
		// The ready fast path needs no lock, so a repeated draw only pays two
		// atomic loads.
		if (m_state.load(std::memory_order_acquire) == interpreter_library_state::compiling)
		{
			std::unique_lock lock(m_mutex);
			m_cv.wait_for(lock, timeout, [this]
			{
				return m_state.load(std::memory_order_acquire) != interpreter_library_state::compiling;
			});
		}

		release_task();
		return m_state.load(std::memory_order_acquire) == interpreter_library_state::ready;
	}

	interpreter_pipeline::~interpreter_pipeline()
	{
		if (state)
		{
			state->release();
		}
	}

	void interpreter_cache::init(MTL4::Compiler* compiler, MTL4::CompilerTaskOptions* options, pipeline_jobs* jobs,
		const std::atomic<bool>* device_lost)
	{
		m_compiler = compiler;
		m_options = options;
		m_jobs = jobs;
		m_device_lost = device_lost;

		// The variants are submitted here, off the draw path and without a
		// progress dialog (LLD-shader-interpreter.md §9.3).
		prebuild();
	}

	void interpreter_cache::shutdown()
	{
		// Drain the prebuild libraries before the device can go away, with the
		// same bounded wait the guest registry uses.  A lost device abandons the
		// wait; the completion handler then finishes the entry on its own thread.
		if (!(m_device_lost && m_device_lost->load(std::memory_order_acquire)))
		{
			for (auto& entry : m_vertex_shaders)
			{
				if (entry.second)
				{
					entry.second->wait(std::chrono::seconds(5));
				}
			}

			for (auto& entry : m_fragment_shaders)
			{
				if (entry.second)
				{
					entry.second->wait(std::chrono::seconds(5));
				}
			}
		}

		m_memo = {};
		m_pipelines.clear();
		m_fragment_shaders.clear();
		m_vertex_shaders.clear();
		m_compiler = nullptr;
		m_options = nullptr;
		m_jobs = nullptr;
		m_device_lost = nullptr;
	}

	void interpreter_cache::begin_frame()
	{
		// The shader and pipeline caches survive frames; only the per-draw
		// instruction blocks are frame data and live in constants_heap.
	}

	bool interpreter_cache::build_request(const RSXVertexProgram& vertex,
		const RSXFragmentProgram& fragment,
		const program_hash_util::vertex_program_utils::vertex_program_metadata& vertex_metadata,
		const program_hash_util::fragment_program_utils::fragment_program_metadata& fragment_metadata,
		interpreter_request& request)
	{
		request = {};

		// The shared body cannot fetch vertex textures at all (R21).
		if (vertex_metadata.referenced_textures_mask)
		{
			// TODO(excise): level 2; the reason the interpreter stands aside.
			MTL_PROBE_LATCH(2, "interpreter/reject vertex textures");
			return false;
		}

		// Features whose RSX behaviour is more than the shared body reproduces.
		// Attribute interpolation is intentionally absent: it is normal triangle
		// state, not an interpreter limitation (R20).
		if (fragment.ctrl & (RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE |
			RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE |
			RSX_SHADER_CONTROL_TEXTURE_ALPHA_KILL |
			RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT |
			RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING |
			RSX_SHADER_CONTROL_LOGIC_OP))
		{
			// TODO(excise): level 2; the reason the interpreter stands aside.
			MTL_PROBE_LATCH(2, "interpreter/reject fragment control");
			return false;
		}

		if (fragment.texture_state.shadow_textures ||
			fragment.texture_state.redirected_textures ||
			fragment.texture_state.multisampled_textures ||
			(fragment.ctrl & RSX_SHADER_CONTROL_ROP_MULTISAMPLED))
		{
			// TODO(excise): level 2; the reason the interpreter stands aside.
			MTL_PROBE_LATCH(2, "interpreter/reject texture state");
			return false;
		}

		if (!fragment.mrt_buffers_count)
		{
			// TODO(excise): level 2; the reason the interpreter stands aside.
			MTL_PROBE_LATCH(2, "interpreter/reject no output");
			return false;
		}

		request.output_count = std::min<u32>(4, fragment.mrt_buffers_count);

		u64 options = 0;

		if (rsx::method_registers.alpha_test_enabled())
		{
			switch (rsx::method_registers.alpha_func())
			{
			case rsx::comparison_function::always:
				break;
			case rsx::comparison_function::never:
				// TODO(excise): level 2; the reason the interpreter stands aside.
				MTL_PROBE_LATCH(2, "interpreter/reject alpha never");
				return false;
			case rsx::comparison_function::greater_or_equal:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_GE;
				break;
			case rsx::comparison_function::greater:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_G;
				break;
			case rsx::comparison_function::less_or_equal:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_LE;
				break;
			case rsx::comparison_function::less:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_L;
				break;
			case rsx::comparison_function::equal:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_EQ;
				break;
			case rsx::comparison_function::not_equal:
				options |= COMPILER_OPT_ENABLE_ALPHA_TEST_NE;
				break;
			}
		}

		if (fragment.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) options |= COMPILER_OPT_ENABLE_DEPTH_EXPORT;
		if (fragment.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) options |= COMPILER_OPT_ENABLE_F32_EXPORT;
		if (fragment.ctrl & RSX_SHADER_CONTROL_USES_KIL) options |= COMPILER_OPT_ENABLE_KIL;
		if (fragment.ctrl & RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP) options |= COMPILER_OPT_ENABLE_ROP_REMAP;
		if (fragment_metadata.has_branch_instructions) options |= COMPILER_OPT_ENABLE_FLOW_CTRL;
		if (fragment_metadata.has_pack_instructions) options |= COMPILER_OPT_ENABLE_PACKING;
		if (rsx::method_registers.polygon_stipple_enabled()) options |= COMPILER_OPT_ENABLE_STIPPLING;
		if (vertex.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) options |= COMPILER_OPT_ENABLE_INSTANCING;

		interpreter_texture_mapping mapping = interpreter_texture_mapping::from_mask(
			fragment_metadata.referenced_textures_mask, fragment.texture_state.texture_dimensions);

		if (!mapping.empty())
		{
			options |= COMPILER_OPT_ENABLE_TEXTURES;
		}

		request.vertex_options = options & COMPILER_OPT_ALL_VS_MASK;
		request.fragment_options = options & COMPILER_OPT_ALL_FS_MASK;
		request.mapping = mapping;
		return true;
	}

	std::shared_ptr<interpreter_shader> interpreter_cache::get_vertex_shader(u64 options)
	{
		return await_library(submit_vertex_shader(options));
	}

	std::shared_ptr<interpreter_shader> interpreter_cache::submit_vertex_shader(u64 options)
	{
		if (auto found = m_vertex_shaders.find(options); found != m_vertex_shaders.end())
		{
			return found->second;
		}

		std::string msl;
		if (!build_vertex_msl(options, msl))
		{
			m_vertex_shaders[options] = nullptr;
			return nullptr;
		}

		auto shader = std::make_shared<interpreter_shader>();
		m_vertex_shaders[options] = shader;
		submit_library(shader, msl);
		return shader;
	}

	bool interpreter_cache::build_vertex_msl(u64 options, std::string& msl)
	{
		std::string glsl = build_vertex_glsl(options);
		std::vector<u32> spv;

		if (!spirv::compile_glsl_to_spv(spv, glsl, ::glsl::glsl_vertex_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: interpreter vertex GLSL compilation failed");
			return false;
		}

		if (!spv_to_msl(spv, true, msl, nullptr, rsx_push_slot))
		{
			rsx_log.error("Metal: interpreter vertex MSL compilation failed");
			return false;
		}

		return true;
	}

	std::shared_ptr<interpreter_shader> interpreter_cache::get_fragment_shader(const interpreter_fragment_key& key)
	{
		return await_library(submit_fragment_shader(key));
	}

	std::shared_ptr<interpreter_shader> interpreter_cache::submit_fragment_shader(const interpreter_fragment_key& key)
	{
		if (auto found = m_fragment_shaders.find(key); found != m_fragment_shaders.end())
		{
			return found->second;
		}

		std::string msl;
		if (!build_fragment_msl(key, msl))
		{
			m_fragment_shaders[key] = nullptr;
			return nullptr;
		}

		auto shader = std::make_shared<interpreter_shader>();
		m_fragment_shaders[key] = shader;
		submit_library(shader, msl);
		return shader;
	}

	bool interpreter_cache::build_fragment_msl(const interpreter_fragment_key& key, std::string& msl)
	{
		std::string glsl = build_fragment_glsl(key);
		std::vector<u32> spv;

		if (!spirv::compile_glsl_to_spv(spv, glsl, ::glsl::glsl_fragment_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: interpreter fragment GLSL compilation failed");
			return false;
		}

		if (!spv_to_msl(spv, false, msl, nullptr, rsx_push_slot))
		{
			rsx_log.error("Metal: interpreter fragment MSL compilation failed");
			return false;
		}

		return true;
	}

	void interpreter_cache::submit_library(const std::shared_ptr<interpreter_shader>& shader, const std::string& msl)
	{
		if (!m_jobs)
		{
			// No registry (a bare unit test): compile on this thread.
			NS::Error* error = nullptr;
			shader->finish(compile_msl(m_compiler, msl, &error), error);
			return;
		}

		MTL::NewLibraryCompletionHandlerFunction handler =
			[shader](MTL::Library* library, NS::Error* error)
		{
			// The handler runs on a Metal compiler thread, where the library is
			// autoreleased; retain it for the cache.
			NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

			if (library)
			{
				library->retain();
			}

			shader->finish(library, error);
			pool->release();
		};

		shader->attach(compile_msl_async(m_compiler, msl, std::move(handler)));
	}

	std::shared_ptr<interpreter_shader> interpreter_cache::await_library(std::shared_ptr<interpreter_shader> shader)
	{
		if (!shader)
		{
			return nullptr;
		}

		// A lost device never completes the compile; do not hold the RSX thread
		// for the full timeout.
		if (m_device_lost && m_device_lost->load(std::memory_order_acquire))
		{
			return nullptr;
		}

		return shader->wait(std::chrono::seconds(10)) ? std::move(shader) : nullptr;
	}

	void interpreter_cache::prebuild()
	{
		if (!m_compiler)
		{
			return;
		}

		// The base variants (LLD-shader-interpreter.md §9.3): both vertex control
		// shapes, the textureless fragment and the four single-type identity
		// mappings, at the one-output key.  A draw whose variant is not covered
		// compiles it on first use.  The compiles are asynchronous, so the init
		// thread does not stall and there is no progress dialog.
		submit_vertex_shader(0);
		submit_vertex_shader(COMPILER_OPT_ENABLE_INSTANCING);

		interpreter_fragment_key fragment_key;
		fragment_key.output_count = 1;
		submit_fragment_shader(fragment_key);

		fragment_key.options |= COMPILER_OPT_ENABLE_TEXTURES;

		for (u32 type = 0; type < 4; ++type)
		{
			// One referenced unit of `type` canonicalises to that dimension's
			// single-type variant.
			fragment_key.mapping = interpreter_texture_mapping::from_mask(1u, type);
			submit_fragment_shader(fragment_key);
		}
	}

	std::shared_ptr<const interpreter_pipeline> interpreter_cache::get(const interpreter_request& request,
		const pipeline_properties& properties)
	{
		if (!m_compiler || (m_device_lost && m_device_lost->load(std::memory_order_acquire)))
		{
			return nullptr;
		}

		if (m_memo.valid && m_memo.request == request && m_memo.properties == properties)
		{
			return m_memo.pipeline;
		}

		interpreter_fragment_key fragment_key;
		fragment_key.options = request.fragment_options;
		fragment_key.output_count = request.output_count;
		fragment_key.mapping = request.mapping;

		const auto vertex_shader = get_vertex_shader(request.vertex_options);
		const auto fragment_shader = get_fragment_shader(fragment_key);

		if (!vertex_shader || !fragment_shader)
		{
			return nullptr;
		}

		pipeline_key key;
		key.vertex = vertex_shader.get();
		key.fragment = fragment_shader.get();
		key.properties = properties;

		if (auto found = m_pipelines.find(key); found != m_pipelines.end())
		{
			m_memo.valid = true;
			m_memo.request = request;
			m_memo.properties = properties;
			m_memo.pipeline = found->second;
			return found->second;
		}

		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(vertex_shader->library);
		vertex_function->setName(NS::String::string(msl_entry_point, NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(fragment_shader->library);
		fragment_function->setName(NS::String::string(msl_entry_point, NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);
		apply_pipeline_properties(descriptor, properties);

		MTL::RenderPipelineState* state = nullptr;
		NS::Error* error = nullptr;
		auto link = [&]()
		{
			state = m_compiler->newRenderPipelineState(descriptor, m_options, &error);
		};

		if (m_jobs)
		{
			if (!m_jobs->run_link_sync(std::move(link)))
			{
				descriptor->release();
				vertex_function->release();
				fragment_function->release();
				return nullptr;
			}
		}
		else
		{
			link();
		}

		descriptor->release();
		vertex_function->release();
		fragment_function->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: interpreter render pipeline creation failed: %s", message);
			return nullptr;
		}

		auto pipeline = std::make_shared<interpreter_pipeline>();
		pipeline->state = state;
		pipeline->vertex_shader = vertex_shader;
		pipeline->fragment_shader = fragment_shader;
		pipeline->mapping = request.mapping;
		pipeline->output_count = request.output_count;

		m_pipelines[key] = pipeline;
		m_memo.valid = true;
		m_memo.request = request;
		m_memo.properties = properties;
		m_memo.pipeline = pipeline;
		return pipeline;
	}
}

#endif // RSX_ENABLE_METAL4
