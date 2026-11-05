#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLPipeline.h"

#include "../Program/ProgramStateCache.h"
#include "../Program/RSXFragmentProgram.h"
#include "../Program/RSXVertexProgram.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mtl
{
	class pipeline_jobs;

	// The compact sampler layout of one interpreter fragment shader
	// (LLD-shader-interpreter.md §6).  MSL caps a stage at 16 sampler indices,
	// while the RSX names units by index 0..15 and mixes dimensions freely.  The
	// layout keeps the four dimension arrays only when they are referenced and
	// packs them into one contiguous 0..15 sampler range: the array of type t
	// starts at `array_size` prefix `array_base[t]`, so a unit's sampler index is
	// `array_base[dimension] + index`.
	//
	// A program that references exactly one dimension takes the fast path: its
	// shader declares one 16-element array and indexes it by the RSX unit
	// directly, so the key does not depend on the referenced mask or the
	// dimensions and the four prebuilt variants are reused (R19).
	struct interpreter_texture_mapping
	{
		static constexpr u8 no_single_type = 0xff;

		u16 referenced_mask = 0;
		u32 dimensions = 0;
		std::array<u8, 16> unit_type{};  // rsx::texture_dimension_extended of each unit
		std::array<u8, 16> unit_index{}; // index inside the type's array
		std::array<u8, 4> array_size{};  // number of units of each type
		std::array<u8, 4> array_base{};  // first sampler index of each type's array
		u8 total = 0;                    // array sizes, summed
		u8 single_type = no_single_type;

		// A mapping references no texture when it has neither a raw mask (mixed
		// dimensions) nor a canonical single dimension.  referenced_mask alone is
		// not enough: canonicalize() clears it for the single-type fast path.
		bool empty() const { return !is_single() && referenced_mask == 0; }
		bool is_single() const { return single_type != no_single_type; }
		u32 global_index(u32 unit) const { return array_base[unit_type[unit]] + unit_index[unit]; }

		// Build the layout of a referenced-unit mask and the per-unit two-bit
		// texture dimensions.  A mask that references one dimension canonicalises.
		static interpreter_texture_mapping from_mask(u16 mask, u32 dimensions);

		// Drop the details the single-type shader does not bake in, so two draws
		// that differ only in which units of the same dimension they reference
		// share one library and one pipeline.
		void canonicalize();

		bool operator==(const interpreter_texture_mapping& other) const;
	};

	// What the shader source is keyed by.  The options are the shared
	// program_common::interpreter::compiler_option bits; output_count trims the
	// ocol declarations (R7).
	struct interpreter_fragment_key
	{
		u64 options = 0;
		u32 output_count = 1;
		interpreter_texture_mapping mapping;

		bool operator==(const interpreter_fragment_key& other) const
		{
			return options == other.options && output_count == other.output_count && mapping == other.mapping;
		}
	};

	struct interpreter_fragment_key_hash
	{
		usz operator()(const interpreter_fragment_key& key) const;
	};

	// The resolved request of one draw.  `build_request` rejects a program the
	// shared interpreter body cannot render faithfully; the caller then uses the
	// real pipeline (synchronously if it must).
	struct interpreter_request
	{
		u64 vertex_options = 0;
		u64 fragment_options = 0;
		u32 output_count = 1;
		interpreter_texture_mapping mapping;

		bool operator==(const interpreter_request& other) const
		{
			return vertex_options == other.vertex_options && fragment_options == other.fragment_options &&
				output_count == other.output_count && mapping == other.mapping;
		}
	};

	// The library state of one interpreter shader.  The prebuild submits the
	// library on the asynchronous compiler at init, so an entry can be in flight
	// before its first draw and that draw waits for it (LLD §9.3-§9.4).  The
	// state machine mirrors a guest `shader_program`
	// (MTLProgramPipeline.h:41-104).
	enum class interpreter_library_state : u8
	{
		compiling,
		ready,
		failed
	};

	struct interpreter_shader
	{
		interpreter_shader() = default;
		~interpreter_shader();
		interpreter_shader(const interpreter_shader&) = delete;
		interpreter_shader& operator=(const interpreter_shader&) = delete;

		// Valid only when the state is `ready`; a waiter publishes it.  The
		// pipeline link reads it after `wait` returned true.
		MTL::Library* library = nullptr;

		// Block until the library leaves `compiling` (bounded, and abandoned when
		// the device is lost) and release the compiler task.  True when ready.
		bool wait(std::chrono::milliseconds timeout);

		// The compiler completion handler and the submission path.
		void finish(MTL::Library* value, NS::Error* error);
		void attach(MTL4::CompilerTask* task);

	private:
		void release_task();

		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::atomic<interpreter_library_state> m_state{interpreter_library_state::compiling};
		std::atomic<MTL4::CompilerTask*> m_task{nullptr};
	};

	struct interpreter_pipeline
	{
		MTL::RenderPipelineState* state = nullptr;
		std::shared_ptr<interpreter_shader> vertex_shader;
		std::shared_ptr<interpreter_shader> fragment_shader;
		interpreter_texture_mapping mapping;
		u32 output_count = 1;

		interpreter_pipeline() = default;
		~interpreter_pipeline();
		interpreter_pipeline(const interpreter_pipeline&) = delete;
		interpreter_pipeline& operator=(const interpreter_pipeline&) = delete;
	};

	// The Metal 4 shader interpreter (LLD-shader-interpreter.md).  It reuses the
	// shared GLSL body, translates it through the same SPIR-V -> MSL route as a
	// real program, and links a real pipeline synchronously.  It never skips a
	// draw: an unsupported program is reported as such and the caller falls back
	// to the real pipeline.
	class interpreter_cache
	{
	public:
		void init(MTL4::Compiler* compiler, MTL4::CompilerTaskOptions* options, pipeline_jobs* jobs,
			const std::atomic<bool>* device_lost);
		void shutdown();
		void begin_frame();

		// True when the program can be rendered by the shared body.  It rejects
		// only features whose RSX behaviour the body does not reproduce; the
		// attribute-interpolation control bit is not one of them (R20).
		static bool build_request(const RSXVertexProgram& vertex,
			const RSXFragmentProgram& fragment,
			const program_hash_util::vertex_program_utils::vertex_program_metadata& vertex_metadata,
			const program_hash_util::fragment_program_utils::fragment_program_metadata& fragment_metadata,
			interpreter_request& request);

		std::shared_ptr<const interpreter_pipeline> get(const interpreter_request& request,
			const pipeline_properties& properties);

		// The GLSL source of a shader variant.  Exposed for the unit test and for
		// shader logs; the draw path goes through `get`.
		static std::string build_vertex_source(u64 options);
		static std::string build_fragment_source(const interpreter_fragment_key& key);

	private:
		std::shared_ptr<interpreter_shader> get_vertex_shader(u64 options);
		std::shared_ptr<interpreter_shader> get_fragment_shader(const interpreter_fragment_key& key);

		// The prebuild (LLD §9.3).  Called from `init`; it submits the base
		// variant libraries on the asynchronous compiler.  There is no progress
		// dialog and no guest shader is compiled.
		void prebuild();

		// Build the MSL source of one variant and compile its library.  `source`
		// returns false and logs when the variant does not translate.
		std::shared_ptr<interpreter_shader> submit_vertex_shader(u64 options);
		std::shared_ptr<interpreter_shader> submit_fragment_shader(const interpreter_fragment_key& key);
		void submit_library(const std::shared_ptr<interpreter_shader>& shader, const std::string& msl);
		bool build_vertex_msl(u64 options, std::string& msl);
		bool build_fragment_msl(const interpreter_fragment_key& key, std::string& msl);

		// Wait for a variant library.  The device-lost flag and the shutdown drain
		// bound the wait.
		std::shared_ptr<interpreter_shader> await_library(std::shared_ptr<interpreter_shader> shader);

		MTL4::Compiler* m_compiler = nullptr;
		MTL4::CompilerTaskOptions* m_options = nullptr;
		pipeline_jobs* m_jobs = nullptr;
		const std::atomic<bool>* m_device_lost = nullptr;

		std::unordered_map<u64, std::shared_ptr<interpreter_shader>> m_vertex_shaders;
		std::unordered_map<interpreter_fragment_key, std::shared_ptr<interpreter_shader>, interpreter_fragment_key_hash> m_fragment_shaders;

		struct pipeline_key
		{
			const interpreter_shader* vertex = nullptr;
			const interpreter_shader* fragment = nullptr;
			pipeline_properties properties;

			bool operator==(const pipeline_key& other) const
			{
				return vertex == other.vertex && fragment == other.fragment && properties == other.properties;
			}
		};

		struct pipeline_key_hash
		{
			usz operator()(const pipeline_key& key) const;
		};

		std::unordered_map<pipeline_key, std::shared_ptr<interpreter_pipeline>, pipeline_key_hash> m_pipelines;

		// The pipeline of the previous request.  A draw stream repeats one clause and
		// its render state across many draws, so this skips the two shader-cache probes
		// and the pipeline hash (and the `pipeline_properties` copy) for the common
		// case.  It holds a reference, so it must be dropped when the caches are.
		struct pipeline_memo
		{
			bool valid = false;
			interpreter_request request;
			pipeline_properties properties;
			std::shared_ptr<const interpreter_pipeline> pipeline;
		};

		pipeline_memo m_memo;
	};
}

#endif // RSX_ENABLE_METAL4
