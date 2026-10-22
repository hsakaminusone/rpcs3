#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLPipeline.h"

#include "../Program/ProgramStateCache.h"
#include "../Program/program_util.h"
#include "../rsx_cache.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mtl
{
	// The guest ucode -> GLSL step.  The Metal decompiler subclass lands with the
	// shader front-end unit (mirrors VKVertexProgram; HLD-05 §2.1); the traits
	// call this seam so the SPIR-V -> MSL step, the library and the pipeline can
	// land first.  These are the only hop not implemented by this unit.
	//
	// The fragment seam also returns the literal relocation table, which the draw
	// path needs to fill the fragment constant file
	// (ProgramStateCache.h:141; MTLFragmentProgram.h).
	std::string decompile_vertex_program(const RSXVertexProgram& program, std::vector<u16>& constant_ids, bool& has_indexed_constants);
	std::string decompile_fragment_program(const RSXFragmentProgram& program, std::vector<u32>& constant_offsets);

	// The library stage of one program.  The shared cache has only a real entry
	// or a null entry; the registry needs the difference between absent, in
	// flight and failed so a second draw does not submit the same MSL again.
	enum class library_state : u8
	{
		absent,
		compiling,
		ready,
		failed
	};

	// One compiled guest shader: the SPIR-V, the MSL and the MTL::Library that
	// owns the entry.  The library is per program id, not per pipeline, because
	// SPIRV-Cross is not free (HLD-05 §3.6).  The library is created lazily, when
	// the MTL4::Compiler is first available.
	struct shader_program
	{
		std::vector<u32> spv;
		std::string msl;
		u32 color_output_mask = 0; // bit i set when the stage writes [[color(i)]]

		// The fragment literal relocation table, in vec4 units
		// (FragmentProgramDecompiler.h:189).  Empty for a vertex program, and for a
		// fragment program with no literals.
		std::vector<u32> constant_offsets;

		shader_program() = default;
		~shader_program();
		shader_program(const shader_program&) = delete;
		shader_program& operator=(const shader_program&) = delete;
		shader_program(shader_program&& other) noexcept;
		shader_program& operator=(shader_program&& other) noexcept;

		MTL::Library* library() const noexcept;
		library_state library_compilation_state() const noexcept;
		const char* library_error() const noexcept;

		// The async library state machine.  begin is called under the registry
		// mutex; finish is called by the Metal completion handler.
		u64 begin_library_compile() const;
		void finish_library_compile(MTL::Library* library, NS::Error* error, u64 generation) const;
		void attach_library_task(MTL4::CompilerTask* task) const;
		void release_finished_library_task() const;

		// Create the library once.  One MTL4::Compiler per device (workaround
		// W11).  `ensure_library` waits for an in-flight async compile, so the
		// synchronous preload path remains correct after a draw submitted the same
		// program.  `ensure_library_sync` is the body used by build_pipeline_sync.
		bool ensure_library(MTL4::Compiler* compiler) const;
		bool ensure_library_sync(MTL4::Compiler* compiler) const;

	private:
		// Mutable: build_pipeline receives the program by const reference
		// (ProgramStateCache.h:438-444) and the library is created there.
		mutable MTL::Library* m_library = nullptr;
		mutable MTL4::CompilerTask* m_library_task = nullptr;

		// Several preload workers reach one program through different pipelines,
		// so the creation above must happen once (HLD-07 §4.3).  The mutex is not
		// held across the compiler call: the async handler and a synchronous waiter
		// both need it.
		mutable std::mutex m_library_mutex;
		mutable std::condition_variable m_library_cv;
		mutable std::atomic<library_state> m_library_state{library_state::absent};
		mutable std::atomic<u64> m_library_generation{0};
		mutable char m_library_error_text[256]{};
	};

	// The vertex program also carries the constant relocation table that the
	// shared constant upload consumes (program_util.h:59-66; VKVertexProgram.h:56).
	struct vertex_program : shader_program, rsx::VertexProgramBase
	{
	};

	struct fragment_program : shader_program
	{
		u32 id = 0;
	};

	// The linked object: one MTL::RenderPipelineState.  The two shader_program
	// objects own their libraries, so the pipeline does not (HLD-05 §3.4).
	struct pipeline
	{
		MTL::RenderPipelineState* state = nullptr;

		pipeline() = default;
		~pipeline();
		pipeline(const pipeline&) = delete;
		pipeline& operator=(const pipeline&) = delete;
	};

	class pipeline_jobs;

	// The extra argument the shared cache forwards to build_pipeline.  The header
	// comment names it ExtraData (ProgramStateCache.h:153,157); the tree passes it
	// as the trailing Args... of get_graphics_pipeline (ProgramStateCache.h:438-444).
	struct shader_context
	{
		MTL4::Compiler* compiler = nullptr;

		// The MTL4 driver pipeline cache lookup, null on a cold start.  The
		// serializer records every pipeline the compiler builds, so the object is
		// absent when the archive is absent (HLD-07 §4.2).
		MTL4::CompilerTaskOptions* options = nullptr;

		pipeline_jobs* jobs = nullptr;

		std::string vertex_entry;
		std::string fragment_entry;

		// False during preload: only the loader drains.  The draw path leaves it
		// true so a miss submits and then drains at the next begin()/end().
		bool drain_on_submit = true;

		// The device-loss observer flag.  A lost device never submits new work.
		const std::atomic<bool>* device_lost = nullptr;
	};

	// The Metal-side registry of asynchronous shader jobs.  It owns the two
	// compiler stages and the pipeline callback; completion handlers only retain
	// the delivered object and publish a phase.  Submission and publication are
	// serialized by one mutex, while callbacks run after the mutex is released.
	class pipeline_jobs
	{
	public:
		pipeline_jobs();
		~pipeline_jobs();

		bool submit(const vertex_program& vertex,
			const fragment_program& fragment,
			const pipeline_properties& properties,
			const shader_context& context,
			std::function<pipeline* (std::unique_ptr<pipeline>&)> callback);

		void advance();
		bool wait_for_idle(std::chrono::milliseconds timeout,
			const std::function<void(u32, u32)>& progress,
			const std::function<bool()>& stop = {});
		bool shutdown();

		// Run one compiler call synchronously, serialized with the link worker.
		// Metal 4's compiler is not re-entrant with the pipeline-data-set
		// serializer (L18), and the interpreter's first use has no other way to
		// publish a pipeline.  The RSX thread is the only caller.
		bool run_link_sync(std::function<void()>&& compile);
		bool permanently_failed(u32 vertex_id, u32 fragment_id,
			const pipeline_properties& properties) const;

	private:
		struct key
		{
			u32 vertex_id = 0;
			u32 fragment_id = 0;
			pipeline_properties properties;

			bool operator==(const key& other) const
			{
				return vertex_id == other.vertex_id && fragment_id == other.fragment_id &&
					properties == other.properties;
			}
		};

		struct key_equal
		{
			bool operator()(const key& a, const key& b) const { return a == b; }
		};

		struct key_hash
		{
			usz operator()(const key& value) const
			{
				usz hash = 0;
				hash ^= rpcs3::hash_base<unsigned>(value.vertex_id);
				hash ^= rpcs3::hash_base<unsigned>(value.fragment_id);
				hash ^= rpcs3::hash_struct<pipeline_properties>(value.properties);
				return hash;
			}
		};

		enum class job_phase : u8
		{
			libraries,
			link,
			link_ready,
			link_failed,
			abandoned
		};

		struct job
		{
			key id;
			const vertex_program* vertex = nullptr;
			const fragment_program* fragment = nullptr;
			shader_context context;
			std::function<pipeline* (std::unique_ptr<pipeline>&)> callback;
			std::atomic<job_phase> phase{job_phase::libraries};
			MTL::RenderPipelineState* state = nullptr;
			char error[256]{};
			u8 attempts = 0;
			std::chrono::steady_clock::time_point retry_at{};

			void finish_link(MTL::RenderPipelineState* value, NS::Error* error_object);
		};

		bool submit_library(const shader_program& program, MTL4::Compiler* compiler);
		bool submit_link(const std::shared_ptr<job>& work);
		bool enqueue_link(std::function<void()>&& compile);
		void link_worker_main();
		void stop_link_worker();
		void abandon_locked();

		mutable std::mutex m_mutex;
		std::condition_variable m_condition;
		std::unordered_map<key, std::shared_ptr<job>, key_hash, key_equal> m_jobs;
		std::unordered_map<key, u8, key_hash, key_equal> m_failures;
		bool m_accepting = true;
		bool m_shutting_down = false;
		bool m_abandoned = false;
		u32 m_submitted = 0;
		u32 m_published = 0;
		const std::atomic<bool>* m_lost_flag = nullptr;

		// Metal 4's completion-handler render-pipeline path is not usable when a
		// pipeline data set serializer is attached: even one task crashes the
		// driver in MTLHashKey.  The synchronous call is safe, so links run on one
		// worker.  The RSX thread still never waits for a compile.
		std::mutex m_link_mutex;
		std::condition_variable m_link_condition;
		std::deque<std::function<void()>> m_link_queue;
		bool m_link_stop = false;
		std::thread m_link_worker;

		// Serializes every compiler call: the worker's synchronous links and the
		// interpreter's first-use compile.  It is not the job-submission mutex.
		std::mutex m_compile_mutex;
	};

	// The program_state_cache backend traits for Metal (HLD-01 §3.3; mirrors the
	// role of VKProgramBuffer.h:9, not its structure).
	struct MTLTraits
	{
		using vertex_program_type = vertex_program;
		using fragment_program_type = fragment_program;
		using pipeline_type = pipeline;
		using pipeline_storage_type = std::unique_ptr<pipeline_type>;
		using pipeline_properties = mtl::pipeline_properties;
		using ExtraData = shader_context;
		// RSX ucode -> GLSL -> glslang -> SPIR-V -> SPIRV-Cross -> MSL.  The
		// library is not built here: the device is not available to the cache.
		static void recompile_fragment_program(const RSXFragmentProgram& program, fragment_program_type& data, usz id);
		static void recompile_vertex_program(const RSXVertexProgram& program, vertex_program_type& data, usz id);

		// Mask the write mask by the fragment program's live colour outputs, so an
		// unwritten attachment cannot be written.
		static void validate_pipeline_properties(const vertex_program_type& vertex, const fragment_program_type& fragment, pipeline_properties& properties);

		// Assemble the MTL4::RenderPipelineDescriptor and link it.  The
		// `compile_async` flag and the callback match the shared cache contract
		// (ProgramStateCache.h:401-446).
		static pipeline_type* build_pipeline(
			const vertex_program_type& vertex,
			const fragment_program_type& fragment,
			const pipeline_properties& properties,
			bool compile_async,
			std::function<pipeline_type*(pipeline_storage_type&)> callback,
			const ExtraData& extra);

		// The synchronous body.  It is kept for the preload and for a null
		// registry, so the async dispatcher has one fallback that is already tested.
		static pipeline_type* build_pipeline_sync(
			const vertex_program_type& vertex,
			const fragment_program_type& fragment,
			const pipeline_properties& properties,
			std::function<pipeline_type*(pipeline_storage_type&)> callback,
			const ExtraData& extra);
	};

	class program_cache : public program_state_cache<MTLTraits>
	{
	public:
		explicit program_cache(decompiler_callback_t callback)
		{
			notify_pipeline_compiled = callback;
		}

		// The disk payload contract, the vk::program_cache surface
		// (VKProgramBuffer.h:79-109).  The shared set calls these.
		u64 get_hash(const mtl::pipeline_properties& props)
		{
			return rpcs3::hash_struct<mtl::pipeline_properties>(props);
		}

		u64 get_hash(const RSXVertexProgram& prog)
		{
			return program_hash_util::vertex_program_utils::get_vertex_program_ucode_hash(prog);
		}

		u64 get_hash(const RSXFragmentProgram& prog)
		{
			return program_hash_util::fragment_program_utils::get_fragment_program_ucode_hash(prog);
		}

		void set_preload_jobs(pipeline_jobs* jobs)
		{
			m_preload_jobs = jobs;
		}

		// The loader calls this after every cache entry has been submitted.  It is
		// the only drain owner during preload, so the L2 set stays single-writer.
		void preload_wait(rsx::shader_loading_dialog* dlg);

		template <typename... Args>
		void add_pipeline_entry(RSXVertexProgram& vp, RSXFragmentProgram& fp, mtl::pipeline_properties& props, Args&& ...args)
		{
			get_graphics_pipeline(nullptr, vp, fp, props, true, false, std::forward<Args>(args)...);
		}

		pipeline_jobs* m_preload_jobs = nullptr;

		void preload_programs(rsx::program_cache_hint_t* cache_hint, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
		{
			search_vertex_program(cache_hint, vp);
			search_fragment_program(cache_hint, fp);
		}
	};

	// The shared on-disk pipeline set (HLD-07 §4.1).  The payload is the pipeline
	// key itself: the MSL, the library and the state object are rebuilt from it,
	// so the set survives a Metal toolchain change.  The class string keeps the
	// Metal entries out of the Vulkan and OpenGL directories (rsx_cache.h:233-240).
	using shader_cache = rsx::shaders_cache<mtl::pipeline_properties, program_cache>;
}

#endif // RSX_ENABLE_METAL4
