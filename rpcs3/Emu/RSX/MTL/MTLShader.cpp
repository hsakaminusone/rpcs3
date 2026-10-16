#include "MTLShader.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"

#include "util/logs.hpp"

#include <cctype>
#include <cstdio>

// SPIRV-Cross is third-party.  The include directory is SYSTEM
// (3rdparty/spirv-cross/rpcs3.cmake), and the pragmas keep the diagnostics out
// if the header is ever reached as a normal include (the SPIRVCommon.cpp idiom).
#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#ifdef __clang__
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#endif
#endif
#include <spirv_msl.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

namespace mtl
{
	namespace
	{
		// One SPIRV-Cross resource -> MSL binding.  The Vulkan binding the Metal
		// GLSL wrapper declares is already the fixed RSX slot (HLD-05 §3.1), so
		// the MSL index is that binding.  A combined image sampler becomes a
		// texture and a sampler at the same index (HLD-05 §3.2).
		void remap_resource_binding(spirv_cross::CompilerMSL& compiler, spirv_cross::ExecutionModel stage, const spirv_cross::Resource& resource)
		{
			const u32 set = compiler.has_decoration(resource.id, spv::DecorationDescriptorSet)
				? compiler.get_decoration(resource.id, spv::DecorationDescriptorSet)
				: 0;
			const u32 binding = compiler.has_decoration(resource.id, spv::DecorationBinding)
				? compiler.get_decoration(resource.id, spv::DecorationBinding)
				: 0;

			spirv_cross::MSLResourceBinding msl_binding{};
			msl_binding.stage = stage;
			msl_binding.desc_set = set;
			msl_binding.binding = binding;
			msl_binding.msl_buffer = binding;
			msl_binding.msl_texture = binding;
			msl_binding.msl_sampler = binding;
			compiler.add_msl_resource_binding(msl_binding);
		}

		// A float literal for injected MSL text; `1` without a suffix would be an
		// int in MSL, and `1f` is not a valid literal, so a decimal point is added
		// when the value has no fraction or exponent.
		std::string float_literal(f32 value)
		{
			char buffer[32]{};
			std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
			std::string text = buffer;

			if (text.find_first_of(".eEni") == std::string::npos)
			{
				text += ".0";
			}

			text += "f";
			return text;
		}

		bool is_identifier_char(char c)
		{
			return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
		}

		// The index of the '(' at `open`, or npos.
		usz matching_paren(const std::string& text, usz open)
		{
			if (open >= text.size() || text[open] != '(')
			{
				return std::string::npos;
			}

			int depth = 0;
			for (usz index = open; index < text.size(); ++index)
			{
				if (text[index] == '(')
				{
					++depth;
				}
				else if (text[index] == ')')
				{
					if (--depth == 0)
					{
						return index;
					}
				}
			}

			return std::string::npos;
		}
	}

	bool spv_to_msl(const std::vector<u32>& spv, bool is_vertex, std::string& msl, u32* color_output_mask)
	{
		if (spv.empty())
		{
			rsx_log.error("Metal: the SPIR-V module is empty");
			return false;
		}

		spirv_cross::CompilerMSL compiler(spv);
		spirv_cross::CompilerMSL::Options options{};
		// MSL 2.3 covers the [[color(n)]] framebuffer fetch and the
		// [[user(locnN)]] varyings the RSX map needs (HLD-05 §6 Q5).
		options.set_msl_version(2, 3);
		// The VS reads its attributes from the texture_buffer streams, not from a
		// vertex descriptor (HLD-05 §3.5).
		options.texture_buffer_native = true;
		compiler.set_msl_options(options);

		const spirv_cross::ExecutionModel stage = is_vertex
			? spirv_cross::ExecutionModelVertex
			: spirv_cross::ExecutionModelFragment;

		const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
		const spirv_cross::SmallVector<spirv_cross::Resource>* lists[] =
		{
			&resources.uniform_buffers,
			&resources.storage_buffers,
			&resources.storage_images,
			&resources.sampled_images,
			&resources.separate_images,
			&resources.separate_samplers,
			&resources.atomic_counters,
			&resources.subpass_inputs,
		};

		for (const auto* list : lists)
		{
			for (const spirv_cross::Resource& resource : *list)
			{
				remap_resource_binding(compiler, stage, resource);
			}
		}

		// The two RSX push-constant blocks become constant buffers at the
		// substituted slots (HLD-05 §3.3, §3.2 †; MTLArgumentBinds.h:42-43).
		for (const spirv_cross::Resource& resource : resources.push_constant_buffers)
		{
			(void)resource;
			spirv_cross::MSLResourceBinding msl_binding{};
			msl_binding.stage = stage;
			msl_binding.desc_set = spirv_cross::kPushConstDescSet;
			msl_binding.binding = spirv_cross::kPushConstBinding;
			msl_binding.msl_buffer = is_vertex ? vertex_draw_params_slot : fragment_blend_slot;
			compiler.add_msl_resource_binding(msl_binding);
		}

		if (color_output_mask)
		{
			u32 mask = 0;
			for (const spirv_cross::Resource& output : resources.stage_outputs)
			{
				if (compiler.has_decoration(output.id, spv::DecorationLocation))
				{
					const u32 location = compiler.get_decoration(output.id, spv::DecorationLocation);
					if (location < 8)
					{
						mask |= (1u << location);
					}
				}
			}
			*color_output_mask = mask;
		}

		msl = compiler.compile();
		if (msl.empty())
		{
			rsx_log.error("Metal: SPIRV-Cross produced no MSL");
			return false;
		}

		return true;
	}

	MTL::Library* compile_msl(MTL4::Compiler* compiler, const std::string& msl, NS::Error** error)
	{
		if (!compiler || msl.empty())
		{
			return nullptr;
		}

		MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
		descriptor->setSource(NS::String::string(msl.c_str(), NS::UTF8StringEncoding));

		MTL::Library* library = compiler->newLibrary(descriptor, error);
		descriptor->release();
		return library;
	}

	std::string rewrite_programmable_blend(std::string msl, u32 color_index)
	{
		// The input attachment is the texture whose `.read(` the shader calls.
		const std::string read_token = ".read(";
		const usz read_pos = msl.find(read_token);
		if (read_pos == std::string::npos)
		{
			return msl;
		}

		usz name_start = read_pos;
		while (name_start > 0 && is_identifier_char(msl[name_start - 1]))
		{
			--name_start;
		}

		const std::string name = msl.substr(name_start, read_pos - name_start);
		if (name.empty())
		{
			return msl;
		}

		// The declaration `texture2d<float> NAME [[texture(N)]]` becomes a
		// `[[color(color_index)]]` fragment input of type float4.
		const std::string declaration = "texture2d<float> " + name;
		const usz decl_pos = msl.find(declaration);
		if (decl_pos == std::string::npos)
		{
			return msl;
		}

		const usz attribute_pos = msl.find("[[texture(", decl_pos);
		const usz attribute_end = (attribute_pos == std::string::npos) ? std::string::npos : msl.find("]]", attribute_pos);
		if (attribute_pos == std::string::npos || attribute_end == std::string::npos)
		{
			return msl;
		}

		const std::string attribute = "[[color(" + std::to_string(color_index) + ")]]";
		msl.replace(decl_pos, (attribute_end + 2) - decl_pos, "float4 " + name + " " + attribute);

		// Each `NAME.read(<coordinate>)` becomes the fetched value NAME.
		const std::string call = name + read_token;
		usz pos = 0;
		while ((pos = msl.find(call, pos)) != std::string::npos)
		{
			const usz close = matching_paren(msl, pos + call.size() - 1);
			if (close == std::string::npos)
			{
				break;
			}

			msl.replace(pos, (close + 1) - pos, name);
			pos += name.size();
		}

		return msl;
	}

	std::string rewrite_push_constant_slot(std::string msl, const std::string& block_name, u32 slot)
	{
		if (block_name.empty())
		{
			return msl;
		}

		// SPIRV-Cross emits `constant BLOCK& NAME [[buffer(N)]]`.
		const std::string declaration = "constant " + block_name + "&";
		const usz decl_pos = msl.find(declaration);
		if (decl_pos == std::string::npos)
		{
			return msl;
		}

		const usz attribute_pos = msl.find("[[buffer(", decl_pos);
		const usz attribute_end = (attribute_pos == std::string::npos) ? std::string::npos : msl.find("]]", attribute_pos);
		if (attribute_pos == std::string::npos || attribute_end == std::string::npos)
		{
			return msl;
		}

		msl.replace(attribute_pos, (attribute_end + 2) - attribute_pos, "[[buffer(" + std::to_string(slot) + ")]]");
		return msl;
	}

	std::string inject_sampler_border_and_lod(std::string msl, const sampler_injection& spec)
	{
		if (spec.texture.empty() || spec.sampler.empty())
		{
			return msl;
		}

		// The helper names float4/sampler/texture2d unqualified, so it needs the
		// `using namespace metal;` line.  Without the anchor the rewrite would not
		// compile, so leave the source unchanged.
		const std::string anchor = "using namespace metal;";
		const usz anchor_pos = msl.find(anchor);
		if (anchor_pos == std::string::npos)
		{
			return msl;
		}

		const std::string call = spec.texture + ".sample(" + spec.sampler + ", ";
		usz pos = 0;
		bool rewritten = false;
		while ((pos = msl.find(call, pos)) != std::string::npos)
		{
			const usz close = matching_paren(msl, pos + spec.texture.size() + std::string(".sample").size());
			if (close == std::string::npos)
			{
				break;
			}

			const usz arguments_start = pos + call.size();
			const std::string coordinate = msl.substr(arguments_start, close - arguments_start);
			// Only the two-argument sample form is rewritten; a third LOD
			// argument would need to fold into the bias and is left alone.
			if (coordinate.find(',') != std::string::npos)
			{
				pos = close + 1;
				continue;
			}

			const std::string replacement = "rsx_sampler_fetch(" + spec.texture + ", " + spec.sampler + ", " + coordinate
				+ ", " + float_literal(spec.lod_bias)
				+ ", float4(" + float_literal(spec.border[0]) + ", " + float_literal(spec.border[1])
				+ ", " + float_literal(spec.border[2]) + ", " + float_literal(spec.border[3]) + ")"
				+ ", " + (spec.border_enabled ? "true" : "false") + ")";

			msl.replace(pos, (close + 1) - pos, replacement);
			pos += replacement.size();
			rewritten = true;
		}

		if (!rewritten)
		{
			return msl;
		}

		// The border test is on normalized coordinates; the RSX 2D case the port
		// targets uses normalized UVs.
		const std::string helper =
			"\nstatic inline float4 rsx_sampler_fetch(texture2d<float> tex, sampler smp, float2 uv, float lod_bias, float4 border, bool border_enabled)\n"
			"{\n"
			"\tif (border_enabled && (any(uv < float2(0.0f)) || any(uv > float2(1.0f))))\n"
			"\t{\n"
			"\t\treturn border;\n"
			"\t}\n"
			"\treturn tex.sample(smp, uv, level(lod_bias));\n"
			"}\n";

		msl.insert(anchor_pos + anchor.size(), helper);
		return msl;
	}
}

#endif // RSX_ENABLE_METAL4
