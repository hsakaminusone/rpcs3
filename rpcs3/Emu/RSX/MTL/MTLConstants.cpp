#include "MTLDevice.h"

#include "MTLConstants.h"
#include "MTLInstrumentation.h"

#ifdef RSX_ENABLE_METAL4

#include "util/logs.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace mtl
{
	namespace
	{
		// A texture-buffer view over a whole ring, so a draw's data is a starting
		// offset in attrib_data rather than a new view.  R8Uint makes one texel one
		// byte, which is what the shared GLSL's byte addressing needs (HLD-05 §3.5).
		MTL::Texture* make_stream_view(MTL::Buffer* buffer, u64 length)
		{
			MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::textureBufferDescriptor(
				MTL::PixelFormatR8Uint, length, MTL::ResourceStorageModeShared, MTL::TextureUsageShaderRead);

			// A class-method descriptor is autoreleased; hold it for the call only.
			descriptor->retain();

			// For a texture buffer the row stride is the buffer length, and Metal
			// requires it to be at least four bytes.
			MTL::Texture* view = buffer->newTexture(descriptor, 0, std::max<u64>(length, 4));

			descriptor->release();
			return view;
		}
	}

	void stream_ring::init(resource_manager& resources, u64 capacity)
	{
		m_resources = &resources;
		m_capacity = capacity;
		m_buffer = resources.new_buffer(capacity, MTL::StorageModeShared);

		if (m_buffer)
		{
			m_view = make_stream_view(m_buffer, m_buffer->length());
		}

		// A ring without a view binds a null texture, and the vertex fetch then
		// reads zeros with no error from Metal (the same silent class that
		// resource_manager::check_residency reports).
		if (!m_buffer || !m_view)
		{
			rsx_log.error("Metal 4: the vertex stream ring has no %s", m_buffer ? "texture view" : "buffer");
		}

		m_count = 0;
	}

	void stream_ring::shutdown()
	{
		if (m_resources)
		{
			for (MTL::Texture* view : m_retired_views)
			{
				m_resources->defer_release(view);
			}

			for (MTL::Buffer* buffer : m_retired)
			{
				m_resources->defer_release(buffer);
			}

			if (m_view)
			{
				m_resources->defer_release(m_view);
			}

			if (m_buffer)
			{
				m_resources->defer_release(m_buffer);
			}
		}

		m_retired_views.clear();
		m_retired.clear();
		m_view = nullptr;
		m_buffer = nullptr;
		m_resources = nullptr;
	}

	void stream_ring::begin_frame()
	{
		for (MTL::Texture* view : m_retired_views)
		{
			m_resources->defer_release(view);
		}

		for (MTL::Buffer* buffer : m_retired)
		{
			m_resources->defer_release(buffer);
		}

		m_retired_views.clear();
		m_retired.clear();
		m_count = 0;
	}

	void* stream_ring::alloc(u64 size, u32& offset)
	{
		offset = 0;

		if (!m_buffer || size == 0)
		{
			return nullptr;
		}

		const ring_block reservation = allocate_ring_block(m_count, size, m_capacity, 4);

		if (reservation.grow)
		{
			// A new buffer needs a new view.  The retired pair is still referenced
			// by the draws already encoded this frame, so both releases are
			// deferred (HLD-02 I7).  A single allocation can exceed the working-set
			// capacity; the replacement must hold it, or the copy runs past the
			// buffer end.
			MTL::Buffer* next = m_resources->new_buffer(reservation.capacity, MTL::StorageModeShared);
			if (!next)
			{
				return nullptr;
			}

			MTL::Texture* next_view = make_stream_view(next, next->length());
			if (!next_view)
			{
				m_resources->defer_release(next);
				return nullptr;
			}

			m_retired.push_back(m_buffer);
			m_retired_views.push_back(m_view);
			m_buffer = next;
			m_view = next_view;
			m_capacity = reservation.capacity;
		}

		m_count = reservation.end;
		offset = static_cast<u32>(reservation.offset);
		return static_cast<std::byte*>(m_buffer->contents()) + reservation.offset;
	}

	void byte_ring::init(resource_manager& resources, u64 capacity)
	{
		m_resources = &resources;
		m_capacity = capacity;
		m_buffer = resources.new_buffer(capacity, MTL::StorageModeShared);
		m_count = 0;
	}

	void byte_ring::shutdown()
	{
		if (m_resources)
		{
			for (MTL::Buffer* buffer : m_retired)
			{
				m_resources->defer_release(buffer);
			}

			if (m_buffer)
			{
				m_resources->defer_release(m_buffer);
			}
		}

		m_retired.clear();
		m_buffer = nullptr;
		m_resources = nullptr;
	}

	void byte_ring::begin_frame()
	{
		for (MTL::Buffer* buffer : m_retired)
		{
			m_resources->defer_release(buffer);
		}

		m_retired.clear();
		m_count = 0;
	}

	void* byte_ring::alloc(u64 size, u32& offset, u64 alignment)
	{
		offset = 0;

		if (!m_buffer || size == 0)
		{
			return nullptr;
		}

		const ring_block reservation = allocate_ring_block(m_count, size, m_capacity, alignment);

		if (reservation.grow)
		{
			MTL::Buffer* next = m_resources->new_buffer(reservation.capacity, MTL::StorageModeShared);
			if (!next)
			{
				return nullptr;
			}

			m_retired.push_back(m_buffer);
			m_buffer = next;
			m_capacity = reservation.capacity;
		}

		m_count = reservation.end;
		offset = static_cast<u32>(reservation.offset);
		return static_cast<std::byte*>(m_buffer->contents()) + reservation.offset;
	}

	void constants_heap::init(resource_manager& resources, u32 frame_count)
	{
		// The heap sizes to the recorder's ring, because a slot's blocks are reused
		// only after that slot's frame has completed (HLD-04 §3.2).
		ensure(frame_count == frame_slot_count);

		for (ring_set& set : m_sets)
		{
			set.vertex_context.init(resources, draw_capacity);
			set.draw_parameters.init(resources, draw_capacity);
			set.fragment_context.init(resources, draw_capacity);
			set.sampler_info.init(resources, draw_capacity);
			set.stipple.init(resources, draw_capacity);
			set.blend_values.init(resources, draw_capacity);
			set.fill_values.init(resources, fill_capacity);
			set.vertex_draw_params.init(resources, draw_capacity);
			set.fragment_constants.init(resources, constant_capacity);
			// One change is one 468-vec4 file, so the ring holds the changes, not
			// the vec4 (MTLConstants.h:256).
			set.transform_constants.init(resources, transform_capacity * vertex_constant_count);
			set.instancing_constants.init(resources, constant_capacity);
			set.instancing_lookup.init(resources, constant_capacity);
		}

		for (stream_set& set : m_streams)
		{
			set.persistent.init(resources, stream_capacity);
			set.volatile_stream.init(resources, volatile_capacity);
			set.index.init(resources, index_capacity);
			set.instructions.init(resources, instruction_capacity);
			set.overlay_vertices.init(resources, overlay_vertex_capacity);
			set.overlay_uniforms.init(resources, overlay_uniform_capacity);
		}
	}

	void constants_heap::shutdown()
	{
		for (ring_set& set : m_sets)
		{
			set.vertex_context.shutdown();
			set.draw_parameters.shutdown();
			set.fragment_context.shutdown();
			set.sampler_info.shutdown();
			set.stipple.shutdown();
			set.blend_values.shutdown();
			set.fill_values.shutdown();
			set.vertex_draw_params.shutdown();
			set.fragment_constants.shutdown();
			set.transform_constants.shutdown();
			set.instancing_constants.shutdown();
			set.instancing_lookup.shutdown();
		}

		for (stream_set& set : m_streams)
		{
			set.persistent.shutdown();
			set.volatile_stream.shutdown();
			set.index.shutdown();
			set.instructions.shutdown();
			set.overlay_vertices.shutdown();
			set.overlay_uniforms.shutdown();
		}

		m_slot = 0;
	}

	void constants_heap::capture_generations(u32* out) const
	{
		const ring_set& set = m_sets[m_slot];
		const stream_set& streams = m_streams[m_slot];

		const u32 generations[frame_snapshot::ring_count] = {
			streams.persistent.generation(), streams.volatile_stream.generation(),
			streams.index.generation(), streams.instructions.generation(),
			streams.overlay_vertices.generation(), streams.overlay_uniforms.generation(),
			set.vertex_context.generation(), set.draw_parameters.generation(),
			set.fragment_context.generation(), set.sampler_info.generation(),
			set.stipple.generation(), set.blend_values.generation(),
			set.fill_values.generation(), set.vertex_draw_params.generation(),
			set.fragment_constants.generation(), set.transform_constants.generation(),
			set.instancing_constants.generation(), set.instancing_lookup.generation()
		};

		for (u32 ring = 0; ring < frame_snapshot::ring_count; ++ring)
		{
			out[ring] = generations[ring];
		}
	}

	void constants_heap::begin_frame(u32 slot)
	{
		m_slot = slot % frame_slot_count;

		ring_set& set = m_sets[m_slot];
		set.vertex_context.begin_frame();
		set.draw_parameters.begin_frame();
		set.fragment_context.begin_frame();
		set.sampler_info.begin_frame();
		set.stipple.begin_frame();
		set.blend_values.begin_frame();
		set.fill_values.begin_frame();
		set.vertex_draw_params.begin_frame();
		set.fragment_constants.begin_frame();
		set.transform_constants.begin_frame();
		set.instancing_constants.begin_frame();
		set.instancing_lookup.begin_frame();

		stream_set& streams = m_streams[m_slot];
		streams.persistent.begin_frame();
		streams.volatile_stream.begin_frame();
		streams.index.begin_frame();
		streams.instructions.begin_frame();
		streams.overlay_vertices.begin_frame();
		streams.overlay_uniforms.begin_frame();

		// The records describe the same bytes as the rings, so they retire with
		// them.
		m_draws[m_slot].clear();

		// The generation at the start of the frame is what makes a mid-frame
		// growth visible: a ring that replaces its buffer after this point
		// retires the bytes the earlier draws read.
		capture_generations(m_begin_generation[m_slot].data());
	}

	namespace
	{
		// TODO(excise): level 5; one snapshot written as the files a capture reads
		// back.  `age` is the distance from the photographed frame: "m3".."m0" are
		// the frames before it, "p1".."p4" the ones after.
		void dump_snapshot(const frame_snapshot& frame, u32 index, const char* age)
		{
			if (!frame.valid)
			{
				return;
			}

			std::error_code error;
			std::filesystem::create_directories(probe::dump_directory(), error);

			char path[512];
			char manifest_path[512];
			std::snprintf(manifest_path, sizeof(manifest_path), "%s/frame-%u-%s-manifest.txt", probe::dump_directory(), index, age);

			FILE* manifest = std::fopen(manifest_path, "w");

			if (!manifest)
			{
				return;
			}

			// One file per ring, named by the block type the shader indexes, so the
			// manifest and the bytes line up by inspection.
			const auto write = [&](const char* name, const std::vector<u8>& data)
			{
				if (data.empty())
				{
					return;
				}

				std::snprintf(path, sizeof(path), "%s/frame-%u-%s-%s.bin", probe::dump_directory(), index, age, name);

				if (FILE* file = std::fopen(path, "wb"))
				{
					std::fwrite(data.data(), 1, data.size(), file);
					std::fclose(file);
				}
			};

			// Every ring the shader can read.  A collapsed glyph is decided by the
			// transform-constant block, so the capture that omitted it could not
			// answer the question it was built for.
			write("persistent", frame.persistent);
			write("volatile", frame.volatile_stream);
			write("index", frame.index);
			write("instructions", frame.instructions);
			write("overlay-vertices", frame.overlay_vertices);
			write("overlay-uniforms", frame.overlay_uniforms);
			write("vertex-context", frame.vertex_context);
			write("draw-parameters", frame.draw_parameters);
			write("fragment-context", frame.fragment_context);
			write("sampler-info", frame.sampler_info);
			write("stipple", frame.stipple);
			write("blend-values", frame.blend_values);
			write("fill-values", frame.fill_values);
			write("vertex-draw-params", frame.vertex_draw_params);
			write("fragment-constants", frame.fragment_constants);
			write("transform-constants", frame.transform_constants);
			write("instancing-constants", frame.instancing_constants);
			write("instancing-lookup", frame.instancing_lookup);

			// A ring that grew mid-frame retired the buffer the earlier draws read.
			// Those bytes are named by ring id, so a record's generation can be
			// paired with the exact buffer it addresses.
			for (usz retired_index = 0; retired_index < frame.retired.size(); ++retired_index)
			{
				const frame_snapshot::retired_ring& retired = frame.retired[retired_index];
				std::snprintf(path, sizeof(path), "%s/frame-%u-%s-retired-%u-%zu.bin",
					probe::dump_directory(), index, age, retired.id, retired_index);

				if (FILE* file = std::fopen(path, "wb"))
				{
					std::fwrite(retired.data.data(), 1, retired.data.size(), file);
					std::fclose(file);
				}
			}

			// The per-draw records are binary because there are ~1000 of them per
			// frame and every field matters.  The text manifest carries the same
			// identity so a human can read a frame without the parser.
			if (!frame.draws.empty())
			{
				std::snprintf(path, sizeof(path), "%s/frame-%u-%s-draws.bin", probe::dump_directory(), index, age);

				if (FILE* file = std::fopen(path, "wb"))
				{
					std::fwrite(frame.draws.data(), sizeof(draw_record), frame.draws.size(), file);
					std::fclose(file);
				}
			}

			std::fprintf(manifest, "present %u age %s draws %u retired %zu grew 0x%x\n",
				frame.present, age, frame.draw_count, frame.retired.size(), frame.grew_mask);

			for (u32 ring = 0; ring < frame_snapshot::ring_count; ++ring)
			{
				std::fprintf(manifest, "ring %u generation %u\n", ring, frame.ring_generation[ring]);
			}

			std::fprintf(manifest, "persistent %zu\n", frame.persistent.size());
			std::fprintf(manifest, "volatile %zu\n", frame.volatile_stream.size());
			std::fprintf(manifest, "index %zu\n", frame.index.size());
			std::fprintf(manifest, "instructions %zu\n", frame.instructions.size());
			std::fprintf(manifest, "overlay-vertices %zu\n", frame.overlay_vertices.size());
			std::fprintf(manifest, "overlay-uniforms %zu\n", frame.overlay_uniforms.size());
			std::fprintf(manifest, "vertex-context %zu\n", frame.vertex_context.size() / sizeof(vertex_context));
			std::fprintf(manifest, "draw-parameters %zu\n", frame.draw_parameters.size() / sizeof(draw_parameters));
			std::fprintf(manifest, "fragment-context %zu\n", frame.fragment_context.size() / sizeof(fragment_context));
			std::fprintf(manifest, "sampler-info %zu\n", frame.sampler_info.size() / sizeof(sampler_info));
			std::fprintf(manifest, "stipple %zu\n", frame.stipple.size() / sizeof(vec4_block));
			std::fprintf(manifest, "blend-values %zu\n", frame.blend_values.size() / sizeof(blend_values));
			std::fprintf(manifest, "fill-values %zu\n", frame.fill_values.size() / sizeof(fill_values));
			std::fprintf(manifest, "vertex-draw-params %zu\n", frame.vertex_draw_params.size() / sizeof(u32));
			std::fprintf(manifest, "fragment-constants %zu\n", frame.fragment_constants.size() / sizeof(vec4_block));
			std::fprintf(manifest, "transform-constants %zu\n", frame.transform_constants.size() / sizeof(vec4_block));
			std::fprintf(manifest, "instancing-constants %zu\n", frame.instancing_constants.size() / sizeof(vec4_block));
			std::fprintf(manifest, "instancing-lookup %zu\n", frame.instancing_lookup.size() / sizeof(s32));
			std::fprintf(manifest, "draws-record %zu\n", frame.draws.size());
			std::fprintf(manifest, "draws-record-size %zu\n", sizeof(draw_record));

			// The draw parameters are the map from a draw to its place in the
			// streams: `attrib_data[unit]` is (packed descriptor, byte offset) and
			// `vertex_base_index` is the first vertex the draw fetched.
			const draw_parameters* parameters = reinterpret_cast<const draw_parameters*>(frame.draw_parameters.data());

			for (u64 draw = 0; parameters && draw < frame.draw_parameters.size() / sizeof(draw_parameters); ++draw)
			{
				const draw_parameters& p = parameters[draw];

				std::fprintf(manifest, "draw %llu id %u base %u index %u xform %u vs %u fs %u fstex %u stipple %u",
					static_cast<unsigned long long>(draw), p.draw_id, p.vertex_base_index, p.vertex_index_offset,
					p.xform_constants_offset, p.vs_context_offset, p.fs_context_offset, p.fs_texture_base_index,
					p.fs_stipple_pattern_offset);

				// Bit 30 of the descriptor is the volatile flag, so the pair names
				// the stream and the offset inside it (RSXDrawCommands.cpp:352-360).
				for (u32 unit = 0; unit < 16; ++unit)
				{
					if (p.attrib_data[unit][0] || p.attrib_data[unit][1])
					{
						std::fprintf(manifest, " a%u=%d:%d", unit, p.attrib_data[unit][0], p.attrib_data[unit][1]);
					}
				}

				std::fprintf(manifest, "\n");
			}

			// The per-draw record.  The draw-parameter block above is the map to
			// the streams; this is the state the command buffer captured, including
			// the pipeline, the descriptor and the texture units the block cannot
			// name.
			for (const draw_record& r : frame.draws)
			{
				std::fprintf(manifest,
					"record %u frame %u sub %u cmd %u prim %u flags 0x%x idx %u/%u off %u len %u "
					"vtx %u base %u min %u max %u pbase %u vbase %u pbytes %u vbytes %u "
					"vp %u fp %u key %u pipe 0x%llx desc 0x%x xform %u vs %u fs %u fsc %u fstex %u stipple %u blend %u "
					"color 0x%x/0x%x/0x%x/0x%x fmt 0x%x depth 0x%x fmt 0x%x %ux%u scissor %u,%u,%u,%u "
					"vp4 %.3f,%.3f,%.3f,%.3f blendh 0x%x depthh 0x%x rasterh 0x%x "
					"hash p 0x%x v 0x%x i 0x%x x 0x%x fc 0x%x s 0x%x\n",
					r.global_id, r.frame_id, r.subdraw, r.command, r.primitive, r.flags,
					r.index_type, r.index_count, r.index_offset, r.index_length,
					r.vertex_count, r.vertex_base, r.min_index, r.max_index,
					r.persistent_base, r.volatile_base, r.persistent_bytes, r.volatile_bytes,
					r.vp_id, r.fp_id, r.pipeline_key,
					static_cast<unsigned long long>(r.pipeline_state), r.vertex_desc_hash,
					r.xform_offset, r.vs_context_offset, r.fs_context_offset, r.fs_constants_offset,
					r.fs_texture_base_index, r.fs_stipple_offset, r.blend_values_index,
					r.color_target[0], r.color_target[1], r.color_target[2], r.color_target[3],
					r.color_format, r.depth_target, r.depth_format, r.width, r.height,
					r.scissor[0], r.scissor[1], r.scissor[2], r.scissor[3],
					static_cast<f64>(r.viewport[0]), static_cast<f64>(r.viewport[1]),
					static_cast<f64>(r.viewport[2]), static_cast<f64>(r.viewport[3]),
					r.blend_hash, r.depth_hash, r.raster_hash,
					r.hash_persistent, r.hash_volatile, r.hash_index, r.hash_xform,
					r.hash_fragment_constants, r.hash_sampler);

				for (u32 unit = 0; unit < 16; ++unit)
				{
					if (r.tex_addr[unit] || r.tex_format[unit])
					{
						std::fprintf(manifest, "t%u 0x%x/0x%x/%ux%u/s0x%x/f0x%x/lod %.3f,%.3f,%.3f/mip %u/pitch %u/dim %u/remap 0x%x ",
							unit, r.tex_addr[unit], r.tex_format[unit], r.tex_width[unit],
							r.tex_height[unit], r.tex_sampler_hash[unit], r.tex_sampler_flags[unit],
							static_cast<f64>(r.tex_lod[unit][0]), static_cast<f64>(r.tex_lod[unit][1]),
							static_cast<f64>(r.tex_lod[unit][2]), r.tex_mip[unit], r.tex_pitch[unit],
							r.tex_dimension[unit], r.tex_format_ex[unit]);
					}
				}

				for (u32 unit = 0; unit < 16; ++unit)
				{
					if (r.attrib_desc[unit] || r.attrib_offset[unit])
					{
						std::fprintf(manifest, "a%u %d:%d ", unit, r.attrib_desc[unit], r.attrib_offset[unit]);
					}
				}

				std::fprintf(manifest, "\n");
			}

			std::fclose(manifest);
		}

		// TODO(excise): level 5; the copy a flip makes of one frame's rings.
		void copy_ring(std::vector<u8>& dst, const void* source, u64 size)
		{
			if (!source || !size)
			{
				dst.clear();
				return;
			}

			dst.resize(size);
			std::memcpy(dst.data(), source, size);
		}

		// TODO(excise): level 5; the buffers a ring retired inside the frame.  A
		// grow replaces the buffer, so the draws encoded before it address a buffer
		// that is no longer the ring's current one.  Copying the retired buffers is
		// what makes a mid-frame grow visible instead of silently truncating the
		// capture.
		template <typename Ring>
		void collect_retired(std::vector<frame_snapshot::retired_ring>& out, u32 id, const Ring& ring)
		{
			for (MTL::Buffer* buffer : ring.retired())
			{
				if (!buffer)
				{
					continue;
				}

				frame_snapshot::retired_ring entry;
				entry.id = id;
				entry.data.resize(buffer->length());
				std::memcpy(entry.data.data(), buffer->contents(), entry.data.size());
				out.push_back(std::move(entry));
			}
		}
	}

	// TODO(excise): level 5; see the declaration.  This runs on every flip, so it
	// is a bounded memcpy and nothing else: the rings are already in memory, and
	// the copy is the only way to keep a frame past its slot's next reset.
	void constants_heap::snapshot_frame(u32 present)
	{
		if (!probe::at_least(5))
		{
			return;
		}

		const ring_set& set = m_sets[m_slot];
		const stream_set& streams = m_streams[m_slot];

		frame_snapshot& frame = m_history[m_history_pos];
		m_history_pos = (m_history_pos + 1) % history_depth;

		copy_ring(frame.persistent, streams.persistent.data(), streams.persistent.used());
		copy_ring(frame.volatile_stream, streams.volatile_stream.data(), streams.volatile_stream.used());
		copy_ring(frame.index, streams.index.data(), streams.index.used());
		copy_ring(frame.instructions, streams.instructions.data(), streams.instructions.used());
		copy_ring(frame.overlay_vertices, streams.overlay_vertices.data(), streams.overlay_vertices.used());
		copy_ring(frame.overlay_uniforms, streams.overlay_uniforms.data(), streams.overlay_uniforms.used());
		copy_ring(frame.vertex_context, set.vertex_context.data(), set.vertex_context.used() * sizeof(vertex_context));
		copy_ring(frame.draw_parameters, set.draw_parameters.data(), set.draw_parameters.used() * sizeof(draw_parameters));
		copy_ring(frame.fragment_context, set.fragment_context.data(), set.fragment_context.used() * sizeof(fragment_context));
		copy_ring(frame.sampler_info, set.sampler_info.data(), set.sampler_info.used() * sizeof(sampler_info));
		copy_ring(frame.stipple, set.stipple.data(), set.stipple.used() * sizeof(vec4_block));
		copy_ring(frame.blend_values, set.blend_values.data(), set.blend_values.used() * sizeof(blend_values));
		copy_ring(frame.fill_values, set.fill_values.data(), set.fill_values.used() * sizeof(fill_values));
		copy_ring(frame.vertex_draw_params, set.vertex_draw_params.data(), set.vertex_draw_params.used() * sizeof(u32));
		copy_ring(frame.fragment_constants, set.fragment_constants.data(), set.fragment_constants.used() * sizeof(vec4_block));
		copy_ring(frame.transform_constants, set.transform_constants.data(), set.transform_constants.used() * sizeof(vec4_block));
		copy_ring(frame.instancing_constants, set.instancing_constants.data(), set.instancing_constants.used() * sizeof(vec4_block));
		copy_ring(frame.instancing_lookup, set.instancing_lookup.data(), set.instancing_lookup.used() * sizeof(s32));

		// The records are the draw-level map to the rings above.
		frame.draws = m_draws[m_slot];

		// The generation of each ring, in the order the dump names them.  The
		// manifest carries the absolute generation, so two frames can be compared;
		// `grew_mask` is the subset that changed after begin_frame, which is the
		// condition the retired dump exists for.
		u32 generations[frame_snapshot::ring_count] = {};
		capture_generations(generations);

		frame.grew_mask = 0;

		for (u32 ring = 0; ring < frame_snapshot::ring_count; ++ring)
		{
			frame.ring_generation[ring] = generations[ring];
			frame.grew_mask |= (generations[ring] != m_begin_generation[m_slot][ring]) ? (1u << ring) : 0u;
		}

		frame.retired.clear();
		collect_retired(frame.retired, 0, streams.persistent);
		collect_retired(frame.retired, 1, streams.volatile_stream);
		collect_retired(frame.retired, 2, streams.index);
		collect_retired(frame.retired, 3, streams.instructions);
		collect_retired(frame.retired, 4, streams.overlay_vertices);
		collect_retired(frame.retired, 5, streams.overlay_uniforms);
		collect_retired(frame.retired, 6, set.vertex_context);
		collect_retired(frame.retired, 7, set.draw_parameters);
		collect_retired(frame.retired, 8, set.fragment_context);
		collect_retired(frame.retired, 9, set.sampler_info);
		collect_retired(frame.retired, 10, set.stipple);
		collect_retired(frame.retired, 11, set.blend_values);
		collect_retired(frame.retired, 12, set.fill_values);
		collect_retired(frame.retired, 13, set.vertex_draw_params);
		collect_retired(frame.retired, 14, set.fragment_constants);
		collect_retired(frame.retired, 15, set.transform_constants);
		collect_retired(frame.retired, 16, set.instancing_constants);
		collect_retired(frame.retired, 17, set.instancing_lookup);

		frame.present = present;
		frame.draw_count = static_cast<u32>(m_draws[m_slot].size());
		frame.valid = true;
	}

	// TODO(excise): level 5; one subdraw as the GPU received it.
	void constants_heap::record_draw(const draw_record& record)
	{
		if (!probe::at_least(5))
		{
			return;
		}

		m_draws[m_slot].push_back(record);
	}

	// TODO(excise): level 5; the key press.  The frames before the press are the
	// ones that can hold the defect, because the screenshot is written a frame or
	// more later, so they are written first and the frames after it follow.
	void constants_heap::begin_dump(u32 index)
	{
		for (u32 step = 0; step < history_depth; ++step)
		{
			// m_history_pos already points at the oldest frame.
			const frame_snapshot& frame = m_history[(m_history_pos + step) % history_depth];
			char age[8];
			std::snprintf(age, sizeof(age), "m%u", history_depth - step);
			dump_snapshot(frame, index, age);
		}

		m_dump_index = index;
		m_dump_burst = 6;
	}

	// TODO(excise): level 5; the frames that follow a key press, so a defect that
	// lasts more than one frame is caught from both sides of the press.
	void constants_heap::continue_dump()
	{
		if (!m_dump_burst)
		{
			return;
		}

		const frame_snapshot& frame = m_history[(m_history_pos + history_depth - 1) % history_depth];

		char age[8];
		std::snprintf(age, sizeof(age), "p%u", 7 - m_dump_burst);
		dump_snapshot(frame, m_dump_index, age);

		--m_dump_burst;
	}
}

#endif // RSX_ENABLE_METAL4
