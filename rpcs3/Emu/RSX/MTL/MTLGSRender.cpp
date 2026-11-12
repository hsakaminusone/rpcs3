// metal-cpp must be reached before GSRender.h: gcm_enums.h:1183 has
// `using namespace gcm`, which makes Objective-C's `Method` ambiguous in a TU
// that reaches metal-cpp afterwards.
#include "MTLDevice.h"

#include "MTLGSRender.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLCommandRecorder.h"
#include "MTLConstants.h"
#include "MTLDrawState.h"
#include "MTLEncoderState.h"
#include "MTLFill.h"
#include "MTLFormats.h"
#include "MTLFramebuffer.h"
#include "MTLPassPolicy.h"
#include "MTLPassScheduler.h"
#include "MTLPresent.h"
#include "MTLPresenter.h"
#include "MTLProgramPipeline.h"
#include "MTLResourceManager.h"
#include "MTLSamplerState.h"
#include "MTLShader.h"
#include "MTLShaderInterpreter.h"
#include "MTLSurfaceCache.h"
#include "MTLSurfaceState.h"
#include "MTLTextureCache.h"
#include "MTLZCULL.h"

#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_locking.h"
#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/Core/RSXDrawCommands.h"
#include "Emu/RSX/Core/RSXRendererFactory.h"
#include "Emu/RSX/NV47/HW/context_accessors.define.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog_native.h"
#include "Emu/RSX/Overlays/overlay_debug_overlay.h"
#include "Emu/RSX/Program/program_util.h"
#include "Emu/RSX/Program/SPIRVCommon.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "util/asm.hpp"
#include "util/video_provider.h"
#include "util/logs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

// The two media-capture triggers are owned by the Qt side and the shared report
// path (RSXThread.cpp:43, VKPresent.cpp:16).
extern atomic_t<bool> g_user_asked_for_screenshot;
extern atomic_t<recording_mode> g_recording_mode;

namespace mtl
{


	namespace
	{


		// The guest bytes an RSX texture can sample.  The footprint is the texture's
		// own guest size, across its mip chain, because the guest memory is what a
		// hazard aliases.  The host image the cache binds may be a view, a temporary
		// copy or a differently-sized section; none of that changes which guest
		// bytes the draw reads.
		template <typename Texture>
		utils::address_range32 sampled_guest_range(const rsx::context* ctx, const Texture& texture)
		{
			if (!texture.enabled())
			{
				return {};
			}

			const u32 address = rsx::get_address(ctx, texture.offset(), texture.location());
			const usz size = rsx::get_texture_size(texture);

			if (!address || !size)
			{
				return {};
			}

			// The RSX address space is 32-bit.  A footprint that would run past the
			// end cannot be expressed; the clamped range is still conservative.
			const usz limit = 0x1'0000'0000ull - address;
			return utils::address_range32::start_length(address, static_cast<u32>(std::min(size, limit)));
		}


		// The host index array and the vertex range of one subdraw
		// (GLVertexBuffers.cpp:66-150).  Metal has no fan, no quad list, no polygon
		// and no line loop (MTLDrawState.cpp:25), so those primitives expand into a
		// native index array in the shared writer (Common/BufferUtils.h:31).
		struct vertex_input_state
		{
			bool index_rebase = false;
			u32 min_index = 0;
			u32 max_index = 0;
			u32 vertex_draw_count = 0;
			u32 vertex_index_offset = 0;

			// Metal takes the index array as a GPU address argument, not an
			// argument-table entry, so it is a ring offset and a byte length.
			bool has_index_buffer = false;
			rsx::index_array_type index_type = rsx::index_array_type::u32;
			u32 index_offset = 0;
			u32 index_length = 0;
		};

		// The expansion predicate.  A native primitive reads the guest array
		// directly; the rest go through the shared writer.
		bool expands_primitive(rsx::primitive_type primitive)
		{
			return !::is_primitive_native(primitive);
		}

		// Metal restarts a strip at the largest index value and has no toggle, so a
		// guest strip with restart disabled cannot keep 0xFFFF as a vertex index in
		// a 16-bit array (MTLDrawState.cpp:25).  The strip array is widened instead.
		bool strip_topology(rsx::primitive_type primitive)
		{
			return primitive == rsx::primitive_type::line_strip ||
				primitive == rsx::primitive_type::triangle_strip ||
				primitive == rsx::primitive_type::quad_strip;
		}

		std::tuple<u32, u32, u32> widen_index_array(std::span<std::byte> dst, std::span<const std::byte> src)
		{
			const u32 count = static_cast<u32>(std::min(dst.size() / sizeof(u32), src.size() / sizeof(u16)));
			u32 min_index = 0;
			u32 max_index = 0;

			for (u32 i = 0; i < count; ++i)
			{
				be_t<u16> raw;
				std::memcpy(&raw, src.data() + i * sizeof(u16), sizeof(u16));

				const u32 index = raw.value();
				std::memcpy(dst.data() + i * sizeof(u32), &index, sizeof(u32));

				min_index = i ? std::min(min_index, index) : index;
				max_index = i ? std::max(max_index, index) : index;
			}

			return { min_index, max_index, count };
		}

		struct draw_command_visitor
		{
			const rsx::context& ctx;
			byte_ring& index_ring;
			const rsx::vertex_input_layout& layout;

			vertex_input_state operator()(const rsx::draw_array_command&) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const u32 vertex_count = clause.get_elements_count();
				const u32 min_index = clause.min_index();

				vertex_input_state result;
				result.min_index = min_index;
				result.max_index = (min_index + vertex_count) - 1;
				result.vertex_draw_count = vertex_count;

				if (expands_primitive(clause.primitive))
				{
					const u32 index_count = ::get_index_count(clause.primitive, vertex_count);
					u32 offset = 0;
					void* dst = index_ring.alloc(index_count * sizeof(u32), offset);
					if (!dst)
					{
						return {};
					}

					::write_index_array_for_non_indexed_non_native_primitive_to_buffer(
						static_cast<char*>(dst), clause.primitive, vertex_count);


					result.vertex_draw_count = index_count;
					result.has_index_buffer = true;
					result.index_type = rsx::index_array_type::u32;
					result.index_offset = offset;
					result.index_length = index_count * sizeof(u32);
				}

				return result;
			}

			vertex_input_state operator()(const rsx::draw_indexed_array_command& command) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const rsx::index_array_type type = clause.is_immediate_draw
					? rsx::index_array_type::u32
					: ctx.register_state->index_type();

				const u32 vertex_count = clause.get_elements_count();
				const u32 index_count = expands_primitive(clause.primitive)
					? ::get_index_count(clause.primitive, vertex_count)
					: vertex_count;

				const bool widen = type == rsx::index_array_type::u16 &&
					!ctx.register_state->restart_index_enabled() &&
					!expands_primitive(clause.primitive) &&
					strip_topology(clause.primitive);

				const u32 type_size = widen ? static_cast<u32>(sizeof(u32)) : ::get_index_type_size(type);
				const u32 length = index_count * type_size;
				u32 offset = 0;
				void* dst = index_ring.alloc(length, offset);
				if (!dst)
				{
					return {};
				}

				u32 min_index = 0;
				u32 max_index = 0;
				u32 written = index_count;

				if (widen)
				{
					std::tie(min_index, max_index, written) = widen_index_array(
						{ static_cast<std::byte*>(dst), length },
						command.raw_index_buffer);
				}
				else
				{
					std::tie(min_index, max_index, written) = ::write_index_array_data_to_buffer(
						{ static_cast<std::byte*>(dst), length },
						command.raw_index_buffer,
						type,
						clause.primitive,
						ctx.register_state->restart_index_enabled(),
						ctx.register_state->restart_index(),
						expands_primitive);
				}


				vertex_input_state result;
				result.min_index = min_index;
				result.max_index = max_index;
				result.vertex_draw_count = written;
				result.has_index_buffer = true;
				result.index_type = widen ? rsx::index_array_type::u32 : type;
				result.index_offset = offset;
				result.index_length = length;

				if (min_index > max_index || (min_index == max_index && clause.primitive != rsx::primitive_type::points))
				{
					// An empty set: nothing to draw.
					result.vertex_draw_count = 0;
					return result;
				}

				// The vertex range is offset by the base and the shader is told to
				// offset the vertex id, which the modulo op needs
				// (RSXVertexFetch.glsl:198).
				result.index_rebase = true;
				result.vertex_index_offset = ctx.register_state->vertex_data_base_index();
				return result;
			}

			vertex_input_state operator()(const rsx::draw_inlined_array&) const
			{
				const auto& clause = ctx.register_state->current_draw_clause;
				const u32 stream_length = static_cast<u32>(clause.inline_vertex_array.size());
				const u32 vertex_count = (stream_length * sizeof(u32)) / layout.interleaved_blocks[0]->attribute_stride;

				vertex_input_state result;
				// max_index is the last index, as it is in the other two visitors: the
				// upload derives its vertex count as (max - min) + 1.
				result.max_index = vertex_count ? vertex_count - 1 : 0;
				result.vertex_draw_count = vertex_count;

				if (expands_primitive(clause.primitive))
				{
					const u32 index_count = ::get_index_count(clause.primitive, vertex_count);
					u32 offset = 0;
					void* dst = index_ring.alloc(index_count * sizeof(u32), offset);
					if (!dst)
					{
						return {};
					}

					::write_index_array_for_non_indexed_non_native_primitive_to_buffer(
						static_cast<char*>(dst), clause.primitive, vertex_count);


					result.vertex_draw_count = index_count;
					result.has_index_buffer = true;
					result.index_type = rsx::index_array_type::u32;
					result.index_offset = offset;
					result.index_length = index_count * sizeof(u32);
				}

				return result;
			}
		};

		// The PSO key of one draw (HLD-05 §3.5).  Every field is a latch the encoder
		// state does not carry, so the key holds exactly the pipeline's inputs.
		pipeline_properties capture_pipeline_properties(const rsx::context& ctx, const surface_state& surface, const RSXVertexProgram& vertex)
		{
			blend_state blend = capture_blend_state(ctx);
			const raster_state raster = capture_raster_state(ctx);
			const anti_aliasing_state aa = capture_anti_aliasing_state(ctx.draw_state.multisample);

			// The guest drives the compose epilogue with the programmable-blend
			// descriptor bits (signed equations, forced blending); the epilogue blends
			// in the fragment shader through the framebuffer fetch.  The fixed-function
			// blend unit must not run on top of it — VK drops the FF blend here for the
			// same reason (RSX forces PB for any draw with a signed equation).  The
			// RSX bytes are otherwise unchanged; this only clears the FF enable mask.
			if (ctx.program.fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
			{
				blend.enable_mask = 0;
			}

			pipeline_properties properties;
			properties.color_format = surface.color_format;
			properties.attachment_count = static_cast<u8>(surface.color_attachment_count());
			properties.sample_count = surface.sample_count();
			properties.alpha_to_coverage = aa.alpha_to_coverage;
			properties.alpha_to_one = aa.alpha_to_one;
			properties.blend = blend;

			// The class names the draw's primitive family, which the encoder's draw
			// call restates.  A vertex program whose output mask declares the
			// point-size output writes [[point_size]] (MTLVertexProgram.cpp:408), and
			// the Metal compiler accepts that output with the point or the unspecified
			// class only: "Vertex shader writes point size but inputPrimitiveTopology
			// is MTLPrimitiveTopologyClassTriangle" (measured on the target device).
			// The RSX ignores the output of a non-point primitive, so such a draw
			// takes the class that constrains nothing; a point draw keeps the point
			// class, which is the class the output belongs to.
			const MTL::PrimitiveTopologyClass draw_class = get_primitive_topology_class(ctx.register_state->current_draw_clause.primitive);
			properties.input_primitive_topology = draw_class;

			if (draw_class != MTL::PrimitiveTopologyClassPoint &&
				(vertex.output_mask & CELL_GCM_ATTRIB_OUTPUT_MASK_POINTSIZE))
			{
				properties.input_primitive_topology = MTL::PrimitiveTopologyClassUnspecified;
			}

			// The RSX culls triangles only and Metal has no cull-both mode, so a
			// triangle-class cull-both draw drops the rasteriser while a point or
			// line draw keeps it (VK_CULL_MODE_FRONT_AND_BACK spares them too;
			// VKGSRender.cpp:212-218).  The draw's family decides this, not the class
			// the point-size rule above may have put in the key.
			properties.raster_enabled = !(culls_both_faces(raster) && draw_class == MTL::PrimitiveTopologyClassTriangle);

			for (u32 index = 0; index < properties.attachment_count; ++index)
			{
				properties.write_mask[index] = capture_color_write_mask(ctx.draw_state.blend.color_mask[index]);
			}

			return properties;
		}

		// The programmable-blend push constant (MTLFragmentProgram.cpp:183).  The
		// factor and equation fields pack the RSX enums, RGB in the low half and
		// alpha in the high half (RSXProgrammableBlendPrologue.glsl:7-9,106-108).
		u32 pack_blend_pair(u32 rgb, u32 a)
		{
			return (rgb & 0xffff) | (a << 16);
		}
	}

	// The backend objects of one renderer instance.  They are built in
	// on_init_thread and torn down in on_exit, in reverse order.
	struct MTLGSRender::frame_resources
	{
		frame_resources()
			: programs([this](const pipeline_properties& properties, const RSXVertexProgram& vp, const RSXFragmentProgram& fp)
			{
				// A pipeline the draw path built joins the on-disk set, so the next
				// boot starts warm (HLD-07 §4.1).  The preload does not notify, so a
				// load never rewrites what it read.
				if (shaders)
				{
					shaders->store(properties, vp, fp);
				}
			})
		{
		}

		resource_manager resources;
		command_recorder recorder;
		surface_cache cache;
		framebuffer attachments;
		fill_pass fill;
		presenter present;
		frame_output output;

		// The draw path (HLD-05 §3.5).
		constants_heap constants;
		depth_stencil_cache depth_cache;
		sampler_cache samplers;
		texture_cache textures;
		program_cache programs;

		// The asynchronous shader registry.  It is declared after the program
		// cache, so the cache is still alive when the registry destroys its jobs,
		// and before the disk set, which is only touched by the drain callback.
		pipeline_jobs jobs;

		// The fallback interpreter's shader and pipeline cache.  It lives beside
		// the real registry because its first-use link is serialized through the
		// same compiler gate (LLD-shader-interpreter.md §10).
		interpreter_cache interpreter;

		// The on-disk pipeline set of this title (HLD-07 §4.1).  It indexes
		// `programs`, so it shares this scope and dies before it.
		std::unique_ptr<shader_cache> shaders;
		rsx::vertex_input_layout vertex_layout;
		argument_binder binder;

		// The transform-constant file changes rarely, so it is uploaded only when
		// the shared state flag says so (Core/RSXDriverState.h:15) and the block
		// index is remembered between draws.
		u32 transform_constants_index = 0;
		bool transform_constants_valid = false;

		// The indexed-constant tables of the current instanced draw, by address.
		MTL::GPUAddress instancing_lookup = 0;
		MTL::GPUAddress instancing_constants = 0;

		// The compiled fragment program of the open clause.  emit_geometry needs its
		// literal relocation table.
		const fragment_program* compiled_fragment = nullptr;

		// The compiled vertex program of the open clause.  emit_geometry needs its
		// constant relocation table (GLGSRender.cpp:1159-1171).
		const vertex_program* compiled_vertex = nullptr;

		// The interpreter state of the open clause.  The mapping and output count
		// come from the linked interpreter pipeline; the instruction addresses are
		// frame-ring blocks uploaded once per draw, before the subdraw loop.
		bool interpreter_active = false;
		interpreter_texture_mapping interpreter_mapping;
		u32 interpreter_output_count = 1;
		// The compact bind range of the open fragment clause: the highest
		// referenced unit plus one.  A single-type shader indexes its array by RSX
		// unit, so this bounds the units it can read (LLD-shader-interpreter §6).
		u32 interpreter_texture_count = 0;
		MTL::GPUAddress interpreter_vertex_instructions = 0;
		MTL::GPUAddress interpreter_fragment_instructions = 0;
		u64 interpreter_vertex_key = 0;
		u64 interpreter_fragment_key = 0;
		MTL::GPUAddress interpreter_vertex_address = 0;
		MTL::GPUAddress interpreter_fragment_address = 0;
		argument_binder interpreter_binder;

		// The instruction blocks already uploaded in this frame, by program key.
		// The ring is per slot and reset at begin_frame, so the map is per frame.
		std::unordered_map<u64, MTL::GPUAddress> interpreter_vertex_blocks;
		std::unordered_map<u64, MTL::GPUAddress> interpreter_fragment_blocks;

		// The resolved texture units of the open clause.  The unit resolution runs
		// once per clause in end() and the subdraws only re-bind it, because a
		// subdraw never changes the texture registers (HLD-05 §3.2).
		std::array<texture_bind, fragment_texture_count> fragment_textures{};
		std::array<texture_bind, vertex_texture_count> vertex_textures{};

		// The depth-as-texture reads of the open clause.  A redirected depth unit
		// reads the stencil view of its surface at [30..45]; the emulated depth
		// compare reads the depth mirror at [50] (MTLArgumentBinds.h).
		std::array<texture_bind, fragment_texture_count> fragment_stencil{};
		texture_bind depth_input{};

		// The hazard record of the frame and its pass boundaries (HLD-03 §2.4).
		pass_scheduler scheduler;

		// The occlusion-query counters.  One shared visibility buffer serves every
		// pass of every slot (MTLZCULL.h).
		zcull zcull_control;

		// The open pass.  The header is metal-cpp-free, so the encoder lives here.
		MTL4::RenderCommandEncoder* encoder = nullptr;

	};

	// The deferred flush queue.  A guest write into a range the texture cache owns
	// faults on the protected page, and the guest bytes are canonical, so the dirty
	// sections must reach guest memory before the faulting instruction is retried.
	// The fault runs on the guest thread and the flush needs an encoder, so the
	// faulting thread posts the request, raises the backend interrupt and waits
	// (GLGSRender.cpp:1298-1307, :1385-1396).
	//
	// The wait is not an optimisation: the page keeps its protection until the
	// flush completes, so a request that is never drained leaves the guest
	// faulting on the same instruction forever.
	struct MTLGSRender::flush_queue
	{
		struct request
		{
			texture_cache::thrashed_set sections;
			bool invalidate_samplers = false;

			atomic_t<bool> processed = false;
			volatile bool received = false;

			void producer_wait()
			{
				utils::spin_wait(processed, [](bool v) { return v; });
				received = true;
			}
		};

		std::list<request> pending;
		std::mutex guard;
	};

	MTLGSRender::MTLGSRender(rsx::context* ctx, rsx::display_output* display) noexcept
		: render_backend(ctx, display)
	{
		// The guest shaders are compiled from GLSL, so the glslang context belongs
		// to the renderer instance and comes up before the first one is compiled
		// (VKGSRender.cpp:704).  Without it every shader parses against zero limits.
		spirv::initialize_compiler_context();

		// bind_context returns early for the initial context, so the backend binds
		// it explicitly (VKGSRender.cpp:438, GLGSRender.cpp:39).
		on_bind_context(m_ctx);
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender()
	{
		spirv::finalize_compiler_context();
	}

	void MTLGSRender::on_init()
	{
		m_device = device::create();
		if (!m_device)
		{
			die("no Metal 4 device is available");
		}

		// The 16-bool backend contract (RSXThread.h:80; 10 §2.2).
		m_device->fill_backend_config(m_config);

		// Which side writes the guest label decides whether an acquire can park
		// the FIFO reader on a GPU wait (DEVIATIONS-SYNC SY13, L138).
		rsx_log.notice("Metal 4: guest labels are written by the %s",
			m_config.supports_host_gpu_labels ? "GPU (host label synchronization)" : "CPU (host store)");

		// The resource layer and the recorder come up with the device, because the
		// guest window joins the persistent residency set and the ring is sized once
		// (HLD-02 I5, HLD-04 §3.2).
		m_frame_resources = std::make_unique<frame_resources>();
		m_flush_queue = std::make_unique<flush_queue>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->textures.set_bc_texture_support(m_frame_resources->resources.supports_bc_texture_compression());
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);
		m_frame_resources->fill.init(*m_device, m_frame_resources->resources);
		m_frame_resources->constants.init(m_frame_resources->resources, frame_slot_count);
		m_frame_resources->depth_cache.init(m_device->dev());
		m_frame_resources->samplers.init(m_device->dev());

		// The occlusion counters, and every pass carries their buffer
		// (MTLFramebuffer.cpp).  occlusion_queries() hands them to the thread.
		m_frame_resources->zcull_control.init(*m_device, m_frame_resources->resources, m_frame_resources->recorder);
		m_frame_resources->attachments.set_visibility_buffer(m_frame_resources->zcull_control.visibility_buffer(0));

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = m_frame)
		{
			m_frame_resources->present.init(*m_device, frame->handle());
		}

		// The output tier compiles its passes once, with the device (HLD-06 §2.6).
		if (!m_frame_resources->output.init(*m_device, m_frame_resources->resources))
		{
			die("Metal: output pass initialization failed");
		}

		// A warm start: the pipeline set of a previous run is rebuilt here, off the
		// draw path, so a draw never waits for a link (HLD-07 §4.1).  The load reports
		// its progress through the same overlay dialog as the other backends
		// (VKGSRender.cpp:1379-1391); a run with no overlay manager hides the frame
		// instead, because the load draws nothing of its own.
		m_frame_resources->shaders = std::make_unique<shader_cache>(m_frame_resources->programs, "metal", "v1");
		const shader_context context{ m_device->compiler(), m_device->pipeline_task_options(),
			&m_frame_resources->jobs, msl_entry_point, msl_entry_point, false, &m_device->lost_flag() };
		m_frame_resources->programs.set_preload_jobs(&m_frame_resources->jobs);
		m_frame_resources->interpreter.init(m_device->compiler(), m_device->pipeline_task_options(),
			&m_frame_resources->jobs, &m_device->lost_flag());

		if (!RSX(m_ctx)->overlay_manager())
		{
			GSFrameBase* frame = m_frame;

			if (frame)
			{
				frame->hide();
			}

			m_frame_resources->shaders->load(nullptr, context);

			if (frame)
			{
				frame->show();
			}
		}
		else
		{
			rsx::shader_loading_dialog_native dlg(RSX(m_ctx));
			m_frame_resources->shaders->load(&dlg, context);
		}

		create_frame_context();

		// The caches were built without a context; bind this one now.  bind_context
		// skips the initial context, so the explicit call is the only one.
		on_bind_context(m_ctx);

	}

	void MTLGSRender::on_shutdown()
	{
		// The tables are printed one last time, because a bounded run ends in a
		// signal and its periodic report is the only other chance to see them.

		if (m_frame_resources)
		{
			// The compiler is the first owner that can outlive the renderer.  A
			// timeout here leaves the frame resources and the device alive; the
			// completion handler may still hold a program address.
			if (!m_frame_resources->jobs.shutdown())
			{
				rsx_log.error("Metal: shader jobs did not drain; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				destroy_frame_context();
				return;
			}

			if (m_device && m_device->lost_flag().load(std::memory_order_acquire))
			{
				// A removed device has no valid queue or compiler.  The archive flush
				// and the ring shutdown both touch it, so leave both objects alive.
				rsx_log.error("Metal: device removed; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				destroy_frame_context();
				return;
			}

			// The ring drains first: a frame that is still in flight refers to the
			// drawable's residency set, so dropping the layer's set before the drain
			// can fault the GPU and lose the very commit feedback the drain waits for.
			m_frame_resources->recorder.shutdown();
			// The GPU has drained at this point, so the interpreter's pipeline
			// states and libraries can go with the frame resources.
			m_frame_resources->interpreter.shutdown();
			m_frame_resources->present.shutdown();
			m_frame_resources->output.shutdown();
			m_frame_resources->zcull_control.shutdown();
			m_frame_resources->textures.destroy();
			m_frame_resources->samplers.shutdown();
			m_frame_resources->depth_cache.shutdown();
			m_frame_resources->constants.shutdown();
			m_frame_resources->fill.shutdown();
			m_frame_resources->resources.shutdown();
			m_frame_resources.reset();
		}

		if (m_device)
		{
			// The compiled pipelines are written last, when no new one can join
			// (HLD-07 §4.2).
			m_device->flush_pipeline_archive();
		}

		m_device.reset();
		destroy_frame_context();
	}

	void MTLGSRender::open_frame()
	{
		if (m_frame_open || !m_frame_resources)
		{
			return;
		}

		// The ring's slot is only reusable after its previous frame has completed,
		// so begin_frame waits on the commit feedback before it resets the
		// allocator (HLD-04 §3.2).
		// A slot whose previous commit never reported is abandoned, and its command
		// buffer may still write.  The query gate must not treat that work as done
		// (Q3), so it receives the same result the recorder computed.
		const bool reported = m_frame_resources->recorder.begin_frame();
		const u32 slot = m_frame_resources->recorder.slot();
		m_frame_resources->output.on_slot_complete(slot);
		m_frame_resources->zcull_control.on_slot_complete(slot, reported);
		m_frame_resources->constants.begin_frame(m_frame_resources->recorder.slot());
		m_frame_resources->scheduler.begin_frame();
		m_frame_resources->binder.reset();
		m_frame_resources->interpreter_binder.reset();
		m_frame_resources->interpreter.begin_frame();
		m_frame_resources->interpreter_active = false;
		m_frame_resources->interpreter_vertex_key = 0;
		m_frame_resources->interpreter_fragment_key = 0;
		m_frame_resources->interpreter_vertex_address = 0;
		m_frame_resources->interpreter_fragment_address = 0;
		m_frame_resources->interpreter_vertex_blocks.clear();
		m_frame_resources->interpreter_fragment_blocks.clear();
		m_frame_resources->transform_constants_valid = false;
		m_frame_open = true;

	}

	void MTLGSRender::begin_draw()
	{
		if (!m_frame_resources)
		{
			return;
		}

		// Publish a link that completed since the previous clause.  This is the only
		// drain point inside the draw loop, and it does not wait.
		m_frame_resources->jobs.advance();
		open_frame();
	}

	bool MTLGSRender::on_framebuffer_bound(const rsx::framebuffer_layout& layout)
	{
		if (!m_frame_resources)
		{
			return false;
		}

		open_frame();

		// The core calls this hook even when the layout did not change, so the bound
		// set is still current and must not be rebuilt (RSXGraphEngine.cpp:827-839).
		if (layout.ignore_change && m_frame_resources->attachments.valid())
		{
			return true;
		}

		m_frame_resources->attachments.bind(m_frame_resources->recorder, m_frame_resources->cache,
			m_ctx->fb.layout, RSX(m_ctx)->resolution_scaling_config);
		reconcile_render_targets();


		return m_frame_resources->attachments.valid();
	}

	bool MTLGSRender::is_framebuffer_bound() const
	{
		return m_frame_resources && m_frame_resources->attachments.valid();
	}

	void MTLGSRender::reconcile_render_targets()
	{
		frame_resources& frame = *m_frame_resources;
		surface_cache& store = frame.cache;
		texture_cache& textures = frame.textures;
		command_recorder& cmd = frame.recorder;

		const u8 color_bpp = rsx::get_format_block_size_in_bytes(m_ctx->fb.layout.color_format);
		const u8 samples = rsx::get_format_sample_count(m_ctx->fb.layout.aa_mode);

		// A range that stops being a target is written back or dropped before the
		// record of it is replaced, because the cache no longer knows the address
		// afterwards (VKGSRender.cpp:2659-2692).
		for (u32 index = 0; index < rsx::limits::color_buffers_count; ++index)
		{
			if (m_ctx->fb.surface_info[index].pitch && g_cfg.video.write_color_buffers)
			{
				const auto stale_range = m_ctx->fb.surface_info[index].get_memory_range();
				textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
				textures.flush_if_cache_miss_likely(cmd, stale_range);
			}

			m_ctx->fb.surface_info[index].address = 0;
			m_ctx->fb.surface_info[index].pitch = 0;
			m_ctx->fb.surface_info[index].width = m_ctx->fb.layout.width;
			m_ctx->fb.surface_info[index].height = m_ctx->fb.layout.height;
			m_ctx->fb.surface_info[index].color_format = m_ctx->fb.layout.color_format;
			m_ctx->fb.surface_info[index].bpp = color_bpp;
			m_ctx->fb.surface_info[index].samples = samples;
		}

		if (m_ctx->fb.depth_surface_info.pitch && g_cfg.video.write_depth_buffer)
		{
			const auto stale_range = m_ctx->fb.depth_surface_info.get_memory_range();
			textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
			textures.flush_if_cache_miss_likely(cmd, stale_range);
		}

		m_ctx->fb.depth_surface_info.address = 0;
		m_ctx->fb.depth_surface_info.pitch = 0;
		m_ctx->fb.depth_surface_info.width = m_ctx->fb.layout.width;
		m_ctx->fb.depth_surface_info.height = m_ctx->fb.layout.height;
		m_ctx->fb.depth_surface_info.depth_format = m_ctx->fb.layout.depth_format;
		m_ctx->fb.depth_surface_info.bpp = rsx::get_format_block_size_in_bytes(m_ctx->fb.layout.depth_format);
		m_ctx->fb.depth_surface_info.samples = samples;

		// The surfaces the bind superseded or orphaned are released first, so a
		// release cannot drop a lock the bound-target pass below takes
		// (VKGSRender.cpp:2732-2776 then :2778-2810).
		for (surface* replaced : store.superseded_surfaces)
		{
			textures.discard_framebuffer_memory_region(cmd, replaced->get_memory_range());
		}

		store.superseded_surfaces.clear();

		for (auto& orphan : store.orphaned_surfaces)
		{
			surface* target = orphan.second;
			const auto range = target->get_memory_range();
			bool write_back = target->is_depth_surface()
				? !!g_cfg.video.write_depth_buffer
				: !!g_cfg.video.write_color_buffers;

			// An orphan that is already locked keeps its lock; locking it again
			// would protect a range its current owner does not own
			// (VKGSRender.cpp:2749-2761).
			if (write_back && !target->is_locked())
			{
				write_back = false;
			}

			if (write_back)
			{
				textures.lock_memory_region(cmd, target, range, false,
					target->get_surface_width<rsx::surface_metrics::pixels>(),
					target->get_surface_height<rsx::surface_metrics::pixels>(),
					target->get_rsx_pitch(), target);
			}
			else
			{
				textures.commit_framebuffer_memory_region(cmd, range);
			}
		}

		store.orphaned_surfaces.clear();

		for (u8 index : rsx::utility::get_rtt_indexes(m_ctx->fb.layout.target))
		{
			surface* target = store.m_bound_render_targets[index].second;
			if (!target)
			{
				continue;
			}

			m_ctx->fb.surface_info[index].address = m_ctx->fb.layout.color_addresses[index];
			m_ctx->fb.surface_info[index].pitch = m_ctx->fb.layout.actual_color_pitch[index];

			// A target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2780).
			if (!m_ctx->fb.surface_info[index].address || !m_ctx->fb.surface_info[index].pitch)
			{
				continue;
			}

			ensure(target->get_rsx_pitch() == m_ctx->fb.layout.actual_color_pitch[index]);

			const auto range = m_ctx->fb.surface_info[index].get_memory_range(m_ctx->fb.layout.aa_factors);

			// A cached view of this range, and every scaled or cropped view derived
			// from it, is stale the moment the range becomes an attachment.
			textures.notify_surface_changed(range);

			if (g_cfg.video.write_color_buffers)
			{
				// The section becomes the guest-side backing of the surface, so the
				// write-back at flip has somewhere to land (texture_cache.h:1410).
				textures.lock_memory_region(cmd, target, range, true,
					m_ctx->fb.surface_info[index].width, m_ctx->fb.surface_info[index].height,
					m_ctx->fb.surface_info[index].pitch, target);
			}
			else
			{
				// The guest copy is never updated, so drop the cached section: the
				// sample must resolve to the surface instead (texture_cache.h:2046).
				textures.commit_framebuffer_memory_region(cmd, range);
			}
		}


		if (surface* depth = store.m_bound_depth_stencil.second)
		{
			m_ctx->fb.depth_surface_info.address = m_ctx->fb.layout.zeta_address;
			m_ctx->fb.depth_surface_info.pitch = m_ctx->fb.layout.actual_zeta_pitch;

			// A depth target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2796).
			if (m_ctx->fb.depth_surface_info.address && m_ctx->fb.depth_surface_info.pitch)
			{
				ensure(depth->get_rsx_pitch() == m_ctx->fb.layout.actual_zeta_pitch);

				const auto range = m_ctx->fb.depth_surface_info.get_memory_range(m_ctx->fb.layout.aa_factors);
				textures.notify_surface_changed(range);

				if (g_cfg.video.write_depth_buffer)
				{
					textures.lock_memory_region(cmd, depth, range, true,
						m_ctx->fb.depth_surface_info.width, m_ctx->fb.depth_surface_info.height,
						m_ctx->fb.depth_surface_info.pitch, depth);
				}
				else
				{
					textures.commit_framebuffer_memory_region(cmd, range);
				}
			}
		}
	}

	void MTLGSRender::clear()
	{
		if (!m_frame_resources)
		{
			return;
		}

		// The core has already checked the frame skip, dropped the stencil bit for a
		// zero write mask, decoded draw_state.clear and bound the framebuffer.
		const rsx::clear_state& clear = m_ctx->draw_state.clear;
		const rsx::flags32_t mask = clear.mask;
		if (!mask)
		{
			return;
		}

		surface_state state = capture_surface_state(*m_ctx);

		// The colour fields are only decoded when a colour clear is possible; for a
		// float target the core deliberately zeroes the mask and leaves them stale.
		const bool write_color = clear.color_mask != 0;
		if (write_color)
		{
			state.clear_color = MTL::ClearColor::Make(
				clear.color_r / 255.f, clear.color_g / 255.f,
				clear.color_b / 255.f, clear.color_a / 255.f);
			state.clear_colormask = clear.color_mask;
			state.clear_color_fast = clear.color_mask_full;
		}

		const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
		const bool write_stencil = (mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0;
		if (write_depth)
		{
			// Format-decoded by the core; no host encoding is applied.
			state.clear_depth = clear.depth_value;
		}
		if (write_stencil)
		{
			state.clear_stencil = clear.stencil_value;
		}

		// A stencil write mask below 0xff makes the stencil clear a partial write,
		// which MTLLoadActionClear cannot express.  The fill then writes the
		// reference through the mask (S5; VKGSRender.cpp:1624-1638).
		const bool partial_stencil = write_stencil && clear.stencil_write_mask != 0xff;
		state.clear_stencil_fast = !partial_stencil;

		// A clear writes the attachments, so the store marks them newest for the same
		// reason a draw does: a later sample of the range must resolve to the surface
		// and not to the value the clear replaced (VKGSRender.cpp:1648-1651).
		m_frame_resources->cache.on_write({ write_color, write_color, write_color, write_color }, write_depth || write_stencil);

		framebuffer& attachments = m_frame_resources->attachments;

		if (clear.scissor_covers_surface && (!write_color || clear.color_mask_full) && !partial_stencil)
		{

			// The clear is the whole attachment, so it is a load action and needs no
			// draw (LLD-surface invariant 4).  The pass is opened so the clear is
			// recorded, then closed.
			m_frame_resources->encoder = attachments.open_pass(m_frame_resources->recorder, mask, state);
			record_pass_hazards(false);
			m_frame_resources->encoder = nullptr;
			m_frame_resources->recorder.close_encoder();
			return;
		}


		// Metal has no scissored clear, so a partial clear is a full-screen fill
		// confined by the scissor (CONFLICTS.md C1).  The core's resolve_scissor
		// already clamped the region and cleared rtt_config_valid when it is invalid.
		const areau region = scaled_scissor(m_ctx->draw_state.scissor, m_ctx->fb.layout,
			RSX(m_ctx)->resolution_scaling_config);

		MTL4::RenderCommandEncoder* encoder = attachments.open_pass(m_frame_resources->recorder, 0, state);
		if (!encoder)
		{
			return;
		}

		m_frame_resources->encoder = encoder;
		record_pass_hazards(false);

		u16 x = static_cast<u16>(region.x1);
		u16 y = static_cast<u16>(region.y1);
		u16 width = static_cast<u16>(region.width());
		u16 height = static_cast<u16>(region.height());

		std::tie(x, y, width, height) = rsx::clip_region<u16>(attachments.width(), attachments.height(), x, y, width, height, true);

		const MTL::ScissorRect scissor = build_scissor_rect(x, y, static_cast<u32>(x) + width,
			static_cast<u32>(y) + height, static_cast<u16>(attachments.height()));
		encoder->setScissorRect(scissor);

		const std::array<f32, 4> color = {
			static_cast<f32>(state.clear_color.red),
			static_cast<f32>(state.clear_color.green),
			static_cast<f32>(state.clear_color.blue),
			static_cast<f32>(state.clear_color.alpha) };
		const MTL::PixelFormat color_format = attachments.color_count() != 0 ? attachments.color(0)->pixel_format() : MTL::PixelFormatInvalid;

		// Every fill takes its own value block, because a Metal 4 command buffer does
		// not snapshot buffer contents (MTLConstants.h fill_values).
		if (const MTL::GPUAddress values = m_frame_resources->constants.alloc_fill_values(color, state.clear_depth))
		{
			const fill_stencil stencil = write_stencil
				? fill_stencil{ clear.stencil_value, clear.stencil_write_mask }
				: fill_stencil{};
			m_frame_resources->fill.fill(m_frame_resources->recorder.fragment_table(), encoder, values, color_format,
				attachments.sample_count(), to_color_write_mask(clear.color_mask), write_depth, stencil);
		}

		m_frame_resources->encoder = nullptr;
		m_frame_resources->recorder.close_encoder();
	}


	bool MTLGSRender::resolve_sampler_state(rsx::flags32_t& invalidated_programs)
	{
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;

		// The stencil half of a redirected depth unit samples through its own
		// sampler (VKDraw.cpp:707-717), shared by every unit of the clause.
		MTL::SamplerState* stencil_sampler = frame.samplers.get(stencil_mirror_sampler());
		frame.fragment_stencil = {};


		// A unit the program does not reference is never sampled, so it is skipped
		// (GLDraw.cpp:340-425).  The cache resolves the guest range to an image and
		// the sampler is a separate object, because MSL has no combined image
		// sampler (HLD-05 §3.2).
		for (u32 textures_ref = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!m_fs_sampler_descriptors[i])
			{
				m_fs_sampler_descriptors[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_fs_sampler_descriptors[i].get());
			const auto& tex = m_ctx->register_state->fragment_textures[i];

			// A disabled unit must not keep the image of the previous draw.  The
			// argument writer clears the table slot when the bind becomes null (B3).
			if (!tex.enabled())
			{
				frame.fragment_textures[i] = {};
				frame.fragment_stencil[i] = {};
				continue;
			}

			const rsx::format_class previous_class = descriptor->format_class;
			// The core clears the referenced mask after this hook returns true; do
			// not consume the bit here.
			const bool dirty = m_ctx->textures.fragment_textures_dirty & (1u << i);
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;


			}

			if (!descriptor->validate())
			{
				frame.fragment_textures[i] = {};
				continue;
			}

			descriptor->format_ex = tex.format_ex();

			if (descriptor->format_class != previous_class)
			{
				// The host image changed class under the program's feet.  The core
				// propagates the flag into graphics_state.
				invalidated_programs |= rsx::fragment_program_state_dirty;
			}

			bool deferred = false;
			frame.fragment_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_fragment_sampler(*m_ctx->register_state, static_cast<u8>(i))), deferred);


			// The transfer that filled an uploaded image is a write, and the
			// scheduler orders only the edges it holds a write record for, so the
			// read this draw makes needs that record (HLD-03 §2.4).
			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.fragment_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.fragment_textures[i].texture,
					sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i]),
					access_stages(rsx::surface_access::transfer_write, false));
			}

			// A redirected depth unit also reads the stencil half of its surface,
			// through a castable view (VKDraw.cpp:702-721).  The view is the surface
			// itself when the surface carries no stencil, and the slot then stays
			// unbound.
			if ((m_ctx->program.fragment_program.texture_state.redirected_textures & (1u << i)) &&
				stencil_sampler && descriptor->image_handle)
			{
				if (texture* stencil = descriptor->image_handle->stencil_view())
				{
					frame.fragment_stencil[i] = { stencil->resource_id(), stencil_sampler->gpuResourceID() };
				}
			}
		}

		for (u32 textures_ref = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!m_vs_sampler_descriptors[i])
			{
				m_vs_sampler_descriptors[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(m_vs_sampler_descriptors[i].get());
			const auto& tex = m_ctx->register_state->vertex_textures[i];

			// A disabled unit binds null, not the image from the previous draw (B4).
			if (!tex.enabled())
			{
				frame.vertex_textures[i] = {};
				continue;
			}

			const bool dirty = m_ctx->textures.vertex_textures_dirty & (1u << i);
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;


			}

			if (!descriptor->validate())
			{
				frame.vertex_textures[i] = {};
				continue;
			}

			bool deferred = false;
			frame.vertex_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_vertex_sampler(*m_ctx->register_state, static_cast<u8>(i))), deferred);


			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.vertex_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.vertex_textures[i].texture,
					sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i]),
					access_stages(rsx::surface_access::transfer_write, false));
			}
		}

		// The core adds textures_upload_time after this hook returns true.

		// A unit that cannot be resolved binds null; that is not a reason to skip
		// the draw or the program resolution that follows.
		return true;
	}

	void MTLGSRender::record_pass_hazards(bool sample_textures)
	{
		frame_resources& frame = *m_frame_resources;
		pass_scheduler& scheduler = frame.scheduler;

		scheduler.begin_pass();

		// The read set is the units the programs reference, not the whole resolved
		// array: an unreferenced unit still holds the previous clause's image, and a
		// barrier for it would consume a write record the frame still needs.
		if (sample_textures)
		{
			for (u32 referenced = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.fragment_textures[i].texture._impl)
				{
					scheduler.note_read(frame.fragment_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i]),
						access_stages(rsx::surface_access::shader_read, false));
				}
			}

			for (u32 referenced = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.vertex_textures[i].texture._impl)
				{
					scheduler.note_read(frame.vertex_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i]),
						access_stages(rsx::surface_access::shader_read, true));
				}
			}
		}

		// The attachment set is the pass's write set.  The store is a tile-stage
		// operation on Apple silicon (StoreActionStore), so the note names the
		// fragment and tile stages — a fragment-only producer never claims to
		// order the flush of the tile the pass wrote.
		for (u32 index = 0; index < frame.attachments.color_count(); ++index)
		{
			surface* color = frame.attachments.color(index);
			scheduler.note_write(color->resource_id(), color->get_memory_range(),
				access_stages(rsx::surface_access::shader_write, false) | MTL::StageTile);

			// A multisampled attachment resolves into its resolve target when the
			// pass ends, and the resolve target is the image a later draw samples
			// (HLD-02 §2.2).  The resolve is a tile-stage write of this pass, and
			// the scheduler orders only the writes it holds a record for, so the
			// read of the resolved image needs this one (HLD-03 §2.4).
			if (texture* resolve = color->resolve_surface.get())
			{
				// The resolve target is a private image the cache does not key by
				// guest address, but it is the surface's resolved image, so a later
				// sample of the surface range must be ordered behind it.  The write
				// therefore carries the surface's footprint, not a range of its own.
				scheduler.note_write(resolve->resource_id(), color->get_memory_range(), MTL::StageTile);
			}
		}

		if (frame.attachments.has_depth())
		{
			scheduler.note_write(frame.attachments.depth()->resource_id(), frame.attachments.depth()->get_memory_range(),
				access_stages(rsx::surface_access::shader_write, false) | MTL::StageTile);

			// A multisampled depth attachment resolves into its resolve target in
			// the resolve pass, which is a tile-stage write of that pass — the same
			// note the colour branch carries for its resolve target (HLD-02 §2.2).
			if (texture* depth_resolve = frame.attachments.depth()->resolve_surface.get())
			{
				scheduler.note_write(depth_resolve->resource_id(), frame.attachments.depth()->get_memory_range(), MTL::StageTile);
			}

			// A redirected depth unit also reads the stencil half of its surface
			// through a castable view (MTLArgumentBinds.cpp:131-138).  The view is a
			// separate Metal texture with no neighbour the scheduler can see, so the
			// read is noted here against the depth surface's footprint — the stencil
			// bytes live in the same guest range.  Coverage duplicates the mirror
			// barrier for the mirror image, and extends it to the attachment write.
			for (u32 unit = 0; unit < frame.fragment_stencil.size(); ++unit)
			{
				if (frame.fragment_stencil[unit].texture._impl)
				{
					scheduler.note_read(frame.fragment_stencil[unit].texture, frame.attachments.depth()->get_memory_range(), MTL::StageFragment);
				}
			}
		}

		// A cyclic reference the cache met while it resolved this clause: the surface
		// is a render target and a sampled input at once, and only a pass boundary
		// orders that pair (REVIEW-FINDINGS.md, I4).
		if (frame.textures.consume_texture_barrier_request())
		{
			scheduler.request_texture_barrier();
		}

		// The carried edges of the pass that split, then this pass's own.
		scheduler.emit_split_barriers(frame.encoder);
		scheduler.flush(frame.encoder);
	}

	// The draw execution (HLD-05 §3.5).  One RSX draw clause is a sequence of
	// subdraws: the pipeline, the pass and the encoder state are per clause, and
	// the vertex streams and the ABI blocks are per subdraw.  end() runs before
	// rsx::thread::end(), which owns the post-execute cleanup (RSXThread.cpp:971).
	bool MTLGSRender::end_draw()
	{

		// The core decoded the draw state, resolved the samplers and the programs,
		// and moved the pipeline bits into draw_state.dirty before this hook.  The
		// framebuffer is bound and a skipped draw never reaches here.
		if (!m_frame_resources)
		{
			return false;
		}

		frame_resources& frame = *m_frame_resources;
		auto& draw_call = m_ctx->register_state->current_draw_clause;

		// The recorder can have ended the encoder since the previous draw: an
		// upload, a resolve, a readback, a present, or a slot re-entry.  The cached
		// pointer is only valid while it still names the recorder's encoder.
		if (frame.encoder && static_cast<MTL4::CommandEncoder*>(frame.encoder) != frame.recorder.current_encoder())
		{
			frame.encoder = nullptr;
		}

		// The core's resolve_scissor already intersected the viewport, clamped to the
		// window and marked the invalid case.
		const areau scissor_region = scaled_scissor(m_ctx->draw_state.scissor, m_ctx->fb.layout,
			RSX(m_ctx)->resolution_scaling_config);


		// The programs of this draw.  The core resolved both, and the texture units
		// resolved before them.


		if (m_ctx->program.vertex_program.data.empty() || !m_ctx->program.fragment_program.ucode_length)
		{

			return false;
		}

		const surface_state surface = capture_surface_state(*m_ctx);
		pipeline_properties properties = capture_pipeline_properties(*m_ctx, surface, m_ctx->program.vertex_program);


		// The shader mode chooses how a first-use miss is covered.  The interpreter
		// renders the clause through the shared body while the real pipeline links;
		// `interpreter_only` never builds a real program at all
		// (LLD-shader-interpreter.md §10).
		const shader_mode shadermode = g_cfg.video.shadermode.get();
		const bool interpreter_enabled = shadermode == shader_mode::async_with_interpreter ||
			shadermode == shader_mode::interpreter_only;
		const bool interpreter_only = shadermode == shader_mode::interpreter_only;

		interpreter_request interpreted_request;
		std::shared_ptr<const interpreter_pipeline> interpreted;

		if (interpreter_enabled &&
			interpreter_cache::build_request(*m_ctx, m_ctx->program.vertex_program, m_ctx->program.fragment_program,
				m_ctx->program.vp_metadata, m_ctx->program.fp_metadata, interpreted_request))
		{
			// The interpreter declares only the outputs it writes.  The real
			// pipeline still sees the unmodified mask, so the trim is a copy.
			pipeline_properties interpreter_properties = properties;

			for (u32 index = interpreted_request.output_count; index < interpreter_properties.write_mask.size(); ++index)
			{
				interpreter_properties.write_mask[index] = MTL::ColorWriteMaskNone;
			}

			interpreted = frame.interpreter.get(interpreted_request, interpreter_properties);

		}

		const pipeline* program = nullptr;
		const vertex_program* compiled_vertex = nullptr;
		const fragment_program* compiled_fragment = nullptr;
		bool use_interpreter = false;

		if (interpreter_only && interpreted)
		{
			use_interpreter = true;
		}
		else
		{
			// A first-use miss yields once while the registry links.  The
			// interpreter covers that miss when it is available; a real build is
			// synchronous only for the two non-async modes and the interpreter-only
			// fallback (which means the interpreter rejected the program).
			const bool compile_async = !interpreter_only && shadermode != shader_mode::recompiler;

			const auto data = frame.programs.get_graphics_pipeline(
				&m_ctx->program.cache_hint,
				m_ctx->program.vertex_program,
				m_ctx->program.fragment_program,
				properties,
				compile_async,
				true,
				shader_context{ m_device->compiler(), m_device->pipeline_task_options(), &frame.jobs,
					msl_entry_point, msl_entry_point, true, &m_device->lost_flag() });

			program = std::get<0>(data);
			compiled_vertex = std::get<1>(data);
			compiled_fragment = std::get<2>(data);

			if (program && program->state)
			{
				// The real pipeline won.
			}
			else if (!interpreter_only && interpreted)
			{
				use_interpreter = true;
			}
			else
			{


				// The host miss contract yields once; the link is on Metal's compiler
				// threads.  Do not drain here: the next end()/begin() publishes.
				std::this_thread::yield();
				return false;
			}
		}

		// A transform-constant file is relocated against the compiled vertex
		// program.  Switching to the interpreter (or to a different compiled
		// program) invalidates that relocation, so the next upload is full.
		if (frame.interpreter_active != use_interpreter ||
			(!use_interpreter && frame.compiled_vertex != compiled_vertex))
		{
			frame.transform_constants_valid = false;
		}

		frame.interpreter_active = use_interpreter;


		if (use_interpreter)
		{
			frame.compiled_vertex = nullptr;
			frame.compiled_fragment = nullptr;
			frame.interpreter_mapping = interpreted->mapping;
			frame.interpreter_output_count = interpreted->output_count;
			frame.interpreter_texture_count = 0;
			for (u32 unit = 0; unit < rsx::limits::fragment_textures_count; ++unit)
			{
				if (m_ctx->program.fp_metadata.referenced_textures_mask & (1u << unit))
				{
					frame.interpreter_texture_count = unit + 1;
				}
			}

			// The instruction blocks of this clause.  A block is 16 header bytes
			// followed by the guest ucode; the header carries the guest addresses
			// and the per-clause state the body switches on
			// (VKShaderInterpreter.cpp:2255-2293).
			byte_ring& ring = frame.constants.instruction_ring();

			const u64 vertex_key = static_cast<u64>(m_ctx->program.vp_metadata.ucode_hash) ^
				(static_cast<u64>(m_ctx->program.vertex_program.base_address) << 32) ^
				(static_cast<u64>(m_ctx->program.vertex_program.entry) << 16) ^
				static_cast<u64>(m_ctx->program.vertex_program.output_mask) ^
				(static_cast<u64>(m_ctx->program.vp_metadata.ucode_length) << 48);

			if (auto found = frame.interpreter_vertex_blocks.find(vertex_key); found != frame.interpreter_vertex_blocks.end())
			{
				frame.interpreter_vertex_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + m_ctx->program.vp_metadata.ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter vertex instruction ring exhausted");
					return false;
				}

				u32* header = static_cast<u32*>(block);
				header[0] = m_ctx->program.vertex_program.base_address;
				header[1] = m_ctx->program.vertex_program.entry;
				header[2] = m_ctx->program.vertex_program.output_mask;
				header[3] = m_ctx->register_state->two_side_light_en() ? 1u : 0u;
				std::memcpy(static_cast<std::byte*>(block) + 16, m_ctx->program.vertex_program.data.data(), m_ctx->program.vp_metadata.ucode_length);

				frame.interpreter_vertex_instructions = ring.address_of(offset);
				frame.interpreter_vertex_blocks.emplace(vertex_key, frame.interpreter_vertex_instructions);
			}

			const u64 fragment_key = static_cast<u64>(m_ctx->program.fp_metadata.ucode_hash) ^
				(static_cast<u64>(m_ctx->register_state->shader_control()) << 32) ^
				(static_cast<u64>(m_ctx->program.fragment_program.texture_state.texture_dimensions) << 48) ^
				(static_cast<u64>(m_ctx->program.fp_metadata.program_ucode_length) << 24);

			if (auto found = frame.interpreter_fragment_blocks.find(fragment_key); found != frame.interpreter_fragment_blocks.end())
			{
				frame.interpreter_fragment_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + m_ctx->program.fp_metadata.program_ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter fragment instruction ring exhausted");
					return false;
				}

				u32 resident = 0;

				for (u32 unit = 0; unit < 16; ++unit)
				{
					if ((m_ctx->program.fp_metadata.referenced_textures_mask & (1u << unit)) &&
						frame.fragment_textures[unit].texture._impl)
					{
						resident |= 1u << unit;
					}
				}

				u32* header = static_cast<u32*>(block);
				header[0] = m_ctx->register_state->shader_control();
				header[1] = m_ctx->program.fragment_program.texture_state.texture_dimensions;
				header[2] = resident;
				header[3] = 0;
				std::memcpy(static_cast<std::byte*>(block) + 16, m_ctx->program.fragment_program.get_data(), m_ctx->program.fp_metadata.program_ucode_length);

				frame.interpreter_fragment_instructions = ring.address_of(offset);
				frame.interpreter_fragment_blocks.emplace(fragment_key, frame.interpreter_fragment_instructions);
			}
		}
		else
		{
			frame.compiled_vertex = compiled_vertex;
			frame.compiled_fragment = compiled_fragment;
		}


		// The emulated depth compare samples the depth surface, which this pass also
		// holds as its depth attachment.  A render pass cannot read its own
		// attachment, so the surface is copied to a mirror in its own encoder first
		// and the shader reads the mirror (REALISATION.md, "The depth/stencil-as-
		// texture cluster").
		frame.depth_input = {};
		if ((m_ctx->program.fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE) && frame.attachments.has_depth())
		{
			const MTL::ResourceID mirror = frame.attachments.depth()->mirror_depth(frame.recorder);
			MTL::SamplerState* sampler = frame.samplers.get(stencil_mirror_sampler());
			if (sampler && mirror._impl)
			{
				frame.depth_input = { mirror, sampler->gpuResourceID() };
			}
		}

		// C3: a draw cannot sample an attachment the open encoder is still rendering.
		// Metal has no feedback primitive and a stage barrier cannot make tile memory
		// samplable, so the encoder closes first.  The store plus the recorder's
		// boundary barrier then make the stored image legal input to the fresh pass
		// (H2).  The same-clause copy (L204) covers the case where the cache already
		// resolved the unit to a temporary: that copy closed the encoder itself.
		const auto samples_open_pass = [&]() -> bool
		{
			const auto conflicts = [&](MTL::ResourceID resource, const utils::address_range32& range)
			{
				for (u32 index = 0; index < frame.attachments.color_count(); ++index)
				{
					mtl::surface* color = frame.attachments.color(index);

					if (!color)
					{
						continue;
					}

					if (color->resource_id()._impl == resource._impl ||
						(range.valid() && range.overlaps(color->get_memory_range())))
					{
						return true;
					}
				}

				if (mtl::surface* depth = frame.attachments.depth())
				{
					if (depth->resource_id()._impl == resource._impl ||
						(range.valid() && range.overlaps(depth->get_memory_range())))
					{
						return true;
					}
				}

				return false;
			};

			for (u32 referenced = m_ctx->program.fp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.fragment_textures[i].texture._impl &&
					conflicts(frame.fragment_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->fragment_textures[i])))
				{
					return true;
				}
			}

			for (u32 referenced = m_ctx->program.vp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.vertex_textures[i].texture._impl &&
					conflicts(frame.vertex_textures[i].texture,
						sampled_guest_range(m_ctx, m_ctx->register_state->vertex_textures[i])))
				{
					return true;
				}
			}

			return false;
		};

		// A copy or upload during texture resolution can have ended the encoder
		// since the top of this function; the pointer is only valid while the
		// recorder still owns it.  A copy, upload, readback, framebuffer change or a
		// re-entered slot closes the encoder behind this pointer, so the recorder is
		// the authority (C1).  The depth mirror above is one such close: it runs in
		// its own encoder.
		if (frame.encoder &&
			static_cast<MTL4::CommandEncoder*>(frame.encoder) != frame.recorder.current_encoder())
		{
			frame.encoder = nullptr;
		}

		draw_pass_policy policy;
		policy.encoder_open = frame.encoder != nullptr;
		policy.recorder_owns_encoder = policy.encoder_open;
		policy.pass_key_matches = policy.encoder_open && frame.attachments.can_absorb();
		policy.samples_attachment = policy.encoder_open && samples_open_pass();

		if (policy.samples_attachment)
		{
			frame.encoder = nullptr;
			policy.encoder_open = false;
			frame.recorder.close_encoder();
		}

		MTL4::RenderCommandEncoder* encoder = nullptr;

		if (policy.absorbs())
		{
			encoder = frame.encoder;
		}
		else
		{
			encoder = frame.attachments.open_pass(frame.recorder, 0, surface);

			if (!encoder)
			{

				// Returning false runs the NOP draw and the shared bookkeeping
				// (VKDraw.cpp:1283-1288).
				return false;
			}

			frame.encoder = encoder;

			// The hazard notes describe a pass, not a draw.  Within one encoder the
			// only writes are attachment writes, ordered by the hardware's defined
			// raster order, and a sampled attachment is copied before the pass opens.
			// So the notes are recorded once per pass, and the recorder's boundary
			// barrier orders the few pass transitions (C2/C6).
			record_pass_hazards(true);
		}

		// The mirror copy ran in the encoder that open_pass closed, so the pass reads
		// the mirror across a queue-scope edge.
		if (frame.depth_input.texture._impl)
		{
			emit_queue_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });
		}

		encoder->setRenderPipelineState(use_interpreter ? interpreted->state : program->state);

		const auto encoder_state = capture_encoder_state(*m_ctx, !!(m_ctx->program.fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE));
		MTL::ScissorRect scissor = build_scissor_rect(scissor_region.x1, scissor_region.y1, scissor_region.x1 + scissor_region.width(), scissor_region.y1 + scissor_region.height(), encoder_state.clip_height);
		apply_encoder_state(encoder, encoder_state, scissor, frame.depth_cache);


		// The indexed-constant tables of a hardware-instanced draw.  The shared
		// helper replays the clause, so it runs before the subdraw loop
		// (RSXDrawCommands.cpp:743-751).
		if (m_ctx->program.vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS)
		{
			rsx::io_buffer lookup([&](usz size) -> std::tuple<void*, usz>
			{
				s32* block = frame.constants.alloc_instancing_lookup(static_cast<u32>(size / sizeof(s32)));
				frame.instancing_lookup = frame.constants.instancing_lookup_address();
				return { block, size };
			});

			rsx::io_buffer constants([&](usz size) -> std::tuple<void*, usz>
			{
				vec4_block* block = frame.constants.alloc_instancing_constants(static_cast<u32>(size / sizeof(vec4_block)));
				frame.instancing_constants = frame.constants.instancing_constants_address();
				return { block, size };
			});

			// A null program asks for a full re-upload, which is always correct
			// (RSXDrawCommands.cpp:752-754).  The compiled program supplies the
			// relocation table, so the instance tables stride like the shader
			// (RSXDrawCommands.cpp:755-756; VKGSRender.cpp:2167-2168).
			RSX(m_ctx)->GRAPH_frontend().fill_constants_instancing_buffer(lookup, constants, frame.compiled_vertex);
		}
		else
		{
			frame.instancing_lookup = 0;
			frame.instancing_constants = 0;
		}

		draw_call.begin();


		u32 sub_index = 0;
		do
		{
			emit_geometry(sub_index++);

			if (draw_call.is_trivial_instanced_draw && m_config.supports_hw_instanced_rendering)
			{
				draw_call.end();
			}
		}
		while (draw_call.next());

		// The post-draw instruments (tape, pixel probes, the first-draw dumps) read
		// the attachment after the draw, so they need the pass closed.  They are the
		// only post-draw reason to end a pass: every other draw leaves the encoder
		// open for the next one, which is where the pass count collapses (C1).
		constexpr bool close_for_diagnostics = false;

		if (close_for_diagnostics)
		{
			frame.encoder = nullptr;
			frame.recorder.close_encoder();
		}


		// The draw wrote the attachments, so the surfaces now hold the newest pixels
		// for their guest ranges.  A later sample resolves to them only if the store
		// says so: its write tag is what wins the tie against a cached copy
		// (texture_cache.h:2680, VKDraw.cpp:1424).
		frame.cache.on_write(m_ctx->fb.layout.color_write_enabled, m_ctx->fb.layout.zeta_write_enabled);

		// A colour attachment resolves when its pass stores, so the resolved image
		// already holds this draw.  on_write() sets require_resolve for the next
		// consumer, but that consumer would resolve the same image a second time.
		// The depth attachment is different: its resolve is the explicit pass in
		// surface::resolve(), so it keeps the flag.
		for (u32 index = 0; index < frame.attachments.color_count(); ++index)
		{
			if (auto* color = frame.attachments.color(index); color && color->resolve_surface)
			{
				color->msaa_flags &= ~rsx::surface_state_flags::require_resolve;
			}
		}

		return true;
	}

	void MTLGSRender::emit_geometry(u32 sub_index)
	{
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;
		MTL4::RenderCommandEncoder* encoder = frame.encoder;
		auto& draw_call = m_ctx->register_state->current_draw_clause;

		// The vertex state of this subdraw (GLDraw.cpp:730-760).
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: (draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask);


		if (vertex_state & rsx::vertex_arrays_changed)
		{
			RSX(m_ctx)->GRAPH_frontend().analyse_inputs_interleaved(frame.vertex_layout, m_ctx->program.vp_metadata);
		}
		else if (vertex_state & rsx::vertex_base_changed)
		{
			for (auto& info : frame.vertex_layout.interleaved_blocks)
			{
				info->vertex_range.second = 0;
				const u32 vertex_base_offset = m_ctx->register_state->vertex_data_base_offset();
				info->real_offset_address = rsx::get_address(m_ctx, rsx::get_vertex_offset_from_base(vertex_base_offset, info->base_offset), info->memory_location);
			}
		}
		else
		{
			for (auto& info : frame.vertex_layout.interleaved_blocks)
			{
				info->vertex_range.second = 0;
			}
		}

		if (vertex_state && !frame.vertex_layout.validate())
		{
			// No vertex inputs are enabled.  Nothing reaches the GPU, but the
			// clause's remaining pipeline dependencies still have to run
			// (VKDraw.cpp:1026-1034).
			do
			{
				draw_call.execute_pipeline_dependencies(m_ctx);
			}
			while (draw_call.next());

			draw_call.end();
			return;
		}

		// The index array and the vertex range of this subdraw
		// (GLVertexBuffers.cpp:66-150).
		const vertex_input_state input = std::visit(
			draw_command_visitor{ *m_ctx, frame.constants.index_ring(), frame.vertex_layout },
			RSX(m_ctx)->GRAPH_frontend().get_draw_command(*m_ctx->register_state));


		if (input.vertex_draw_count == 0)
		{
			return;
		}

		// The vertex upload.  The stream rings are bound once per frame, so only
		// the ring offsets travel per draw (HLD-05 §3.5).
		const u32 vertex_count = (input.max_index - input.min_index) + 1;
		u32 vertex_base = input.min_index;
		if (input.index_rebase)
		{
			vertex_base = rsx::get_index_from_base(vertex_base, m_ctx->register_state->vertex_data_base_index());
		}

		const auto required = RSX(m_ctx)->GRAPH_frontend().calculate_memory_requirements(frame.vertex_layout, vertex_base, vertex_count);
		u32 persistent_base = 0;
		u32 volatile_base = 0;
		void* persistent_data = nullptr;
		void* volatile_data = nullptr;

		if (required.first)
		{
			persistent_data = frame.constants.persistent_stream().alloc(required.first, persistent_base);
			if (!persistent_data)
			{
				return;
			}
		}

		if (required.second)
		{
			volatile_data = frame.constants.volatile_stream().alloc(required.second, volatile_base);
			if (!volatile_data)
			{
				return;
			}
		}

		m_ctx->stats.setup_time += m_profiler.duration();

		if (persistent_data || volatile_data)
		{
			RSX(m_ctx)->GRAPH_frontend().write_vertex_data_to_memory(frame.vertex_layout, vertex_base, vertex_count, persistent_data, volatile_data);
		}

		m_ctx->stats.vertex_upload_time += m_profiler.duration();


		// The ABI blocks of this subdraw (HLD-05 §3.5).  The rings are per frame
		// slot, so a block is written, never read back.
		vertex_context* context = frame.constants.alloc_vertex_context();
		draw_parameters* params = frame.constants.alloc_draw_parameters();
		fragment_context* fragment = frame.constants.alloc_fragment_context();
		sampler_info* sampler = frame.constants.alloc_sampler_info();
		vec4_block* stipple = frame.constants.alloc_stipple();
		u32* vs_params = frame.constants.alloc_vertex_draw_params();

		if (!context || !params || !fragment || !sampler || !stipple || !vs_params)
		{
			return;
		}

		RSX(m_ctx)->GRAPH_frontend().fill_scale_offset_data(context->scale_offset_mat, false);
		RSX(m_ctx)->GRAPH_frontend().fill_user_clip_data(&context->user_clip_configuration_bits);
		context->transform_branch_bits = m_ctx->register_state->transform_branch_bits();
		context->point_size = m_ctx->register_state->point_size() * RSX(m_ctx)->resolution_scaling_config.scale_factor();
		context->z_near = m_ctx->register_state->clip_min();
		context->z_far = m_ctx->register_state->clip_max();

		RSX(m_ctx)->GRAPH_frontend().fill_fragment_state_buffer(fragment, m_ctx->program.fragment_program);
		m_ctx->program.fragment_program.texture_params.write_to(sampler, m_ctx->program.fp_metadata.referenced_textures_mask);
		std::memcpy(stipple, m_ctx->register_state->polygon_stipple_pattern(), stipple_block_count * sizeof(vec4_block));

		// The transform constants.  The full 468-vec4 file is re-uploaded only when
		// the shared state flag says the file changed
		// (Core/RSXDriverState.h:15).  The shader reads the compacted file, so the
		// upload applies the program's relocation table (GLGSRender.cpp:1159-1171).
		if (!frame.transform_constants_valid || m_ctx->draw_state.dirty.test(rsx::draw_state_t::group::transform_constants))
		{
			vec4_block* block = frame.constants.alloc_transform_constants();
			if (!block)
			{
				return;
			}

			const std::span<const u16> reloc_table = (frame.compiled_vertex && !frame.compiled_vertex->has_indexed_constants)
				? std::span<const u16>(frame.compiled_vertex->constant_ids)
				: std::span<const u16>{};

			RSX(m_ctx)->GRAPH_frontend().fill_vertex_program_constants_data(block, reloc_table);
			frame.transform_constants_index = frame.constants.transform_constants_index();
			frame.transform_constants_valid = true;
			m_ctx->draw_state.dirty.clear(rsx::draw_state_t::group::transform_constants);

		}

		params->vertex_base_index = input.index_rebase ? input.min_index : 0;
		params->vertex_index_offset = input.vertex_index_offset;
		params->draw_id = 0;
		params->xform_constants_offset = frame.transform_constants_index;
		params->vs_context_offset = frame.constants.vertex_context_index();
		params->fs_context_offset = frame.constants.fragment_context_index();
		params->fs_texture_base_index = frame.constants.sampler_info_index();
		params->fs_stipple_pattern_offset = frame.constants.stipple_index();
		params->reserved = 0;

		// The fragment literals, when the program has any
		// (ProgramStateCache.h:141).  The relocation table is the compiled
		// program's, not the RSX program's.
		params->fs_constants_offset = 0;
		const usz fragment_constant_count = frame.compiled_fragment ? frame.compiled_fragment->constant_offsets.size() : 0;
		if (fragment_constant_count)
		{
			vec4_block* constants = frame.constants.alloc_fragment_constants(static_cast<u32>(fragment_constant_count));
			if (!constants)
			{
				return;
			}

			rsx::write_fragment_constants_to_buffer(
				{ reinterpret_cast<f32*>(constants), fragment_constant_count * 4 },
				m_ctx->program.fragment_program,
				frame.compiled_fragment->constant_offsets,
				true);

			params->fs_constants_offset = frame.constants.fragment_constants_index();

		}

		RSX(m_ctx)->GRAPH_frontend().fill_vertex_layout_state(
			frame.vertex_layout,
			m_ctx->program.vp_metadata,
			vertex_base,
			vertex_count,
			&params->attrib_data[0][0],
			persistent_base,
			volatile_base);


		// The substituted vertex push constant carries the draw-parameter block
		// index (HLD-05 §3.3).
		*vs_params = frame.constants.draw_parameters_index();

		// The binds of this subdraw.  A block the shader indexes by offset is bound
		// once at the ring base; a block it reads at offset zero is bound at its
		// own block (HLD-05 §3.5).
		argument_binds binds;
		binds.vertex_context = frame.constants.vertex_context_address();
		binds.vertex_constants = frame.constants.vertex_constants_address();
		binds.vertex_persistent_stream = frame.constants.persistent_stream().resource_id();
		binds.vertex_volatile_stream = frame.constants.volatile_stream().resource_id();
		binds.vertex_draw_parameters = frame.constants.draw_parameters_address();
		binds.vertex_draw_params = frame.constants.vertex_draw_params_block_address();
		binds.instancing_lookup = frame.instancing_lookup;
		binds.instancing_constants = frame.instancing_constants;
		binds.fragment_constants = frame.constants.fragment_constants_address();
		binds.fragment_state = frame.constants.fragment_context_address();
		binds.fragment_texture_params = frame.constants.sampler_info_address();
		binds.fragment_stipple = frame.constants.stipple_address();

		// The units resolved for this clause (HLD-05 §3.2).
		binds.vertex_textures = frame.vertex_textures;
		binds.fragment_textures = frame.fragment_textures;
		binds.fragment_stencil = frame.fragment_stencil;
		binds.fragment_depth_input = frame.depth_input;


		if (m_ctx->program.fragment_program.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
		{
			blend_values* blend = frame.constants.alloc_blend_values();
			if (!blend)
			{
				return;
			}

			// The block carries the blend configuration and the logic op, so it is
			// filled when either feature is on; the program reads only the part it
			// emulates (REVIEW-FINDINGS.md C1).
			blend->reserved = 0;
			blend->blend_eqn = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_equation_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_equation_a()));
			blend->blend_sfactors = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_func_sfactor_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_func_sfactor_a()));
			blend->blend_dfactors = pack_blend_pair(
				static_cast<u32>(m_ctx->register_state->blend_func_dfactor_rgb()),
				static_cast<u32>(m_ctx->register_state->blend_func_dfactor_a()));

			std::copy(m_ctx->draw_state.blend.constant_color.begin(), m_ctx->draw_state.blend.constant_color.end(), blend->blend_constants);

			blend->logic_op = static_cast<u32>(m_ctx->register_state->logic_operation());

			binds.fragment_blend = frame.constants.blend_values_block_address();
		}

		// Metal 4 captures the argument table once per draw, when the draw is
		// encoded, so the table is safe to mutate between draws but nothing is
		// captured at setArgumentTable (measured, 2026-10-22).  The binder writes
		// only the slots whose value changed, including a change to zero, and the
		// table is re-bound on every subdraw (REVIEW-FINDINGS.md F19).
		bool vertex_written [[maybe_unused]] = false;
		bool fragment_written [[maybe_unused]] = false;

		{

			// The interpreter draws from its own tables, so its compact sampler
			// arrays never collide with a regular draw's unit bindings in the shared
			// tables (LLD-shader-interpreter.md §4.2).
			MTL4::ArgumentTable* const vertex_argument_table = frame.interpreter_active
				? frame.recorder.interpreter_vertex_table()
				: frame.recorder.vertex_table();
			MTL4::ArgumentTable* const fragment_argument_table = frame.interpreter_active
				? frame.recorder.interpreter_fragment_table()
				: frame.recorder.fragment_table();

			if (frame.interpreter_active)
			{
				interpreter_bindings interpreter_vertex;
				interpreter_vertex.instructions = frame.interpreter_vertex_instructions;

				interpreter_bindings interpreter_fragment;
				interpreter_fragment.instructions = frame.interpreter_fragment_instructions;

				const interpreter_texture_mapping& mapping = frame.interpreter_mapping;

				if (mapping.is_single())
				{
					// The single-type shader declares a 16-element array and indexes
					// it by the RSX unit.  The compact range is the highest referenced
					// unit plus one, so only the units the body can read are bound
					// (LLD-shader-interpreter.md §6).
					interpreter_fragment.texture_count = frame.interpreter_texture_count;

					for (u32 unit = 0; unit < frame.interpreter_texture_count; ++unit)
					{
						interpreter_fragment.textures[unit] = frame.fragment_textures[unit];
					}
				}
				else
				{
					// The mixed shader's arrays are packed by dimension.  The compact
					// index of a unit is its type's prefix plus its position in that
					// type, and the binder writes that index into the interpreter's
					// 12..27 texture range (LLD-shader-interpreter.md §6).
					u32 count = 0;

					for (u32 unit = 0; unit < 16; ++unit)
					{
						if (!(mapping.referenced_mask & (1u << unit)))
						{
							continue;
						}

						const u32 index = mapping.global_index(unit);
						interpreter_fragment.textures[index] = frame.fragment_textures[unit];
						count = std::max(count, index + 1);
					}

					interpreter_fragment.texture_count = count;
				}

				vertex_written = frame.interpreter_binder.bind_interpreter(
					vertex_argument_table, true, binds, interpreter_vertex);
				fragment_written = frame.interpreter_binder.bind_interpreter(
					fragment_argument_table, false, binds, interpreter_fragment);
			}
			else
			{
				vertex_written = frame.binder.bind(vertex_argument_table, true, binds);
				fragment_written = frame.binder.bind(fragment_argument_table, false, binds);
			}

			encoder->setArgumentTable(vertex_argument_table, MTL::RenderStageVertex);
			encoder->setArgumentTable(fragment_argument_table, MTL::RenderStageFragment);
		}


		m_ctx->stats.setup_time += m_profiler.duration();

		const MTL::PrimitiveType primitive = get_primitive_type(draw_call.primitive);

		// A ZCULL query that started or ended between draws takes effect on the
		// encoder that carries the following draws (MTLZCULL.h).
		frame.zcull_control.apply(encoder);


		if (input.has_index_buffer)
		{
			const MTL::IndexType index_type = get_index_type(input.index_type);
			const MTL::GPUAddress index_address = frame.constants.index_ring().address() + input.index_offset;
			const u32 type_size = ::get_index_type_size(input.index_type);

			if (draw_call.is_trivial_instanced_draw)
			{
				encoder->drawIndexedPrimitives(primitive, input.vertex_draw_count, index_type,
					index_address, input.index_length, draw_call.pass_count(), 0, 0);
			}
			else if (draw_call.is_single_draw())
			{
				encoder->drawIndexedPrimitives(primitive, input.vertex_draw_count, index_type,
					index_address, input.index_length, 1, 0, 0);
			}
			else
			{
				// One draw per clause subrange: a primitive_restart_barrier inside
				// the clause ends a strip, so the ranges must not join into one
				// draw (VKDraw.cpp:1242-1252).
				u32 index_offset = 0;

				for (const auto& range : draw_call.get_subranges())
				{
					const u32 count = ::get_index_count(draw_call.primitive, range.count);
					const u32 byte_offset = index_offset * type_size;

					// Metal 4 has no separate index-buffer offset: the address is the
					// buffer base and the seventh argument is baseVertex.  Advance the
					// address and shrink the declared length instead (VKDraw.cpp:1242).
					if (byte_offset >= input.index_length)
					{
						break;
					}

					encoder->drawIndexedPrimitives(primitive, count, index_type, index_address + byte_offset,
						input.index_length - byte_offset, 1, 0, 0);
					index_offset += count;
				}
			}
		}
		else if (draw_call.is_trivial_instanced_draw)
		{
			encoder->drawPrimitives(primitive, 0, input.vertex_draw_count, draw_call.pass_count(), 0);
		}
		else if (draw_call.is_single_draw())
		{
			encoder->drawPrimitives(primitive, 0, input.vertex_draw_count, 1, 0);
		}
		else
		{
			// The non-indexed sibling of the subrange split above
			// (VKDraw.cpp:1194-1200).
			u32 vertex_offset = 0;

			for (const auto& range : draw_call.get_subranges())
			{
				encoder->drawPrimitives(primitive, vertex_offset, range.count, 1, 0);
				vertex_offset += range.count;
			}
		}

		m_ctx->stats.draw_exec_time += m_profiler.duration();
		m_ctx->stats.draw_calls++;
	}


	// Resolve the image that carries the RSX output.  A surface at the display
	// address wins, then a texture the cache already holds, then a copy of the guest
	// bytes.  `width` and `height` come back as the image's own extent.
	void MTLGSRender::get_present_source(u32 address, u32& width, u32& height, u32 pitch, u32 format, u8 eye,
		MTL::Texture*& image)
	{
		auto& resources = *m_frame_resources;

		// The display format is a GCM texture format throughout: the AV configuration
		// reports it that way (rsx_utils.cpp:151), the image lookup and the pixel
		// format below take it that way, and the Vulkan present path hands it to this
		// same overload (VKPresent.cpp:314).  The surface-colour overload rejects it
		// ("Invalid color format 0x85" for CELL_GCM_TEXTURE_A8R8G8B8) and the throw
		// ends the RSX thread.
		const u32 format_bpp = rsx::get_format_block_size_in_bytes(format);

		const auto overlap_info = resources.cache.get_merged_texture_memory_region(
			resources.recorder, address, width, height, pitch, format_bpp, rsx::surface_access::transfer_read);

		if (!overlap_info.empty())
		{
			const auto& section = overlap_info.back();
			surface* target = section.surface;

			if (section.base_address >= address)
			{
				const u32 surface_width = target->get_surface_width<rsx::surface_metrics::samples>();
				const u32 surface_height = target->get_surface_height<rsx::surface_metrics::samples>();
				bool viable = false;

				if (section.base_address == address)
				{
					// The surface either fills the buffer or crops into it.
					viable = (surface_width >= width && surface_height >= height);
				}
				else
				{
					// The surface sits inside the buffer, so the buffer carries a
					// border and the insets must be symmetric (GLPresent.cpp:82).
					const u32 inset_offset = section.base_address - address;
					const u32 inset_y = inset_offset / pitch;
					const u32 inset_x = (inset_offset % pitch) / format_bpp;

					viable = ((surface_width + inset_x + inset_x) == width &&
						(surface_height + inset_y + inset_y) == height);
				}

				if (viable)
				{
					// A transfer access resolves a multisampled surface, so the
					// present always reads one sample per pixel
					// (VKRenderTargets.cpp:839).
					target->memory_barrier(resources.recorder, rsx::surface_access::transfer_read);

					if (texture* output = target->get_surface(rsx::surface_access::transfer_read))
					{
						image = output->value();


						std::tie(width, height) = rsx::apply_resolution_scale<true>(
							RSX(m_ctx)->resolution_scaling_config, std::min(surface_width, width), std::min(surface_height, height));
						return;
					}
				}
			}
		}
		else if (auto* cached = resources.textures.find_texture_from_dimensions<true>(address, format);
			cached && cached->get_raw_texture() && cached->get_raw_texture()->width() >= width &&
			cached->get_raw_texture()->height() >= height)
		{
			// The frame was drawn offscreen or in software and blitted to display.
			image = cached->get_raw_texture()->value();

			return;
		}


		const MTL::PixelFormat expected_format = mtl::get_texture_format(format, resources.resources.supports_bc_texture_compression());
		const u32 pixel_size = get_pixel_size(expected_format);

		if (!pixel_size || !address || !pitch)
		{
			return;
		}

		const u32 row_bytes = width * pixel_size;
		const u64 length = static_cast<u64>(row_bytes) * height;

		MTL::Texture*& scratch = m_flip_scratch[eye];

		if (!scratch || scratch->width() != width || scratch->height() != height || scratch->pixelFormat() != expected_format)
		{
			if (scratch)
			{
				m_frame_resources->resources.defer_release(scratch);
				scratch = nullptr;
			}

			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
			descriptor->setTextureType(MTL::TextureType2D);
			descriptor->setPixelFormat(expected_format);
			descriptor->setWidth(width);
			descriptor->setHeight(height);
			descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
			scratch = m_frame_resources->resources.new_texture(descriptor, MTL::StorageModePrivate);
			descriptor->release();

			if (!scratch)
			{
				return;
			}

			m_frame_resources->resources.make_persistent(scratch);
		}

		// The guest pitch may not be the host row length, so the staging copy
		// repacks each row (GLPresent.cpp:127).
		MTL::Buffer* staging = m_frame_resources->resources.new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

		if (!staging)
		{
			return;
		}

		// Read through the unprotected mirror, the same view the guest window
		// import exposes to the GPU (vm::get_super_ptr).
		const u8* guest = vm::get_super_ptr<const u8>(address);
		u8* host = static_cast<u8*>(staging->contents());

		if (!guest || !host)
		{
			m_frame_resources->resources.defer_release(staging);
			return;
		}

		const u32 copy_bytes = std::min(row_bytes, pitch);

		for (u32 row = 0; row < height; ++row)
		{
			const u64 offset = static_cast<u64>(row) * row_bytes;
			std::memcpy(host + offset, guest + (static_cast<u64>(row) * pitch), copy_bytes);

			if (copy_bytes < row_bytes)
			{
				// A guest pitch shorter than the host row leaves a tail that the
				// staging buffer never receives; it is carried to the display, so
				// it is cleared rather than left as whatever the heap held.
				std::memset(host + offset + copy_bytes, 0, row_bytes - copy_bytes);
			}
		}

		MTL4::ComputeCommandEncoder* encoder = m_frame_resources->recorder.open_compute();

		if (!encoder)
		{
			m_frame_resources->resources.defer_release(staging);
			return;
		}

		encoder->copyFromBuffer(staging, 0, row_bytes, row_bytes * height, MTL::Size::Make(width, height, 1),
			scratch, 0, 0, MTL::Origin::Make(0, 0, 0));

		m_frame_resources->recorder.close_encoder();
		m_frame_resources->resources.defer_release(staging);

		image = scratch;
	}

	// C7.  The RSX output is the surface at the flip address.  It is composed into
	// the drawable by the output tier and presented through the one submit path the
	// recorder owns (HLD-03 §4.9, HLD-06 §2.4).
	bool MTLGSRender::present_frame(const rsx::display_flip_info_t& info)
	{

		// A flip always owns a frame.  The guest can flip with no draw since the
		// last flip, and the output pass still needs a command buffer, so the frame
		// opens here when it is not already open (VKPresent.cpp:471-493).  A flip
		// never returns with no present, because that is a frozen image
		// (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if (!m_frame_resources || !m_device || m_device->lost_flag())
		{
			return false;
		}

		open_frame();

		m_profiler.start();


		// The resolution scale is a live setting.  A change clones the resident
		// surfaces at the next flip, before the output pass reads them
		// (VKPresent.cpp:1000-1020).
		const rsx::surface_scaling_config_t active_scaling_config =
		{
			.scale_percent = static_cast<u16>(g_cfg.video.resolution_scale_percent),
			.min_scalable_dimension = static_cast<u16>(g_cfg.video.min_scalable_dimension),
		};

		if (active_scaling_config != RSX(m_ctx)->resolution_scaling_config)
		{
			m_frame_resources->cache.sync_scaling_config(m_frame_resources->recorder, active_scaling_config);
			RSX(m_ctx)->resolution_scaling_config = active_scaling_config;
		}

		auto& resources = *m_frame_resources;
		const u32 slot = resources.recorder.slot();


		// The recorder waited for this slot when the frame opened, so a capture that
		// slot recorded its previous time round is readable now (HLD-06 §2.7).
		{
			resources.output.deliver(slot, *m_frame);
		}

		const auto& avconfig = g_fxo->get<rsx::avconf>();

		u32 buffer_width = 0;
		u32 buffer_height = 0;
		u32 buffer_pitch = 0;
		u32 av_format = CELL_GCM_TEXTURE_A8R8G8B8;
		MTL::Texture* image = nullptr;
		MTL::Texture* image2 = nullptr;

		// A skipped frame is dropped, not presented late, and the display buffer's
		// declared extent decides whether a present is attempted
		// (GLPresent.cpp:164, :173-201).
		if (!info.skip_frame && info.buffer < m_ctx->display_buffers_count)
		{
			buffer_width = m_ctx->display_buffers[info.buffer].width;
			buffer_height = m_ctx->display_buffers[info.buffer].height;
			buffer_pitch = m_ctx->display_buffers[info.buffer].pitch;

			if (!buffer_width)
			{
				buffer_width = avconfig.resolution_x;
				buffer_height = avconfig.resolution_y;
			}

			if (avconfig.state)
			{
				av_format = avconfig.get_compatible_gcm_format();

				if (!buffer_pitch)
				{
					buffer_pitch = buffer_width * avconfig.get_bpp();
				}

				const size2u video_frame_size = avconfig.video_frame_size();
				buffer_width = std::min(buffer_width, video_frame_size.width);
				buffer_height = std::min(buffer_height, video_frame_size.height);
			}
			else if (!buffer_pitch)
			{
				buffer_pitch = buffer_width * 4;
			}

			if (buffer_width && buffer_height)
			{
				// The source is the surface at the display buffer's guest address,
				// not the display-buffer index (VKPresent.cpp:551).
				const u32 address = rsx::get_address(m_ctx, m_ctx->display_buffers[info.buffer].offset, CELL_GCM_LOCATION_LOCAL);
				get_present_source(address, buffer_width, buffer_height, buffer_pitch, av_format, 0, image);


				if (!image)
				{
					buffer_width = 0;
					buffer_height = 0;
				}
			}

			if (image && avconfig.stereo_enabled) [[unlikely]]
			{
				// The second eye sits below the first, separated by the RSX blanking
				// interval (VKPresent.cpp:565).
				const auto [unused, min_expected_height] = rsx::apply_resolution_scale<true>(
					RSX(m_ctx)->resolution_scaling_config, RSX_SURFACE_DIMENSION_IGNORED, static_cast<u32>(m_ctx->display_buffers[info.buffer].height) + 30);

				if (image->height() < min_expected_height)
				{
					const u32 image_offset = (m_ctx->display_buffers[info.buffer].height + 30) * buffer_pitch + m_ctx->display_buffers[info.buffer].offset;
					get_present_source(rsx::get_address(m_ctx, image_offset, CELL_GCM_LOCATION_LOCAL), buffer_width, buffer_height, buffer_pitch, av_format, 1, image2);
				}
			}
		}

		present_source source{ image, image2, buffer_width, buffer_height };

		// The capture reads the source before the output path scales it
		// (VKPresent.cpp:684).  Only a capture the user asked for happens in a
		// run that is not tracing; the periodic one is instrumentation.
		const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false);
		const bool user_is_recording = (g_recording_mode != recording_mode::stopped && m_frame->can_consume_frame());

		if (source.image && (user_asked_for_screenshot || user_is_recording))
		{
			resources.output.request_capture(resources.recorder, resources.constants, source, user_asked_for_screenshot,
				g_cfg.video.record_with_overlays.get(), RSX(m_ctx)->overlay_manager());
		}


		// The CPU-reduction limits are re-evaluated on an emu flip only
		// (VKPresent.cpp:586-589, GLPresent.cpp:253-257).
		if (info.emu_flip)
		{
			RSX(m_ctx)->display_ctrl().evaluate_cpu_usage_reduction_limits();
			resources.present.update_vsync();
		}

		const u32 target_width = static_cast<u32>(std::max(0, m_frame->client_width()));
		const u32 target_height = static_cast<u32>(std::max(0, m_frame->client_height()));

		MTL::Texture* drawable = nullptr;

		// The drawable is acquired even when the present source is missing, so the
		// window shows this flip's black clear and not the previous image
		// (GLPresent.cpp:276-281, VKPresent.cpp:781-802).
		if (!info.skip_frame && target_width && target_height &&
			resources.present.acquire(resources.recorder, target_width, target_height, drawable))
		{

			// The pass writes the drawable it was handed, so the size it covers is the
			// drawable's and not the client area the acquire asked for: a resize leaves
			// the drawables the layer already carries at the old size until they come
			// back round the pool, and a pass that drew past its attachment would be a
			// validation error.  VK measures the swapchain image after the acquire for
			// the same reason (VKPresent.cpp:658-660).
			const u32 present_width = static_cast<u32>(drawable->width());
			const u32 present_height = static_cast<u32>(drawable->height());
			const areau display_area{ 0, 0, present_width, present_height };
			const areau aspect = (g_cfg.video.stretch_to_display_area.get() || !source.width || !source.height)
				? display_area
				: avconfig.aspect_convert_region({ source.width, source.height }, { present_width, present_height });

			MTL4::RenderPassDescriptor* pass = MTL4::RenderPassDescriptor::alloc()->init();
			MTL::RenderPassColorAttachmentDescriptor* attachment = pass->colorAttachments()->object(0);
			attachment->setTexture(drawable);
			// The area outside the aspect rectangle is the letterbox, and it is black
			// (GLPresent.cpp:279).
			attachment->setLoadAction(MTL::LoadActionClear);
			attachment->setStoreAction(MTL::StoreActionStore);
			attachment->setClearColor(MTL::ClearColor::Make(0.0, 0.0, 0.0, 1.0));

			if (g_cfg.video.debug_overlay)
			{
				// The overlay is a guest-facing view; the counters are the shared
				// frame statistics Vulkan and OpenGL already print
				// (VKPresent.cpp:922-985).  Metal has no cache counters of its own,
				// so the lines it cannot answer are left out rather than invented.
				rsx::overlays::set_debug_overlay_text(fmt::format(
					"Internal Resolution:      %s\n"
					"RSX Load:                 %3u%%\n"
					"draw calls: %17u\n"
					"submits: %20u\n"
					"draw call setup: %12lldus\n"
					"vertex upload time: %9lldus\n"
					"texture upload time: %8lldus\n"
					"draw call execution: %8lldus\n"
					"submit and flip: %12lldus\n"
					"Program cache lookup ellision: %u/%u",
					info.stats.framebuffer_stats.to_string(RSX(m_ctx)->resolution_scaling_config, !m_config.supports_hw_msaa),
					RSX(m_ctx)->get_load(), info.stats.draw_calls, info.stats.submit_count,
					static_cast<long long>(info.stats.setup_time),
					static_cast<long long>(info.stats.vertex_upload_time),
					static_cast<long long>(info.stats.textures_upload_time),
					static_cast<long long>(info.stats.draw_exec_time),
					static_cast<long long>(info.stats.flip_time),
					info.stats.program_cache_lookups_ellided, info.stats.program_cache_lookups_total));
			}

			if (MTL4::RenderCommandEncoder* encoder = resources.recorder.open_pass(pass))
			{
				// The output tier samples images that the frame's earlier encoders
				// wrote, and it is not an RSX pass, so the scheduler does not hold
				// this edge.  The read is a later encoder, so the queue form
				// applies (HLD-03 §2.1 item 6).
				emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });

				resources.output.compose(encoder, resources.constants, drawable, source, aspect, display_area,
					avconfig.gamma, !g_cfg.video.full_rgb_range_output.get(), avconfig.stereo_enabled, RSX(m_ctx)->overlay_manager());
			}
			else
			{
				// The output pass could not open, so the drawable stays empty.  It is
				// still presented, because a drawable the queue signalled must reach
				// the display or the layer keeps it.
				rsx_log.error("Metal 4: the output pass could not open, so the frame presents empty");
			}

			pass->release();
		}

		// Present submits the frame and presents the drawable it holds, so a frame
		// that acquired one always presents it and the ring advances.
		{
			resources.present.present(resources.recorder);
		}


		// The overlays the frame retired lose their temporary images here, and the
		// manager then frees them (GLPresent.cpp:283).
		{
			resources.output.retire_overlays(RSX(m_ctx)->overlay_manager());

			// The cache retires the temporary subresources and the predictor state of
			// the frame that just ended (GLPresent.cpp:535).
			resources.textures.on_frame_end();
		}


		m_frame_open = false;

		m_ctx->stats.flip_time = m_profiler.duration();

		// Window bookkeeping only; the present itself happened above.  A skipped
		// frame is dropped, not presented late (GLPresent.cpp:174-178).
		m_frame->flip(m_context, info.skip_frame);

		return drawable != nullptr;
	}

	void MTLGSRender::post_present()
	{
	}

	rsx::device_status MTLGSRender::check_device_status() const
	{
		return (m_device && m_device->lost_flag().load(std::memory_order_acquire))
			? rsx::device_status::lost
			: rsx::device_status::ok;
	}

	std::unique_ptr<rsx::context_resources> MTLGSRender::create_context_resources(rsx::context*)
	{
		// The Metal caches are device-scoped and owned by the backend, so a context
		// carries no resources of its own.
		return std::make_unique<rsx::context_resources>();
	}

	void MTLGSRender::on_release_context_resources(rsx::context*)
	{
	}

	void MTLGSRender::on_bind_context(rsx::context* ctx)
	{
		if (!m_frame_resources)
		{
			return;
		}

		// The caches are shared by every context, so the bound one moves with the
		// switch.  The attachments belong to the previous context's layout.
		m_frame_resources->vertex_layout.set_context(ctx);
		m_frame_resources->cache.set_context(ctx);
		m_frame_resources->textures.set_context(ctx);
		m_frame_resources->attachments.reset();
	}

	void MTLGSRender::on_unbind_context(rsx::context*)
	{
		if (m_frame_resources)
		{
			m_frame_resources->attachments.reset();
		}
	}

	rsx::reports::occlusion_query_provider* MTLGSRender::occlusion_queries()
	{
		return m_frame_resources ? &m_frame_resources->zcull_control : nullptr;
	}

	void MTLGSRender::patch_transform_constants(rsx::context* ctx, u32, u32)
	{
		if (ctx != m_ctx)
		{
			return;
		}

		// The group is the upload trigger; emit_geometry reads it before each draw.
		m_ctx->draw_state.dirty.set(rsx::draw_state_t::group::transform_constants);
	}

	bool MTLGSRender::is_current_program_interpreted() const
	{
		return m_frame_resources && m_frame_resources->interpreter_active;
	}

	std::string MTLGSRender::logical_id() const
	{
		if (!m_device || !m_device->dev() || !m_device->dev()->name())
		{
			return "Metal | NULL";
		}

		return "Metal | " + std::string(m_device->dev()->name()->utf8String());
	}

	std::string MTLGSRender::configured_logical_id() const
	{
		if (rsx::resolve_render_backend_type(g_cfg.video.renderer) != video_renderer::metal)
		{
			return "";
		}

		return logical_id();
	}

	// A mid-frame wait must submit: the guest can wait on a label that only the
	// GPU can release, and the GPU cannot progress on a frame that has not been
	// committed (HLD-04 §4.3).
	void MTLGSRender::on_semaphore_acquire_wait()
	{

		// This path is a guest spin, so it fires millions of times a session.  It is
		// counted and classified and it never prints a line: a trace line here
		// produced three quarters of one measured log (anti-flood rule).

		// A guest waiting on a label waits on a write that is still being recorded, so
		// the frame commits here.  The commit does not wait: the guest's own wait is
		// the wait, and the slot comes back on the next encoder (HLD-04 §4.3).
		if (m_frame_open && m_frame_resources->recorder.label_writes_pending())
		{
			m_frame_resources->recorder.commit_pending_labels();
		}

		// A pending flush request, or an emu flip the guest is waiting on, runs the
		// local task here (GLGSRender.cpp:1328-1335, VKGSRender.cpp:1136-1144).  The
		// wait does not submit or close the frame: the frame stays open until its
		// flip presents it, because a flip that finds the frame closed is a frozen
		// image (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if ((m_flush_queue && !m_flush_queue->pending.empty()) ||
			RSX(m_ctx)->display_ctrl().has_flip_request(rsx::thread::flip_request::emu_requested))
		{
			RSX(m_ctx)->do_local_task(rsx::FIFO::state::lock_wait);
		}
	}

	// A back-end semaphore release publishes to guest memory.  Metal has no host
	// GPU label path, so the CPU write must follow the GPU work it releases.  The
	// current slot commits and returns; its later commit proves every earlier
	// commit complete because the queue is serialized (MTLCommandRecorder Q4).
	// Without this wait the guest can recycle the FIFO while the GPU still reads
	// it, and the RSX thread then dies with a dead FIFO queue (LOG-FINDINGS L11).
	// The FIFO desync capture (RR7-FREEZE-FORENSICS.md §8.1).  The shared FIFO
	// gathers the data; the backend prints it, through the probe facility so the
	// bulk text stays out of the shared log and the run's cost is nil at level 0.

	bool MTLGSRender::sync_label(u32 address)
	{
		// The recorder owns the label addresses and the commits that carry them.
		// Without frame resources there is no GPU label write to wait for.
		if (!m_frame_resources)
		{
			return false;
		}

		return m_frame_resources->recorder.sync_label(address);
	}

	bool MTLGSRender::release_GCM_label(u32 type, u32 address, u32 data)
	{

		if (!m_frame_resources)
		{
			return false;
		}

		// A label is the only signal the GPU gives the guest, so the GPU performs the
		// write, in stream order.  A CPU write cannot be ordered against the work the
		// release hands back, and making the RSX thread wait for the GPU at every
		// release keeps the frame rate at the GPU's rate (L38, L129).
		open_frame();

		// The guest word is big-endian, so the copy moves the bytes a CPU store would
		// (VKGSRender.cpp:1726).
		const u32 value = std::bit_cast<u32, be_t<u32>>(data);


		if (m_frame_open && m_frame_resources->recorder.write_label(address, value))
		{
			// A back-end release is waited on by the guest without an RSX
			// acquire, and the next encoder boundary may never arrive while the
			// FIFO is parked.  Submit the batch now, without waiting, as VK does
			// with its label command buffer (VKGSRender.cpp:1744-1759); the slot
			// comes back at the next encoder.
			if (type == NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE)
			{
				m_frame_resources->recorder.commit_pending_labels();
			}

			return true;
		}

		// The GPU cannot reach the address, so the caller stores it after the GPU has
		// stopped reading the range.
		if (m_frame_open)
		{
			m_frame_resources->recorder.flush_for_readback();
		}
		else if (!m_frame_resources->recorder.wait_idle())
		{
			rsx_log.error("Metal 4: a label release found GPU work in flight after the feedback timeout");
		}


		return false;
	}

	// The flush runs here, on the RSX thread, because it needs an encoder and the
	// fault that asked for it runs on a guest thread (GLGSRender.cpp:1329-1350).
	// A request is released by the faulting thread once it has consumed its result,
	// so the list keeps the requests that are still in flight.
	bool MTLGSRender::on_local_task(rsx::FIFO::state)
	{
		if (m_flush_queue && m_frame_resources)
		{

			std::lock_guard lock(m_flush_queue->guard);

			// A request leaves the queue once its owner has stopped waiting and the
			// RSX thread has serviced it.  A request with no flush work is never
			// waited on, so its `received` is set at the post; the `processed` term
			// keeps it until the loop below has applied its sampler flags.
			m_flush_queue->pending.remove_if([](const flush_queue::request& q) { return q.received && q.processed; });

			for (flush_queue::request& request : m_flush_queue->pending)
			{
				if (request.processed)
				{
					continue;
				}

				// The request may have arrived between two frames, and the recorder
				// has no command buffer open while the ring slot is idle.
				{
					open_frame();
				}

				{
					m_frame_resources->textures.flush_all(m_frame_resources->recorder, request.sections);
				}


				if (request.invalidate_samplers)
				{
					// A cached image became invalid, so every unit re-resolves on its
					// next draw (VKGSRender.cpp:1028).  The flags are the RSX thread's
					// to write, because the faulting thread cannot touch them.
					invalidate_samplers();
				}

				request.processed = true;
			}
		}

		// The thread owns the flips, the invalidations and the native-UI path; the
		// backend only drains its deferred work.
		return true;
	}

	// A guest write to a range a surface owns invalidates that surface, because
	// the guest bytes are canonical and the host copy is now stale
	// (VKGSRender.cpp:1483).
	bool MTLGSRender::scaled_image_from_memory([[maybe_unused]] const rsx::blit_src_info& src, [[maybe_unused]] const rsx::blit_dst_info& dst, [[maybe_unused]] bool interpolate)
	{


		// Metal has no 2D-engine path for this transfer, so the shared code falls
		// back to a CPU copy (nv3089.cpp:599-603).  That copy reads and writes guest
		// memory directly, bypassing the cache's protection.  Materialize the source
		// sections first, then drop the destination sections: otherwise the next
		// sampler reads the stale host copy of the destination (the black Xevious
		// panel was this path).  A GPU blit is still the cheaper route (SF8/SF11).
		if (m_frame_resources && src.rsx_address && src.pitch && src.height)
		{
			const u32 src_length = src.pitch * static_cast<u32>(src.height);
			m_frame_resources->textures.invalidate_range(m_frame_resources->recorder,
				utils::address_range32::start_length(src.rsx_address, src_length),
				rsx::invalidation_cause::read);
		}

		if (m_frame_resources && dst.rsx_address && dst.pitch && dst.clip_height)
		{
			const u32 dst_length = dst.pitch * static_cast<u32>(dst.clip_height);
			m_frame_resources->textures.invalidate_range(m_frame_resources->recorder,
				utils::address_range32::start_length(dst.rsx_address, dst_length),
				rsx::invalidation_cause::write);
		}

		return false;
	}

	void MTLGSRender::write_barrier(u32 memory_address, u32 memory_range)
	{
		if (!m_frame_resources)
		{
			return;
		}


		m_frame_resources->cache.invalidate_range(utils::address_range32::start_length(memory_address, memory_range));
	}

	// A guest access to a locked page invalidates the sections that overlap it
	// (VKGSRender.cpp:1277).  A fault on a guest thread is recorded and queued for
	// the RSX thread, which owns the encoders.  A fault on the RSX thread itself
	// cannot be queued for that thread, so it flushes in place
	// (GLGSRender.cpp:1281).
	bool MTLGSRender::on_access_violation(rsx::context* ctx, u32 address, bool is_writing)
	{
		if (ctx != m_ctx || !m_frame_resources)
		{
			return false;
		}

		// TODO: the RSX thread faults on this page while it owns it, every frame,
		// and the access that reaches a protected page is still to be found.  A
		// wait for this thread stalls the whole run ("Thread [rsx::thread] is too
		// sleepy"), so the flush runs in place until the access is known.
		const bool can_flush = RSX(m_ctx)->is_current_thread();

		if (can_flush)
		{
			// The fault of the RSX thread is the one that stalls the run, so it is
			// always reported; a fault of a guest thread is routine and handled by
			// the deferred path below.
			rsx_log.notice("Metal 4: access violation 0x%x, writing %u, on the RSX thread", address, is_writing ? 1u : 0u);
		}

		const rsx::invalidation_cause cause = is_writing
			? (can_flush ? rsx::invalidation_cause::write : rsx::invalidation_cause::deferred_write)
			: (can_flush ? rsx::invalidation_cause::read : rsx::invalidation_cause::deferred_read);


		{
			rsx::mm_flush(address);
		}

		auto result = [&] {
			return m_frame_resources->textures.invalidate_address(m_frame_resources->recorder, address, cause);
		}();

		if (!result.violation_handled)
		{
			// The fault is not the texture cache's; the thread then asks ZCULL.
			return false;
		}


		if (can_flush)
		{
			// The flush ran on this call, so nothing is left to wait for, and the
			// flags below are this thread's to write in any case.
			if (result.invalidate_samplers)
			{
				invalidate_samplers();
			}

			return true;
		}

		const bool should_invalidate_samplers = result.invalidate_samplers;

		// The deferred path keeps sections_to_flush only when the RSX thread has to
		// write them back; every other handled fault unprotects in place, so an empty
		// set means the page is already writable and the faulting instruction can be
		// retried at once (texture_cache.h:1329-1351).
		const bool flush_pending = !result.sections_to_flush.empty();

		flush_queue::request* request = nullptr;
		{
			std::lock_guard lock(m_flush_queue->guard);

			auto& queued = m_flush_queue->pending.emplace_back();
			queued.sections = std::move(result);
			queued.invalidate_samplers = should_invalidate_samplers;
			queued.received = !flush_pending;
			request = &queued;

		}

		RSX(m_ctx)->raise_backend_interrupt();
		vm::temporary_unlock();

		if (flush_pending)
		{
			// The page keeps its protection until the flush reaches guest memory, so
			// the faulting instruction must not be retried before the request
			// completes (GLGSRender.cpp:1298-1307).
			request->producer_wait();
		}

		return true;
	}

	// The guest unmapped a range, so every host copy of it is invalid
	// (VKGSRender.cpp:1312).
	void MTLGSRender::on_invalidate_memory_range(rsx::context* ctx, const utils::address_range32& range, rsx::invalidation_cause cause)
	{
		if (ctx != m_ctx || !m_frame_resources)
		{
			return;
		}


		const auto data = m_frame_resources->textures.invalidate_range(m_frame_resources->recorder, range, cause);

		if (cause == rsx::invalidation_cause::unmap && data.violation_handled)
		{
			m_frame_resources->textures.purge_unreleased_sections();
		}
	}

	// A tile is unbound when the guest re-tiles memory, so every cached section
	// may have been re-laid-out underneath it.  The units re-resolve on their
	// next draw (VKGSRender.cpp:1407).
	void MTLGSRender::notify_tile_unbound(u32 /*tile*/)
	{

		invalidate_samplers();
	}

	void MTLGSRender::invalidate_samplers()
	{
		m_ctx->textures.fragment_textures_dirty = (1u << rsx::limits::fragment_textures_count) - 1;
		m_ctx->textures.vertex_textures_dirty = (1u << rsx::limits::vertex_textures_count) - 1;
	}

	void MTLGSRender::on_unmap_memory_range(const utils::address_range32&)
	{
		// The purge lives in on_invalidate_memory_range, where the thread delivers
		// an unmap cause.  Metal has no DMA-side unmap.
	}
}

#endif // RSX_ENABLE_METAL4
