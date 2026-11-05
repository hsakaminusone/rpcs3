// Metal 4 semantics experiment for the RSX Metal 4 backend.  Not part of the
// build, and not used by the emulator; it is the measurement behind
// LOG-FINDINGS.md L70-L74 and CONFLICTS.md X5.  Rebuild and run on the target
// machine before changing the binder or the render-pass sizing:
//
//   clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
//       -framework Metal -framework Foundation \
//       docs/research/metal4/design/EXPERIMENT-metal4-semantics.mm -o /tmp/metal4-semantics
//   /tmp/metal4-semantics
//
// It answers four questions the backend's code depends on:
//  1. Is MTL4ArgumentTable::setAddress(0, i) legal, and does it read back as 0?
//  2. Is setTexture(ResourceID{}, i) legal, and does the sample read 0?
//  3. When is the table captured — at setArgumentTable, at the draw encode, or
//     when the GPU executes the draw?
//  4. Does MTL4RenderPassDescriptor's renderTargetWidth/Height clip or scale the
//     render area when it differs from the attachment's size?

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal/Metal.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

static MTL::Texture* make_rgba8(MTL::Device* dev, uint32_t w, uint32_t h, const void* fill)
{
	MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatRGBA8Unorm, w, h, false);
	d->setStorageMode(MTL::StorageModeShared);
	d->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
	MTL::Texture* t = dev->newTexture(d);
	d->release();

	if (fill)
	{
		t->replaceRegion(MTL::Region::Make2D(0, 0, w, h), 0, fill, w * 4);
	}

	return t;
}

static void print_px(const char* what, MTL::Texture* t, uint32_t x, uint32_t y)
{
	uint8_t px[4] = {};
	t->getBytes(px, 4, MTL::Region::Make2D(x, y, 1, 1), 0);
	printf("%-46s (%2u,%2u) = %02x %02x %02x %02x\n", what, x, y, px[0], px[1], px[2], px[3]);
}

static void print_px4(const char* what, MTL::Texture* t, uint32_t x, uint32_t y)
{
	uint8_t px[16] = {};
	t->getBytes(px, 16, MTL::Region::Make2D(x, y, 4, 1), 0);
	printf("%-46s (%2u,%2u..) =", what, x, y);
	for (int i = 0; i < 4; i++) printf(" %02x%02x%02x%02x", px[i*4+0], px[i*4+1], px[i*4+2], px[i*4+3]);
	printf("\n");
}

static const char* kSource = R"(
#include <metal_stdlib>
using namespace metal;

struct vout { float4 pos [[position]]; };

vertex vout vmain(uint vid [[vertex_id]])
{
	float2 p = float2((vid << 1) & 2, vid & 2);
	vout o;
	o.pos = float4(p * 2.0f - 1.0f, 0.0f, 1.0f);
	return o;
}

fragment float4 fmain(constant uint& v [[buffer(0)]],
                      texture2d<float> tex [[texture(0)]],
                      sampler s [[sampler(0)]])
{
	float4 t = tex.sample(s, float2(0.5f, 0.5f));
	return float4(float(v) / 255.0f, t.r, t.g, 1.0f);
}
)";

int main()
{
	MTL::Device* dev = MTL::CreateSystemDefaultDevice();
	if (!dev) { printf("no device\n"); return 1; }
	printf("device: %s\n", dev->name()->utf8String());

	NS::Error* err = nullptr;
	MTL4::CommandQueue* queue = dev->newMTL4CommandQueue();
	MTL4::Compiler* compiler = dev->newCompiler(MTL4::CompilerDescriptor::alloc()->init(), &err);
	if (!compiler) { printf("compiler: %s\n", err->localizedDescription()->utf8String()); return 1; }

	MTL4::LibraryDescriptor* ld = MTL4::LibraryDescriptor::alloc()->init();
	ld->setSource(NS::String::string(kSource, NS::UTF8StringEncoding));
	MTL::Library* lib = compiler->newLibrary(ld, &err);
	ld->release();
	if (!lib) { printf("library: %s\n", err->localizedDescription()->utf8String()); return 1; }

	MTL4::LibraryFunctionDescriptor* vf = MTL4::LibraryFunctionDescriptor::alloc()->init();
	vf->setLibrary(lib);
	vf->setName(NS::String::string("vmain", NS::UTF8StringEncoding));
	MTL4::LibraryFunctionDescriptor* ff = MTL4::LibraryFunctionDescriptor::alloc()->init();
	ff->setLibrary(lib);
	ff->setName(NS::String::string("fmain", NS::UTF8StringEncoding));

	MTL4::RenderPipelineDescriptor* pd = MTL4::RenderPipelineDescriptor::alloc()->init();
	pd->setVertexFunctionDescriptor(vf);
	pd->setFragmentFunctionDescriptor(ff);
	MTL4::RenderPipelineColorAttachmentDescriptor* ca = pd->colorAttachments()->object(0);
	ca->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
	MTL::RenderPipelineState* pso = compiler->newRenderPipelineState(pd, nullptr, &err);
	if (!pso) { printf("pipeline: %s\n", err->localizedDescription()->utf8String()); return 1; }

	MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
	td->setMaxBufferBindCount(1);
	td->setMaxTextureBindCount(1);
	td->setMaxSamplerStateBindCount(1);
	td->setInitializeBindings(true);
	MTL4::ArgumentTable* table = dev->newArgumentTable(td, &err);
	td->release();
	if (!table) { printf("table: %s\n", err->localizedDescription()->utf8String()); return 1; }

	uint32_t white_px[4 * 4];
	for (auto& v : white_px) v = 0xffffffffu;
	MTL::Texture* white = make_rgba8(dev, 4, 4, white_px);

	uint32_t buf100[4] = {100, 0, 0, 0};
	uint32_t buf200[4] = {200, 0, 0, 0};
	MTL::Buffer* b100 = dev->newBuffer(buf100, sizeof(buf100), MTL::ResourceStorageModeShared);
	MTL::Buffer* b200 = dev->newBuffer(buf200, sizeof(buf200), MTL::ResourceStorageModeShared);

	MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter(MTL::SamplerMinMagFilterNearest);
	sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
	MTL::SamplerState* samp = dev->newSamplerState(sd);
	sd->release();

	// ------------------------------------------------------------------ pass 1
	MTL::Texture* rt = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a0 = rp->colorAttachments()->object(0);
	a0->setTexture(rt);
	a0->setLoadAction(MTL::LoadActionClear);
	a0->setStoreAction(MTL::StoreActionStore);
	a0->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp->setRenderTargetWidth(64);
	rp->setRenderTargetHeight(64);

	MTL4::CommandAllocator* alloc = dev->newCommandAllocator();
	MTL4::CommandBuffer* cb = dev->newCommandBuffer();
	cb->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* enc = cb->renderCommandEncoder(rp);
	enc->setRenderPipelineState(pso);

	const auto cell = [&](uint32_t x, uint32_t y) {
		MTL::Viewport vp = { (double)x, (double)y, 16.0, 16.0, 0.0, 1.0 };
		enc->setViewport(vp);
		enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	};

	// A: baseline.  v=100, white texture.
	table->setAddress(b100->gpuAddress(), 0);
	table->setTexture(white->gpuResourceID(), 0);
	table->setSamplerState(samp->gpuResourceID(), 0);
	enc->setArgumentTable(table, MTL::RenderStageVertex);
	enc->setArgumentTable(table, MTL::RenderStageFragment);
	cell(0, 0);

	// B: mutate the table WITHOUT re-binding it, then draw.  If setArgumentTable
	// snapshots, B still reads 100; if the table is live, B reads 200.
	table->setAddress(b200->gpuAddress(), 0);
	cell(16, 0);

	// C: mutate and re-bind.  Must read 200 in either model.
	enc->setArgumentTable(table, MTL::RenderStageFragment);
	cell(32, 0);

	// D: clear the address binding.  Must read 0 if setAddress(0) clears.
	table->setAddress(0, 0);
	enc->setArgumentTable(table, MTL::RenderStageFragment);
	cell(48, 0);

	// E: restore the address, clear the texture.  Must sample 0 if a null
	// ResourceID is a deterministic null.
	table->setAddress(b100->gpuAddress(), 0);
	table->setTexture(MTL::ResourceID(), 0);
	enc->setArgumentTable(table, MTL::RenderStageFragment);
	cell(0, 16);

	// F: restore the texture, clear the sampler.
	table->setTexture(white->gpuResourceID(), 0);
	table->setSamplerState(MTL::ResourceID(), 0);
	enc->setArgumentTable(table, MTL::RenderStageFragment);
	cell(16, 16);

	enc->endEncoding();
	cb->endCommandBuffer();

	// -------------------------------------------------- pass 2: oversize target
	MTL::Texture* rt_big = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp2 = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a2 = rp2->colorAttachments()->object(0);
	a2->setTexture(rt_big);
	a2->setLoadAction(MTL::LoadActionClear);
	a2->setStoreAction(MTL::StoreActionStore);
	a2->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp2->setRenderTargetWidth(128);   // twice the attachment
	rp2->setRenderTargetHeight(128);

	MTL4::CommandBuffer* cb2 = dev->newCommandBuffer();
	cb2->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* e2 = cb2->renderCommandEncoder(rp2);
	e2->setRenderPipelineState(pso);
	table->setAddress(b100->gpuAddress(), 0);
	table->setTexture(white->gpuResourceID(), 0);
	table->setSamplerState(samp->gpuResourceID(), 0);
	e2->setArgumentTable(table, MTL::RenderStageFragment);
	{
		MTL::Viewport vp = { 0.0, 0.0, 64.0, 64.0, 0.0, 1.0 };
		e2->setViewport(vp);
		e2->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	}
	e2->endEncoding();
	cb2->endCommandBuffer();

	// --------------------------------------------------- pass 3: undersize target
	MTL::Texture* rt_small = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp3 = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a3 = rp3->colorAttachments()->object(0);
	a3->setTexture(rt_small);
	a3->setLoadAction(MTL::LoadActionClear);
	a3->setStoreAction(MTL::StoreActionStore);
	a3->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp3->setRenderTargetWidth(32);    // half the attachment
	rp3->setRenderTargetHeight(32);

	MTL4::CommandBuffer* cb3 = dev->newCommandBuffer();
	cb3->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* e3 = cb3->renderCommandEncoder(rp3);
	e3->setRenderPipelineState(pso);
	e3->setArgumentTable(table, MTL::RenderStageFragment);
	{
		MTL::Viewport vp = { 0.0, 0.0, 64.0, 64.0, 0.0, 1.0 };
		e3->setViewport(vp);
		e3->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	}
	e3->endEncoding();
	cb3->endCommandBuffer();

	// ------------------------------- pass 4: matching target, oversize viewport
	MTL::Texture* rt_vp = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp4 = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a4 = rp4->colorAttachments()->object(0);
	a4->setTexture(rt_vp);
	a4->setLoadAction(MTL::LoadActionClear);
	a4->setStoreAction(MTL::StoreActionStore);
	a4->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp4->setRenderTargetWidth(64);
	rp4->setRenderTargetHeight(64);
	MTL4::CommandBuffer* cb4 = dev->newCommandBuffer();
	cb4->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* e4 = cb4->renderCommandEncoder(rp4);
	e4->setRenderPipelineState(pso);
	e4->setArgumentTable(table, MTL::RenderStageFragment);
	{
		MTL::Viewport vp = { 0.0, 0.0, 128.0, 128.0, 0.0, 1.0 };
		e4->setViewport(vp);
		e4->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	}
	e4->endEncoding();
	cb4->endCommandBuffer();

	// ------------------------------- pass 5: same, with a matching scissor rect
	MTL::Texture* rt_vp2 = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp5 = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a5 = rp5->colorAttachments()->object(0);
	a5->setTexture(rt_vp2);
	a5->setLoadAction(MTL::LoadActionClear);
	a5->setStoreAction(MTL::StoreActionStore);
	a5->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp5->setRenderTargetWidth(64);
	rp5->setRenderTargetHeight(64);
	MTL4::CommandBuffer* cb5 = dev->newCommandBuffer();
	cb5->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* e5 = cb5->renderCommandEncoder(rp5);
	e5->setRenderPipelineState(pso);
	e5->setArgumentTable(table, MTL::RenderStageFragment);
	{
		MTL::Viewport vp = { 0.0, 0.0, 128.0, 128.0, 0.0, 1.0 };
		e5->setViewport(vp);
		e5->setScissorRect(MTL::ScissorRect{0, 0, 64, 64});
		e5->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	}
	e5->endEncoding();
	cb5->endCommandBuffer();

	// --------------------------- pass 6: undersize target with a fitting viewport
	MTL::Texture* rt_small2 = make_rgba8(dev, 64, 64, nullptr);
	MTL4::RenderPassDescriptor* rp6 = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a6 = rp6->colorAttachments()->object(0);
	a6->setTexture(rt_small2);
	a6->setLoadAction(MTL::LoadActionClear);
	a6->setStoreAction(MTL::StoreActionStore);
	a6->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp6->setRenderTargetWidth(32);
	rp6->setRenderTargetHeight(32);
	MTL4::CommandBuffer* cb6 = dev->newCommandBuffer();
	cb6->beginCommandBuffer(alloc);
	MTL4::RenderCommandEncoder* e6 = cb6->renderCommandEncoder(rp6);
	e6->setRenderPipelineState(pso);
	e6->setArgumentTable(table, MTL::RenderStageFragment);
	{
		MTL::Viewport vp = { 0.0, 0.0, 32.0, 32.0, 0.0, 1.0 };
		e6->setViewport(vp);
		e6->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	}
	e6->endEncoding();
	cb6->endCommandBuffer();

	const MTL4::CommandBuffer* list[6] = { cb, cb2, cb3, cb4, cb5, cb6 };
	queue->commit(list, 6);

	MTL::SharedEvent* ev = dev->newSharedEvent();
	queue->signalEvent(ev, 1);
	const bool done = ev->waitUntilSignaledValue(1, 10000);
	printf("gpu wait: %s\n\n", done ? "ok" : "TIMEOUT");

	// ---------------------------------------------------------------- results
	printf("== pass 1: 64x64 attachment, renderTarget 64x64 ==\n");
	print_px("A  v=100, white tex",                rt, 8, 8);
	print_px("B  v=200, NO rebind (snapshot?)",     rt, 24, 8);
	print_px("C  v=200, rebind",                    rt, 40, 8);
	print_px("D  setAddress(0)",                    rt, 56, 8);
	print_px("E  setTexture(null), v=100",          rt, 8, 24);
	print_px("F  setSamplerState(null), v=100",     rt, 24, 24);

	printf("\n== pass 2: 64x64 attachment, renderTarget 128x128 (oversize) ==\n");
	print_px4("top-left row",  rt_big, 0, 32);
	print_px4("right edge row", rt_big, 60, 32);

	printf("\n== pass 3: 64x64 attachment, renderTarget 32x32 (undersize) ==\n");
	print_px4("left row",  rt_small, 0, 32);
	print_px4("right row", rt_small, 32, 32);

	printf("\n== pass 4: 64x64/64x64 target, viewport 128x128, default scissor ==\n");
	print_px4("left row  ", rt_vp, 0, 32);
	print_px4("right row ", rt_vp, 32, 32);

	printf("\n== pass 5: 64x64/64x64 target, viewport 128x128, scissor 64x64 ==\n");
	print_px4("left row  ", rt_vp2, 0, 32);
	print_px4("right row ", rt_vp2, 32, 32);

	printf("\n== pass 6: 64x64 attachment, renderTarget 32x32, viewport 32x32 ==\n");
	print_px4("left row  ", rt_small2, 0, 16);
	print_px4("right row ", rt_small2, 32, 16);

	return 0;
}
