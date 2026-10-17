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
#include "MTLFramebuffer.h"
#include "MTLPresenter.h"
#include "MTLProgramPipeline.h"
#include "MTLResourceManager.h"
#include "MTLSamplerState.h"
#include "MTLSurfaceCache.h"
#include "MTLSurfaceState.h"
#include "MTLTextureCache.h"

#include "Emu/RSX/Common/BufferUtils.h"
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/Core/RSXDrawCommands.h"
#include "Emu/RSX/Program/program_util.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/rsx_utils.h"

#include <tuple>

namespace mtl
{
	namespace
	{
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

		struct draw_command_visitor
		{
			byte_ring& index_ring;
			const rsx::vertex_input_layout& layout;

			vertex_input_state operator()(const rsx::draw_array_command&) const
			{
				const auto& clause = rsx::method_registers.current_draw_clause;
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
				const auto& clause = rsx::method_registers.current_draw_clause;
				const rsx::index_array_type type = clause.is_immediate_draw
					? rsx::index_array_type::u32
					: rsx::method_registers.index_type();

				const u32 type_size = ::get_index_type_size(type);
				const u32 vertex_count = clause.get_elements_count();
				const u32 index_count = expands_primitive(clause.primitive)
					? ::get_index_count(clause.primitive, vertex_count)
					: vertex_count;

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

				std::tie(min_index, max_index, written) = ::write_index_array_data_to_buffer(
					{ static_cast<std::byte*>(dst), length },
					command.raw_index_buffer,
					type,
					clause.primitive,
					rsx::method_registers.restart_index_enabled(),
					rsx::method_registers.restart_index(),
					expands_primitive);

				vertex_input_state result;
				result.min_index = min_index;
				result.max_index = max_index;
				result.vertex_draw_count = written;
				result.has_index_buffer = true;
				result.index_type = type;
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
				result.vertex_index_offset = rsx::method_registers.vertex_data_base_index();
				return result;
			}

			vertex_input_state operator()(const rsx::draw_inlined_array&) const
			{
				const auto& clause = rsx::method_registers.current_draw_clause;
				const u32 stream_length = static_cast<u32>(clause.inline_vertex_array.size());
				const u32 vertex_count = (stream_length * sizeof(u32)) / layout.interleaved_blocks[0]->attribute_stride;

				vertex_input_state result;
				result.max_index = vertex_count;
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
		pipeline_properties capture_pipeline_properties(const surface_state& surface)
		{
			const blend_state blend = capture_blend_state();
			const raster_state raster = capture_raster_state();
			const anti_aliasing_state aa = capture_anti_aliasing_state();

			pipeline_properties properties;
			properties.color_format = surface.color_format;
			properties.attachment_count = static_cast<u8>(surface.color_attachment_count());
			properties.sample_count = surface.sample_count();
			properties.alpha_to_coverage = aa.alpha_to_coverage;
			properties.alpha_to_one = aa.alpha_to_one;
			properties.raster_enabled = !culls_both_faces(raster);
			properties.blend = blend;
			properties.input_primitive_topology = get_primitive_topology_class(rsx::method_registers.current_draw_clause.primitive);

			for (u32 index = 0; index < properties.attachment_count; ++index)
			{
				properties.write_mask[index] = capture_color_write_mask(index, surface.color_format);
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
			: programs([](const pipeline_properties&, const RSXVertexProgram&, const RSXFragmentProgram&) {})
		{
		}

		resource_manager resources;
		command_recorder recorder;
		surface_cache cache;
		framebuffer attachments;
		fill_pass fill;
		presenter present;

		// The draw path (HLD-05 §3.5).
		constants_heap constants;
		depth_stencil_cache depth_cache;
		sampler_cache samplers;
		texture_cache textures;
		program_cache programs;
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

		// The resolved texture units of the open clause.  The unit resolution runs
		// once per clause in end() and the subdraws only re-bind it, because a
		// subdraw never changes the texture registers (HLD-05 §3.2).
		std::array<texture_bind, fragment_texture_count> fragment_textures{};
		std::array<texture_bind, vertex_texture_count> vertex_textures{};

		// The open pass.  The header is metal-cpp-free, so the encoder lives here.
		MTL4::RenderCommandEncoder* encoder = nullptr;
	};

	MTLGSRender::MTLGSRender(utils::serial* ar) noexcept
		: GSRender(ar)
	{
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender() = default;

	u64 MTLGSRender::get_cycles()
	{
		return thread_ctrl::get_cycles(static_cast<named_thread<MTLGSRender>&>(*this));
	}

	void MTLGSRender::on_init_thread()
	{
		m_device = device::create();
		if (!m_device)
		{
			die("no Metal 4 device is available");
		}

		// The 16-bool backend contract (RSXThread.h:80; 10 §2.2).
		m_device->fill_backend_config(backend_config);

		// The resource layer and the recorder come up with the device, because the
		// guest window joins the persistent residency set and the ring is sized once
		// (HLD-02 I5, HLD-04 §3.2).
		m_frame_resources = std::make_unique<frame_resources>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);
		m_frame_resources->fill.init(*m_device, m_frame_resources->resources);
		m_frame_resources->constants.init(m_frame_resources->resources, frame_slot_count);
		m_frame_resources->depth_cache.init(m_device->dev());
		m_frame_resources->samplers.init(m_device->dev());

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = get_frame())
		{
			m_frame_resources->present.init(*m_device, frame->handle());
		}

		GSRender::on_init_thread();
	}

	void MTLGSRender::on_exit()
	{
		if (m_frame_resources)
		{
			// The layer goes first, then the ring drains, then the resources go.
			m_frame_resources->present.shutdown();
			m_frame_resources->textures.destroy();
			m_frame_resources->samplers.shutdown();
			m_frame_resources->depth_cache.shutdown();
			m_frame_resources->constants.shutdown();
			m_frame_resources->fill.shutdown();
			m_frame_resources->recorder.shutdown();
			m_frame_resources->resources.shutdown();
			m_frame_resources.reset();
		}

		m_device.reset();
		GSRender::on_exit();
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
		m_frame_resources->recorder.begin_frame();
		m_frame_resources->constants.begin_frame(m_frame_resources->recorder.slot());
		m_frame_resources->binder.reset();
		m_frame_resources->transform_constants_valid = false;
		m_frame_open = true;
	}

	void MTLGSRender::begin()
	{
		open_frame();
		rsx::thread::begin();
	}

	bool MTLGSRender::update_scissor(bool clip_viewport)
	{
		areau region;
		if (get_scissor(region, clip_viewport))
		{
			m_scissor = region;
			return true;
		}

		return !m_graphics_state.test(rsx::pipeline_state::scissor_setup_invalid);
	}

	bool MTLGSRender::bind_framebuffer(rsx::framebuffer_creation_context context)
	{
		open_frame();

		const bool clipped_scissor = (context == rsx::framebuffer_creation_context::context_draw);

		if (m_frame_resources->attachments.valid() && m_framebuffer_layout.ignore_change)
		{
			// The layout has not changed, so the bound set is still current
			// (VKGSRender.cpp:2618).
			update_scissor(clipped_scissor);
			return true;
		}

		m_current_framebuffer_context = context;

		m_graphics_state.clear(rsx::rtt_config_dirty | rsx::rtt_config_contested | rsx::rtt_config_valid | rsx::rtt_cache_state_dirty);
		get_framebuffer_layout(context, m_framebuffer_layout);

		if (!m_graphics_state.test(rsx::rtt_config_valid))
		{
			return false;
		}

		m_frame_resources->attachments.bind(m_frame_resources->recorder, m_frame_resources->cache, m_framebuffer_layout, resolution_scaling_config);
		update_scissor(clipped_scissor);
		return m_frame_resources->attachments.valid();
	}

	void MTLGSRender::clear_surface(u32 mask)
	{
		if (skip_current_frame || !m_frame_resources)
		{
			return;
		}

		// A stencil clear cannot run with the stencil write mask off
		// (VKGSRender.cpp:1413).
		if (!rsx::method_registers.stencil_mask())
		{
			mask &= ~RSX_GCM_CLEAR_STENCIL_BIT;
		}

		if (!(mask & RSX_GCM_CLEAR_ANY_MASK))
		{
			return;
		}

		// The clear context is what makes the store create the attachments it needs.
		u8 context = rsx::framebuffer_creation_context::context_draw;
		if (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK)
		{
			context |= rsx::framebuffer_creation_context::context_clear_color;
		}

		if (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK)
		{
			context |= rsx::framebuffer_creation_context::context_clear_depth;
		}

		if (!bind_framebuffer(rsx::framebuffer_creation_context{context}))
		{
			return;
		}

		const surface_state state = capture_surface_state();
		framebuffer& attachments = m_frame_resources->attachments;

		if (state.clear_covers_attachment())
		{
			// The clear is the whole attachment, so it is a load action and needs no
			// draw (LLD-surface invariant 4).  The pass is opened so the clear is
			// recorded, then closed.
			attachments.open_pass(m_frame_resources->recorder, mask, state);
			m_frame_resources->recorder.close_encoder();
			return;
		}

		// Metal has no scissored clear, so a partial clear is a full-screen fill
		// confined by the scissor (CONFLICTS.md C1).
		if (!update_scissor(false))
		{
			return;
		}

		MTL4::RenderCommandEncoder* encoder = attachments.open_pass(m_frame_resources->recorder, 0, state);
		if (!encoder)
		{
			return;
		}

		u16 x = static_cast<u16>(m_scissor.x1);
		u16 y = static_cast<u16>(m_scissor.y1);
		u16 width = static_cast<u16>(m_scissor.width());
		u16 height = static_cast<u16>(m_scissor.height());

		std::tie(x, y, width, height) = rsx::clip_region<u16>(attachments.width(), attachments.height(), x, y, width, height, true);

		MTL::ScissorRect scissor;
		scissor.x = x;
		scissor.y = y;
		scissor.width = width;
		scissor.height = height;
		encoder->setScissorRect(scissor);

		const bool write_color = (mask & RSX_GCM_CLEAR_COLOR_RGBA_MASK) != 0;
		const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_STENCIL_MASK) != 0;
		const std::array<f32, 4> color = {
			static_cast<f32>(state.clear_color.red),
			static_cast<f32>(state.clear_color.green),
			static_cast<f32>(state.clear_color.blue),
			static_cast<f32>(state.clear_color.alpha) };
		const MTL::PixelFormat color_format = attachments.color_count() != 0 ? attachments.color(0)->pixel_format() : MTL::PixelFormatInvalid;

		m_frame_resources->fill.fill(m_frame_resources->recorder.fragment_table(), encoder, color_format, attachments.sample_count(), color, state.clear_depth, write_color, write_depth);
		m_frame_resources->recorder.close_encoder();
	}

	void MTLGSRender::load_texture_env()
	{
		frame_resources& frame = *m_frame_resources;

		// A unit the program does not reference is never sampled, so it is skipped
		// (GLDraw.cpp:340-425).  The cache resolves the guest range to an image and
		// the sampler is a separate object, because MSL has no combined image
		// sampler (HLD-05 §3.2).
		for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!fs_sampler_state[i])
			{
				fs_sampler_state[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());
			const auto& tex = rsx::method_registers.fragment_textures[i];
			const rsx::format_class previous_class = descriptor->format_class;
			const bool dirty = m_textures_dirty[i];
			m_textures_dirty[i] = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
			}

			if (!descriptor->validate())
			{
				frame.fragment_textures[i] = {};
				continue;
			}

			descriptor->format_ex = tex.format_ex();

			if (descriptor->format_class != previous_class)
			{
				// The host image changed class under the program's feet.
				m_graphics_state |= rsx::fragment_program_state_dirty;
			}

			frame.fragment_textures[i] = frame.textures.resolve_texture_bind(
				descriptor,
				frame.samplers.get(capture_fragment_sampler(static_cast<u8>(i))));
		}

		for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
		{
			if (!(textures_ref & 1))
			{
				continue;
			}

			if (!vs_sampler_state[i])
			{
				vs_sampler_state[i] = std::make_unique<texture_cache::sampled_image_descriptor>();
			}

			auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(vs_sampler_state[i].get());
			const auto& tex = rsx::method_registers.vertex_textures[i];
			const bool dirty = m_vertex_textures_dirty[i];
			m_vertex_textures_dirty[i] = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
			}

			if (!descriptor->validate())
			{
				frame.vertex_textures[i] = {};
				continue;
			}

			frame.vertex_textures[i] = frame.textures.resolve_texture_bind(
				descriptor,
				frame.samplers.get(capture_vertex_sampler(static_cast<u8>(i))));
		}
	}

	// The draw execution (HLD-05 §3.5).  One RSX draw clause is a sequence of
	// subdraws: the pipeline, the pass and the encoder state are per clause, and
	// the vertex streams and the ABI blocks are per subdraw.  end() runs before
	// rsx::thread::end(), which owns the post-execute cleanup (RSXThread.cpp:971).
	void MTLGSRender::end()
	{
		if (should_skip_draw())
		{
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		if (!bind_framebuffer(rsx::framebuffer_creation_context::context_draw))
		{
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		frame_resources& frame = *m_frame_resources;
		auto& draw_call = rsx::method_registers.current_draw_clause;

		// The programs of this draw.  analyse_current_rsx_pipeline fills the
		// metadata (RSXThread.cpp:2318).  The texture units resolve first, because
		// the program builders read the resolved descriptors (RSXThread.cpp:2488).
		analyse_current_rsx_pipeline();
		load_texture_env();

		get_current_vertex_program(vs_sampler_state);
		get_current_fragment_program(fs_sampler_state);

		if (current_vertex_program.data.empty() || !current_fragment_program.ucode_length)
		{
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		const surface_state surface = capture_surface_state();
		pipeline_properties properties = capture_pipeline_properties(surface);

		// The pipeline is the shared program cache's object.  The Metal PSO key is
		// its pipeline_properties, so no second cache exists (HLD-05 §3.5).
		const auto data = frame.programs.get_graphics_pipeline(
			&m_program_cache_hint,
			current_vertex_program,
			current_fragment_program,
			properties,
			false,
			false,
			shader_context{ m_device->compiler(), "main0", "main0" });

		const pipeline* program = std::get<0>(data);
		if (!program || !program->state)
		{
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		frame.compiled_fragment = std::get<2>(data);
		frame.compiled_vertex = std::get<1>(data);

		MTL4::RenderCommandEncoder* encoder = frame.attachments.open_pass(frame.recorder, 0, surface);
		if (!encoder)
		{
			rsx::thread::end();
			return;
		}

		frame.encoder = encoder;
		encoder->setRenderPipelineState(program->state);

		MTL::ScissorRect scissor = build_scissor_rect(m_scissor.x1, m_scissor.y1, m_scissor.x1 + m_scissor.width(), m_scissor.y1 + m_scissor.height());
		apply_encoder_state(encoder, capture_encoder_state(), scissor, frame.depth_cache);

		// The indexed-constant tables of a hardware-instanced draw.  The shared
		// helper replays the clause, so it runs before the subdraw loop
		// (RSXDrawCommands.cpp:743-751).
		if (current_vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS)
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
			GRAPH_frontend().fill_constants_instancing_buffer(lookup, constants, frame.compiled_vertex);
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

			if (draw_call.is_trivial_instanced_draw && backend_config.supports_hw_instanced_rendering)
			{
				draw_call.end();
			}
		}
		while (draw_call.next());

		frame.encoder = nullptr;
		frame.recorder.close_encoder();

		rsx::thread::end();
	}

	void MTLGSRender::emit_geometry(u32 sub_index)
	{
		frame_resources& frame = *m_frame_resources;
		MTL4::RenderCommandEncoder* encoder = frame.encoder;
		auto& draw_call = rsx::method_registers.current_draw_clause;

		// The vertex state of this subdraw (GLDraw.cpp:730-760).
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: (draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask);

		if (vertex_state & rsx::vertex_arrays_changed)
		{
			GRAPH_frontend().analyse_inputs_interleaved(frame.vertex_layout, current_vp_metadata);
		}
		else if (vertex_state & rsx::vertex_base_changed)
		{
			for (auto& info : frame.vertex_layout.interleaved_blocks)
			{
				info->vertex_range.second = 0;
				const u32 vertex_base_offset = rsx::method_registers.vertex_data_base_offset();
				info->real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(vertex_base_offset, info->base_offset), info->memory_location);
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
			// No vertex inputs are enabled.
			return;
		}

		// The index array and the vertex range of this subdraw
		// (GLVertexBuffers.cpp:66-150).
		const vertex_input_state input = std::visit(
			draw_command_visitor{ frame.constants.index_ring(), frame.vertex_layout },
			GRAPH_frontend().get_draw_command(rsx::method_registers));

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
			vertex_base = rsx::get_index_from_base(vertex_base, rsx::method_registers.vertex_data_base_index());
		}

		const auto required = calculate_memory_requirements(frame.vertex_layout, vertex_base, vertex_count);
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

		if (persistent_data || volatile_data)
		{
			GRAPH_frontend().write_vertex_data_to_memory(frame.vertex_layout, vertex_base, vertex_count, persistent_data, volatile_data);
		}

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

		GRAPH_frontend().fill_scale_offset_data(context->scale_offset_mat, false);
		GRAPH_frontend().fill_user_clip_data(&context->user_clip_configuration_bits);
		context->transform_branch_bits = rsx::method_registers.transform_branch_bits();
		context->point_size = rsx::method_registers.point_size() * resolution_scaling_config.scale_factor();
		context->z_near = rsx::method_registers.clip_min();
		context->z_far = rsx::method_registers.clip_max();

		GRAPH_frontend().fill_fragment_state_buffer(fragment, current_fragment_program);
		current_fragment_program.texture_params.write_to(sampler, current_fp_metadata.referenced_textures_mask);
		std::memcpy(stipple, rsx::method_registers.polygon_stipple_pattern(), stipple_block_count * sizeof(vec4_block));

		// The transform constants.  The full 468-vec4 file is re-uploaded only when
		// the shared state flag says the file changed
		// (Core/RSXDriverState.h:15).  The shader reads the compacted file, so the
		// upload applies the program's relocation table (GLGSRender.cpp:1159-1171).
		if (!frame.transform_constants_valid || m_graphics_state.test(rsx::pipeline_state::transform_constants_dirty))
		{
			vec4_block* block = frame.constants.alloc_transform_constants();
			if (!block)
			{
				return;
			}

			const std::span<const u16> reloc_table = (frame.compiled_vertex && !frame.compiled_vertex->has_indexed_constants)
				? std::span<const u16>(frame.compiled_vertex->constant_ids)
				: std::span<const u16>{};

			GRAPH_frontend().fill_vertex_program_constants_data(block, reloc_table);
			frame.transform_constants_index = frame.constants.transform_constants_index();
			frame.transform_constants_valid = true;
			m_graphics_state.clear(rsx::pipeline_state::transform_constants_dirty);
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
				current_fragment_program,
				frame.compiled_fragment->constant_offsets,
				true);

			params->fs_constants_offset = frame.constants.fragment_constants_index();
		}

		GRAPH_frontend().fill_vertex_layout_state(
			frame.vertex_layout,
			current_vp_metadata,
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

		if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
		{
			blend_values* blend = frame.constants.alloc_blend_values();
			if (!blend)
			{
				return;
			}

			blend->reserved = 0;
			blend->blend_eqn = pack_blend_pair(
				static_cast<u32>(rsx::method_registers.blend_equation_rgb()),
				static_cast<u32>(rsx::method_registers.blend_equation_a()));
			blend->blend_sfactors = pack_blend_pair(
				static_cast<u32>(rsx::method_registers.blend_func_sfactor_rgb()),
				static_cast<u32>(rsx::method_registers.blend_func_sfactor_a()));
			blend->blend_dfactors = pack_blend_pair(
				static_cast<u32>(rsx::method_registers.blend_func_dfactor_rgb()),
				static_cast<u32>(rsx::method_registers.blend_func_dfactor_a()));

			const std::array<f32, 4> colors = rsx::get_constant_blend_colors();
			std::copy(colors.begin(), colors.end(), blend->blend_constants);

			binds.fragment_blend = frame.constants.blend_values_block_address();
		}

		// Metal 4 snapshots the argument table when the encoder binds it, so the
		// table is re-bound only when a slot was written (REALISATION.md,
		// "Argument tables").
		if (frame.binder.bind(frame.recorder.vertex_table(), true, binds))
		{
			encoder->setArgumentTable(frame.recorder.vertex_table(), MTL::RenderStageVertex);
		}

		if (frame.binder.bind(frame.recorder.fragment_table(), false, binds))
		{
			encoder->setArgumentTable(frame.recorder.fragment_table(), MTL::RenderStageFragment);
		}

		const MTL::PrimitiveType primitive = get_primitive_type(draw_call.primitive);
		const u32 instance_count = draw_call.is_trivial_instanced_draw ? draw_call.pass_count() : 1;

		if (input.has_index_buffer)
		{
			encoder->drawIndexedPrimitives(
				primitive,
				input.vertex_draw_count,
				get_index_type(input.index_type),
				frame.constants.index_ring().address() + input.index_offset,
				input.index_length,
				instance_count,
				0,
				0);
		}
		else
		{
			encoder->drawPrimitives(primitive, 0, input.vertex_draw_count, instance_count, 0);
		}
	}


	// C7.  The RSX output is the surface at the flip address, and it is presented
	// through the one submit path the recorder owns (HLD-03 §4.9).
	void MTLGSRender::flip(const rsx::display_flip_info_t& info)
	{
		if (m_frame_resources && m_frame_open)
		{
			// The presenter submits the frame it fills.  A frame that cannot be
			// presented (a dropped frame, or no native view) is submitted here
			// instead, so the ring advances either way.
			bool presented = false;

			if (m_frame_resources->present.valid())
			{
				if (surface* source = m_frame_resources->cache.get_surface_at(info.buffer))
				{
					if (texture* output = source->get_surface(rsx::surface_access::memory_read))
					{
						presented = m_frame_resources->present.present(output->value(), m_frame_resources->recorder);
					}
				}
			}

			if (!presented)
			{
				m_frame_resources->recorder.submit();
			}

			// The cache retires the temporary subresources and the predictor state
			// of the frame that just ended (GLPresent.cpp:535).
			m_frame_resources->textures.on_frame_end();

			m_frame_open = false;
		}

		GSRender::flip(info);
	}

	// A mid-frame wait must submit: the guest can wait on a label that only the
	// GPU can release, and the GPU cannot progress on a frame that has not been
	// committed (HLD-04 §4.3).
	void MTLGSRender::on_semaphore_acquire_wait()
	{
		if (m_frame_open && m_frame_resources)
		{
			m_frame_resources->recorder.submit();
			m_frame_open = false;
		}
	}

	// A guest write to a range a surface owns invalidates that surface, because
	// the guest bytes are canonical and the host copy is now stale
	// (VKGSRender.cpp:1483).
	void MTLGSRender::write_barrier(u32 memory_address, u32 memory_range)
	{
		if (!m_frame_resources)
		{
			return;
		}

		m_frame_resources->cache.invalidate_range(utils::address_range32::start_length(memory_address, memory_range));
	}

	// A guest access to a locked page invalidates the sections that overlap it
	// (VKGSRender.cpp:1277).  The call runs on the RSX thread between draws, so no
	// encoder is open and the flush may open its own.
	bool MTLGSRender::on_access_violation(u32 address, bool is_writing)
	{
		if (!m_frame_resources)
		{
			return false;
		}

		rsx::mm_flush(address);

		const rsx::invalidation_cause cause = is_writing
			? rsx::invalidation_cause::deferred_write
			: rsx::invalidation_cause::deferred_read;

		const auto result = m_frame_resources->textures.invalidate_address(m_frame_resources->recorder, address, cause);

		if (!result.violation_handled)
		{
			// The fault is not the texture cache's, so it belongs to a query.
			return zcull_ctrl->on_access_violation(address);
		}

		return true;
	}

	// The guest unmapped a range, so every host copy of it is invalid
	// (VKGSRender.cpp:1312).
	void MTLGSRender::on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause)
	{
		if (!m_frame_resources)
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
		m_textures_dirty.fill(true);
		m_vertex_textures_dirty.fill(true);
	}
}

#endif // RSX_ENABLE_METAL4
