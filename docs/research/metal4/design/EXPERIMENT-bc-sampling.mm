// Metal 4 BC (DXT) sampling experiment for the RSX Metal 4 backend.  Not part
// of the build and not used by the emulator.  It measures whether this device
// actually samples BC1/BC3 texels, and whether a BC3 cubemap is sampled from
// the face the shader asks for.
//
// TODO(excise): diagnostic experiment for the black-texture investigation.
// Delete together with the black-texture findings once the backend is fixed.
//
// Build and run on the target machine:
//
//   clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
//       -framework Metal -framework Foundation \
//       docs/research/metal4/design/EXPERIMENT-bc-sampling.mm -o /tmp/metal4-bc
//   /tmp/metal4-bc

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal/Metal.hpp"

#include <cstdio>
#include <cstring>

static MTL::Texture* make_rgba8(MTL::Device* dev, uint32_t w, uint32_t h)
{
	MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatRGBA8Unorm, w, h, false);
	d->setStorageMode(MTL::StorageModeShared);
	d->setUsage(MTL::TextureUsageRenderTarget | MTL::TextureUsageShaderRead);
	MTL::Texture* t = dev->newTexture(d);
	d->release();
	return t;
}

static void print_px(const char* what, MTL::Texture* t, uint32_t x, uint32_t y)
{
	uint8_t px[4] = {};
	t->getBytes(px, 4, MTL::Region::Make2D(x, y, 1, 1), 0);
	printf("%-40s (%2u,%2u) = %02x %02x %02x %02x\n", what, x, y, px[0], px[1], px[2], px[3]);
}

// One BC3 block: alpha = 255 everywhere, colour = `rgb565` everywhere.
static void make_bc3_block(uint8_t* out, uint16_t rgb565)
{
	// alpha block: a0=255, a1=255, 48 bits of index 0.
	out[0] = 0xff; out[1] = 0xff;
	std::memset(out + 2, 0, 6);
	// colour block: c0 = rgb565, c1 = rgb565, 32 bits of index 0.
	out[8] = uint8_t(rgb565 & 0xff); out[9] = uint8_t(rgb565 >> 8);
	out[10] = uint8_t(rgb565 & 0xff); out[11] = uint8_t(rgb565 >> 8);
	std::memset(out + 12, 0, 4);
}

// One BC1 block: c0 = rgb565 (4-colour mode), index 0 everywhere.
static void make_bc1_block(uint8_t* out, uint16_t rgb565)
{
	out[0] = uint8_t(rgb565 & 0xff); out[1] = uint8_t(rgb565 >> 8);
	out[2] = 0x00; out[3] = 0x00;
	std::memset(out + 4, 0, 4);
}

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
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

fragment float4 f2d(texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]])
{
	return float4(tex.sample(s, float2(0.5f, 0.5f)).rgb, 1.0f);
}

fragment float4 fcube(texturecube<float> tex [[texture(0)]], sampler s [[sampler(0)]],
                      constant float4& dir [[buffer(0)]])
{
	return float4(tex.sample(s, dir.xyz).rgb, 1.0f);
}
)";

int main()
{
	MTL::Device* dev = MTL::CreateSystemDefaultDevice();
	if (!dev) { printf("no device\n"); return 1; }

	printf("device: %s\n", dev->name()->utf8String());
	printf("supportsBCTextureCompression: %s\n", dev->supportsBCTextureCompression() ? "true" : "false");

	NS::Error* err = nullptr;
	MTL4::CommandQueue* queue = dev->newMTL4CommandQueue();
	MTL4::Compiler* compiler = dev->newCompiler(MTL4::CompilerDescriptor::alloc()->init(), &err);
	if (!compiler) { printf("compiler: %s\n", err->localizedDescription()->utf8String()); return 1; }

	MTL4::LibraryDescriptor* ld = MTL4::LibraryDescriptor::alloc()->init();
	ld->setSource(NS::String::string(kSource, NS::UTF8StringEncoding));
	MTL::Library* lib = compiler->newLibrary(ld, &err);
	ld->release();
	if (!lib) { printf("library: %s\n", err->localizedDescription()->utf8String()); return 1; }

	auto make_pso = [&](const char* name) -> MTL::RenderPipelineState* {
		MTL4::LibraryFunctionDescriptor* vf = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vf->setLibrary(lib);
		vf->setName(NS::String::string("vmain", NS::UTF8StringEncoding));
		MTL4::LibraryFunctionDescriptor* ff = MTL4::LibraryFunctionDescriptor::alloc()->init();
		ff->setLibrary(lib);
		ff->setName(NS::String::string(name, NS::UTF8StringEncoding));
		MTL4::RenderPipelineDescriptor* pd = MTL4::RenderPipelineDescriptor::alloc()->init();
		pd->setVertexFunctionDescriptor(vf);
		pd->setFragmentFunctionDescriptor(ff);
		pd->colorAttachments()->object(0)->setPixelFormat(MTL::PixelFormatRGBA8Unorm);
		MTL::RenderPipelineState* pso = compiler->newRenderPipelineState(pd, nullptr, &err);
		if (!pso) printf("pipeline %s: %s\n", name, err->localizedDescription()->utf8String());
		return pso;
	};

	MTL::RenderPipelineState* pso2d = make_pso("f2d");
	MTL::RenderPipelineState* psocube = make_pso("fcube");
	if (!pso2d || !psocube) return 1;

	MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
	td->setMaxBufferBindCount(1);
	td->setMaxTextureBindCount(1);
	td->setMaxSamplerStateBindCount(1);
	td->setInitializeBindings(true);
	MTL4::ArgumentTable* table = dev->newArgumentTable(td, &err);
	td->release();
	if (!table) { printf("table: %s\n", err->localizedDescription()->utf8String()); return 1; }

	MTL::SamplerDescriptor* sd = MTL::SamplerDescriptor::alloc()->init();
	sd->setMinFilter(MTL::SamplerMinMagFilterNearest);
	sd->setMagFilter(MTL::SamplerMinMagFilterNearest);
	sd->setMipFilter(MTL::SamplerMipFilterNearest);
	MTL::SamplerState* samp = dev->newSamplerState(sd);
	sd->release();

	// ------------------------------------------------------------- textures
	// 4x4 BC3, solid red.
	uint8_t bc3_block[16];
	make_bc3_block(bc3_block, rgb(255, 0, 0));

	MTL::TextureDescriptor* d3 = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatBC3_RGBA, 4, 4, false);
	d3->setStorageMode(MTL::StorageModeShared);
	d3->setUsage(MTL::TextureUsageShaderRead);
	MTL::Texture* tex_bc3 = dev->newTexture(d3);
	d3->release();
	tex_bc3->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, bc3_block, 16);

	// 4x4 BC3 with two mip levels, level 0 uploaded, level 1 left untouched.
	MTL::TextureDescriptor* d3m = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatBC3_RGBA, 4, 4, true);
	d3m->setStorageMode(MTL::StorageModeShared);
	d3m->setUsage(MTL::TextureUsageShaderRead);
	MTL::Texture* tex_bc3_mip = dev->newTexture(d3m);
	d3m->release();
	tex_bc3_mip->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, bc3_block, 16);

	// 4x4 BC1, solid green.
	uint8_t bc1_block[8];
	make_bc1_block(bc1_block, rgb(0, 255, 0));
	MTL::TextureDescriptor* d1 = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatBC1_RGBA, 4, 4, false);
	d1->setStorageMode(MTL::StorageModeShared);
	d1->setUsage(MTL::TextureUsageShaderRead);
	MTL::Texture* tex_bc1 = dev->newTexture(d1);
	d1->release();
	tex_bc1->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, bc1_block, 8);

	// 4x4 BC3 cube: face n gets colour n.
	MTL::TextureDescriptor* dc = MTL::TextureDescriptor::textureCubeDescriptor(
		MTL::PixelFormatBC3_RGBA, 4, false);
	dc->setStorageMode(MTL::StorageModeShared);
	dc->setUsage(MTL::TextureUsageShaderRead);
	MTL::Texture* tex_cube = dev->newTexture(dc);
	dc->release();

	const uint8_t face_colors[6][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
		{255, 255, 0}, {255, 0, 255}, {0, 255, 255}
	};

	for (int face = 0; face < 6; ++face)
	{
		uint8_t block[16];
		make_bc3_block(block, rgb(face_colors[face][0], face_colors[face][1], face_colors[face][2]));
		tex_cube->replaceRegion(MTL::Region::Make2D(0, 0, 4, 4), 0, face, block, 16, 16);
	}

	// -------------------------------------------------------------- pass
	MTL::Texture* rt = make_rgba8(dev, 64, 64);
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

	const auto cell2d = [&](uint32_t x, uint32_t y, MTL::Texture* tex) {
		enc->setRenderPipelineState(pso2d);
		table->setTexture(tex->gpuResourceID(), 0);
		table->setSamplerState(samp->gpuResourceID(), 0);
		enc->setArgumentTable(table, MTL::RenderStageFragment);
		MTL::Viewport vp = { (double)x, (double)y, 16.0, 16.0, 0.0, 1.0 };
		enc->setViewport(vp);
		enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
	};

	const auto cellcube = [&](uint32_t x, uint32_t y, float dx, float dy, float dz) {
		static float dirs[8][4];
		static uint32_t next = 0;
		float* dir = dirs[next];
		dir[0] = dx; dir[1] = dy; dir[2] = dz; dir[3] = 0.0f;
		MTL::Buffer* b = dev->newBuffer(dir, sizeof(float) * 4, MTL::ResourceStorageModeShared);
		enc->setRenderPipelineState(psocube);
		table->setAddress(b->gpuAddress(), 0);
		table->setTexture(tex_cube->gpuResourceID(), 0);
		table->setSamplerState(samp->gpuResourceID(), 0);
		enc->setArgumentTable(table, MTL::RenderStageFragment);
		MTL::Viewport vp = { (double)x, (double)y, 16.0, 16.0, 0.0, 1.0 };
		enc->setViewport(vp);
		enc->drawPrimitives(MTL::PrimitiveTypeTriangle, (NS::UInteger)0, (NS::UInteger)3);
		++next;
	};

	cell2d(0, 0, tex_bc3);          // expect red
	cell2d(16, 0, tex_bc3_mip);     // expect red (level 0)
	cell2d(32, 0, tex_bc1);         // expect green

	cellcube(48, 0,   1, 0, 0);     // +X
	cellcube(0, 16,  -1, 0, 0);     // -X
	cellcube(16, 16,  0, 1, 0);     // +Y
	cellcube(32, 16,  0, -1, 0);    // -Y
	cellcube(48, 16,  0, 0, 1);     // +Z
	cellcube(0, 32,   0, 0, -1);    // -Z

	enc->endEncoding();
	cb->endCommandBuffer();

	queue->commit(&cb, 1);
	MTL::SharedEvent* ev = dev->newSharedEvent();
	queue->signalEvent(ev, 1);
	const bool done = ev->waitUntilSignaledValue(1, 10000);
	printf("gpu wait: %s\n\n", done ? "ok" : "TIMEOUT");

	printf("== BC sampling (RGBA8 target) ==\n");
	print_px("BC3 4x4 red (no mips)",        rt, 8, 8);
	print_px("BC3 4x4 red (2 mips, level 0)", rt, 24, 8);
	print_px("BC1 4x4 green",                 rt, 40, 8);

	printf("\n== BC3 cube, faces red green blue yellow magenta cyan ==\n");
	print_px("+X (expect face 0 red)",        rt, 56, 8);
	print_px("-X (expect face 1 green)",      rt, 8, 24);
	print_px("+Y (expect face 2 blue)",       rt, 24, 24);
	print_px("-Y (expect face 3 yellow)",     rt, 40, 24);
	print_px("+Z (expect face 4 magenta)",    rt, 56, 24);
	print_px("-Z (expect face 5 cyan)",       rt, 8, 40);

	printf("\n== texture storage readback (BC3 level 0, first block) ==\n");
	uint8_t raw[16] = {};
	tex_bc3->getBytes(raw, 16, MTL::Region::Make2D(0, 0, 4, 4), 0);
	printf("getBytes:");
	for (uint8_t b : raw) printf(" %02x", b);
	printf("\n");

	return 0;
}
