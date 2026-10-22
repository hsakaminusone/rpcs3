#include "MTLFragmentProgram.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"
#include "MTLVertexProgram.h"

#include "../Program/GLSLCommon.h"

#include "Emu/system_config.h"
#include "util/logs.hpp"

#include <iterator>
#include <set>
#include <string>
#include <utility>

namespace mtl
{
	MTLFragmentDecompilerThread::MTLFragmentDecompilerThread(const RSXFragmentProgram& prog, u32& size, std::string& shader)
		: FragmentProgramDecompiler(prog, size)
		, m_shader(shader)
	{
	}

	std::string MTLFragmentDecompilerThread::getFloatTypeName(usz elementCount)
	{
		return glsl::getFloatTypeNameImpl(elementCount);
	}

	std::string MTLFragmentDecompilerThread::getHalfTypeName(usz elementCount)
	{
		return glsl::getHalfTypeNameImpl(elementCount);
	}

	std::string MTLFragmentDecompilerThread::getFunction(FUNCTION f)
	{
		return glsl::getFunctionImpl(f);
	}

	std::string MTLFragmentDecompilerThread::compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1)
	{
		return glsl::compareFunctionImpl(f, Op0, Op1);
	}

	void MTLFragmentDecompilerThread::insertHeader(std::stringstream& OS)
	{
		OS << "#version 450\n";
		OS << "#extension GL_EXT_scalar_block_layout: require\n";
		OS << "#extension GL_EXT_uniform_buffer_unsized_array: require\n";

		if (properties.multisampled_sampler_mask)
		{
			OS << "#extension GL_ARB_shader_texture_image_samples: require\n";
		}

		// The stencil mirror and the depth input are separate textures rather than
		// combined image samplers, so they are read with an unfiltered texel fetch
		// (REVIEW-FINDINGS.md, A9).  The extension also carries textureSize and
		// textureSamples on a separate texture.
		if (properties.redirected_sampler_mask || (m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE))
		{
			OS << "#extension GL_EXT_samplerless_texture_functions: require\n";
		}

		if (m_prog.ctrl & RSX_SHADER_CONTROL_ATTRIBUTE_INTERPOLATION)
		{
			OS << "#extension GL_EXT_fragment_shader_barycentric: require\n";
		}

		OS << "#extension GL_ARB_separate_shader_objects: enable\n\n";

		glsl::insert_subheader_block(OS);
	}

	void MTLFragmentDecompilerThread::insertConstants(std::stringstream& OS)
	{
		for (const ParamType& PT : m_parr.params[PF_PARAM_UNIFORM])
		{
			if (!PT.type.starts_with("sampler"))
			{
				continue;
			}

			for (const ParamItem& PI : PT.items)
			{
				std::string samplerType = PT.type;

				const int index = texture_index(PI.name);
				const u32 mask = 1u << static_cast<u32>(index);

				if (properties.multisampled_sampler_mask & mask)
				{
					if (samplerType != "sampler1D" && samplerType != "sampler2D")
					{
						rsx_log.error("Metal: unexpected multisampled image type '%s'", samplerType);
					}

					samplerType = "sampler2DMS";
				}
				else if (properties.shadow_sampler_mask & mask)
				{
					if (properties.common_access_sampler_mask & mask)
					{
						rsx_log.error("Metal: texture unit %d is sampled as both a shadow texture and a depth texture", index);
					}
					else
					{
						samplerType += "Shadow";
					}
				}

				OS << "layout(set=0, binding=" << (texture_slot_base + static_cast<u32>(index)) << ") uniform " << samplerType << " " << PI.name << ";\n";

				if (properties.redirected_sampler_mask & mask)
				{
					// The mirror is a separate texture, not a combined image sampler: a
					// combined one takes a sampler index of its own, and MSL caps that
					// index at 15 while the 16 units already use every one of them
					// (REVIEW-FINDINGS.md, A9).  It is a 2D depth image.
					if (samplerType != "sampler2D" && samplerType != "sampler2DMS")
					{
						rsx_log.error("Metal: unexpected redirected image type '%s'", samplerType);
					}

					const std::string_view mirror_type = (properties.multisampled_sampler_mask & mask)
						? "utexture2DMS"sv
						: "utexture2D"sv;

					OS << "layout(set=0, binding=" << (fragment_stencil_slot_base + static_cast<u32>(index)) << ") uniform " << mirror_type << " " << PI.name << "_stencil;\n";
				}
			}
		}

		if (m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE)
		{
			// A separate texture, for the same reason as the mirror (REVIEW-FINDINGS.md
			// A9): the shared ROP epilogue already reads it with texelFetch.
			const auto frag_depth_type = (m_prog.ctrl & RSX_SHADER_CONTROL_ROP_MULTISAMPLED)
				? "texture2DMS"
				: "texture2D";

			OS << "layout(set=0, binding=" << fragment_depth_input_slot << ") uniform " << frag_depth_type << " frag_depth;\n";
		}

		if (m_prog.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
		{
			// The post-pass turns each subpass input into a [[color(i)]] fetch
			// (HLD-05 §2.4, §3.2), so the Vulkan binding is an intermediate.  The
			// programmable-blend path reads the destination it blends with; the
			// logic-op path reads every output (REVIEW-FINDINGS.md C1).
			const std::string_view att_type = (m_prog.ctrl & RSX_SHADER_CONTROL_ROP_MULTISAMPLED)
				? "subpassInputMS"sv
				: "subpassInput"sv;

			for (u32 i = 0; i < m_prog.mrt_buffers_count; ++i)
			{
				OS << "layout(input_attachment_index= " << i << ", set=0, binding=" << (fragment_subpass_slot_base + i) << ") uniform " << att_type << " frag_src_" << i << ";\n";
			}
		}

		// Draw params are always provided by the vertex program as a varying.
		if (!(m_prog.ctrl & RSX_SHADER_CONTROL_INTERPRETER_MODEL))
		{
			OS << "layout(location=" << varying_location("usr") << ") in flat uvec4 draw_params_payload;\n\n";
		}

		if (m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE)
		{
			OS << "layout(location=" << varying_location("depth_range") << ") in flat vec2 depth_range;\n\n";
		}

		OS <<
			"#define _fs_constants_offset draw_params_payload.x\n"
			"#define _fs_context_offset draw_params_payload.y\n"
			"#define _fs_texture_base_index draw_params_payload.z\n"
			"#define _fs_stipple_pattern_array_offset draw_params_payload.w\n\n";

		if (!properties.constant_offsets.empty())
		{
			OS << "layout(std430, set=0, binding=" << fragment_constants_slot << ") uniform FragmentConstantsBuffer\n";
			OS << "{\n";
			OS << "	vec4 fc[];\n";
			OS << "};\n";
			OS << "#define _fetch_constant(x) fc[x + _fs_constants_offset]\n\n";
		}

		OS <<
			"layout(std430, set=0, binding=" << fragment_state_slot << ") uniform FragmentStateBuffer\n"
			"{\n"
			"	fragment_context_t fs_contexts[];\n"
			"};\n\n";

		OS << "layout(std430, set=0, binding=" << fragment_texture_params_slot << ") uniform TextureParametersBuffer\n";
		OS << "{\n";
		OS << "	sampler_info texture_parameters[];\n";
		OS << "};\n\n";

		OS << "layout(std430, set=0, binding=" << fragment_stipple_slot << ") readonly buffer RasterizerHeap\n";
		OS << "{\n";
		OS << "	uvec4 stipple_pattern[];\n";
		OS << "};\n\n";

		if (m_prog.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
		{
			// MSL has no push constants; MTLShader.cpp maps this block onto the
			// substituted slot fragment_blend_slot (HLD-05 §3.3, §3.2 †).  The op
			// travels here so a change of op does not recompile the shader
			// (REVIEW-FINDINGS.md C1).
			OS <<
				"layout(push_constant) uniform push_constants_block\n"
				"{\n"
				"	layout(offset = 4) uint blend_eqn;\n"
				"	uint blend_sfactors;\n"
				"	uint blend_dfactors;\n"
				"	vec4 blend_constants;\n"
				"	uint logic_op;\n"
				"};\n\n";
		}
	}

	void MTLFragmentDecompilerThread::insertInputs(std::stringstream& OS)
	{
		glsl::insert_fragment_shader_inputs_block(
			OS,
			glsl::extension_flavour::EXT,
			m_prog,
			m_parr.params[PF_PARAM_IN],
			{
				.two_sided_color = !!(properties.in_register_mask & in_diff_color),
				.two_sided_specular = !!(properties.in_register_mask & in_spec_color)
			},
			varying_location);
	}

	void MTLFragmentDecompilerThread::insertOutputs(std::stringstream& OS)
	{
		const std::pair<std::string, std::string> table[] =
		{
			{ "ocol0", m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r0" : "h0" },
			{ "ocol1", m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r2" : "h4" },
			{ "ocol2", m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r3" : "h6" },
			{ "ocol3", m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS ? "r4" : "h8" },
		};

		// NOTE: We do not skip outputs, the only possible combinations are a(0), b(0), ab(0,1), abc(0,1,2), abcd(0,1,2,3)
		u8 output_index = 0;
		const bool float_type = (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS) || !device_props.has_native_half_support;
		const auto reg_type = float_type ? "vec4" : getHalfTypeName(4);
		for (uint i = 0; i < std::size(table); ++i)
		{
			if (!m_parr.HasParam(PF_PARAM_NONE, reg_type, table[i].second))
			{
				continue;
			}

			if (i >= m_prog.mrt_buffers_count)
			{
				// Dead writes. Declare as temp variables for DCE to clean up.
				OS << "vec4 " << table[i].first << "; // Unused\n";
				continue;
			}

			OS << "layout(location=" << std::to_string(output_index++) << ") " << "out vec4 " << table[i].first << ";\n";
		}
	}

	void MTLFragmentDecompilerThread::insertGlobalFunctions(std::stringstream& OS)
	{
		m_shader_props.domain = glsl::glsl_fragment_program;
		m_shader_props.require_lit_emulation = properties.has_lit_op;
		m_shader_props.fp32_outputs = !!(m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS);
		m_shader_props.require_depth_conversion = properties.redirected_sampler_mask != 0;
		m_shader_props.require_wpos = !!(properties.in_register_mask & in_wpos);
		m_shader_props.require_texture_ops = properties.has_tex_op;
		m_shader_props.require_tex_shadow_ops = properties.shadow_sampler_mask != 0;
		m_shader_props.require_msaa_ops = m_prog.texture_state.multisampled_textures != 0;
		m_shader_props.require_texture_expand = properties.has_exp_tex_op;
		m_shader_props.require_srgb_to_linear = properties.has_upg;
		m_shader_props.require_linear_to_srgb = properties.has_pkg;
		m_shader_props.require_fog_read = properties.in_register_mask & in_fogc;
		m_shader_props.emulate_shadow_compare = device_props.emulate_depth_compare || device_props.emulated_depth_storage;

		m_shader_props.low_precision_tests = false;
		// Apple silicon executes a discarded fragment's stores, so the discard is
		// deferred to the end of main (HLD-05 F1).
		m_shader_props.disable_early_discard = true;
		m_shader_props.supports_native_fp16 = device_props.has_native_half_support;

		m_shader_props.ROP_output_rounding = (g_cfg.video.shader_precision != gpu_preset_level::low) && !!(m_prog.ctrl & RSX_SHADER_CONTROL_8BIT_FRAMEBUFFER);
		m_shader_props.ROP_sRGB_packing = !!(m_prog.ctrl & RSX_SHADER_CONTROL_SRGB_FRAMEBUFFER);
		m_shader_props.ROP_alpha_test = !!(m_prog.ctrl & RSX_SHADER_CONTROL_ALPHA_TEST);
		m_shader_props.ROP_alpha_to_coverage_test = !!(m_prog.ctrl & RSX_SHADER_CONTROL_ALPHA_TO_COVERAGE);
		m_shader_props.ROP_polygon_stipple_test = !!(m_prog.ctrl & RSX_SHADER_CONTROL_POLYGON_STIPPLE);
		m_shader_props.ROP_discard = !!(m_prog.ctrl & RSX_SHADER_CONTROL_USES_KIL);
		m_shader_props.ROP_channel_remap = !!(m_prog.ctrl & RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP);
		m_shader_props.ROP_programmable_blend = !!(m_prog.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING);
		m_shader_props.ROP_logic_op = !!(m_prog.ctrl & RSX_SHADER_CONTROL_LOGIC_OP);

		m_shader_props.require_tex1D_ops = properties.has_tex1D;
		m_shader_props.require_tex2D_ops = properties.has_tex2D;
		m_shader_props.require_tex3D_ops = properties.has_tex3D;
		m_shader_props.require_shadowProj_ops = properties.shadow_sampler_mask != 0 && properties.has_texShadowProj;
		m_shader_props.require_alpha_kill = !!(m_prog.ctrl & RSX_SHADER_CONTROL_TEXTURE_ALPHA_KILL);
		m_shader_props.require_color_format_convert = !!(m_prog.ctrl & RSX_SHADER_CONTROL_TEXTURE_FORMAT_CONVERT);
		m_shader_props.emulate_depth_compare = !!(m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE);
		m_shader_props.ROP_output_multisampled = !!(m_prog.ctrl & RSX_SHADER_CONTROL_ROP_MULTISAMPLED);
		m_shader_props.ROP_emulate_depth_range = !!(m_prog.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE);
		m_shader_props.ROP_depth_export = !!(m_prog.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT) && m_shader_props.ROP_emulate_depth_range;
		m_shader_props.emulated_depth_storage = device_props.emulated_depth_storage;

		if (m_shader_props.require_fog_read)
		{
			OS <<
				"#define fog_param0 fs_contexts[_fs_context_offset].fog_param0\n"
				"#define fog_param1 fs_contexts[_fs_context_offset].fog_param1\n"
				"#define fog_mode fs_contexts[_fs_context_offset].fog_mode\n\n";
		}

		if (m_shader_props.require_wpos)
		{
			OS <<
				"#define wpos_scale fs_contexts[_fs_context_offset].wpos_scale\n"
				"#define wpos_bias fs_contexts[_fs_context_offset].wpos_bias\n\n";
		}

		OS <<
			"#define texture_base_index _fs_texture_base_index\n"
			"#define TEX_PARAM(index) texture_parameters_##index\n\n";

		glsl::insert_glsl_legacy_function(OS, m_shader_props);

		// The shared snippets read the stencil mirror with texture() on a combined
		// image sampler, which MSL cannot express without spending a sampler index the
		// 16 units already use (REVIEW-FINDINGS.md, A9).  The mirror is a separate
		// texture, so the two macros that read it are re-defined here.  Its state is
		// clamp-to-border with an opaque black border, nearest, no mip
		// (MTLSamplerState.cpp:187-196), so a coordinate outside the texture reads zero
		// and one inside reads the nearest texel.
		if (m_shader_props.require_depth_conversion)
		{
			OS <<
				"uint _mtl_stencil_fetch(utexture2D tex, vec2 coord)\n"
				"{\n"
				"	const ivec2 size = textureSize(tex, 0);\n"
				"	const ivec2 texel = clamp(ivec2(coord * vec2(size)), ivec2(0), size - ivec2(1));\n"
				"	const bool inside = all(greaterThanEqual(coord, vec2(0.))) && all(lessThan(coord, vec2(1.)));\n"
				"	return inside ? texelFetch(tex, texel, 0).x : 0u;\n"
				"}\n"
				"#undef ZS_READ\n"
				"// The depth half still comes from the unit, which keeps its sampler.\n"
				"#define ZS_READ(index, coord) vec2(texture(TEX_NAME(index), coord).r, float(_mtl_stencil_fetch(TEX_NAME_STENCIL(index), coord).x))\n\n";
		}

		if (m_shader_props.require_depth_conversion && m_shader_props.require_msaa_ops)
		{
			// The multisampled mirror read is the samplerless counterpart of
			// texelFetch2DMS (RSXFragmentTextureMSAAOpsInternal.glsl:2-11).  That
			// helper is emitted once per combined sampler type, so it cannot take a
			// separate texture; the coordinate derivation is the same.
			OS <<
				"uint _mtl_stencil_fetch_ms(utexture2DMS tex, vec2 coord, sampler_info tex_params)\n"
				"{\n"
				"	const vec2 sample_count = vec2(2., textureSamples(tex) * 0.5);\n"
				"	const vec2 normalized_coords = texture2DMSCoord(_texcoord_xform(coord, tex_params), tex_params.flags);\n"
				"	const ivec2 image_size = ivec2(textureSize(tex) * sample_count);\n"
				"	const vec2 resolve_coords = clamp(vec2(ivec2(normalized_coords * vec2(image_size))), vec2(0.), vec2(image_size - ivec2(1)));\n"
				"	const vec2 aa_coords = floor(resolve_coords / sample_count);\n"
				"	const vec2 sample_loc = fma(aa_coords, -sample_count, resolve_coords);\n"
				"	return texelFetch(tex, ivec2(aa_coords), int(fma(sample_loc.y, sample_count.y, sample_loc.x))).x;\n"
				"}\n"
				"#undef ZS_READ_MS\n"
				"#define ZS_READ_MS(index, coord) vec2(sampleTexture2DMS(TEX_NAME(index), coord, TEX_PARAM(index)).r, float(_mtl_stencil_fetch_ms(TEX_NAME_STENCIL(index), coord, TEX_PARAM(index)).x))\n\n";
		}
	}

	void MTLFragmentDecompilerThread::insertMainStart(std::stringstream& OS)
	{
		std::set<std::string> output_registers;
		if (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_32_BITS_EXPORTS)
		{
			output_registers = { "r0", "r2", "r3", "r4" };
		}
		else
		{
			output_registers = { "h0", "h4", "h6", "h8" };
		}

		if (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			output_registers.insert("r1");
		}

		std::string registers;
		std::string reg_type;
		const auto half4 = getHalfTypeName(4);
		for (auto& reg_name : output_registers)
		{
			const auto type = (reg_name[0] == 'r' || !device_props.has_native_half_support) ? "vec4" : half4;
			if (reg_type == type) [[likely]]
			{
				registers += ", " + reg_name + " = " + type + "(0.)";
			}
			else
			{
				if (!registers.empty())
				{
					registers += ";\n";
				}

				registers += type + " " + reg_name + " = " + type + "(0.)";
			}

			reg_type = type;
		}

		if (!registers.empty())
		{
			OS << registers << ";\n";
		}

		OS << "void fs_main()\n";
		OS << "{\n";

		for (const ParamType& PT : m_parr.params[PF_PARAM_NONE])
		{
			for (const ParamItem& PI : PT.items)
			{
				if (output_registers.find(PI.name) != output_registers.end())
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

		if (properties.has_w_access)
		{
			OS << "	float in_w = (1. / gl_FragCoord.w);\n";
		}

		if (properties.in_register_mask & in_ssa)
		{
			OS << "	vec4 ssa = gl_FrontFacing ? vec4(1.) : vec4(-1.);\n";
		}

		if (properties.in_register_mask & in_wpos)
		{
			OS << "	vec4 wpos = get_wpos();\n";
		}

		if (properties.in_register_mask & in_fogc)
		{
			OS << "	vec4 fogc = fetch_fog_value(fog_mode);\n";
		}

		if (m_prog.two_sided_lighting)
		{
			if (properties.in_register_mask & in_diff_color)
			{
				OS << "	vec4 diff_color = gl_FrontFacing ? diff_color0 : diff_color1;\n";
			}

			if (properties.in_register_mask & in_spec_color)
			{
				OS << "	vec4 spec_color = gl_FrontFacing ? spec_color0 : spec_color1;\n";
			}
		}

		for (u16 i = 0, mask = (properties.common_access_sampler_mask | properties.shadow_sampler_mask); mask != 0; ++i, mask >>= 1)
		{
			if (!(mask & 1))
			{
				continue;
			}

			OS << "	const sampler_info texture_parameters_" << i << " = texture_parameters[texture_base_index + " << i << "];\n";
		}
	}

	void MTLFragmentDecompilerThread::insertMainEnd(std::stringstream& OS)
	{
		OS << "}\n\n";

		OS << "void main()\n";
		OS << "{\n";

		constexpr u32 ROP_control_access_options =
			RSX_SHADER_CONTROL_ALPHA_TEST |
			RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE |
			RSX_SHADER_CONTROL_ROP_OUTPUT_REMAP |
			RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING |
			RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE;

		if (m_prog.ctrl & ROP_control_access_options)
		{
			OS << "	const uint rop_control = fs_contexts[_fs_context_offset].rop_control;\n";

			if (m_prog.ctrl & RSX_SHADER_CONTROL_ALPHA_TEST)
			{
				OS << "	const float alpha_ref = fs_contexts[_fs_context_offset].alpha_ref;\n";
			}

			OS << "\n";
		}

		glsl::insert_rop_init(OS, m_prog.mrt_buffers_count);

		OS << "\n" << "	fs_main();\n\n";

		if (m_prog.ctrl & RSX_SHADER_CONTROL_DISABLE_EARLY_Z)
		{
			// A pseudo-barrier sequence disables early-Z; depth export has stronger
			// semantics than discard (VKFragmentProgram.cpp:578-585).
			OS <<
				"	// Insert pseudo-barrier sequence to disable early-Z\n"
				"	gl_FragDepth = gl_FragCoord.z;\n\n";
		}

		glsl::insert_rop(OS, m_shader_props);

		if (m_prog.ctrl & CELL_GCM_SHADER_CONTROL_DEPTH_EXPORT)
		{
			if (m_parr.HasParam(PF_PARAM_NONE, "vec4", "r1"))
			{
				OS << "	gl_FragDepth = r1.z;\n";
			}
			else
			{
				OS << "	//gl_FragDepth = r1.z;\n";
			}
		}

		glsl::insert_fragment_epilogue(OS, m_shader_props);

		OS << "}\n";
	}

	void MTLFragmentDecompilerThread::emitInterpreterPrelude(std::stringstream& OS)
	{
		insertHeader(OS);
		insertConstants(OS);
	}

	void MTLFragmentDecompilerThread::emitInterpreterGlobals(std::stringstream& OS)
	{
		insertGlobalFunctions(OS);
	}

	void MTLFragmentDecompilerThread::Task()
	{
		// Apple silicon executes fp16 arithmetic natively, so the low precision
		// setting can use f16 registers instead of widening every intermediate
		// (VKFragmentProgram.cpp:76; HLD-05 F6).  Depth compare and depth storage
		// are native, so the two depth emulation paths stay off.
		device_props.has_native_half_support = (g_cfg.video.shader_precision == gpu_preset_level::low);
		device_props.emulate_depth_compare = false;
		device_props.has_low_precision_rounding = false;
		device_props.emulated_depth_storage = false;

		m_shader = Decompile();
	}

	std::string decompile_fragment_glsl(const RSXFragmentProgram& prog, std::vector<u32>* constant_offsets)
	{
		u32 size = 0;
		std::string shader;
		MTLFragmentDecompilerThread decompiler(prog, size, shader);
		decompiler.Task();

		if (constant_offsets)
		{
			*constant_offsets = decompiler.properties.constant_offsets;
		}

		return shader;
	}
}

#endif // RSX_ENABLE_METAL4
