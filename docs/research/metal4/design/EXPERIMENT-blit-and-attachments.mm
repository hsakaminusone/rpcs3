// Standalone metal-cpp probe for the four facts the gaps 1-4 design depends on.
// Not part of the build; run it while designing, not at runtime.
//
//   A. Zero-attachment pass: legal with a default sample count, and does the
//      fragment stage run there at all (a fragment-shader atomic proves it)?
//   B. A pass with a colour attachment and a pipeline with no colour attachment:
//      is the rejection at pipeline creation or at the bind?
//   C. Depth-only pass + a pipeline with no colour attachment whose fragment
//      function returns a colour: legal, and does it count fragments?
//   D. Does a DontCare colour attachment with a visibility buffer count
//      rasterised fragments, with and without a DontCare depth attachment?
//   E. Can a float4 fragment output render into a 16-bit attachment
//      (BGR5A1Unorm) with the expected quantisation?
//   F. Can a Depth32Float texture be sampled with texture2d<float> + a filter?
//   G. Is a Metal texture view from Depth32Float to R32Float accepted?
//   H. Can a BGR5A1Unorm texture be sampled as texture2d<float> and rendered into
//      a BGRA8 target (the D5/D6 blit shape)?
//
// Build:
//   clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
//       -framework Metal -framework Foundation \
//       docs/research/metal4/design/EXPERIMENT-blit-and-attachments.mm \
//       -o /tmp/mtl4-blit-attachments
// Run (validation on, so the API's own complaints are visible):
//   MTL_DEBUG_LAYER=1 /tmp/mtl4-blit-attachments
//
// Every case runs in its own child: an API assertion aborts the child, not the
// probe.

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal/Metal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usz = std::size_t;
using f64 = double;

namespace
{
	struct context
	{
		MTL::Device* device = nullptr;
		MTL4::Compiler* compiler = nullptr;
		MTL::Library* library = nullptr;
		MTL4::CommandQueue* queue = nullptr;
		MTL::SharedEvent* event = nullptr;
		u64 signal = 0;
	};

	context g;

	void report(const char* name, bool ok, const char* detail = "")
	{
		std::printf("%-58s %s %s\n", name, ok ? "PASS" : "FAIL", detail);
		std::fflush(stdout);
	}

	const char* source = R"(
#include <metal_stdlib>
using namespace metal;

struct vertex_out
{
	float4 position [[position]];
	float2 uv;
};

vertex vertex_out fs_quad(uint vid [[vertex_id]])
{
	float2 corner = float2(vid & 1, vid >> 1);
	vertex_out out;
	out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	out.uv = corner;
	return out;
}

fragment float4 fs_color(vertex_out in [[stage_in]])
{
	return float4(0.25, 0.5, 0.75, 1.0);
}

fragment float4 fs_texture(vertex_out in [[stage_in]],
	texture2d<float> image [[texture(0)]],
	sampler samp [[sampler(0)]])
{
	return image.sample(samp, in.uv);
}

fragment float4 fs_atomic(vertex_out in [[stage_in]],
	device atomic_uint* counter [[buffer(0)]])
{
	atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
	return float4(1.0);
}
)";

	bool setup()
	{
		g.device = MTL::CreateSystemDefaultDevice();

		if (!g.device)
		{
			return false;
		}

		MTL4::CompilerDescriptor* cd = MTL4::CompilerDescriptor::alloc()->init();
		g.compiler = g.device->newCompiler(cd, nullptr);
		cd->release();
		g.queue = g.device->newMTL4CommandQueue();
		g.event = g.device->newSharedEvent();

		MTL4::LibraryDescriptor* ld = MTL4::LibraryDescriptor::alloc()->init();
		ld->setSource(NS::String::string(source, NS::UTF8StringEncoding));
		NS::Error* error = nullptr;
		g.library = g.compiler->newLibrary(ld, &error);
		ld->release();

		if (!g.library)
		{
			std::printf("  library error: %s\n", error ? error->localizedDescription()->utf8String() : "unknown");
		}

		if (!g.compiler || !g.queue || !g.event || !g.library)
		{
			std::printf("  device %p compiler %p queue %p event %p library %p\n",
				static_cast<void*>(g.device), static_cast<void*>(g.compiler),
				static_cast<void*>(g.queue), static_cast<void*>(g.event),
				static_cast<void*>(g.library));
		}

		return g.compiler && g.queue && g.event && g.library;
	}

	MTL::RenderPipelineState* pipeline(const char* fs, const std::vector<MTL::PixelFormat>& colors)
	{
		MTL4::LibraryFunctionDescriptor* vfd = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vfd->setLibrary(g.library);
		vfd->setName(NS::String::string("fs_quad", NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* ffd = MTL4::LibraryFunctionDescriptor::alloc()->init();
		ffd->setLibrary(g.library);
		ffd->setName(NS::String::string(fs, NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vfd);
		descriptor->setFragmentFunctionDescriptor(ffd);
		descriptor->setRasterSampleCount(1);
		descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);

		for (usz i = 0; i < colors.size(); ++i)
		{
			descriptor->colorAttachments()->object(i)->setPixelFormat(colors[i]);
		}

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = g.compiler->newRenderPipelineState(descriptor, nullptr, &error);

		if (!state)
		{
			std::printf("  pipeline error: %s\n", error ? error->localizedDescription()->utf8String() : "unknown");
		}

		vfd->release();
		ffd->release();
		descriptor->release();
		return state;
	}

	MTL::Texture* texture(MTL::PixelFormat format, u32 width, u32 height, MTL::TextureUsage usage)
	{
		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(MTL::TextureType2D);
		descriptor->setPixelFormat(format);
		descriptor->setWidth(width);
		descriptor->setHeight(height);
		descriptor->setUsage(usage);
		descriptor->setStorageMode(MTL::StorageModeShared);
		MTL::Texture* result = g.device->newTexture(descriptor);
		descriptor->release();
		return result;
	}

	// Encode one full-target quad into `pass` and wait for it.
	bool draw(MTL4::RenderPassDescriptor* pass, NS::UInteger width, NS::UInteger height,
		MTL::RenderPipelineState* state, MTL::Texture* sampled = nullptr,
		MTL::SamplerState* sampler = nullptr, MTL::Buffer* atomic = nullptr)
	{
		pass->setRenderTargetWidth(width);
		pass->setRenderTargetHeight(height);

		MTL4::CommandAllocator* allocator = g.device->newCommandAllocator();
		MTL4::CommandBuffer* buffer = g.device->newCommandBuffer();
		buffer->beginCommandBuffer(allocator);

		MTL4::RenderCommandEncoder* encoder = buffer->renderCommandEncoder(pass);
		bool encoded = false;

		if (encoder && state)
		{
			encoder->setRenderPipelineState(state);
			encoder->setCullMode(MTL::CullModeNone);

			MTL::Viewport viewport{};
			viewport.width = static_cast<f64>(width);
			viewport.height = static_cast<f64>(height);
			viewport.znear = 0.0;
			viewport.zfar = 1.0;
			encoder->setViewport(viewport);

			MTL::ScissorRect scissor{};
			scissor.width = width;
			scissor.height = height;
			encoder->setScissorRect(scissor);

			MTL::DepthStencilDescriptor* dd = MTL::DepthStencilDescriptor::alloc()->init();
			dd->setDepthCompareFunction(MTL::CompareFunctionAlways);
			dd->setDepthWriteEnabled(true);
			MTL::DepthStencilState* ds = g.device->newDepthStencilState(dd);
			dd->release();
			encoder->setDepthStencilState(ds);
			ds->release();

			if (sampled || sampler || atomic)
			{
				MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
				td->setMaxBufferBindCount(1);
				td->setMaxTextureBindCount(1);
				td->setMaxSamplerStateBindCount(1);
				td->setInitializeBindings(true);
				MTL4::ArgumentTable* table = g.device->newArgumentTable(td, nullptr);
				td->release();

				if (atomic)
				{
					table->setAddress(reinterpret_cast<MTL::GPUAddress>(atomic->contents()), 0);
				}

				if (sampled)
				{
					table->setTexture(sampled->gpuResourceID(), 0);
				}

				if (sampler)
				{
					table->setSamplerState(sampler->gpuResourceID(), 0);
				}

				encoder->setArgumentTable(table, MTL::RenderStageFragment);
			}

			if (pass->visibilityResultBuffer())
			{
				encoder->setVisibilityResultMode(MTL::VisibilityResultModeCounting, 0);
			}

			encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 0, 4, 1, 0);
			encoded = true;
		}

		if (encoder)
		{
			encoder->endEncoding();
		}

		buffer->endCommandBuffer();

		const MTL4::CommandBuffer* buffers[] = { buffer };
		g.queue->commit(buffers, 1);
		g.queue->signalEvent(g.event, ++g.signal);

		const bool done = g.event->waitUntilSignaledValue(g.signal, 5000);

		buffer->release();
		allocator->release();
		return encoded && done;
	}

	MTL4::RenderPassDescriptor* color_pass(MTL::Texture* color, MTL::LoadAction load, MTL::StoreAction store)
	{
		MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
		pass->colorAttachments()->object(0)->setTexture(color);
		pass->colorAttachments()->object(0)->setLoadAction(load);
		pass->colorAttachments()->object(0)->setStoreAction(store);
		pass->colorAttachments()->object(0)->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
		return pass;
	}

	MTL::SamplerState* sampler(bool linear)
	{
		MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
		sd->setMinFilter(linear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		sd->setMagFilter(linear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		MTL::SamplerState* result = g.device->newSamplerState(sd);
		sd->release();
		return result;
	}

	// One call runs one case; main re-execs this binary per case.
	void dispatch(char only)
	{
		const std::vector<MTL::PixelFormat> no_colors;
		const std::vector<MTL::PixelFormat> bgra{ MTL::PixelFormatBGRA8Unorm };
		const std::vector<MTL::PixelFormat> bgr565{ MTL::PixelFormatBGR5A1Unorm };
		const bool all = only == 'X';

		if (all || only == 'A')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_atomic", no_colors);
			report("A1 colour-less pipeline built for a colour-less pass", pso != nullptr);

			if (pso)
			{
				MTL::Buffer* atomic = g.device->newBuffer(4, MTL::ResourceStorageModeShared);
				*static_cast<u32*>(atomic->contents()) = 0;

				MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
				pass->setDefaultRasterSampleCount(1);
				const bool ok = draw(pass, 64, 64, pso, nullptr, nullptr, atomic);
				report("A2 zero-attachment pass runs the fragment stage", ok && *static_cast<u32*>(atomic->contents()) == 4096);
				std::printf("   fragment invocations %u of %u\n", *static_cast<u32*>(atomic->contents()), 64u * 64u);
				pass->release();
				atomic->release();
			}
		}

		if (all || only == 'B')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_color", no_colors);
			report("B1 colour-less pipeline object is created", pso != nullptr);

			if (pso)
			{
				MTL::Texture* color = texture(MTL::PixelFormatBGRA8Unorm, 16, 16, MTL::TextureUsageRenderTarget);
				MTL4::RenderPassDescriptor* pass = color_pass(color, MTL::LoadActionDontCare, MTL::StoreActionDontCare);
				const bool ok = draw(pass, 16, 16, pso);
				report("B2 colour pass accepts the colour-less pipeline", ok);
				pass->release();
				color->release();
			}
		}

		if (all || only == 'C')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_atomic", no_colors);
			report("C1 colour-less pipeline built for a depth-only pass", pso != nullptr);

			for (int store = 0; store < 2; ++store)
			{
				MTL::Buffer* atomic = g.device->newBuffer(4, MTL::ResourceStorageModeShared);
				MTL::Buffer* counter = g.device->newBuffer(8, MTL::ResourceStorageModeShared);
				*static_cast<u32*>(atomic->contents()) = 0;
				std::memset(counter->contents(), 0, 8);

				MTL::Texture* depth = texture(MTL::PixelFormatDepth32Float, 64, 64, MTL::TextureUsageRenderTarget);
				MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
				pass->depthAttachment()->setTexture(depth);
				pass->depthAttachment()->setLoadAction(MTL::LoadActionDontCare);
				pass->depthAttachment()->setStoreAction(store ? MTL::StoreActionStore : MTL::StoreActionDontCare);
				pass->setVisibilityResultBuffer(counter);
				pass->setVisibilityResultType(MTL::VisibilityResultTypeAccumulate);
				const bool ok = draw(pass, 64, 64, pso, nullptr, nullptr, atomic);
				const u64 occlusion = *static_cast<const u64*>(counter->contents());
				const u32 fragments = *static_cast<u32*>(atomic->contents());
				report(store ? "C3 depth-only, Store: rasterises" : "C2 depth-only, DontCare: rasterises",
					ok && occlusion == 4096 && fragments == 4096);
				std::printf("   occlusion %llu, fragment invocations %u of %u\n", occlusion, fragments, 64u * 64u);
				pass->release();
				depth->release();
				atomic->release();
				counter->release();
			}
		}

		if (all || only == 'D')
		{
			for (int with_depth = 0; with_depth < 2; ++with_depth)
			for (int store = with_depth ? 0 : 0; store < (with_depth ? 1 : 2); ++store)
			{
				// Colour pass -> a pipeline with a colour attachment and a colour
				// output.  Depth-only pass -> a colour-less pipeline.
				MTL::RenderPipelineState* pso = with_depth ? pipeline("fs_atomic", no_colors) : pipeline("fs_color", bgra);
				MTL::Buffer* counter = g.device->newBuffer(8, MTL::ResourceStorageModeShared);
				MTL::Buffer* atomic = g.device->newBuffer(4, MTL::ResourceStorageModeShared);
				std::memset(counter->contents(), 0, 8);
				*static_cast<u32*>(atomic->contents()) = 0;

				MTL::Texture* color = nullptr;
				MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();

				if (!with_depth)
				{
					color = texture(MTL::PixelFormatBGRA8Unorm, 64, 64, MTL::TextureUsageRenderTarget);
					pass->colorAttachments()->object(0)->setTexture(color);
					pass->colorAttachments()->object(0)->setLoadAction(MTL::LoadActionDontCare);
					pass->colorAttachments()->object(0)->setStoreAction(store ? MTL::StoreActionStore : MTL::StoreActionDontCare);
				}

				MTL::Texture* depth = nullptr;

				if (with_depth)
				{
					depth = texture(MTL::PixelFormatDepth32Float, 64, 64, MTL::TextureUsageRenderTarget);
					pass->depthAttachment()->setTexture(depth);
					pass->depthAttachment()->setLoadAction(MTL::LoadActionDontCare);
					pass->depthAttachment()->setStoreAction(MTL::StoreActionDontCare);
				}

				pass->setVisibilityResultBuffer(counter);
				pass->setVisibilityResultType(MTL::VisibilityResultTypeAccumulate);

				const bool ok = draw(pass, 64, 64, pso, nullptr, nullptr, with_depth ? atomic : nullptr);
				const u64 value = *static_cast<const u64*>(counter->contents());
				const u32 fragments = *static_cast<const u32*>(atomic->contents());
				if (with_depth)
				{
					report("D3 depth-only pass: occlusion + fragments", ok && value == 4096 && fragments == 4096);
				}
				else
				{
					report(store ? "D1 colour Store pass: occlusion" : "D0 colour DontCare pass: occlusion", ok && value == 4096);
				}
				std::printf("   occlusion %llu, fragments %u of %u\n", value, fragments, 64u * 64u);

				pass->release();

				if (color)
				{
					color->release();
				}

				if (depth)
				{
					depth->release();
				}

				counter->release();
				atomic->release();

				if (pso)
				{
					pso->release();
				}
			}
		}

		if (all || only == 'E')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_color", bgr565);
			report("E1 BGR5A1 pipeline built", pso != nullptr);

			if (pso)
			{
				MTL::Texture* color = texture(MTL::PixelFormatBGR5A1Unorm, 16, 16, MTL::TextureUsageRenderTarget);
				MTL4::RenderPassDescriptor* pass = color_pass(color, MTL::LoadActionClear, MTL::StoreActionStore);
				const bool ok = draw(pass, 16, 16, pso);

				u16 texel = 0;
				MTL::Region region{};
				region.size = MTL::Size::Make(1, 1, 1);
				color->getBytes(&texel, 2, region, 0);
				report("E2 16-bit attachment takes the float4 output", ok);
				std::printf("   texel 0x%04x (0.25/0.5/0.75 -> r=8,g=16,b=5 packed BGR5A1)\n", texel);

				pass->release();
				color->release();
			}
		}

		if (all || only == 'F')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_texture", bgra);
			report("F1 sampling pipeline built", pso != nullptr);

			if (pso)
			{
				MTL::Texture* depth = texture(MTL::PixelFormatDepth32Float, 16, 16, MTL::TextureUsageShaderRead);
				MTL::SamplerState* s = sampler(false);
				MTL::Texture* color = texture(MTL::PixelFormatBGRA8Unorm, 16, 16, MTL::TextureUsageRenderTarget);
				MTL4::RenderPassDescriptor* pass = color_pass(color, MTL::LoadActionDontCare, MTL::StoreActionStore);
				const bool ok = draw(pass, 16, 16, pso, depth, s);
				report("F2 depth texture sampled with a filter sampler", ok);
				pass->release();
				color->release();
				s->release();
				depth->release();
			}
		}

		if (all || only == 'G')
		{
			MTL::Texture* depth32 = texture(MTL::PixelFormatDepth32Float, 16, 16,
			static_cast<MTL::TextureUsage>(MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView));
			MTL::Texture* view32 = depth32->newTextureView(MTL::PixelFormatR32Float);
			report("G1 depth32float view to r32float", view32 != nullptr);

			MTL::Texture* depth16 = texture(MTL::PixelFormatDepth16Unorm, 16, 16,
			static_cast<MTL::TextureUsage>(MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView));
			MTL::Texture* view16 = depth16->newTextureView(MTL::PixelFormatR16Unorm);
			report("G2 depth16unorm view to r16unorm", view16 != nullptr);

			depth32->release();
			depth16->release();

			if (view32)
			{
				view32->release();
			}

			if (view16)
			{
				view16->release();
			}
		}

		if (all || only == 'H')
		{
			MTL::RenderPipelineState* pso = pipeline("fs_texture", bgra);
			report("H1 sampling pipeline built", pso != nullptr);

			if (pso)
			{
				MTL::Texture* src = texture(MTL::PixelFormatBGR5A1Unorm, 16, 16, MTL::TextureUsageShaderRead);
				MTL::SamplerState* s = sampler(true);
				MTL::Texture* color = texture(MTL::PixelFormatBGRA8Unorm, 16, 16, MTL::TextureUsageRenderTarget);
				MTL4::RenderPassDescriptor* pass = color_pass(color, MTL::LoadActionDontCare, MTL::StoreActionStore);
				const bool ok = draw(pass, 16, 16, pso, src, s);
				report("H2 16-bit texture sampled into BGRA8", ok);
				pass->release();
				color->release();
				s->release();
				src->release();
			}
		}
	}
}

int main(int argc, char** argv)
{
	if (argc > 1)
	{
		std::printf("== case %c\n", argv[1][0]);
		std::fflush(stdout);

		if (!setup())
		{
			std::printf("  setup failed\n");
			return 1;
		}

		dispatch(argv[1][0]);
		std::printf("== case %c done\n", argv[1][0]);
		return 0;
	}

	const char cases[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };

	for (const char c : cases)
	{
		const char* arg[] = { argv[0], nullptr, nullptr };

		std::string letter(1, c);
		arg[1] = letter.c_str();

		pid_t pid = 0;
		const int rc = posix_spawn(&pid, argv[0], nullptr, nullptr, const_cast<char**>(arg), environ);

		if (rc != 0)
		{
			std::printf("-- %c spawn failed (%d)\n", c, rc);
			continue;
		}

		int status = 0;
		waitpid(pid, &status, 0);

		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		{
			std::printf("   (case %c died: the API assertion above is the answer)\n", c);
		}
	}

	std::printf("all cases attempted\n");
	return 0;
}
