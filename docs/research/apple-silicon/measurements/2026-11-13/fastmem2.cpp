// Fastmem go/no-go prototype, part 2.
//
// Adds: percentile reporting, batched W^X patching (the realistic shape, one
// mprotect per batch), a store-only patch path, and an honest demoted site:
// a real out-of-line helper call, not a constant return.

#include <sys/mman.h>
#include <sys/ucontext.h>
#include <signal.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

static constexpr size_t kPage = 16384;
static constexpr size_t kArenaPages = 4096;
static uint8_t* g_arena = nullptr;
static uint8_t* g_code = nullptr;
static size_t g_faults = 0;

static constexpr uint32_t kLdrW0X0 = 0xB9400000u;
static constexpr uint32_t kRet      = 0xD65F03C0u;

using fn_t = uint32_t (*)(const uint8_t*);

extern "C" __attribute__((noinline)) uint32_t slow_load(const uint8_t* p) {
    // Stand-in for the real per-site helper: classify the address, find the
    // mapping, and produce the value.  Kept small and non-inlined.
    return uint32_t((uintptr_t)p >> 4) ^ 0x5A5A5A5Au;
}

static void die(const char* what) { perror(what); _exit(1); }

static void handler(int, siginfo_t*, void* ucv) {
    auto* uc = static_cast<ucontext_t*>(ucv);
    uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
    uintptr_t far = (uintptr_t)uc->uc_mcontext->__es.__far;
    ++g_faults;
    if (pc < (uintptr_t)g_code || pc >= (uintptr_t)g_code + kPage) _exit(90);
    if (far < (uintptr_t)g_arena || far >= (uintptr_t)g_arena + kArenaPages * kPage) _exit(91);
    if (mprotect((void*)(far & ~(kPage - 1)), kPage, PROT_READ | PROT_WRITE) != 0) _exit(92);
}

static uint64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

static void report(std::vector<uint64_t> v, const char* label, const char* unit = "ns") {
    std::sort(v.begin(), v.end());
    uint64_t s = 0; for (auto x : v) s += x;
    auto q = [&](double p) { return v[size_t(p * (v.size() - 1))]; };
    std::printf("%s\n  n=%zu mean=%.0f p50=%llu p90=%llu p99=%llu max=%llu %s\n",
                label, v.size(), double(s) / v.size(),
                (unsigned long long)q(0.50), (unsigned long long)q(0.90),
                (unsigned long long)q(0.99), (unsigned long long)v.back(), unit);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_code = static_cast<uint8_t*>(mmap(nullptr, kPage, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON, -1, 0));
    if (g_code == MAP_FAILED) die("mmap code");
    uint32_t* w = reinterpret_cast<uint32_t*>(g_code);
    w[0] = kLdrW0X0; w[1] = kRet;
    if (mprotect(g_code, kPage, PROT_READ | PROT_EXEC) != 0) die("mprotect RX");

    g_arena = static_cast<uint8_t*>(mmap(nullptr, kArenaPages * kPage, PROT_NONE,
                                         MAP_PRIVATE | MAP_ANON, -1, 0));
    if (g_arena == MAP_FAILED) die("mmap arena");

    struct sigaction sa{};
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) die("sigaction");
    if (sigaction(SIGBUS, &sa, nullptr) != 0) die("sigaction bus");

    fn_t bare = reinterpret_cast<fn_t>(g_code);
    { volatile uint32_t v = bare(g_arena); (void)v; }

    std::vector<uint64_t> pf;
    pf.reserve(kArenaPages);
    for (size_t i = 1; i < kArenaPages; ++i) {
        uint64_t t0 = now_ns();
        volatile uint32_t v = bare(g_arena + i * kPage);
        (void)v;
        pf.push_back(now_ns() - t0);
    }
    report(std::move(pf), "first fault -> commit 16 KB -> retry:");

    {
        constexpr size_t it = 10000000;
        uint64_t t0 = now_ns();
        uint32_t acc = 0;
        for (size_t i = 0; i < it; ++i) acc += bare(g_arena + (i & 63) * 64);
        uint64_t t1 = now_ns();
        std::printf("steady-state bare load: %.3f ns/op (acc=%u)\n", double(t1 - t0) / it, acc);
    }

    // Single-site patch, two mprotects per patch (the naive shape).
    {
        std::vector<uint64_t> p;
        constexpr size_t k = 4096;
        for (size_t i = 0; i < k; ++i) {
            uint64_t t0 = now_ns();
            mprotect(g_code, kPage, PROT_READ | PROT_WRITE);
            w[2] = kRet;
            mprotect(g_code, kPage, PROT_READ | PROT_EXEC);
            sys_icache_invalidate(g_code, kPage);
            p.push_back(now_ns() - t0);
        }
        report(std::move(p), "single-site patch (2x mprotect + store + icache):");
    }

    // Batched patch: one W^X scope for 4096 sites, 4-byte store each.
    {
        constexpr size_t k = 4096;
        std::vector<uint8_t*> sites(k);
        for (size_t i = 0; i < k; ++i) sites[i] = g_code + ((i * 4) % (kPage - 128));
        uint64_t t0 = now_ns();
        mprotect(g_code, kPage, PROT_READ | PROT_WRITE);
        for (size_t i = 0; i < k; ++i) {
            uint32_t ins = 0xD503201Fu; // nop
            memcpy(sites[i], &ins, 4);
        }
        mprotect(g_code, kPage, PROT_READ | PROT_EXEC);
        sys_icache_invalidate(g_code, kPage);
        uint64_t t1 = now_ns();
        std::printf("batched patch, one W^X scope for %zu sites: total %.1f us, %.0f ns/site\n",
                    k, double(t1 - t0) / 1e3, double(t1 - t0) / k);
    }

    // Rebuild the bare body (the batch overwrote it) and test the demoted path.
    {
        mprotect(g_code, kPage, PROT_READ | PROT_WRITE);
        w[0] = 0xB9400000u;                 // ldr w0,[x0]
        w[1] = 0x14000003u;                 // b +12  -> stub at +16
        // stub at +16: str x30,[sp,#-16]! ; bl slow_load ; ldr x30,[sp],#16 ; ret
        // A leaf stub may not clobber LR: without the save/restore, `ret`
        // returns to itself and the process loops forever (measured).
        uint64_t target = (uint64_t)&slow_load;
        int64_t off = (int64_t)(target - ((uint64_t)g_code + 20));
        if (off < -(1LL << 27) || off >= (1LL << 27)) { std::printf("helper too far\n"); return 1; }
        w[4] = 0xF81F0FFEu;  // str x30, [sp, #-16]!
        w[5] = 0x94000000u | uint32_t((off >> 2) & 0x03FFFFFFu);
        w[6] = 0xF84107FEu;  // ldr x30, [sp], #16
        w[7] = kRet;
        mprotect(g_code, kPage, PROT_READ | PROT_EXEC);
        sys_icache_invalidate(g_code, kPage);

        constexpr size_t it = 5000000;
        uint64_t t0 = now_ns();
        uint32_t acc = 0;
        for (size_t i = 0; i < it; ++i) acc += bare(g_arena + (i & 63) * 64);
        uint64_t t1 = now_ns();
        std::printf("demoted site (b -> bl helper -> ret): %.2f ns/op (acc=%u)\n",
                    double(t1 - t0) / it, acc);
    }

    std::printf("faults handled: %zu\n", g_faults);
    return 0;
}