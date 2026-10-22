#include "MTLShader.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"

#include "Emu/system_config.h"

#include "util/logs.hpp"

#include <memory>
#include <utility>

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
		// Which MSL space a SPIRV-Cross resource couples to.  The Vulkan binding the
		// Metal GLSL wrapper declares is already the fixed RSX slot (HLD-05 §3.1), so
		// the texture and buffer indices are that binding.  Only a combined image
		// sampler takes a different sampler index: MSL caps the sampler index at 15
		// and the texture units start at 12, so a unit takes its position inside the
		// stage's texture range instead (REVIEW-FINDINGS.md A9).  The depth mirror and
		// the depth input are separate images, so they take the binding in the texture
		// space and no sampler index at all.
		enum class sampler_coupling
		{
			none,
			unit,
			separate_sampler,
		};

		void remap_resource_binding(spirv_cross::CompilerMSL& compiler, spirv_cross::ExecutionModel stage,
			const spirv_cross::Resource& resource, sampler_coupling coupling)
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

			switch (coupling)
			{
			case sampler_coupling::unit:
				// An RSX texture unit takes its position inside the stage's texture
				// range: the rule lives in the header so the shader side and the table
				// writer cannot disagree.  A combined image sampler outside the units is
				// an overlay pass's own sampler, and the overlay argument table takes
				// the shader's binding as its index (MTLOverlays.cpp binds the sampler
				// state at it), so it keeps the binding.  Two such samplers must not
				// share index zero: the Metal compiler rejects the second declaration
				// with "cannot reserve 'sampler' resource location at index 0".
				msl_binding.msl_sampler = slot_has_sampler(binding) ? texture_sampler_index(binding) : binding;
				break;
			case sampler_coupling::separate_sampler:
				msl_binding.msl_sampler = binding;
				break;
			case sampler_coupling::none:
				break;
			}

			compiler.add_msl_resource_binding(msl_binding);
		}
	}

	// The cross-compiler body.  It is separate from spv_to_msl so the one file can
	// be compiled with exceptions (Emu/CMakeLists.txt): SPIRV-Cross reports an
	// unsupported or malformed module by throwing, and without a catch its own
	// exception-to-assertion form aborts the process instead of failing one
	// pipeline (REALISATION.md, "The remaining risk").
	static bool build_msl(const std::vector<u32>& spv, bool is_vertex, std::string& msl, u32* color_output_mask, u32 push_slot)
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
		// A combined image sampler is the only resource kind that reaches a sampler
		// index the units do not already hold; a bare sampler keeps its binding, and
		// every other kind takes the binding in the buffer and texture spaces only.
		const struct
		{
			const spirv_cross::SmallVector<spirv_cross::Resource>* list;
			sampler_coupling coupling;
		} lists[] =
		{
			{ &resources.uniform_buffers, sampler_coupling::none },
			{ &resources.storage_buffers, sampler_coupling::none },
			{ &resources.storage_images, sampler_coupling::none },
			{ &resources.sampled_images, sampler_coupling::unit },
			{ &resources.separate_images, sampler_coupling::none },
			{ &resources.separate_samplers, sampler_coupling::separate_sampler },
			{ &resources.atomic_counters, sampler_coupling::none },
			{ &resources.subpass_inputs, sampler_coupling::none },
		};

		for (const auto& entry : lists)
		{
			for (const spirv_cross::Resource& resource : *entry.list)
			{
				remap_resource_binding(compiler, stage, resource, entry.coupling);
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
			msl_binding.msl_buffer = (push_slot == rsx_push_slot)
				? (is_vertex ? vertex_draw_params_slot : fragment_blend_slot)
				: push_slot;
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

	bool spv_to_msl(const std::vector<u32>& spv, bool is_vertex, std::string& msl, u32* color_output_mask, u32 push_slot)
	{
		try
		{
			return build_msl(spv, is_vertex, msl, color_output_mask, push_slot);
		}
		catch (...)
		{
			rsx_log.error("Metal: the cross-compiler rejected the module");
			return false;
		}
	}

	MTL::Library* compile_msl(MTL4::Compiler* compiler, const std::string& msl, NS::Error** error)
	{
		if (!compiler || msl.empty())
		{
			return nullptr;
		}

		// The RSX thread has no autorelease pool.  The descriptor builder creates
		// autoreleased NS::String objects, so every compile needs a local pool.
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
		descriptor->setSource(NS::String::string(msl.c_str(), NS::UTF8StringEncoding));

		// The GUI setting asks for precise math; the default stays the fast mode that
		// the backend has always used.
		MTL::CompileOptions* options = MTL::CompileOptions::alloc()->init();
		options->setMathMode(g_cfg.video.disable_msl_fast_math.get() ? MTL::MathModeSafe : MTL::MathModeFast);
		descriptor->setOptions(options);

		MTL::Library* library = compiler->newLibrary(descriptor, error);
		options->release();
		descriptor->release();
		pool->release();
		return library;
	}

	MTL4::CompilerTask* compile_msl_async(MTL4::Compiler* compiler, const std::string& msl,
		MTL::NewLibraryCompletionHandlerFunction handler)
	{
		if (!compiler || msl.empty())
		{
			return nullptr;
		}

		// The descriptor is consumed by the compiler task after this call returns.
		// Keep it and its options alive until the completion handler runs; the
		// synchronous overload copies them during the call, but the asynchronous
		// one serializes the task on a Metal compiler thread.
		struct async_library_owner
		{
			MTL4::LibraryDescriptor* descriptor = nullptr;
			MTL::CompileOptions* options = nullptr;
			MTL::NewLibraryCompletionHandlerFunction handler;

			~async_library_owner()
			{
				if (descriptor)
				{
					descriptor->release();
				}

				if (options)
				{
					options->release();
				}
			}
		};

		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		auto owner = std::make_shared<async_library_owner>();
		owner->descriptor = MTL4::LibraryDescriptor::alloc()->init();
		owner->descriptor->setSource(NS::String::string(msl.c_str(), NS::UTF8StringEncoding));

		owner->options = MTL::CompileOptions::alloc()->init();
		owner->options->setMathMode(g_cfg.video.disable_msl_fast_math.get() ? MTL::MathModeSafe : MTL::MathModeFast);
		owner->descriptor->setOptions(owner->options);
		owner->handler = std::move(handler);

		MTL::NewLibraryCompletionHandlerFunction completion =
			[owner](MTL::Library* library, NS::Error* error)
		{
			NS::AutoreleasePool* completion_pool = NS::AutoreleasePool::alloc()->init();
			owner->handler(library, error);
			completion_pool->release();
		};

		MTL4::CompilerTask* task = compiler->newLibrary(owner->descriptor, std::move(completion));
		pool->release();
		return task;
	}
}

#endif // RSX_ENABLE_METAL4
