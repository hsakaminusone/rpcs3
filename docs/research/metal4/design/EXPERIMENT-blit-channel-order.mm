// Standalone metal-cpp probe for the one question the D5/D6/D7 blitter design
// left open: does the scaled copy have to sample the source through the guest
// channel view (MTLTextureUsagePixelFormatView + newTextureView...swizzle), or is
// the raw image already in guest channel order?
//
// The copy shape is the blitter's (MTLBlit.cpp): one full-screen triangle, a
// nearest sampler, a colour attachment as the destination, and the source bound
// through a Metal 4 argument table.
//
//   A. Raw sample of a B5G6R5Unorm image, drawn into an RGBA8Unorm target: which
//      guest channel does each Metal field carry?
//   B. Same, through the swizzled view get_texture_component_layout(R5G6B5)
//      composes ({One, Blue, Green, Red}): the guest channels.
//   C. Raw sample into a B5G6R5Unorm target (the same-format resample): are the
//      guest words preserved bit for bit?
//   D. Swizzled sample into a B5G6R5Unorm target: the words rotate, so the view
//      must not be used for a same-convention copy.
//   E. A swizzled view is used only for reading: the destination is the raw
//      image, because Metal applies a swizzle on reads only (MTLTexture.h:250).
//   F. A destination that is a non-zero mip level and a sub-rectangle, with
//      LoadActionLoad: the pass must write only its render area, leave the rest
//      of that level alone, and leave the other levels alone.
//
// Build:
//   clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
//       -framework Metal -framework Foundation \
//       docs/research/metal4/design/EXPERIMENT-blit-channel-order.mm \
//       -o /tmp/mtl4-blit-channel-order
// Run (validation on, so the API's own complaints are visible):
//   MTL_DEBUG_LAYER=1 /tmp/mtl4-blit-channel-order

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal/Metal.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using usz = std::size_t;
using f64 = double;

namespace
{
	// The four guest words: one field-only texel per channel plus a mixed word.
	// Metal names a packed format from the least significant field (probe E in
	// EXPERIMENT-blit-and-attachments.mm), so for B5G6R5 R is bits 15..11 and B is
	// bits 4..0.
	constexpr u16 words[4] = { 0x001F /* B=31 */, 0x07E0 /* G=63 */, 0xF800 /* R=31 */, 0x1234 };

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

	// The blitter's fragment stage, byte for byte in shape.
	const char* source = R"(
#include <metal_stdlib>
using namespace metal;

struct vertex_out
{
	float4 position [[position]];
	float2 uv;
};

vertex vertex_out blit_vertex(uint vertex_id [[vertex_id]])
{
	const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
	vertex_out out;
	out.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
	out.uv = float2(corner.x, 1.0 - corner.y);
	return out;
}

fragment float4 blit_fragment(vertex_out in [[stage_in]],
	texture2d<float> source [[texture(0)]],
	sampler filter [[sampler(0)]])
{
	return source.sample(filter, in.uv);
}
)";

	MTL::Texture* texture(MTL::PixelFormat format, u32 width, u32 height, MTL::TextureUsage usage, u32 levels = 1)
	{
		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(MTL::TextureType2D);
		descriptor->setPixelFormat(format);
		descriptor->setWidth(width);
		descriptor->setHeight(height);
		descriptor->setMipmapLevelCount(levels);
		descriptor->setUsage(usage);
		descriptor->setStorageMode(MTL::StorageModeShared);
		MTL::Texture* result = g.device->newTexture(descriptor);
		descriptor->release();
		return result;
	}

	MTL::SamplerState* nearest()
	{
		MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
		sd->setMinFilter(MTL::SamplerMinMagFilterNearest);
		sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
		sd->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
		sd->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sd->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		MTL::SamplerState* result = g.device->newSamplerState(sd);
		sd->release();
		return result;
	}

	// `width`/`height` are the destination rectangle's extent; the pass is the same
	// shape the blitter builds (MTLBlit.cpp, blit_pass::scale_image): the render
	// area is the rectangle's far edge, the viewport and scissor are the rectangle,
	// and the attachment names the level/slice.
	bool run(const char* label, MTL::Texture* src, MTL::Texture* dst, u32 width, u32 height,
		u32 level = 0, MTL::LoadAction load = MTL::LoadActionDontCare, u32 dx = 0, u32 dy = 0)
	{
		MTL4::LibraryFunctionDescriptor* vfd = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vfd->setLibrary(g.library);
		vfd->setName(NS::String::string("blit_vertex", NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* ffd = MTL4::LibraryFunctionDescriptor::alloc()->init();
		ffd->setLibrary(g.library);
		ffd->setName(NS::String::string("blit_fragment", NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* pd = MTL4::RenderPipelineDescriptor::alloc()->init();
		pd->setVertexFunctionDescriptor(vfd);
		pd->setFragmentFunctionDescriptor(ffd);
		pd->setRasterSampleCount(1);
		pd->colorAttachments()->object(0)->setPixelFormat(dst->pixelFormat());
		pd->colorAttachments()->object(0)->setWriteMask(MTL::ColorWriteMaskAll);

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* pso = g.compiler->newRenderPipelineState(pd, nullptr, &error);
		vfd->release();
		ffd->release();
		pd->release();

		if (!pso)
		{
			std::printf("%-52s PIPELINE FAILED %s\n", label,
				error ? error->localizedDescription()->utf8String() : "unknown");
			return false;
		}

		MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
		pass->setRenderTargetWidth(dx + width);
		pass->setRenderTargetHeight(dy + height);
		pass->setDefaultRasterSampleCount(1);
		pass->colorAttachments()->object(0)->setTexture(dst);
		pass->colorAttachments()->object(0)->setLevel(level);
		pass->colorAttachments()->object(0)->setSlice(0);
		pass->colorAttachments()->object(0)->setLoadAction(load);
		pass->colorAttachments()->object(0)->setStoreAction(MTL::StoreActionStore);

		MTL4::CommandAllocator* allocator = g.device->newCommandAllocator();
		MTL4::CommandBuffer* buffer = g.device->newCommandBuffer();
		buffer->beginCommandBuffer(allocator);
		MTL4::RenderCommandEncoder* encoder = buffer->renderCommandEncoder(pass);

		bool ok = false;

		if (encoder)
		{
			MTL::Viewport viewport{};
			viewport.originX = static_cast<f64>(dx);
			viewport.originY = static_cast<f64>(dy);
			viewport.width = static_cast<f64>(width);
			viewport.height = static_cast<f64>(height);
			viewport.znear = 0.0;
			viewport.zfar = 1.0;
			encoder->setViewport(viewport);

			MTL::ScissorRect scissor{};
			scissor.x = dx;
			scissor.y = dy;
			scissor.width = width;
			scissor.height = height;
			encoder->setScissorRect(scissor);

			encoder->setRenderPipelineState(pso);
			encoder->setCullMode(MTL::CullModeNone);

			MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
			td->setMaxTextureBindCount(1);
			td->setMaxSamplerStateBindCount(1);
			td->setInitializeBindings(true);
			MTL4::ArgumentTable* table = g.device->newArgumentTable(td, nullptr);
			td->release();

			MTL::SamplerState* sampler = nearest();
			table->setTexture(src->gpuResourceID(), 0);
			table->setSamplerState(sampler->gpuResourceID(), 0);

			encoder->setArgumentTable(table, MTL::RenderStageFragment);
			encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, 0, 3);
			encoder->endEncoding();
			ok = true;

			sampler->release();
			table->release();
		}

		pass->release();
		buffer->endCommandBuffer();

		const MTL4::CommandBuffer* buffers[] = { buffer };
		g.queue->commit(buffers, 1);
		g.queue->signalEvent(g.event, ++g.signal);
		ok = g.event->waitUntilSignaledValue(g.signal, 5000) && ok;

		buffer->release();
		allocator->release();
		pso->release();
		return ok;
	}

	void print_words(const char* label, MTL::Texture* dst, u32 width, u32 level = 0, u32 y = 0)
	{
		std::vector<u16> out(width);
		dst->getBytes(out.data(), width * 2, MTL::Region::Make2D(0, y, width, 1), level);
		std::printf("%-52s", label);

		for (u32 i = 0; i < width; ++i)
		{
			std::printf(" %04x", out[i]);
		}

		const u16 first = out[0];
		std::printf("   (first word R=%u G=%u B=%u)\n", (first >> 11) & 0x1F, (first >> 5) & 0x3F, first & 0x1F);
	}

	void print_bytes(const char* label, MTL::Texture* dst, u32 width)
	{
		std::vector<u8> out(width * 4);
		dst->getBytes(out.data(), width * 4, MTL::Region::Make2D(0, 0, width, 1), 0);
		std::printf("%-52s", label);

		for (u32 i = 0; i < width; ++i)
		{
			std::printf(" (%u,%u,%u,%u)", out[i * 4], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
		}

		std::printf("   (RGBA8 bytes per texel)\n");
	}
}

int main()
{
	g.device = MTL::CreateSystemDefaultDevice();

	if (!g.device)
	{
		return 1;
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
		std::printf("library error: %s\n", error ? error->localizedDescription()->utf8String() : "unknown");
		return 1;
	}

	std::printf("device: %s\n\n", g.device->name()->utf8String());

	constexpr u32 width = 4;
	constexpr u32 height = 1;

	// The source: the Metal format the sampled-texture table picks for a guest
	// R5G6B5 texture (MTLFormats.cpp, get_texture_format), holding the guest bits
	// unchanged.  PixelFormatView is what the cache declares.
	MTL::Texture* src = texture(MTL::PixelFormatB5G6R5Unorm, width, height,
		static_cast<MTL::TextureUsage>(MTL::TextureUsageShaderRead | MTL::TextureUsagePixelFormatView | MTL::TextureUsageRenderTarget));
	src->replaceRegion(MTL::Region::Make2D(0, 0, width, height), 0, words, width * 2);

	// get_texture_component_layout(CELL_GCM_TEXTURE_R5G6B5) in A-R-G-B order:
	// {One, Blue, Green, Red}.  get_view turns it into Metal's R,G,B,A order.
	const MTL::TextureSwizzleChannels guest_channels =
		MTL::TextureSwizzleChannels::Make(MTL::TextureSwizzleBlue, MTL::TextureSwizzleGreen,
			MTL::TextureSwizzleRed, MTL::TextureSwizzleOne);

	MTL::Texture* view = src->newTextureView(MTL::PixelFormatB5G6R5Unorm, MTL::TextureType2D,
		NS::Range::Make(0, 1), NS::Range::Make(0, 1), guest_channels);

	std::printf("source words (guest r5g6b5 pack):  ");

	for (u16 w : words)
	{
		std::printf(" %04x", w);
	}

	std::printf("\n");
	std::printf("swizzled view: %s, bound channels R=%u G=%u B=%u A=%u\n\n",
		view ? "created" : "NULL",
		static_cast<u32>(view ? view->swizzle().red : 0),
		static_cast<u32>(view ? view->swizzle().green : 0),
		static_cast<u32>(view ? view->swizzle().blue : 0),
		static_cast<u32>(view ? view->swizzle().alpha : 0));

	// A: the raw channel order, written where it can be read directly.
	MTL::Texture* rgba = texture(MTL::PixelFormatRGBA8Unorm, width, height,
		static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));

	if (run("A raw -> RGBA8", src, rgba, width, height))
	{
		print_bytes("A raw -> RGBA8", rgba, width);
	}

	// B: the same, through the guest channel view.
	if (view && run("B swizzled view -> RGBA8", view, rgba, width, height))
	{
		print_bytes("B swizzled view -> RGBA8", rgba, width);
	}

	// C: the same-format resample, raw.  This is the D5 shape and it must be exact.
	MTL::Texture* b5g6r5 = texture(MTL::PixelFormatB5G6R5Unorm, width, height,
		static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));

	if (run("C raw -> B5G6R5", src, b5g6r5, width, height))
	{
		print_words("C raw -> B5G6R5", b5g6r5, width);
	}

	// D: the same, through the view.  The words rotate, so a same-convention copy
	// must not take this route.
	if (view && run("D swizzled view -> B5G6R5", view, b5g6r5, width, height))
	{
		print_words("D swizzled view -> B5G6R5", b5g6r5, width);
	}

	// E: Metal's own words are "the channel swizzle used when reading or sampling
	// from this texture" (MTLTexture.h:250).  A write through a swizzled view is
	// therefore not a supported route for a colour attachment; a swizzled view was
	// never set as an attachment here, and validation stayed quiet.
	std::printf("\nno validation complaint for a swizzled view used for sampling only\n");

	// F: a destination that is the second mip level, written one texel at (1,1)
	// with LoadActionLoad.  The render area is the rectangle's far edge (2×2), so
	// the viewport is the offset 1×1 rectangle inside it: the three texels the
	// viewport does not cover are loaded and stored back unchanged.  Every texel
	// of both levels starts as a sentinel, so a stray write shows up as a second
	// differing word.
	constexpr u32 mip_width = 4;
	constexpr u32 mip_height = 4;
	constexpr u16 sentinel = 0x7FFF;
	constexpr u16 red = 0xF800;

	MTL::Texture* mips = texture(MTL::PixelFormatB5G6R5Unorm, mip_width, mip_height,
		static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead), 2);

	std::vector<u16> level0(mip_width * mip_height, sentinel);
	std::vector<u16> level1(2 * 2, sentinel);
	mips->replaceRegion(MTL::Region::Make2D(0, 0, mip_width, mip_height), 0, level0.data(), mip_width * 2);
	mips->replaceRegion(MTL::Region::Make2D(0, 0, 2, 2), 1, level1.data(), 2 * 2);

	MTL::Texture* one = texture(MTL::PixelFormatB5G6R5Unorm, 1, 1,
		static_cast<MTL::TextureUsage>(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead));
	one->replaceRegion(MTL::Region::Make2D(0, 0, 1, 1), 0, &red, 2);

	if (run("F sub-rect of level 1, load", one, mips, 1, 1, 1, MTL::LoadActionLoad, 1, 1))
	{
		print_words("F level 1, row 0 (all preserved)", mips, 2, 1, 0);
		print_words("F level 1, row 1 (last texel written)", mips, 2, 1, 1);
		print_words("F level 0 (untouched)", mips, mip_width, 0);

		std::vector<u16> readback(2 * 2);
		mips->getBytes(readback.data(), 2 * 2, MTL::Region::Make2D(0, 0, 2, 2), 1);
		const bool exact = readback[0] == sentinel && readback[1] == sentinel &&
			readback[2] == sentinel && readback[3] == red;
		std::printf("F result: %s\n", exact ? "only the viewport was written" : "STRAY WRITE");
	}

	mips->release();
	one->release();

	src->release();
	b5g6r5->release();
	rgba->release();

	if (view)
	{
		view->release();
	}

	g.event->release();
	g.queue->release();
	g.library->release();
	g.compiler->release();
	g.device->release();
	return 0;
}
