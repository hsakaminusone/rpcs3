#include "util/types.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/Thread.h"
#include "JIT.h"
#include "StrFmt.h"
#include "File.h"
#include "util/logs.hpp"
#include "mutex.h"
#include "util/vm.hpp"
#include "util/asm.hpp"
#include "Crypto/unzip.h"
#include "CPUStats.h"
#include "fastmem.h"

#include <charconv>
#include <map>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(ANDROID)
#include "util/cctype.hpp"
#endif

LOG_CHANNEL(jit_log, "JIT");

#ifdef LLVM_AVAILABLE

#include <unordered_map>

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Weffc++"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#endif
#include <llvm/Support/CodeGen.h>
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Target/TargetOptions.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#ifdef ARCH_ARM64
#include "Emu/CPU/Backends/AArch64/AArch64Common.h"
#endif

#if defined(ARCH_ARM64) && defined(__APPLE__)
#include "Emu/CPU/CPUThread.h"
#endif

namespace
{
	thread_local std::string* g_llvm_fatal_message = nullptr;

	template <typename F>
	bool run_recoverable_llvm(F&& func, std::string& error)
	{
		error.clear();

		// Run LLVM codegen in a disposable thread. If LLVM invokes the fatal
		// handler, only this helper thread exits.
		named_thread worker("LLVM JIT", [&]()
		{
			// Codegen writes MAP_JIT pages, so the window must be opened on this
			// thread. It has to be a scope, not a pair of raw toggles: symbol
			// resolution during codegen builds asmjit helpers that take their
			// own scope, and those must nest inside this one instead of closing
			// the write window halfway through the compile.
			jit_write_guard write_guard;

			g_llvm_fatal_message = &error;

			std::forward<F>(func)();

			g_llvm_fatal_message = nullptr;
		});

		worker();
		const bool result = static_cast<thread_state>(worker) == thread_state::finished;

		if (!result && error.empty())
		{
			error = "LLVM crash recovery invoked";
		}

		return result;
	}
}

const bool jit_initialize = []() -> bool
{
#if defined(__APPLE__) && defined(ARCH_ARM64)
	// Code memory is MAP_JIT and is written through the per-thread toggle. Without
	// it the first store into an executable page faults, so refuse to start
	// rather than log and fail later in a signal handler.
	if (!pthread_jit_write_protect_supported_np())
	{
		fmt::throw_exception("JIT: per-thread write protection is unavailable; MAP_JIT pages cannot be written");
	}
#endif

	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	llvm::InitializeNativeTargetAsmParser();
	LLVMLinkInMCJIT();
	return true;
}();

[[noreturn]] static void null(const char* name)
{
	fmt::throw_exception("Null function: %s", name);
}

namespace vm
{
	extern u8* const g_sudo_addr;
}

static shared_mutex null_mtx;

static std::unordered_map<std::string, u64> null_funcs;

static u64 make_null_function(const std::string& name)
{
	if (name.starts_with("__0x"))
	{
		u32 addr = -1;
		auto res = std::from_chars(name.c_str() + 4, name.c_str() + name.size(), addr, 16);

		if (res.ec == std::errc() && res.ptr == name.c_str() + name.size() && addr < 0x8000'0000)
		{
			fmt::throw_exception("Unhandled symbols cementing! (name='%s'", name);
		}
	}

	std::lock_guard lock(null_mtx);

	if (u64& func_ptr = null_funcs[name]) [[likely]]
	{
		// Already exists
		return func_ptr;
	}
	else
	{
		using namespace asmjit;

		// Build a "null" function that contains its name
		const auto func = build_function_asm<void (*)()>("NULL", [&](native_asm& c, auto& args)
		{
#if defined(ARCH_X64)
			Label data = c.newLabel();
			c.lea(args[0], x86::qword_ptr(data, 0));
			c.jmp(Imm(&null));
			c.align(AlignMode::kCode, 16);
			c.bind(data);

			// Copy function name bytes
			for (char ch : name)
				c.db(ch);
			c.db(0);
			c.align(AlignMode::kData, 16);
#else
			// AArch64 implementation
			Label data = c.newLabel();
			Label jump_address = c.newLabel();
			c.ldr(args[0], arm::ptr(data, 0));
			c.ldr(a64::x14, arm::ptr(jump_address, 0));
			c.br(a64::x14);

			// Data frame
			c.align(AlignMode::kCode, 16);
			c.bind(jump_address);
			c.embedUInt64(reinterpret_cast<u64>(&null));

			c.align(AlignMode::kData, 16);
			c.bind(data);
			c.embed(name.c_str(), name.size());
			c.embedUInt8(0U);
			c.align(AlignMode::kData, 16);
#endif
		});

		func_ptr = reinterpret_cast<u64>(func);
		return func_ptr;
	}
}

#if defined(ARCH_ARM64) && defined(__APPLE__)
	// Decode the linker-visible .stack_sizes records emitted by LLVM: an
	// 8-byte relocated code address followed by a ULEB128 frame size.
	// MCJIT never loads non-allocated sections, so this runs on the object
	// buffer before RuntimeDyld discards it.
	//
	// Returns true when a frame does not fit the gateway slab. Such a function
	// would write past the slab into the gateway's own stack, so the caller must
	// refuse the module and let the interpreter run it instead (§8.4.3).
	static bool report_stack_sizes(const llvm::object::ObjectFile& obj)
	{
		bool overflowed = false;

		using namespace llvm;

		if (!isa<object::ELFObjectFileBase>(obj))
		{
			return false;
		}

		std::optional<object::SectionRef> stack_sizes;

		for (const object::SectionRef& sec : obj.sections())
		{
			auto name = sec.getName();

			if (!name)
			{
				consumeError(name.takeError());
				continue;
			}

			if (*name == ".stack_sizes")
			{
				stack_sizes = sec;
				break;
			}
		}

		if (!stack_sizes)
		{
			return false;
		}

		auto contents = stack_sizes->getContents();

		if (!contents)
		{
			consumeError(contents.takeError());
			return false;
		}

		// Function symbols are the only way to turn the relocation addend into
		// a name, because MC emits relocations against the section symbol.
		std::map<std::pair<u64, u64>, std::string> functions;

		for (const object::SymbolRef& sym : obj.symbols())
		{
			auto type = sym.getType();

			if (!type)
			{
				consumeError(type.takeError());
				continue;
			}

			if (*type != object::SymbolRef::ST_Function)
			{
				continue;
			}

			auto name = sym.getName();
			auto value = sym.getValue();
			auto section = sym.getSection();

			if (!name)
			{
				consumeError(name.takeError());
				continue;
			}

			if (!value)
			{
				consumeError(value.takeError());
				continue;
			}

			if (!section)
			{
				consumeError(section.takeError());
				continue;
			}

			functions.emplace(std::make_pair((*section)->getIndex(), *value), name->str());
		}

		for (const object::RelocationRef& rel : stack_sizes->relocations())
		{
			const u64 offset = rel.getOffset();

			if (offset + sizeof(u64) > contents->size())
			{
				continue;
			}

			auto symbol = rel.getSymbol();

			if (symbol == obj.symbol_end())
			{
				continue;
			}

			auto section = symbol->getSection();

			if (!section)
			{
				consumeError(section.takeError());
				continue;
			}

			std::string name = "<unknown>";

			if (auto type = symbol->getType(); type && *type == object::SymbolRef::ST_Function)
			{
				if (auto sym_name = symbol->getName())
				{
					name = sym_name->str();
				}
				else
				{
					consumeError(sym_name.takeError());
				}
			}
			else
			{
				auto addend = object::ELFRelocationRef(rel).getAddend();

				if (!addend)
				{
					consumeError(addend.takeError());
				}
				else if (const auto it = functions.find({(*section)->getIndex(), static_cast<u64>(*addend)}); it != functions.end())
				{
					name = it->second;
				}
			}

			const u8* pos = reinterpret_cast<const u8*>(contents->data()) + offset + sizeof(u64);
			const u8* end = reinterpret_cast<const u8*>(contents->data()) + contents->size();
			const char* error = nullptr;
			const u64 frame = decodeULEB128(pos, nullptr, end, &error);

			if (error)
			{
				continue;
			}

			// The gateway reserves the whole supported frame budget in one
			// `sub sp`, so a frame at or below cpu_thread::max_frame_size cannot
			// overflow it. Anything above that is a decode or codegen bug and is
			// rejected by the caller rather than run with a corrupted host stack.
			const u64 slab = cpu_thread::gateway_slab_size;

			if (frame > cpu_thread::max_frame_size)
			{
				cpu_stats::g_counters.frame_overflows++;
				jit_log.error("LLVM: %s needs %llu bytes of GHC stack, above the %llu-byte gateway slab", name, frame, slab);
				overflowed = true;
			}
			else if (frame >= slab * 3 / 4)
			{
				cpu_stats::g_counters.frame_warnings++;
				jit_log.warning("LLVM: %s needs %llu bytes of GHC stack (gateway slab is %llu bytes)", name, frame, slab);
			}
		}

		return overflowed;
	}
#endif

struct JITAnnouncer : llvm::JITEventListener
{
	jit_compiler& m_owner;
	const bool m_announce_symbols;

	explicit JITAnnouncer(jit_compiler& owner, bool announce_symbols = true) noexcept
		: m_owner(owner)
		, m_announce_symbols(announce_symbols)
	{
	}

	void notifyObjectLoaded(u64, const llvm::object::ObjectFile& obj, const llvm::RuntimeDyld::LoadedObjectInfo& info) override
	{
		using namespace llvm;

#if defined(ARCH_ARM64) && defined(__APPLE__)
		if (report_stack_sizes(obj))
		{
			m_owner.note_frame_overflow();
		}
#endif

		if (!m_announce_symbols)
		{
			return;
		}

		object::OwningBinary<object::ObjectFile> debug_obj_ = info.getObjectForDebug(obj);
		if (!debug_obj_.getBinary())
		{
#ifdef __linux__
			jit_log.error("LLVM: Failed to announce JIT events (no debug object)");
#endif
			return;
		}

		const object::ObjectFile& debug_obj = *debug_obj_.getBinary();

		for (const auto& [sym, size] : computeSymbolSizes(debug_obj))
		{
			Expected<object::SymbolRef::Type> type_ = sym.getType();
			if (!type_ || *type_ != object::SymbolRef::ST_Function)
				continue;

			Expected<StringRef> name = sym.getName();
			if (!name)
				continue;

			Expected<u64> addr = sym.getAddress();
			if (!addr)
				continue;

			jit_announce(*addr, size, {name->data(), name->size()});
		}
	}
};

namespace
{
	// Code written since the last publish point, kept so the flush covers exactly
	// the object that was just loaded instead of the whole arena.
	struct code_flush_range
	{
		u8* begin = nullptr;
		u8* end = nullptr;

		void note(u8* p, uptr size) noexcept
		{
			if (!p)
			{
				return;
			}

			if (!begin)
			{
				begin = p;
			}

			end = p + size;
		}

		void flush() noexcept
		{
			if (begin)
			{
				jit_flush_icache(begin, static_cast<usz>(end - begin));
				begin = nullptr;
				end = nullptr;
			}
		}
	};
}

// Simple memory manager
struct MemoryManager1 final : llvm::RTDyldMemoryManager, jit_code_publisher
{
	// 256 MiB for code or data
	static constexpr u64 c_max_size = 0x1000'0000;

	// Allocation unit (2M)
	static constexpr u64 c_page_size = 2 * 1024 * 1024;

	// Reserve 256 MiB blocks
	void* m_code_mems = nullptr;
	void* m_data_ro_mems = nullptr;
	void* m_data_rw_mems = nullptr;

	u64 code_ptr = 0;
	u64 data_ro_ptr = 0;
	u64 data_rw_ptr = 0;

	// Code written since the last publish point, needed by the icache flush.
	code_flush_range m_flush;

	// First fallback for non-existing symbols
	// May be a memory container internally
	std::function<u64(const std::string&)> m_symbols_cement;

	MemoryManager1(std::function<u64(const std::string&)> symbols_cement = {}) noexcept
		: m_symbols_cement(std::move(symbols_cement))
	{
		auto ptr = reinterpret_cast<u8*>(utils::memory_reserve(c_max_size * 3, true));
		m_code_mems = ptr;
		// ptr += c_max_size;
		// m_data_ro_mems = ptr;
		 ptr += c_max_size;
		m_data_rw_mems = ptr;
	}

	MemoryManager1(const MemoryManager1&) = delete;

	MemoryManager1& operator=(const MemoryManager1&) = delete;

	~MemoryManager1() override
	{
		// A host PC only identifies a fault site while this module's code is
		// mapped. The reservation is kept below to stop the address space being
		// reused, but the site records have to go: a new module at the same
		// address would otherwise inherit the old history.
		fastmem::invalidate_range(reinterpret_cast<uptr>(m_code_mems), reinterpret_cast<uptr>(m_code_mems) + c_max_size);

		// Hack: don't release to prevent reuse of address space, see jit_announce
		// constexpr auto how_much = [](u64 pos) { return utils::align(pos, pos < c_page_size ? c_page_size / 4 : c_page_size); };
		// utils::memory_decommit(m_code_mems, how_much(code_ptr));
		// utils::memory_decommit(m_data_ro_mems, how_much(data_ro_ptr));
		// utils::memory_decommit(m_data_rw_mems, how_much(data_rw_ptr));
		utils::memory_decommit(m_code_mems, c_max_size * 3, true);
	}

	llvm::JITSymbol findSymbol(const std::string& name) override
	{
		u64 addr = RTDyldMemoryManager::getSymbolAddress(name);

		if (!addr && m_symbols_cement)
		{
			addr = m_symbols_cement(name);
		}

		if (!addr)
		{
			addr = make_null_function(name);

			if (!addr)
			{
				fmt::throw_exception("Failed to link '%s'", name);
			}
		}

		return {addr, llvm::JITSymbolFlags::Exported};
	}

	u8* allocate(u64& alloc_pos, void* block, uptr size, u64 align, utils::protection prot)
	{
		align = align ? align : 16;
 
		const u64 sizea = utils::align(size, align);

		if (!size || align > c_page_size || sizea > c_max_size || sizea < size)
		{
			jit_log.fatal("Unsupported size/alignment (size=0x%x, align=0x%x)", size, align);
			return nullptr;
		}

		u64 oldp = alloc_pos;

		u64 olda = utils::align(oldp, align);

		ensure(olda >= oldp);
		ensure(olda < ~sizea);

		u64 newp = olda + sizea;

		if ((newp - 1) / c_max_size != (oldp - 1) / c_max_size)
		{
			constexpr usz num_of_allocations = 1;

			if ((newp - 1) / c_max_size > num_of_allocations)
			{
				// Allocating more than one region does not work for relocations, needs more robust solution
				fmt::throw_exception("Out of memory (size=0x%x, align=0x%x)", size, align);
			}
		}

		// Update allocation counter
		alloc_pos = newp;

		constexpr usz page_quarter = c_page_size / 4;

		// Optimization: split the first allocation to 512 KiB for single-module compilers
		if (oldp < c_page_size && align < page_quarter && (std::min(newp, c_page_size) - 1) / page_quarter != (oldp - 1) / page_quarter)
		{
			const u64 pagea = utils::align(oldp, page_quarter);
			const u64 psize = utils::align(std::min(newp, c_page_size) - pagea, page_quarter);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);

			// Advance
			oldp = pagea + psize;
		}

		if ((newp - 1) / c_page_size != (oldp - 1) / c_page_size)
		{
			// Allocate pages on demand
			const u64 pagea = utils::align(oldp, c_page_size);
			const u64 psize = utils::align(newp - pagea, c_page_size);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);
		}

		return reinterpret_cast<u8*>(block) + (olda % c_max_size);
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
		u8* const p = allocate(code_ptr, m_code_mems, size, align, utils::protection::wx);
		m_flush.note(p, size);
		return p;
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool is_ro) override
	{
		if (is_ro)
		{
			// Disabled
			//return allocate(data_ro_ptr, m_data_ro_mems, size, align, utils::protection::rw);
		}

		return allocate(data_rw_ptr, m_data_rw_mems, size, align, utils::protection::rw);
	}

	void publish_code() noexcept override
	{
		m_flush.flush();
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		// RuntimeDyld has written this object's code and applied its relocations,
		// so sections may execute after this call. See also publish_code().
		m_flush.flush();
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

// Simple memory manager
struct MemoryManager2 final : llvm::RTDyldMemoryManager, jit_code_publisher
{
	// First fallback for non-existing symbols
	// May be a memory container internally
	std::function<u64(const std::string&)> m_symbols_cement;

	// Code written since the last publish point, needed by the icache flush.
	code_flush_range m_flush;

	MemoryManager2(std::function<u64(const std::string&)> symbols_cement = {}) noexcept
		: m_symbols_cement(std::move(symbols_cement))
	{
	}

	~MemoryManager2() override
	{
	}

	llvm::JITSymbol findSymbol(const std::string& name) override
	{
		u64 addr = RTDyldMemoryManager::getSymbolAddress(name);

		if (!addr && m_symbols_cement)
		{
			addr = m_symbols_cement(name);
		}

		if (!addr)
		{
			addr = make_null_function(name);

			if (!addr)
			{
				fmt::throw_exception("Failed to link '%s' (MM2)", name);
			}
		}

		return {addr, llvm::JITSymbolFlags::Exported};
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
		u8* const p = jit_runtime::alloc(size, align, true);
		m_flush.note(p, size);
		return p;
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool /*is_ro*/) override
	{
		return jit_runtime::alloc(size, align, false);
	}

	void publish_code() noexcept override
	{
		m_flush.flush();
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		// See MemoryManager1::finalizeMemory.
		m_flush.flush();
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

// Helper class
class ObjectCache final : public llvm::ObjectCache
{
	const std::string& m_path;
	const std::add_pointer_t<jit_compiler> m_compiler = nullptr;

public:
	ObjectCache(const std::string& path, jit_compiler* compiler = nullptr)
		: m_path(path)
		, m_compiler(compiler)
	{
	}

	~ObjectCache() override = default;

	void notifyObjectCompiled(const llvm::Module* _module, llvm::MemoryBufferRef obj) override
	{
		std::string name = m_path;

		name.append(_module->getName());
		//fs::file(name, fs::rewrite).write(obj.getBufferStart(), obj.getBufferSize());
		name.append(".gz");

		if (!obj.getBufferSize())
		{
			jit_log.error("LLVM: Nothing to write: %s", name);
			return;
		}

		ensure(m_compiler);

		fs::pending_file module_file;

		if (!module_file.open((name)))
		{
			jit_log.error("LLVM: Failed to create module file: %s (%s)", name, fs::g_tls_error);
			return;
		}

		// Bold assumption about upper limit of space consumption
		const usz max_size = obj.getBufferSize() * 4;

		if (!m_compiler->add_sub_disk_space(0 - max_size))
		{
			jit_log.error("LLVM: Failed to create module file: %s (not enough disk space left)", name);
			return;
		}

		if (!zip(obj.getBufferStart(), obj.getBufferSize(), module_file.file))
		{
			jit_log.error("LLVM: Failed to compress module: %s", std::string(_module->getName()));
			return;
		}

		jit_log.trace("LLVM: Created module: %s", std::string(_module->getName()));

		// Restore space that was overestimated
		ensure(m_compiler->add_sub_disk_space(max_size - module_file.file.size()));
		module_file.commit();
	}

	static std::unique_ptr<llvm::MemoryBuffer> load(const std::string& path)
	{
		if (fs::file cached{path + ".gz", fs::read})
		{
			const std::vector<u8> cached_data = cached.to_vector<u8>();

			if (cached_data.size() < 18) [[unlikely]] // gzip header + trailer
			{
				return nullptr;
			}

			std::unique_ptr<llvm::WritableMemoryBuffer> buf;

			// Get out size from trailer
			const u32 out_size = read_from_ptr<u32>(cached_data, cached_data.size() - 4);
			if (out_size != 0 && out_size <= cached_data.size() * 1032) // Check for impossible sizes
			{
				buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(out_size);
			}

			if (!buf || !unzip(cached_data.data(), cached_data.size(), reinterpret_cast<u8*>(buf->getBufferStart()), out_size))
			{
				jit_log.error("LLVM: Failed to unzip module directly into the buffer: '%s'", path);

				// Try again with old unzip method
				const std::vector<u8> out = unzip(cached_data);
				if (out.empty())
				{
					jit_log.error("LLVM: Failed to unzip module: '%s'", path);
					return nullptr;
				}

				buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(out.size());
				std::memcpy(buf->getBufferStart(), out.data(), out.size());
			}

			return buf;
		}

		if (fs::file cached{path, fs::read})
		{
			if (cached.size() == 0) [[unlikely]]
			{
				return nullptr;
			}

			auto buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(cached.size());
			cached.read(buf->getBufferStart(), buf->getBufferSize());
			return buf;
		}

		return nullptr;
	}

	std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* _module) override
	{
		std::string path = m_path;
		path.append(_module->getName().data());

		if (auto buf = load(path))
		{
			jit_log.notice("LLVM: Loaded module: %s", _module->getName().data());
			return buf;
		}

		return nullptr;
	}
};

std::string jit_compiler::cpu(std::string_view _cpu)
{
	std::string m_cpu = std::string(_cpu);

	if (m_cpu.empty())
	{
		m_cpu = llvm::sys::getHostCPUName().str();

		if (m_cpu == "generic")
		{
			// Try to detect a best match based on other criteria
			m_cpu = fallback_cpu_detection();
		}

		if (m_cpu == "sandybridge" ||
			m_cpu == "ivybridge" ||
			m_cpu == "haswell" ||
			m_cpu == "broadwell" ||
			m_cpu == "skylake" ||
			m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake" ||
			m_cpu == "alderlake" ||
			m_cpu == "raptorlake" ||
			m_cpu == "meteorlake")
		{
			// Downgrade if AVX is not supported by some chips
			if (!utils::has_avx())
			{
				m_cpu = "nehalem";
			}
		}

		if (m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake")
		{
			// Downgrade if AVX-512 is disabled or not supported
			if (!utils::has_avx512())
			{
				m_cpu = "skylake";
			}
		}

		if (m_cpu == "znver1" && utils::has_clwb())
		{
			// Upgrade
			m_cpu = "znver2";
		}

		if ((m_cpu == "znver3" || m_cpu == "goldmont" || m_cpu == "alderlake" || m_cpu == "raptorlake" || m_cpu == "meteorlake") && utils::has_avx512_icl())
		{
			// Upgrade
			m_cpu = "icelake-client";
		}

		if (m_cpu == "goldmont" && utils::has_avx2())
		{
			// Upgrade
			m_cpu = "alderlake";
		}
	}

	return m_cpu;
}

std::string jit_compiler::triple1()
{
#if defined(_WIN32)
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android");
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

std::string jit_compiler::triple2()
{
#if defined(_WIN32) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif defined(_WIN32) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-gnu");
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android"); // Set environment to android to reserve x18
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

bool jit_compiler::add_sub_disk_space(ssz space)
{
	if (space >= 0)
	{
		ensure(m_disk_space.fetch_add(space) < ~static_cast<usz>(space));
		return true;
	}

	return m_disk_space.fetch_op([sub_size = static_cast<usz>(0 - space)](usz& val)
	{
		if (val >= sub_size)
		{
			val -= sub_size;
			return true;
		}

		return false;
	}).second;
}

jit_compiler::jit_compiler(const std::unordered_map<std::string, u64>& _link, std::string_view _cpu, u32 flags, std::function<u64(const std::string&)> symbols_cement) noexcept
	: m_context(new llvm::LLVMContext)
	, m_cpu(cpu(_cpu))
{
	[[maybe_unused]] static const bool s_install_llvm_error_handler = []()
	{
		llvm::remove_fatal_error_handler();
		llvm::install_fatal_error_handler([](void*, const char* msg, bool)
		{
			const std::string_view out = msg ? msg : "";

			if (g_llvm_fatal_message)
			{
				*g_llvm_fatal_message = out;
				thread_ctrl::silent_exit();
			}

			fmt::throw_exception("LLVM Emergency Exit Invoked: '%s'", out);
		}, nullptr);

		return true;
	}();

	std::string result;

	auto null_mod = std::make_unique<llvm::Module> ("null_", *m_context);
	null_mod->setTargetTriple(llvm::Triple(jit_compiler::triple1()));

	std::unique_ptr<llvm::RTDyldMemoryManager> mem;

	if (_link.empty())
	{
		// Auxiliary JIT (does not use custom memory manager, only writes the objects)
		if (flags & 0x1)
		{
			mem = std::make_unique<MemoryManager1>(std::move(symbols_cement));
		}
		else
		{
			mem = std::make_unique<MemoryManager2>(std::move(symbols_cement));
			null_mod->setTargetTriple(llvm::Triple(jit_compiler::triple2()));
		}
	}
	else
	{
		mem = std::make_unique<MemoryManager1>(std::move(symbols_cement));
	}

	// Both managers implement it; the engine owns the object for its lifetime.
	m_code_publisher = dynamic_cast<jit_code_publisher*>(mem.get());

	std::vector<std::string> attributes;

#if defined(ARCH_ARM64)
	// LLVM groups FEAT_SHA3 and FEAT_SHA512 under the same target feature
	if (utils::has_sha512() && utils::has_sha3())
		attributes.push_back("+sha3");
	else
		attributes.push_back("-sha3");

	if (utils::has_dotprod())
		attributes.push_back("+dotprod");
	else
		attributes.push_back("-dotprod");

	// The recompilers emit i8mm intrinsics (e.g. ummla) gated on utils::has_i8mm().
	// The JIT target features must advertise i8mm too, otherwise the backend fails
	// with "Cannot select: intrinsic %llvm.aarch64.neon.ummla" whenever the resolved
	// -mcpu does not already imply it (e.g. the cortex-a78 fallback on Apple silicon).
	if (utils::has_i8mm())
		attributes.push_back("+i8mm");
	else
		attributes.push_back("-i8mm");

	if (utils::has_sve())
		attributes.push_back("+sve");
	else
		attributes.push_back("-sve");

	if (utils::has_sve2())
		attributes.push_back("+sve2");
	else
		attributes.push_back("-sve2");
#endif

	llvm::TargetOptions target_options;
#if defined(ARCH_ARM64) && defined(__APPLE__)
	// Emit .stack_sizes so the gateway slab can be audited against real frame
	// sizes. This only adds a non-allocated section; code generation is
	// unchanged. The JIT listener decodes it before RuntimeDyld discards it.
	target_options.EmitStackSizeSection = true;
#endif

	{
		m_engine.reset(llvm::EngineBuilder(std::move(null_mod))
			.setErrorStr(&result)
			.setEngineKind(llvm::EngineKind::JIT)
			.setMCJITMemoryManager(std::move(mem))
			.setTargetOptions(target_options)
			.setOptLevel(llvm::CodeGenOptLevel::Aggressive)
			.setCodeModel(flags & 0x2 ? llvm::CodeModel::Large : llvm::CodeModel::Small)
#ifdef __APPLE__
			//.setCodeModel(llvm::CodeModel::Large)
#endif
			.setRelocationModel(llvm::Reloc::Model::PIC_)
			.setMAttrs(attributes)
			.setMCPU(m_cpu)
			.create());
	}

	if (!_link.empty())
	{
		for (auto&& [name, addr] : _link)
		{
			m_engine->updateGlobalMapping(name, addr);
		}
	}

	if (!_link.empty() || !(flags & 0x1))
	{
		m_engine->RegisterJITEventListener(llvm::JITEventListener::createIntelJITEventListener());
	}

	// Registered for every instance, including the object-only compiler: the
	// frame report has to run on a cold compile, not only when the object is
	// loaded again from the cache. Symbol announcement stays with real engines.
	m_engine->RegisterJITEventListener(new JITAnnouncer(*this, !_link.empty() || !(flags & 0x1)));

	if (!m_engine)
	{
		fmt::throw_exception("LLVM: Failed to create ExecutionEngine: %s", result);
	}

	fs::device_stat stats{};

	if (fs::statfs(fs::get_cache_dir(), stats))
	{
		m_disk_space = stats.avail_free / 4;
	}
}

jit_compiler& jit_compiler::operator=(thread_state s) noexcept
{
	if (s == thread_state::destroying_context)
	{
		// Release resources explicitly
		m_engine.reset();
		m_context.reset();
	}

	return *this;
}

jit_compiler::~jit_compiler() noexcept
{
}

namespace
{
	// A decoded frame above the gateway budget cannot be run: the gateway reserves
	// that budget with one `sub sp` and GHC code writes at positive offsets from
	// it. Refuse the module so the caller falls back to the interpreter (§8.4.3).
	bool reject_on_frame_overflow(jit_compiler& jit, u64 before, const std::string& path) noexcept
	{
		if (jit.frame_overflows() == before)
		{
			return false;
		}

#if defined(ARCH_ARM64) && defined(__APPLE__)
		jit_log.error("LLVM: Refusing %s: a function frame exceeds the %llu-byte gateway slab", path, cpu_thread::gateway_slab_size);
#else
		jit_log.error("LLVM: Refusing %s: a function frame exceeds the gateway slab", path);
#endif
		return true;
	}
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module, const std::string& path)
{
	const auto compile_start = std::chrono::steady_clock::now();
	ObjectCache cache{path, this};
	m_engine->setObjectCache(&cache);

	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));
	m_engine->generateCodeForModule(ptr);
	m_engine->setObjectCache(nullptr);

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	cpu_stats::g_counters.jit_compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compile_start).count();
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, const std::string& path, std::string& error)
{
	const auto compile_start = std::chrono::steady_clock::now();
	ObjectCache cache{path, this};
	m_engine->setObjectCache(&cache);

	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));

	const u64 frame_overflows_before = frame_overflows();

	if (!run_recoverable_llvm([&]()
	{
		m_engine->generateCodeForModule(ptr);
	}, error))
	{
		cpu_stats::g_counters.compile_failures++;
		return false;
	}

	m_engine->setObjectCache(nullptr);

	if (reject_on_frame_overflow(*this, frame_overflows_before, path))
	{
		return false;
	}

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	cpu_stats::g_counters.jit_compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compile_start).count();

	return true;
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module)
{
	const auto compile_start = std::chrono::steady_clock::now();
	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));
	m_engine->generateCodeForModule(ptr);

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	cpu_stats::g_counters.jit_compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compile_start).count();
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, std::string& error)
{
	const auto compile_start = std::chrono::steady_clock::now();
	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));

	const u64 frame_overflows_before = frame_overflows();

	if (!run_recoverable_llvm([&]()
	{
		m_engine->generateCodeForModule(ptr);
	}, error))
	{
		cpu_stats::g_counters.compile_failures++;
		return false;
	}

	if (reject_on_frame_overflow(*this, frame_overflows_before, "module"))
	{
		return false;
	}

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	cpu_stats::g_counters.jit_compile_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - compile_start).count();

	return true;
}

bool jit_compiler::add(const std::string& path)
{
	auto cache = ObjectCache::load(path);

	if (!cache)
	{
		jit_log.error("ObjectCache: Failed to read file. (path='%s', error=%s)", path, fs::g_tls_error);
		return false;
	}

	return add(std::move(cache), path);
}

bool jit_compiler::add(std::unique_ptr<llvm::MemoryBuffer> object, const std::string& path)
{
	if (!object)
	{
		jit_log.error("ObjectCache: Failed to add nullptr object for file. (path='%s')", path);
		return false;
	}

	if (auto object_file = llvm::object::ObjectFile::createObjectFile(*object))
	{
		const u64 frame_overflows_before = frame_overflows();
		m_engine->addObjectFile(llvm::object::OwningBinary<llvm::object::ObjectFile>(std::move(*object_file), std::move(object)));

		if (reject_on_frame_overflow(*this, frame_overflows_before, path))
		{
			return false;
		}

		if (m_code_publisher)
		{
			// addObjectFile has applied the relocations but RuntimeDyld does not
			// call finalizeMemory for it, so publish the bytes explicitly before
			// anything can branch into them.
			m_code_publisher->publish_code();
		}

		jit_log.trace("ObjectCache: Successfully added %s", path);
		return true;
	}
	else
	{
		jit_log.error("ObjectCache: Adding failed: %s (%s)", path, llvm::toString(object_file.takeError()));
		return false;
	}
}

std::unique_ptr<llvm::MemoryBuffer> jit_compiler::load_object(const std::string& path)
{
	if (auto cache = ObjectCache::load(path))
	{
		if (auto object_file = llvm::object::ObjectFile::createObjectFile(*cache))
		{
			return cache;
		}
		else
		{
			llvm::consumeError(object_file.takeError());
		}

		// Remove cached and uncached file
		const bool removed_gz = fs::remove_file(path + ".gz");
		const bool removed = fs::remove_file(path);

		if (removed_gz || removed)
		{
			jit_log.error("ObjectCache: Removed damaged file: %s", path);
		}
	}

	return nullptr;
}

void jit_compiler::update_global_mapping(const std::string& name, u64 addr)
{
	m_engine->updateGlobalMapping(name, addr);
}

void jit_compiler::fin()
{
	m_engine->finalizeObject();
}

bool jit_compiler::try_fin(std::string& error)
{
	return run_recoverable_llvm([&]()
	{
		m_engine->finalizeObject();
	}, error);
}

u64 jit_compiler::get(const std::string& name)
{
	return m_engine->getGlobalValueAddress(name);
}

const char * fallback_cpu_detection()
{
#if defined(ARCH_X64)
	// If we got here we either have a very old and outdated CPU or a new CPU that has not been seen by LLVM yet.
	const std::string brand = utils::get_cpu_brand();
	const auto family = utils::get_cpu_family();
	const auto model = utils::get_cpu_model();

	jit_log.error("CPU wasn't identified by LLVM, brand = %s, family = 0x%x, model = 0x%x", brand, family, model);

	if (brand.starts_with("AMD"))
	{
		switch (family)
		{
		case 0x10:
		case 0x12: // Unimplemented in LLVM
			return "amdfam10";
		case 0x15:
			// Bulldozer class, includes piledriver, excavator, steamroller, etc
			return utils::has_avx2() ? "bdver4" : "bdver1";
		case 0x17:
		case 0x18:
			// No major differences between znver1 and znver2, return the lesser
			return "znver1";
		case 0x19:
			// Models 0-Fh are zen3 as are 20h-60h. The rest we can assume are zen4
			return ((model >= 0x20 && model <= 0x60) || model < 0x10) ? "znver3" : "znver4";
		case 0x1a:
			// Only one generation in family 1a so far, zen5, which we do not support yet.
			// Return zen4 as a workaround until the next LLVM upgrade.
			return "znver4";
		default:
			// Safest guesses
			return utils::has_avx512() ? "znver4" :
			       utils::has_avx2()   ? "znver1" :
			       utils::has_avx()    ? "bdver1" :
			                             "nehalem";
		}
	}
	else if (brand.find("Intel") != std::string::npos)
	{
		if (!utils::has_avx())
		{
			return "nehalem";
		}
		if (!utils::has_avx2())
		{
			return "ivybridge";
		}
		if (!utils::has_avx512())
		{
			return "skylake";
		}
		if (utils::has_avx512_icl())
		{
			return "cannonlake";
		}
		return "icelake-client";
	}
	else if (brand.starts_with("VirtualApple"))
	{
		// No AVX. This will change in MacOS 15+, at which point we may revise this.
		return utils::has_avx() ? "haswell" : "nehalem";
	}

#elif defined(ARCH_ARM64)
#ifdef ANDROID
	static std::string s_result = []() -> std::string
	{
		std::string result = aarch64::get_cpu_name();
		if (result.empty())
		{
			return "cortex-a78";
		}

		std::transform(result.begin(), result.end(), result.begin(), utils::tolower<char>);
		return result;
	}();

	return s_result.c_str();
#else
	// TODO: Read the data from /proc/cpuinfo. ARM CPU registers are not accessible from usermode.
	// This will be a pain when supporting snapdragon on windows but we'll cross that bridge when we get there.
	// Require at least armv8-2a. Older chips are going to be useless anyway.
	return "cortex-a78";
#endif
#endif

	// Failed to guess, use generic fallback
	return "generic";
}

#endif // LLVM_AVAILABLE
