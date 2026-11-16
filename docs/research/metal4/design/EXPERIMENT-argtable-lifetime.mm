// Standalone metal-cpp probe for the AGX fault seen in the Demon's Souls run
// build/probe/run-20261114-010643-ds: the RSX thread died inside
// MTL4::RenderCommandEncoder::setArgumentTable:atStages: with
//   si_addr = 0x28a0, PC inside com.apple.AGXMetalG17P
// on the vertex-stage call, on the first draw of the frame after a present.
//
// This is not part of the build and not used by the emulator.  It answers one
// question: which misuse of setArgumentTable:atStages: makes the AGX driver
// dereference null + 0x28a0?  Each case runs in its own child, so a fault in
// one case does not hide the others, and the handler prints the same three
// fields the emulator's signal handler prints.
//
// Build:
//   clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
//       -framework Metal -framework Foundation \
//       docs/research/metal4/design/EXPERIMENT-argtable-lifetime.mm \
//       -o /tmp/mtl4-argtable-lifetime
// Run:
//   /tmp/mtl4-argtable-lifetime

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal/Metal.hpp"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <dlfcn.h>

// ucontext.h (only for the fault PC) is gated behind _XOPEN_SOURCE on macOS,
// and defining that before dlfcn.h hides RTLD_DEFAULT, so it comes last.
#define _XOPEN_SOURCE 700
#include <ucontext.h>

static void fault_handler(int sig, siginfo_t* info, void* uct)
{
	ucontext_t* c = static_cast<ucontext_t*>(uct);
	void* pc = reinterpret_cast<void*>(c->uc_mcontext->__ss.__pc);
	Dl_info di{};
	dladdr(pc, &di);

	char buf[512];
	const int n = std::snprintf(buf, sizeof(buf), "  FAULT sig=%d addr=%p pc=%p image=%s\n",
		sig, info->si_addr, pc, di.dli_fname ? di.dli_fname : "?");
	(void)!write(1, buf, static_cast<size_t>(n));
	_Exit(99);
}

static void install_handlers()
{
	struct sigaction sa{};
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
}

struct objects
{
	MTL::Device* dev = nullptr;
	MTL4::CommandAllocator* alloc = nullptr;
	MTL4::CommandBuffer* cb = nullptr;
	MTL4::ArgumentTable* table = nullptr;
	MTL4::RenderCommandEncoder* enc = nullptr;
};

// The backend's sequence: one allocator, one command buffer, one bounded
// argument table, one colour target, one open render encoder.
static bool setup(objects& o)
{
	o.dev = MTL::CreateSystemDefaultDevice();
	if (!o.dev) { printf("  no device\n"); return false; }

	o.alloc = o.dev->newCommandAllocator();
	o.cb = o.dev->newCommandBuffer();
	if (!o.alloc || !o.cb) { printf("  allocator/buffer failed\n"); return false; }

	MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
	td->setMaxBufferBindCount(8);
	td->setMaxTextureBindCount(8);
	td->setMaxSamplerStateBindCount(8);
	td->setInitializeBindings(true);
	NS::Error* err = nullptr;
	o.table = o.dev->newArgumentTable(td, &err);
	td->release();
	if (!o.table) { printf("  table failed\n"); return false; }

	MTL::TextureDescriptor* d = MTL::TextureDescriptor::texture2DDescriptor(
		MTL::PixelFormatRGBA8Unorm, 16, 16, false);
	d->setStorageMode(MTL::StorageModePrivate);
	d->setUsage(MTL::TextureUsageRenderTarget);
	MTL::Texture* rt = o.dev->newTexture(d);
	d->release();

	MTL4::RenderPassDescriptor* rp = MTL4::RenderPassDescriptor::alloc()->init();
	MTL::RenderPassColorAttachmentDescriptor* a0 = rp->colorAttachments()->object(0);
	a0->setTexture(rt);
	a0->setLoadAction(MTL::LoadActionClear);
	a0->setStoreAction(MTL::StoreActionStore);
	a0->setClearColor(MTL::ClearColor::Make(0, 0, 0, 1));
	rp->setRenderTargetWidth(16);
	rp->setRenderTargetHeight(16);

	o.cb->beginCommandBuffer(o.alloc);
	o.enc = o.cb->renderCommandEncoder(rp);
	rp->release();

	if (!o.enc) { printf("  encoder failed\n"); return false; }
	return true;
}

// ------------------------------------------------------------------ the cases

static void live_vertex_and_fragment()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->setArgumentTable(o.table, MTL::RenderStageVertex);
	o.enc->setArgumentTable(o.table, MTL::RenderStageFragment);
	printf("  ok: live encoder, vertex then fragment\n");
}

static void null_table_vertex()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->setArgumentTable(nullptr, MTL::RenderStageVertex);
	printf("  ok: null table accepted (vertex)\n");
}

static void null_table_fragment()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->setArgumentTable(nullptr, MTL::RenderStageFragment);
	printf("  ok: null table accepted (fragment)\n");
}

static void ended_encoder_vertex()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->endEncoding();
	o.enc->setArgumentTable(o.table, MTL::RenderStageVertex);
	printf("  ok: ended encoder accepted (vertex)\n");
}

static void ended_encoder_fragment()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->endEncoding();
	o.enc->setArgumentTable(o.table, MTL::RenderStageFragment);
	printf("  ok: ended encoder accepted (fragment)\n");
}

static void ended_buffer_vertex()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->endEncoding();
	o.cb->endCommandBuffer();
	o.enc->setArgumentTable(o.table, MTL::RenderStageVertex);
	printf("  ok: ended command buffer accepted\n");
}

static void reset_allocator_vertex()
{
	objects o{};
	if (!setup(o)) return;
	o.enc->endEncoding();
	o.cb->endCommandBuffer();
	o.alloc->reset();
	o.cb->beginCommandBuffer(o.alloc);
	o.enc->setArgumentTable(o.table, MTL::RenderStageVertex);
	printf("  ok: encoder from a reset allocator accepted\n");
}

static void released_table_vertex()
{
	objects o{};
	if (!setup(o)) return;
	o.table->release();
	o.enc->setArgumentTable(o.table, MTL::RenderStageVertex);
	printf("  ok: released table accepted (vertex)\n");
}

static void released_table_fragment()
{
	objects o{};
	if (!setup(o)) return;
	o.table->release();
	o.enc->setArgumentTable(o.table, MTL::RenderStageFragment);
	printf("  ok: released table accepted (fragment)\n");
}

// The slot re-enter in RPCS3 releases the tables and then, on the same slot,
// builds new ones.  If a stale pointer from the old set reaches the encoder,
// this is what it looks like: a new table exists, but the call names the old.
static void stale_table_after_new()
{
	objects o{};
	if (!setup(o)) return;

	MTL4::ArgumentTable* stale = o.table;

	MTL4::ArgumentTableDescriptor* td = MTL4::ArgumentTableDescriptor::alloc()->init();
	td->setMaxBufferBindCount(8);
	td->setMaxTextureBindCount(8);
	td->setMaxSamplerStateBindCount(8);
	td->setInitializeBindings(true);
	NS::Error* err = nullptr;
	MTL4::ArgumentTable* fresh = o.dev->newArgumentTable(td, &err);
	td->release();

	stale->release();
	o.enc->setArgumentTable(stale, MTL::RenderStageVertex);
	printf("  ok: stale table accepted (fresh=%p, stale=%p)\n", (void*)fresh, (void*)stale);
}

struct test_case
{
	const char* name;
	void (*fn)();
};

static const test_case cases[] =
{
	{ "live-vertex-fragment",   live_vertex_and_fragment },
	{ "null-table-vertex",      null_table_vertex },
	{ "null-table-fragment",    null_table_fragment },
	{ "ended-encoder-vertex",   ended_encoder_vertex },
	{ "ended-encoder-fragment", ended_encoder_fragment },
	{ "ended-buffer-vertex",    ended_buffer_vertex },
	{ "reset-allocator-vertex", reset_allocator_vertex },
	{ "released-table-vertex",  released_table_vertex },
	{ "released-table-fragment",released_table_fragment },
	{ "stale-table-after-new",  stale_table_after_new },
};

int main(int argc, char** argv)
{
	// With a case name, this process runs exactly that one case.  The parent
	// re-executes itself per case rather than forking: Metal is not fork-safe
	// once a device exists, so a forked child faults in IOGPU before it reaches
	// the code under test.
	if (argc > 1)
	{
		install_handlers();
		for (const test_case& c : cases)
		{
			if (std::strcmp(c.name, argv[1]) == 0)
			{
				c.fn();
				fflush(stdout);
				return 0;
			}
		}
		printf("  unknown case %s\n", argv[1]);
		return 2;
	}

	printf("MTL4 setArgumentTable:atStages: lifetime probe\n");
	printf("device:  ");

	{
		MTL::Device* dev = MTL::CreateSystemDefaultDevice();
		if (!dev) { printf("none\n"); return 1; }
		printf("%s\n", dev->name()->utf8String());
	}

	printf("\ncase -> result (a FAULT line carries si_addr and the image of the PC)\n");

	unsigned faults = 0;
	for (const test_case& c : cases)
	{
		printf("%-26s\n", c.name);
		fflush(stdout);

		const pid_t pid = fork();
		if (pid == 0)
		{
			execl(argv[0], argv[0], c.name, static_cast<char*>(nullptr));
			_Exit(127);
		}

		int status = 0;
		waitpid(pid, &status, 0);
		if (WIFSIGNALED(status))
		{
			printf("  killed by signal %d\n", WTERMSIG(status));
			++faults;
		}
		else if (WIFEXITED(status) && WEXITSTATUS(status) == 99)
		{
			++faults;
		}
		else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		{
			printf("  exited %d\n", WEXITSTATUS(status));
			++faults;
		}
	}

	printf("\n%u of %zu cases faulted\n", faults, sizeof(cases) / sizeof(cases[0]));
	return 0;
}
