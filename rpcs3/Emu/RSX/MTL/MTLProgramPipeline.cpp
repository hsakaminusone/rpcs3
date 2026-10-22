#include "MTLProgramPipeline.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLShader.h"
#include "MTLFragmentProgram.h"
#include "MTLVertexProgram.h"

#include "MTLInstrumentation.h"

#include "Emu/System.h"

#include "../Program/GLSLTypes.h"
#include "../Program/SPIRVCommon.h"

#include "util/logs.hpp"
#include "Utilities/File.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace mtl
{
#if MTL_DEBUG_LOGS
	// TODO(excise): level 5; writes the GLSL and the MSL of one program.
	void diagnostic_dump_program(const char* stage, u32 id, const std::string& glsl, const std::string& msl)
	{
		if (id >= 32)
		{
			return;
		}

		std::error_code error;
		std::filesystem::create_directories("/tmp/mtl4-dump", error);
		const std::string base = std::string("/tmp/mtl4-dump/") + stage + "-" + std::to_string(id);

		std::ofstream(base + ".glsl") << glsl;
		std::ofstream(base + ".metal") << msl;
	}
#endif // MTL_DEBUG_LOGS

	// The user-facing "Log shader programs" setting.  Vulkan and OpenGL write the
	// generated source beside the cache, and the two shader stages share the naming
	// there (VKVertexProgram.cpp:532, VKFragmentProgram.cpp:647).  Metal has one
	// extra hop, so the GLSL the decompiler produced and the MSL it became are both
	// written; the pair is what makes a translation defect reproducible.
	void write_program_log(const char* stage, u32 id, const std::string& glsl, const std::string& msl)
	{
		if (!g_cfg.video.log_programs)
		{
			return;
		}

		const std::string base = fs::get_cache_dir() + "shaderlog/" + stage + "Program" + std::to_string(id);
		fs::write_file(base + ".glsl", fs::rewrite, glsl);
		fs::write_file(base + ".metal", fs::rewrite, msl);
	}

	// The RSX ucode -> GLSL hop.  The decompiler subclasses hold the GLSL dialect;
	// the SPIR-V, MSL and library hops stay in this unit (HLD-05 §2.1).
	std::string decompile_vertex_program(const RSXVertexProgram& program, std::vector<u16>& constant_ids, bool& has_indexed_constants)
	{
		return decompile_vertex_glsl(program, &constant_ids, &has_indexed_constants);
	}

	std::string decompile_fragment_program(const RSXFragmentProgram& program, std::vector<u32>& constant_offsets)
	{
		return decompile_fragment_glsl(program, &constant_offsets);
	}

	namespace
	{
		void copy_error_text(char (&destination)[256], NS::Error* error)
		{
			destination[0] = '\0';
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : nullptr;
			if (!message)
			{
				return;
			}

			std::strncpy(destination, message, 255);
			destination[255] = '\0';
		}

		// The link worker serializes the descriptor on its own thread.  Keep every
		// descriptor alive until the synchronous compiler call has returned.
		struct link_compile_owner
		{
			MTL4::LibraryFunctionDescriptor* vertex_function = nullptr;
			MTL4::LibraryFunctionDescriptor* fragment_function = nullptr;
			MTL4::RenderPipelineDescriptor* descriptor = nullptr;
			MTL4::CompilerTaskOptions* options = nullptr;

			~link_compile_owner()
			{
				if (descriptor)
				{
					descriptor->release();
				}

				if (vertex_function)
				{
					vertex_function->release();
				}

				if (fragment_function)
				{
					fragment_function->release();
				}

				if (options)
				{
					options->release();
				}
			}
		};
	}

	shader_program::~shader_program()
	{
		// A live compile owns the library and the task.  The idle path must have
		// drained it; on the timeout path the cache is deliberately leaked.
		if (m_library_state.load(std::memory_order_acquire) != library_state::compiling)
		{
			if (m_library)
			{
				m_library->release();
			}

			if (m_library_task)
			{
				m_library_task->release();
			}
		}
	}

	shader_program::shader_program(shader_program&& other) noexcept
		: spv(std::move(other.spv))
		, msl(std::move(other.msl))
		, color_output_mask(other.color_output_mask)
		, constant_offsets(std::move(other.constant_offsets))
		, m_library(other.m_library)
		, m_library_task(other.m_library_task)
		, m_library_state(other.m_library_state.load(std::memory_order_relaxed))
		, m_library_generation(other.m_library_generation.load(std::memory_order_relaxed))
	{
		std::memcpy(m_library_error_text, other.m_library_error_text, sizeof(m_library_error_text));
		other.m_library = nullptr;
		other.m_library_task = nullptr;
		other.m_library_state.store(library_state::absent, std::memory_order_relaxed);
	}

	shader_program& shader_program::operator=(shader_program&& other) noexcept
	{
		if (this == &other)
		{
			return *this;
		}

		// Invalidate a handler that captured the old generation before the MSL
		// changes.  A live compile is a programming error on this path; leak its
		// objects rather than release them from a handler's owner.
		m_library_generation.fetch_add(1, std::memory_order_release);

		if (m_library_state.load(std::memory_order_acquire) != library_state::compiling)
		{
			if (m_library)
			{
				m_library->release();
			}

			if (m_library_task)
			{
				m_library_task->release();
			}
		}

		spv = std::move(other.spv);
		msl = std::move(other.msl);
		color_output_mask = other.color_output_mask;
		constant_offsets = std::move(other.constant_offsets);
		m_library = other.m_library;
		m_library_task = other.m_library_task;
		m_library_state.store(other.m_library_state.load(std::memory_order_relaxed), std::memory_order_release);
		m_library_generation.store(other.m_library_generation.load(std::memory_order_relaxed), std::memory_order_release);
		other.m_library = nullptr;
		other.m_library_task = nullptr;
		other.m_library_state.store(library_state::absent, std::memory_order_relaxed);
		return *this;
	}

	MTL::Library* shader_program::library() const noexcept
	{
		return m_library;
	}

	library_state shader_program::library_compilation_state() const noexcept
	{
		return m_library_state.load(std::memory_order_acquire);
	}

	const char* shader_program::library_error() const noexcept
	{
		return m_library_error_text;
	}

	u64 shader_program::begin_library_compile() const
	{
		std::lock_guard lock(m_library_mutex);
		if (m_library_state.load(std::memory_order_acquire) != library_state::absent)
		{
			return 0;
		}

		m_library_state.store(library_state::compiling, std::memory_order_release);
		return m_library_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
	}

	void shader_program::finish_library_compile(MTL::Library* library, NS::Error* error, u64 generation) const
	{
		std::lock_guard lock(m_library_mutex);
		if (m_library_generation.load(std::memory_order_acquire) != generation)
		{
			// The program was replaced while the compile ran.  The delivered object
			// is retained by the handler and must not become the new program's
			// library.
			if (library)
			{
				library->release();
			}
			return;
		}

		if (m_library_state.load(std::memory_order_acquire) != library_state::compiling)
		{
			// A duplicate delivery (for example, an immediate failure callback
			// followed by the null-task path) must not replace a ready library.
			if (library)
			{
				library->release();
			}
			return;
		}

		copy_error_text(m_library_error_text, error);
		if (library)
		{
			m_library = library;
		}
		else
		{
			rsx_log.error("Metal: library creation failed for msl bytes %zu: %s", msl.size(),
				m_library_error_text[0] ? m_library_error_text : "unknown");
		}

		m_library_state.store(library ? library_state::ready : library_state::failed, std::memory_order_release);
		m_library_cv.notify_all();
	}

	void shader_program::attach_library_task(MTL4::CompilerTask* task) const
	{
		if (!task)
		{
			return;
		}

		std::lock_guard lock(m_library_mutex);
		if (m_library_state.load(std::memory_order_acquire) == library_state::compiling && !m_library_task)
		{
			m_library_task = task;
			return;
		}

		// The handler already ran, or a task was already attached.  The task is
		// finished or redundant; release it on this thread.
		task->release();
	}

	void shader_program::release_finished_library_task() const
	{
		std::lock_guard lock(m_library_mutex);
		if (m_library_task && m_library_state.load(std::memory_order_acquire) != library_state::compiling)
		{
			m_library_task->release();
			m_library_task = nullptr;
		}
	}

	bool shader_program::ensure_library(MTL4::Compiler* compiler) const
	{
		return ensure_library_sync(compiler);
	}

	bool shader_program::ensure_library_sync(MTL4::Compiler* compiler) const
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "program/library");

		if (!compiler || msl.empty())
		{
			return false;
		}

		u64 generation = 0;
		{
			std::unique_lock lock(m_library_mutex);

			if (m_library_state.load(std::memory_order_acquire) == library_state::ready)
			{
				// TODO(excise): level 2; the library cache served this program.
				MTL_PROBE_LATCH(2, "cache/pipeline hit");
				return true;
			}

			if (m_library_state.load(std::memory_order_acquire) == library_state::failed)
			{
				return false;
			}

			if (m_library_state.load(std::memory_order_acquire) == library_state::compiling)
			{
				// A draw submitted this program; the synchronous preload path may
				// reach it before the handler.  Wait is bounded, and the caller then
				// skips the draw if the compile did not finish.
				if (!m_library_cv.wait_for(lock, std::chrono::seconds(10), [this]
					{
						return m_library_state.load(std::memory_order_acquire) != library_state::compiling;
					}))
				{
					rsx_log.error("Metal: timed out waiting for the async library compile");
					return false;
				}

				return m_library_state.load(std::memory_order_acquire) == library_state::ready;
			}

			m_library_state.store(library_state::compiling, std::memory_order_release);
			generation = m_library_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
		}

		// TODO(excise): level 2; the library cache missed this program.
		MTL_PROBE_LATCH(2, "cache/pipeline miss");
		// TODO(excise): level 3; the library build attempt.
		MTL_NOTE(3, "program/library build, msl bytes %zu", msl.size());
		// TODO(excise): level 2; the hash of one shader, for the cache question.
		MTL_PROBE_LATCH(2, "shader hash");
		// TODO(excise): level 2; which shader hash the title built.
		MTL_PROBE_HISTOGRAM(2, "shader hash", ::mtl::probe::fnv1a(msl.data(), msl.size()), "msl");

		NS::Error* error = nullptr;
		MTL::Library* library = compile_msl(compiler, msl, &error);
		finish_library_compile(library, error, generation);
		return library != nullptr;
	}

	void pipeline_jobs::job::finish_link(MTL::RenderPipelineState* value, NS::Error* error_object)
	{
		if (value)
		{
			value->retain();
		}
		state = value;
		copy_error_text(error, error_object);
		phase.store(value ? job_phase::link_ready : job_phase::link_failed, std::memory_order_release);
	}

	pipeline_jobs::pipeline_jobs()
		: m_link_worker([this] { link_worker_main(); })
	{
	}

	void pipeline_jobs::link_worker_main()
	{
		while (true)
		{
			std::function<void()> compile;
			{
				std::unique_lock lock(m_link_mutex);
				m_link_condition.wait(lock, [this]
					{
						return m_link_stop || !m_link_queue.empty();
					});

				if (m_link_queue.empty())
				{
					if (m_link_stop)
					{
						return;
					}

					continue;
				}

				compile = std::move(m_link_queue.front());
				m_link_queue.pop_front();
			}

			{
				std::lock_guard compile_lock(m_compile_mutex);
				compile();
			}
			m_condition.notify_all();
		}
	}

	void pipeline_jobs::stop_link_worker()
	{
		{
			std::lock_guard lock(m_link_mutex);
			if (!m_link_worker.joinable())
			{
				return;
			}

			m_link_stop = true;
			m_link_queue.clear();
			m_link_condition.notify_all();
		}

		m_link_worker.join();
	}

	pipeline_jobs::~pipeline_jobs()
	{
		stop_link_worker();

		bool abandoned = false;
		{
			std::lock_guard lock(m_mutex);
			if (!m_jobs.empty())
			{
				m_abandoned = true;
				for (auto& entry : m_jobs)
				{
					entry.second->phase.store(job_phase::abandoned, std::memory_order_release);
				}

				m_jobs.clear();
				abandoned = true;
			}
		}

		if (abandoned)
		{
			// The jobs were destroyed; callbacks already hold shared_ptr records if a
			// handler is still in flight.  Delivered objects are leaked deliberately.
			rsx_log.error("Metal: pipeline registry destroyed with unfinished shader jobs");
		}
	}

	bool pipeline_jobs::submit(const vertex_program& vertex, const fragment_program& fragment,
		const pipeline_properties& properties, const shader_context& context,
		std::function<pipeline* (std::unique_ptr<pipeline>&)> callback)
	{
		std::lock_guard lock(m_mutex);

		if (!m_accepting || m_shutting_down || (context.device_lost && context.device_lost->load(std::memory_order_acquire)))
		{
			return false;
		}

		const key id{ vertex.id, fragment.id, properties };

		if (m_failures.find(id) != m_failures.end() || m_jobs.find(id) != m_jobs.end())
		{
			return false;
		}

		auto work = std::make_shared<job>();
		work->id = id;
		work->vertex = &vertex;
		work->fragment = &fragment;
		work->context = context;
		work->callback = std::move(callback);
		m_lost_flag = context.device_lost;
		m_jobs.emplace(id, std::move(work));
		m_submitted++;
		return true;
	}

	bool pipeline_jobs::permanently_failed(u32 vertex_id, u32 fragment_id, const pipeline_properties& properties) const
	{
		std::lock_guard lock(m_mutex);
		const key id{ vertex_id, fragment_id, properties };
		return m_failures.find(id) != m_failures.end();
	}

	bool pipeline_jobs::submit_library(const shader_program& program, MTL4::Compiler* compiler)
	{
		if (program.library_compilation_state() != library_state::absent)
		{
			return false;
		}

		const u64 generation = program.begin_library_compile();
		if (!generation)
		{
			return false;
		}

		MTL::NewLibraryCompletionHandlerFunction handler =
			[&program, generation](MTL::Library* library, NS::Error* error)
		{
			NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
			if (library)
			{
				library->retain();
			}
			program.finish_library_compile(library, error, generation);
			pool->release();
		};

		MTL4::CompilerTask* task = compile_msl_async(compiler, program.msl, std::move(handler));
		program.attach_library_task(task);

		if (!task)
		{
			// The handler may already have delivered a failure.  Only synthesize
			// one when the state is still compiling.
			if (program.library_compilation_state() == library_state::compiling)
			{
				program.finish_library_compile(nullptr, nullptr, generation);
			}
			return false;
		}

		return true;
	}

	bool pipeline_jobs::enqueue_link(std::function<void()>&& compile)
	{
		{
			std::lock_guard lock(m_link_mutex);
			if (m_link_stop)
			{
				return false;
			}

			m_link_queue.push_back(std::move(compile));
		}

		m_link_condition.notify_one();
		return true;
	}

	bool pipeline_jobs::run_link_sync(std::function<void()>&& compile)
	{
		if (!compile)
		{
			return false;
		}

		{
			std::lock_guard lock(m_link_mutex);
			if (m_link_stop)
			{
				return false;
			}
		}

		std::lock_guard compile_lock(m_compile_mutex);
		compile();
		return true;
	}

	bool pipeline_jobs::submit_link(const std::shared_ptr<job>& work)
	{
		const vertex_program& vertex = *work->vertex;
		const fragment_program& fragment = *work->fragment;

		if (!work->context.compiler || !vertex.library() || !fragment.library())
		{
			std::strncpy(work->error, "library unavailable", sizeof(work->error) - 1);
			work->phase.store(job_phase::link_failed, std::memory_order_release);
			return false;
		}

		work->phase.store(job_phase::link, std::memory_order_release);

		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		auto owner = std::make_shared<link_compile_owner>();

		owner->vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		owner->vertex_function->setLibrary(vertex.library());
		owner->vertex_function->setName(NS::String::string(work->context.vertex_entry.c_str(), NS::UTF8StringEncoding));

		owner->fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		owner->fragment_function->setLibrary(fragment.library());
		owner->fragment_function->setName(NS::String::string(work->context.fragment_entry.c_str(), NS::UTF8StringEncoding));

		owner->descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		owner->descriptor->setVertexFunctionDescriptor(owner->vertex_function);
		owner->descriptor->setFragmentFunctionDescriptor(owner->fragment_function);
		apply_pipeline_properties(owner->descriptor, work->id.properties);

		owner->options = nullptr;
		if (work->context.options)
		{
			// Task options are mutable, non-Sendable objects.  The compiler can
			// serialize a task after the call returns, so sharing the device's one
			// options object across links is a data race.  Give every link its own
			// snapshot and keep it alive until the compiler call has returned.
			owner->options = MTL4::CompilerTaskOptions::alloc()->init();
			if (NS::Array* archives = work->context.options->lookupArchives())
			{
				owner->options->setLookupArchives(archives);
			}
		}

		pool->release();

		const bool queued = enqueue_link([work, owner]
		{
			NS::AutoreleasePool* compile_pool = NS::AutoreleasePool::alloc()->init();

			NS::Error* error = nullptr;
			MTL::RenderPipelineState* state = work->context.compiler->newRenderPipelineState(
				owner->descriptor, owner->options, &error);

			// The synchronous call returns +1.  finish_link retains the state for
			// the pipeline; drop the compiler's reference afterwards.
			work->finish_link(state, error);
			if (state)
			{
				state->release();
			}

			compile_pool->release();
		});

		if (!queued)
		{
			std::strncpy(work->error, "link worker stopped", sizeof(work->error) - 1);
			work->phase.store(job_phase::link_failed, std::memory_order_release);
			return false;
		}

		return true;
	}

	void pipeline_jobs::abandon_locked()
	{
		m_abandoned = true;
		for (auto& entry : m_jobs)
		{
			entry.second->phase.store(job_phase::abandoned, std::memory_order_release);
		}
		m_jobs.clear();
	}

	void pipeline_jobs::advance()
	{
		std::vector<std::shared_ptr<job>> publish;
		{
			std::lock_guard lock(m_mutex);

			if (m_jobs.empty())
			{
				return;
			}

			if (m_lost_flag && m_lost_flag->load(std::memory_order_acquire))
			{
				abandon_locked();
				return;
			}

			const auto now = std::chrono::steady_clock::now();

			for (auto& entry : m_jobs)
			{
				const auto& work = entry.second;
				job_phase phase = work->phase.load(std::memory_order_acquire);

				if (phase == job_phase::libraries)
				{
					const shader_program* programs[2] = { work->vertex, work->fragment };
					bool ready = true;

					for (const shader_program* program : programs)
					{
						const library_state state = program->library_compilation_state();

						if (state == library_state::failed)
						{
							std::strncpy(work->error, program->library_error()[0] ? program->library_error() : "library compilation failed",
								sizeof(work->error) - 1);
							work->phase.store(job_phase::link_failed, std::memory_order_release);
							ready = false;
							break;
						}

						if (state == library_state::absent)
						{
							submit_library(*program, work->context.compiler);
						}

						if (program->library_compilation_state() != library_state::ready)
						{
							ready = false;
						}
					}

					if (ready && work->phase.load(std::memory_order_acquire) == job_phase::libraries)
					{
						submit_link(work);
					}
				}
				else if (phase == job_phase::link_failed)
				{
					const bool libraries_done =
						work->vertex->library_compilation_state() != library_state::compiling &&
						work->fragment->library_compilation_state() != library_state::compiling;

					if (work->error[0] == '\0' && work->attempts < 2 &&
						libraries_done && now >= work->retry_at)
					{
						work->attempts++;
						work->retry_at = now + std::chrono::milliseconds(500);
						submit_link(work);
					}
					else if (libraries_done)
					{
						// A second library can still be compiling after the first one
						// failed.  Keep the job in the registry so teardown cannot destroy
						// the program under a live completion handler.
						publish.push_back(work);
					}
				}
				else if (phase == job_phase::link_ready)
				{
					publish.push_back(work);
				}
			}

			for (const auto& work : publish)
			{
				m_jobs.erase(work->id);
			}
		}

		for (const auto& work : publish)
		{
			const job_phase phase = work->phase.load(std::memory_order_acquire);

			if (phase == job_phase::link_ready && work->state)
			{
				std::unique_ptr<pipeline> storage = std::make_unique<pipeline>();
				storage->state = work->state;
				work->state = nullptr;
				work->callback(storage);
				// TODO(excise): level 2; one asynchronous link completed.
				MTL_PROBE_LATCH(2, "program/async link ready");
			}
			else
			{
				{
					std::lock_guard lock(m_mutex);
					m_failures[work->id] = 1;
				}

				rsx_log.error("Metal: asynchronous pipeline failed (vp %u, fp %u): %s",
					work->id.vertex_id, work->id.fragment_id,
					work->error[0] ? work->error : "unknown");
				// TODO(excise): level 2; one asynchronous link failed.
				MTL_PROBE_LATCH(2, "program/async failed");
			}

			if (work->vertex)
			{
				work->vertex->release_finished_library_task();
			}

			if (work->fragment)
			{
				work->fragment->release_finished_library_task();
			}

			std::lock_guard lock(m_mutex);
			m_published++;
		}
	}

	bool pipeline_jobs::wait_for_idle(std::chrono::milliseconds timeout,
		const std::function<void(u32, u32)>& progress,
		const std::function<bool()>& stop)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;

		while (true)
		{
			advance();

			if (stop && stop())
			{
				return false;
			}

			u32 done = 0;
			u32 total = 0;
			bool empty = false;
			bool abandoned = false;
			{
				std::lock_guard lock(m_mutex);
				done = m_published;
				total = m_submitted;
				empty = m_jobs.empty();
				abandoned = m_abandoned;
			}

			if (progress)
			{
				progress(done, total);
			}

			if (abandoned)
			{
				return false;
			}

			if (empty)
			{
				return true;
			}

			if (std::chrono::steady_clock::now() >= deadline)
			{
				return false;
			}

			std::unique_lock lock(m_mutex);
			m_condition.wait_for(lock, std::chrono::milliseconds(10));
		}
	}

	bool pipeline_jobs::shutdown()
	{
		{
			std::lock_guard lock(m_mutex);
			m_accepting = false;
			m_shutting_down = true;
		}

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

		while (true)
		{
			advance();

			bool empty = false;
			{
				std::lock_guard lock(m_mutex);
				empty = m_jobs.empty();
			}

			if (empty)
			{
				stop_link_worker();
				return !m_abandoned;
			}

			if (std::chrono::steady_clock::now() >= deadline)
			{
				break;
			}

			std::unique_lock lock(m_mutex);
			m_condition.wait_for(lock, std::chrono::milliseconds(10));
		}

		u32 remaining = 0;
		{
			std::lock_guard lock(m_mutex);
			remaining = static_cast<u32>(m_jobs.size());
			abandon_locked();
			// TODO(excise): level 1; the compiler did not drain at teardown.
			MTL_PROBE_LATCH(1, "session/archive skipped after abort");
		}
		rsx_log.error("Metal: %u shader jobs did not finish before teardown; the device is left alive", remaining);
		return false;
	}

	void program_cache::preload_wait(rsx::shader_loading_dialog* dlg)
	{
		if (!m_preload_jobs)
		{
			return;
		}

		// The dialog's second bar is the asynchronous stage.  The first bar shows
		// how many archive entries were unpacked; this one shows how many pipeline
		// jobs were published.
		auto progress = [dlg](u32 done, u32 total)
		{
			if (!dlg)
			{
				return;
			}

			dlg->set_limit(1, total);
			dlg->set_value(1, done);
			dlg->update_msg(1, fmt::format("Compiling Metal pipeline %u of %u", done, total));
		};

		// A single stuck driver call must not hold the loading screen forever.
		// The draw path can still submit later, and teardown abandons the rest.
		if (!m_preload_jobs->wait_for_idle(std::chrono::minutes(10), progress, [] { return Emu.IsStopped(); }))
		{
			// TODO(excise): level 1; the preload did not drain.
			MTL_PROBE_LATCH(1, "preload/aborted");
		}
	}

	pipeline::~pipeline()
	{
		if (state)
		{
			state->release();
		}
	}

	void MTLTraits::recompile_vertex_program(const RSXVertexProgram& program, vertex_program_type& data, usz id)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "program/compile vertex");
		data.id = static_cast<u32>(id);
		data.spv.clear();
		data.msl.clear();
		data.color_output_mask = 0;
		data.constant_ids.clear();
		data.has_indexed_constants = false;

		// Vulkan-rule GLSL: glslang needs it to reach SPIR-V (HLD-05 §2.3).
		std::string glsl = decompile_vertex_program(program, data.constant_ids, data.has_indexed_constants);
		if (glsl.empty())
		{
			rsx_log.error("Metal: the vertex decompile produced no GLSL (vp id %u)", data.id);
			return;
		}

		if (!spirv::compile_glsl_to_spv(data.spv, glsl, ::glsl::glsl_vertex_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: vertex SPIR-V compilation failed (vp id %u)", data.id);
			data.spv.clear();
			return;
		}

		if (!spv_to_msl(data.spv, true, data.msl, &data.color_output_mask))
		{
			rsx_log.error("Metal: vertex MSL translation failed (vp id %u)", data.id);
		}

		write_program_log("Vertex", data.id, glsl, data.msl);

		// TODO(excise): level 3; the vertex build reached the MSL stage.
		MTL_NOTE(3, "program/compile vertex, vp id %u, msl bytes %zu", data.id, data.msl.size());
		// TODO(excise): level 2; the vertex build ran.
		MTL_PROBE_LATCH(2, "shader/vertex build");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; writes the vertex GLSL and MSL.
		if (::mtl::probe::at_least(5))
		{
			diagnostic_dump_program("vs", data.id, glsl, data.msl);
		}
#endif
	}

	void MTLTraits::recompile_fragment_program(const RSXFragmentProgram& program, fragment_program_type& data, usz id)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "program/compile fragment");
		data.id = static_cast<u32>(id);
		data.spv.clear();
		data.msl.clear();
		data.color_output_mask = 0;
		data.constant_offsets.clear();

		std::string glsl = decompile_fragment_program(program, data.constant_offsets);
		if (glsl.empty())
		{
			rsx_log.error("Metal: the fragment decompile produced no GLSL (fp id %u)", data.id);
			return;
		}

		if (!spirv::compile_glsl_to_spv(data.spv, glsl, ::glsl::glsl_fragment_program, ::glsl::glsl_rules_vulkan))
		{
			rsx_log.error("Metal: fragment SPIR-V compilation failed (fp id %u)", data.id);
			data.spv.clear();
			return;
		}

		if (!spv_to_msl(data.spv, false, data.msl, &data.color_output_mask))
		{
			rsx_log.error("Metal: fragment MSL translation failed (fp id %u)", data.id);
		}

		write_program_log("Fragment", data.id, glsl, data.msl);

		// TODO(excise): level 3; the fragment build reached the MSL stage.
		MTL_NOTE(3, "program/compile fragment, fp id %u, msl bytes %zu", data.id, data.msl.size());
		// TODO(excise): level 2; the fragment build ran.
		MTL_PROBE_LATCH(2, "shader/fragment build");

		// TODO(excise): level 5; the MSL source of the first fragments, as one block.
		if (data.id < 4)
		{
			MTL_NOTE_DUMP(5, data.msl.c_str());
		}

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; writes the fragment GLSL and MSL.
		if (::mtl::probe::at_least(5))
		{
			diagnostic_dump_program("fp", data.id, glsl, data.msl);
		}
#endif
	}

	void MTLTraits::validate_pipeline_properties(const vertex_program_type&, const fragment_program_type& fragment, pipeline_properties& properties)
	{
		// A program whose MSL translation failed carries no reflection, so its
		// properties must be left alone.  A zero mask on a translated program is
		// not the same thing: it is a program with no colour output, and it must
		// clear every write mask so an unwritten attachment cannot be written.
		if (fragment.msl.empty())
		{
			return;
		}

		// Mask each write mask by the fragment program's live colour outputs (the
		// VKTraits intent, VKProgramBuffer.h:34-41).
		const u32 attachment_count = static_cast<u32>(properties.write_mask.size());

		static atomic_t<u32> diagnostic_validate = 0;
		if (diagnostic_validate++ < 12)
		{
			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: validate fp id %u, msl %s, reflected output mask 0x%x, write mask before 0x%x 0x%x 0x%x 0x%x",
				fragment.id, fragment.msl.empty() ? "empty" : "ok", fragment.color_output_mask,
				static_cast<u32>(properties.write_mask[0]), static_cast<u32>(properties.write_mask[1]),
				static_cast<u32>(properties.write_mask[2]), static_cast<u32>(properties.write_mask[3]));
		}

		for (u32 index = 0; index < attachment_count; ++index)
		{
			if ((fragment.color_output_mask & (1u << index)) == 0)
			{
				properties.write_mask[index] = MTL::ColorWriteMaskNone;
			}
		}

		if (diagnostic_validate <= 12)
		{
			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: validate fp id %u, write mask after 0x%x 0x%x 0x%x 0x%x, attachment count %u",
				fragment.id, static_cast<u32>(properties.write_mask[0]), static_cast<u32>(properties.write_mask[1]),
				static_cast<u32>(properties.write_mask[2]), static_cast<u32>(properties.write_mask[3]),
				properties.attachment_count);
		}
	}

	MTLTraits::pipeline_type* MTLTraits::build_pipeline(
		const vertex_program_type& vertex,
		const fragment_program_type& fragment,
		const pipeline_properties& properties,
		bool compile_async,
		std::function<pipeline_type*(pipeline_storage_type&)> callback,
		const ExtraData& extra)
	{
		// TODO(excise): level 1; block timer for the submission only.
		MTL_PERF_SCOPE(1, "program/pipeline");

		if (!compile_async || !extra.jobs)
		{
			// A synchronous build must not race the link worker's compiler call:
			// the serializer is not re-entrant (L18).  The worker path already
			// owns the compile mutex, so route the fallback through the same gate.
			if (extra.jobs)
			{
				pipeline_type* result = nullptr;
				if (!extra.jobs->run_link_sync([&]
					{
						result = build_pipeline_sync(vertex, fragment, properties, std::move(callback), extra);
					}))
				{
					return nullptr;
				}

				return result;
			}

			return build_pipeline_sync(vertex, fragment, properties, std::move(callback), extra);
		}

		if (extra.jobs->permanently_failed(vertex.id, fragment.id, properties))
		{
			return nullptr;
		}

		if (!extra.jobs->submit(vertex, fragment, properties, extra, std::move(callback)))
		{
			return nullptr;
		}

		// TODO(excise): level 2; one asynchronous job was submitted.
		MTL_PROBE_LATCH(2, "program/async submitted");

		if (extra.drain_on_submit)
		{
			extra.jobs->advance();
		}

		return nullptr;
	}

	MTLTraits::pipeline_type* MTLTraits::build_pipeline_sync(
		const vertex_program_type& vertex,
		const fragment_program_type& fragment,
		const pipeline_properties& properties,
		std::function<pipeline_type*(pipeline_storage_type&)> callback,
		const ExtraData& extra)
	{
		if (!extra.compiler)
		{
			rsx_log.error("Metal: no MTL4 compiler for the pipeline build");
			return nullptr;
		}

		// The synchronous preload path also creates NS objects on a thread that has
		// no autorelease pool.
		NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();

		// The library is per program id and is created once (HLD-05 §3.6).
		if (!vertex.ensure_library(extra.compiler) || !fragment.ensure_library(extra.compiler))
		{
			rsx_log.error("Metal: shader library creation failed");
			pool->release();
			return nullptr;
		}

		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(vertex.library());
		vertex_function->setName(NS::String::string(extra.vertex_entry.c_str(), NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(fragment.library());
		fragment_function->setName(NS::String::string(extra.fragment_entry.c_str(), NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);

		// The landed state applier fills the PSO-relevant subset (MTLPipeline.h).
		apply_pipeline_properties(descriptor, properties);

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = extra.compiler->newRenderPipelineState(descriptor, extra.options, &error);

		vertex_function->release();
		fragment_function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: render pipeline creation failed: %s", message);
			pool->release();
			return nullptr;
		}

		pipeline_storage_type result = std::make_unique<pipeline>();
		result->state = state;
		pipeline_type* compiled = callback(result);
		pool->release();
		return compiled;
	}
}

#endif // RSX_ENABLE_METAL4
