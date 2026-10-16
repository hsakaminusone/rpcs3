#include "MTLShader.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"

#include "util/logs.hpp"

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
		// RSX programmable blending is a tile framebuffer read, and MSL 2.3 has it
		// natively: an input attachment becomes a [[color(i)]] fragment input
		// (HLD-05 §2.4).  Without this the cross-compiler emits it as a texture
		// binding that nothing can supply.
		options.use_framebuffer_fetch_subpasses = true;
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
}

#endif // RSX_ENABLE_METAL4
