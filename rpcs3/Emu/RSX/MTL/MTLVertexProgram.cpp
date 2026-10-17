#include "MTLVertexProgram.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"
#include "MTLVertexState.h"

#include "../Program/GLSLCommon.h"

#include "Utilities/StrFmt.h"
#include "util/logs.hpp"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace mtl
{
	int varying_location(std::string_view name)
	{
		static constexpr std::pair<std::string_view, int> table[] =
		{
			{ "tc0", 0 },
			{ "tc1", 1 },
			{ "tc2", 2 },
			{ "tc3", 3 },
			{ "tc4", 4 },
			{ "tc5", 5 },
			{ "tc6", 6 },
			{ "tc7", 7 },
			{ "tc8", 8 },
			{ "tc9", 9 },
			{ "diff_color", 10 },
			{ "diff_color1", 11 },
			{ "spec_color", 12 },
			{ "spec_color1", 13 },
			{ "fog_c", 14 },
			{ "fogc", 14 },
			{ "usr", 15 },
			{ "depth_range", 17 },
		};

		for (const auto& entry : table)
		{
			if (entry.first == name)
			{
				return entry.second;
			}
		}

		fmt::throw_exception("Metal: unknown varying register '%s'", name);
	}

	int texture_index(std::string_view name)
	{
		const usz length = name.length();
		if (length < 2)
		{
			fmt::throw_exception("Metal: invalid texture name '%s'", name);
		}

		std::string digits;
		for (usz index = length - 2; index < length; ++index)
		{
			if (std::isdigit(static_cast<unsigned char>(name[index])) != 0)
			{
				digits += name[index];
			}
		}

		if (digits.empty())
		{
			fmt::throw_exception("Metal: invalid texture name '%s'", name);
		}

		return std::atoi(digits.c_str());
	}

	MTLVertexDecompilerThread::MTLVertexDecompilerThread(const RSXVertexProgram& prog, std::string& shader)
		: VertexProgramDecompiler(prog)
		, m_shader(shader)
	{
	}

	std::string MTLVertexDecompilerThread::getFloatTypeName(usz elementCount)
	{
		return glsl::getFloatTypeNameImpl(elementCount);
	}

	std::string MTLVertexDecompilerThread::getIntTypeName(usz)
	{
		return "ivec4";
	}

	std::string MTLVertexDecompilerThread::getFunction(FUNCTION f)
	{
		return glsl::getFunctionImpl(f);
	}

	std::string MTLVertexDecompilerThread::compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1, bool scalar)
	{
		return glsl::compareFunctionImpl(f, Op0, Op1, scalar);
	}

	void MTLVertexDecompilerThread::insertHeader(std::stringstream& OS)
	{
		OS <<
			"#version 450\n\n"
			"#extension GL_EXT_scalar_block_layout : require\n"
			"#extension GL_EXT_uniform_buffer_unsized_array : require\n"
			"#extension GL_ARB_separate_shader_objects : enable\n\n";

		glsl::insert_subheader_block(OS);

		// The draw-parameters heap and the vertex context are indexed through the
		// substituted draw-parameters push constant (HLD-05 §3.3).
		OS <<
			"#define get_draw_params() draw_parameters[draw_parameters_offset]\n"
			"#define vs_context_offset get_draw_params().vs_context_offset\n"
			"#define get_vertex_context() vertex_contexts[vs_context_offset]\n"
			"#define get_user_clip_config() get_vertex_context().user_clip_configuration_bits\n\n";

		OS <<
			"layout(std430, set=0, binding=" << vertex_context_slot << ") uniform VertexContextBuffer\n"
			"{\n"
			"	vertex_context_t vertex_contexts[];\n"
			"};\n\n";

		OS <<
			"layout(std430, set=0, binding=" << vertex_draw_parameters_slot << ") readonly restrict buffer DrawParametersBuffer\n"
			"{\n"
			"	draw_parameters_t draw_parameters[];\n"
			"};\n\n";

		// MSL has no push constants; MTLShader.cpp maps this block onto the
		// substituted slot vertex_draw_params_slot (HLD-05 §3.3, §3.2 †).
		OS <<
			"layout(push_constant) uniform push_constants_block\n"
			"{\n"
			"	uint draw_parameters_offset;\n"
			"};\n\n";
	}

	void MTLVertexDecompilerThread::insertInputs(std::stringstream& OS, const std::vector<ParamType>&)
	{
		// Software vertex pulling: the attributes arrive from the texture_buffer
		// streams by gl_VertexIndex, never from a vertex descriptor (HLD-05 §3.5).
		OS << "layout(set=0, binding=" << vertex_buffers_slot << ") uniform usamplerBuffer persistent_input_stream;\n";
		OS << "layout(set=0, binding=" << vertex_volatile_slot << ") uniform usamplerBuffer volatile_input_stream;\n";
	}

	void MTLVertexDecompilerThread::insertConstants(std::stringstream& OS, const std::vector<ParamType>& constants)
	{
		for (const ParamType& PT : constants)
		{
			for (const ParamItem& PI : PT.items)
			{
				if (PI.name.starts_with("vc["))
				{
					if (!(m_prog.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS))
					{
						OS << "layout(std430, set=0, binding=" << vertex_constants_slot << ") uniform VertexConstantsBuffer\n";
						OS << "{\n";
						OS << "	vec4 vc[];\n";
						OS << "};\n\n";
						continue;
					}

					OS << "layout(std430, set=0, binding=" << instancing_lut_slot << ") readonly restrict buffer InstancingData\n";
					OS << "{\n";
					OS << "	int constants_addressing_lookup[];\n";
					OS << "};\n\n";

					OS << "layout(std430, set=0, binding=" << instancing_constants_slot << ") readonly restrict buffer VertexConstantsBuffer\n";
					OS << "{\n";
					OS << "	vec4 instanced_constants_array[];\n";
					OS << "};\n\n";

					OS << "#define CONSTANTS_ARRAY_LENGTH " << (properties.has_indexed_constants ? 468 : ::size32(m_constant_ids)) << "\n\n";
					continue;
				}

				if (!PT.type.starts_with("sampler"))
				{
					continue;
				}

				const int id = texture_index(PI.name);
				std::string samplerType = PT.type;

				if (m_prog.texture_state.multisampled_textures & (1u << id))
				{
					if (samplerType != "sampler1D" && samplerType != "sampler2D")
					{
						rsx_log.error("Metal: unexpected multisampled sampler type '%s'", samplerType);
					}

					samplerType = "sampler2DMS";
				}

				OS << "layout(set=0, binding=" << (vertex_textures_slot + static_cast<u32>(id)) << ") uniform " << samplerType << " " << PI.name << ";\n";
			}
		}
	}

	static const vertex_reg_info reg_table[] =
	{
		{ "gl_Position", false, "dst_reg0", "", false },
		//Technically these two are for both back and front
		{ "diff_color", true, "dst_reg1", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE },
		{ "spec_color", true, "dst_reg2", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR },
		{ "diff_color1", true, "dst_reg3", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTDIFFUSE | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKDIFFUSE },
		{ "spec_color1", true, "dst_reg4", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_FRONTSPECULAR | CELL_GCM_ATTRIB_OUTPUT_MASK_BACKSPECULAR },
		{ "fog_c", true, "dst_reg5", ".xxxx", true, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_FOG },
		//Warning: With spir-v if you declare clip distance var, you must assign a value even when its disabled! Runtime does not assign a default value
		{ "gl_ClipDistance[0]", false, "dst_reg5", ".y * user_clip_factor(0)", false, "is_user_clip_enabled(0)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC0 },
		{ "gl_ClipDistance[1]", false, "dst_reg5", ".z * user_clip_factor(1)", false, "is_user_clip_enabled(1)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC1 },
		{ "gl_ClipDistance[2]", false, "dst_reg5", ".w * user_clip_factor(2)", false, "is_user_clip_enabled(2)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC2 },
		{ "gl_PointSize", false, "dst_reg6", ".x", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE },
		{ "gl_ClipDistance[3]", false, "dst_reg6", ".y * user_clip_factor(3)", false, "is_user_clip_enabled(3)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC3 },
		{ "gl_ClipDistance[4]", false, "dst_reg6", ".z * user_clip_factor(4)", false, "is_user_clip_enabled(4)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC4 },
		{ "gl_ClipDistance[5]", false, "dst_reg6", ".w * user_clip_factor(5)", false, "is_user_clip_enabled(5)", "0.5", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_UC5 },
		{ "tc0", true, "dst_reg7", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX0 },
		{ "tc1", true, "dst_reg8", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX1 },
		{ "tc2", true, "dst_reg9", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX2 },
		{ "tc3", true, "dst_reg10", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX3 },
		{ "tc4", true, "dst_reg11", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX4 },
		{ "tc5", true, "dst_reg12", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX5 },
		{ "tc6", true, "dst_reg13", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX6 },
		{ "tc7", true, "dst_reg14", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX7 },
		{ "tc8", true, "dst_reg15", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX8 },
		{ "tc9", true, "dst_reg6", "", false, "", "", "", true, CELL_GCM_ATTRIB_OUTPUT_MASK_TEX9 }  // In this line, dst_reg6 is correct since dst_reg goes from 0 to 15.
	};

	void MTLVertexDecompilerThread::insertOutputs(std::stringstream& OS, const std::vector<ParamType>&)
	{
		for (auto& i : reg_table)
		{
			if (i.need_declare)
			{
				// All outputs must be declared always to allow setting default values.
				// NV4097_SET_SHADE_MODE applies to the front/back diffuse and specular colors.
				const bool flat_color = (m_prog.ctrl & RSX_SHADER_CONTROL_FLAT_SHADING) &&
					(i.name.starts_with("diff_color"sv) || i.name.starts_with("spec_color"sv));
				OS << "layout(location=" << varying_location(i.name) << ") out "
					<< (flat_color ? "flat " : "") << "vec4 " << i.name << ";\n";
			}
		}

		if (!(m_prog.ctrl & RSX_SHADER_CONTROL_INTERPRETER_MODEL))
		{
			OS << "layout(location=" << varying_location("usr") << ") out flat uvec4 draw_params_payload;\n";
		}

		if (m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE)
		{
			OS << "layout(location=" << varying_location("depth_range") << ") out flat vec2 depth_range;\n";
		}
	}

	void MTLVertexDecompilerThread::insertFSExport(std::stringstream& OS)
	{
		OS <<
			"void write_fs_payload()\n"
			"{\n"
			"	draw_params_payload.x = get_draw_params().fs_constants_offset;\n"
			"	draw_params_payload.y = get_draw_params().fs_context_offset;\n"
			"	draw_params_payload.z = get_draw_params().fs_texture_base_index;\n"
			"	draw_params_payload.w = get_draw_params().fs_stipple_pattern_offset;\n"
			"}\n\n";
	}

	void MTLVertexDecompilerThread::insertMainStart(std::stringstream& OS)
	{
		glsl::shader_properties properties2{};
		properties2.domain = glsl::glsl_vertex_program;
		properties2.require_lit_emulation = properties.has_lit_op;
		properties2.require_clip_plane_functions = true;
		properties2.emulate_zclip_transform = true;
		// Apple silicon has no guaranteed fp64; the fallback transform is fp32.
		properties2.emulate_depth_clip_only = false;
		properties2.require_instanced_render = !!(m_prog.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS);
		properties2.emulate_depth_range = !!(m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE);

		glsl::insert_glsl_legacy_function(OS, properties2);
		glsl::insert_vertex_input_fetch(OS, glsl::glsl_rules_vulkan);

		insertFSExport(OS);

		// Declare global registers with optional initialization
		std::string registers;
		if (ParamType* vec4Types = m_parr.SearchParam(PF_PARAM_OUT, "vec4"))
		{
			for (auto& PI : vec4Types->items)
			{
				if (registers.length())
				{
					registers += ", ";
				}
				else
				{
					registers = "vec4 ";
				}

				registers += PI.name;

				if (!PI.value.empty())
				{
					if (PI.value == "vec4(0.0, 0.0, 0.0, 0.0)")
					{
						registers += " = vec4(0.)";
					}
					else
					{
						registers += " = " + PI.value;
					}
				}
			}
		}

		if (!registers.empty())
		{
			OS << registers << ";\n";
		}

		OS <<
			"#define scale_offset_mat get_vertex_context().scale_offset_mat\n"
			"#define transform_branch_bits get_vertex_context().transform_branch_bits\n"
			"#define point_size get_vertex_context().point_size\n"
			"#define z_near get_vertex_context().z_near\n"
			"#define z_far get_vertex_context().z_far\n\n";

		OS << "void vs_main()\n";
		OS << "{\n";

		//Declare temporary registers, ignoring those mapped to outputs
		for (const ParamType& PT : m_parr.params[PF_PARAM_NONE])
		{
			for (const ParamItem& PI : PT.items)
			{
				if (PI.name.starts_with("dst_reg"))
				{
					continue;
				}

				OS << "	" << PT.type << " " << PI.name;
				if (!PI.value.empty())
				{
					OS << " = " << PI.value;
				}

				OS << ";\n";
			}
		}

		for (const ParamType& PT : m_parr.params[PF_PARAM_IN])
		{
			for (const ParamItem& PI : PT.items)
			{
				OS << "	vec4 " << PI.name << "= read_location(" << std::to_string(PI.location) << ");\n";
			}
		}

		OS << "\nuint xform_constants_offset = get_draw_params().xform_constants_offset;\n\n";
	}

	void MTLVertexDecompilerThread::insertMainEnd(std::stringstream& OS)
	{
		OS << "}\n\n";

		OS << "void main ()\n";
		OS << "{\n\n";

		OS << "	vs_main();\n\n";

		// FS payload
		OS << "write_fs_payload();\n\n";

		for (auto& i : reg_table)
		{
			if (!i.check_mask || i.test(m_prog.output_mask))
			{
				if (m_parr.HasParam(PF_PARAM_OUT, "vec4", i.src_reg))
				{
					std::string condition = (!i.cond.empty()) ? "(" + i.cond + ") " : "";

					if (condition.empty() || i.default_val.empty())
					{
						if (!condition.empty())
						{
							condition = "if " + condition;
						}
						OS << "	" << condition << i.name << " = " << i.src_reg << i.src_reg_mask << ";\n";
					}
					else
					{
						OS << "	" << i.name << " = " << condition << "? " << i.src_reg << i.src_reg_mask << ": " << i.default_val << ";\n";
					}

					continue;
				}
			}

			if (i.need_declare)
			{
				OS << "	" << i.name << " = vec4(0., 0., 0., 1.);\n";
			}
			else if (i.check_mask_value == CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE)
			{
				OS << "	gl_PointSize = point_size;\n";
			}
		}

		OS << "	gl_Position = gl_Position * scale_offset_mat;\n";

		if (!(m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE)) [[ likely ]]
		{
			OS << "	gl_Position = apply_zclip_xform(gl_Position, z_near, z_far);\n";
		}
		else
		{
			OS <<
				"	depth_range = vec2(min(z_near, z_far), max(z_near, z_far));\n"
				"	gl_Position = apply_depth_range_xform(gl_Position, depth_range);\n";
		}

		OS << "}\n";
	}

	void MTLVertexDecompilerThread::Task()
	{
		m_shader = Decompile();
	}

	std::string decompile_vertex_glsl(const RSXVertexProgram& prog, std::vector<u16>* constant_ids, bool* has_indexed_constants)
	{
		std::string shader;
		MTLVertexDecompilerThread decompiler(prog, shader);
		decompiler.Task();

		// The decompiler rewrites every constant read to a compact index, so the
		// upload must supply the relocated file (GLVertexProgram.cpp:344-345,
		// GLGSRender.cpp:1159-1171).
		if (constant_ids)
		{
			*constant_ids = std::vector<u16>(decompiler.m_constant_ids.begin(), decompiler.m_constant_ids.end());
		}

		if (has_indexed_constants)
		{
			*has_indexed_constants = decompiler.properties.has_indexed_constants;
		}

		return shader;
	}
}

#endif // RSX_ENABLE_METAL4
