// JIT memory smoke test for macOS on Apple Silicon.
//
// This is the configuration RPCS3 ships: ad-hoc signed, no Developer ID, no
// notarization, no restricted entitlements. It verifies the two paths the
// native runtime can use for code memory:
//
//   1. MAP_JIT + the per-thread write-protect toggle
//   2. anonymous RW pages promoted with mprotect(RX)
//
// MAP_JIT pages are created with PROT_EXEC as well: the toggle only switches
// the page between writable and executable, it does not add PROT_EXEC.
//
// Both must work on a machine that has no Apple Developer Program account.
// A file-backed RX mapping is expected to fail and is only reported.

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#if !defined(__aarch64__)
#error "JIT smoke test expects an arm64 host"
#endif

static const uint8_t code_return_42[] = {
	0x40, 0x05, 0x80, 0x52, // movz w0, #42
	0xc0, 0x03, 0x5f, 0xd6, // ret
};

static int test_map_jit(size_t page)
{
	if (pthread_jit_write_protect_supported_np() != 1)
	{
		fprintf(stderr, "FAIL: pthread_jit_write_protect_supported_np() != 1\n");
		return 1;
	}

	void* mem = mmap(NULL, page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);

	if (mem == MAP_FAILED)
	{
		fprintf(stderr, "FAIL: MAP_JIT mmap: %s\n", strerror(errno));
		return 1;
	}

	// Enter the write window, copy code, publish it, then leave the window.
	pthread_jit_write_protect_np(0);
	memcpy(mem, code_return_42, sizeof(code_return_42));
	sys_icache_invalidate(mem, sizeof(code_return_42));
	pthread_jit_write_protect_np(1);

	int (*fn)(void) = (int (*)(void))(mem);
	const int result = fn();

	munmap(mem, page);

	if (result != 42)
	{
		fprintf(stderr, "FAIL: MAP_JIT code returned %d, expected 42\n", result);
		return 1;
	}

	printf("ok: MAP_JIT write window and execution\n");
	return 0;
}

static int test_mprotect_rx(size_t page)
{
	void* mem = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

	if (mem == MAP_FAILED)
	{
		fprintf(stderr, "FAIL: anonymous mmap: %s\n", strerror(errno));
		return 1;
	}

	memcpy(mem, code_return_42, sizeof(code_return_42));

	if (mprotect(mem, page, PROT_READ | PROT_EXEC) != 0)
	{
		fprintf(stderr, "FAIL: mprotect(RX): %s\n", strerror(errno));
		munmap(mem, page);
		return 1;
	}

	sys_icache_invalidate(mem, sizeof(code_return_42));

	int (*fn)(void) = (int (*)(void))(mem);
	const int result = fn();

	munmap(mem, page);

	if (result != 42)
	{
		fprintf(stderr, "FAIL: mprotect code returned %d, expected 42\n", result);
		return 1;
	}

	printf("ok: anonymous mmap + mprotect(RX) execution\n");
	return 0;
}

int main()
{
	setbuf(stdout, NULL);

	const size_t page = (size_t)sysconf(_SC_PAGESIZE);
	int failures = 0;

	failures += test_map_jit(page);
	failures += test_mprotect_rx(page);

	if (failures != 0)
	{
		fprintf(stderr, "JIT smoke test failed (%d)\n", failures);
		return 1;
	}

	printf("JIT smoke test passed\n");
	return 0;
}