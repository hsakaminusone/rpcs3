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
#include "MTLInstrumentation.h"
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
#include "Emu/RSX/Common/io_buffer.h"
#include "Emu/RSX/Core/RSXDrawCommands.h"
#include "Emu/RSX/Overlays/Shaders/shader_loading_dialog_native.h"
#include "Emu/RSX/Overlays/overlay_debug_overlay.h"
#include "Emu/RSX/Program/program_util.h"
#include "Emu/RSX/Program/SPIRVCommon.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "util/asm.hpp"
#include "util/video_provider.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

// The two media-capture triggers are owned by the Qt side and the shared report
// path (RSXThread.cpp:43, VKPresent.cpp:16).
extern atomic_t<bool> g_user_asked_for_screenshot;
extern atomic_t<recording_mode> g_recording_mode;

namespace mtl
{
	namespace
	{
#if MTL_DEBUG_LOGS
		// The frame counter the capture window uses.  It advances in open_frame(),
		// the one place every draw frame is opened, so a window covers complete
		// frames rather than an arbitrary run of draws.
		u64 diagnostic_frame_index = 0;
		u64 diagnostic_capture_last_frame = 0;
		bool diagnostic_capture_armed = false;

		void diagnostic_frame_opened()
		{
			++diagnostic_frame_index;

			if (diagnostic_capture_armed || !probe::at_least(5))
			{
				return;
			}

			const f64 at = probe::capture_after_seconds();
			if (at < 0.0 || probe::elapsed_seconds() < at)
			{
				return;
			}

			diagnostic_capture_last_frame = diagnostic_frame_index + probe::capture_frame_count() - 1;
			diagnostic_capture_armed = true;

			MTL_NOTE(2, "Metal 4: probe capture armed at frame %llu, window %llu..%llu, %.3f s",
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<unsigned long long>(diagnostic_capture_last_frame),
				probe::elapsed_seconds());
		}

		bool diagnostic_capture_active()
		{
			return probe::at_least(5) && diagnostic_frame_index <= diagnostic_capture_last_frame;
		}

		// The sampled host images of one capture frame are dumped once each.  The
		// list is small on purpose: every dump submits and waits, and a race frame
		// only needs the units that draw the car and the environment.
		constexpr u32 diagnostic_capture_address_limit = 24;
		u32 diagnostic_capture_addresses[diagnostic_capture_address_limit] = {};
		u32 diagnostic_capture_address_count = 0;

		bool diagnostic_capture_address_seen(u32 address)
		{
			for (u32 index = 0; index < diagnostic_capture_address_count; ++index)
			{
				if (diagnostic_capture_addresses[index] == address)
				{
					return true;
				}
			}

			return false;
		}

		// TODO(excise): level 5; formats a byte block for the deep dumps.
		std::string dump_bytes(const void* data, usz size)
		{
			if (!data)
			{
				return "(null)";
			}

			static constexpr char digits[] = "0123456789abcdef";
			const u8* bytes = static_cast<const u8*>(data);
			std::string result;
			result.reserve(size * 3);

			for (usz index = 0; index < size; ++index)
			{
				if (index && (index % 4) == 0)
				{
					result += ' ';
				}

				result += digits[bytes[index] >> 4];
				result += digits[bytes[index] & 0x0f];
			}

			return result;
		}

		std::string dump_guest(u32 address, u32 words)
		{
			static constexpr char digits[] = "0123456789abcdef";
			std::string result;
			result.reserve(words * 9);

			for (u32 index = 0; index < words; ++index)
			{
				const u32 word = vm::read32(address + index * 4);

				for (int shift = 28; shift >= 0; shift -= 4)
				{
					result += digits[(word >> shift) & 0x0f];
				}

				result += ((index % 4) == 3) ? ' ' : ',';
			}

			return result;
		}
#endif // MTL_DEBUG_LOGS

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

#if MTL_DEBUG_LOGS
					// TODO(excise): level 3; the expanded index array of the first draws.
					{
						static u32 diagnostic_expansions = 0;

						if (diagnostic_expansions < 12)
						{
							const u32* expanded = static_cast<const u32*>(dst);
							const auto at = [&](u32 slot) -> u32 { return slot < index_count ? expanded[slot] : 0u; };

							MTL_NOTE(3, "Metal 4: expansion %u, primitive %u, vertices %u, indices %u, min %u, first %u %u %u %u %u %u",
								diagnostic_expansions, static_cast<u32>(clause.primitive), vertex_count, index_count, min_index,
								at(0), at(1), at(2), at(3), at(4), at(5));
						}

						diagnostic_expansions++;
					}
#endif // MTL_DEBUG_LOGS

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

				const u32 vertex_count = clause.get_elements_count();
				const u32 index_count = expands_primitive(clause.primitive)
					? ::get_index_count(clause.primitive, vertex_count)
					: vertex_count;

				const bool widen = type == rsx::index_array_type::u16 &&
					!rsx::method_registers.restart_index_enabled() &&
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
						rsx::method_registers.restart_index_enabled(),
						rsx::method_registers.restart_index(),
						expands_primitive);
				}

#if MTL_DEBUG_LOGS
				// TODO(excise): level 3; the written index array of the first draws.
				{
					static u32 diagnostic_indices = 0;

					if (diagnostic_indices < 12)
					{
						const u16* short_indices = static_cast<const u16*>(dst);
						const u32* long_indices = static_cast<const u32*>(dst);
						const bool wide = widen || (type == rsx::index_array_type::u32);

						MTL_NOTE(3, "Metal 4: indices %u, primitive %u, wide %u, count %u, written %u, length %u, min %u max %u, first %u %u %u %u %u %u",
							diagnostic_indices, static_cast<u32>(clause.primitive), wide ? 1u : 0u, index_count, written, length,
							min_index, max_index,
							wide ? long_indices[0] : short_indices[0], wide ? long_indices[1] : short_indices[1],
							wide ? long_indices[2] : short_indices[2], wide ? long_indices[3] : short_indices[3],
							wide ? long_indices[4] : short_indices[4], wide ? long_indices[5] : short_indices[5]);
					}

					diagnostic_indices++;
				}
#endif // MTL_DEBUG_LOGS

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
#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the per-draw state block, sampled for the first draws.
		void dump_draw_state(u32 index, const surface_state& surface, const pipeline_properties& properties,
			const framebuffer& attachments, const rsx::framebuffer_layout& layout, const areau& scissor, bool supports_bc)
		{
			auto& m = rsx::method_registers;
			const raster_state raster = capture_raster_state();
			const depth_stencil_state depth = capture_depth_stencil_state();

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, primitive %u, color target %u, color format %u, depth format %u, aa %u, raster type %u, clip %ux%u, depth enabled %u",
				index, static_cast<u32>(m.current_draw_clause.primitive), static_cast<u32>(surface.color_target),
				static_cast<u32>(surface.color_format), static_cast<u32>(surface.depth_format), static_cast<u32>(surface.antialias),
				static_cast<u32>(surface.raster_type), static_cast<u32>(surface.clip_width), static_cast<u32>(surface.clip_height),
				surface.depth_enabled ? 1u : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, rsx color mask %u%u%u%u, blend enable mask 0x%x, factors src %u/%u dst %u/%u, equations %u/%u, alpha test %u, logic op %u/%u, shade %u",
				index, m.color_mask_r(0) ? 1u : 0u, m.color_mask_g(0) ? 1u : 0u, m.color_mask_b(0) ? 1u : 0u, m.color_mask_a(0) ? 1u : 0u,
				m.blend_enabled_mask(),
				static_cast<u32>(m.blend_func_sfactor_rgb()), static_cast<u32>(m.blend_func_sfactor_a()),
				static_cast<u32>(m.blend_func_dfactor_rgb()), static_cast<u32>(m.blend_func_dfactor_a()),
				static_cast<u32>(m.blend_equation_rgb()), static_cast<u32>(m.blend_equation_a()),
				m.alpha_test_enabled() ? 1u : 0u, m.logic_op_enabled() ? 1u : 0u, static_cast<u32>(m.logic_operation()),
				static_cast<u32>(m.shade_mode()));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped write mask 0x%x 0x%x 0x%x 0x%x, attachment count %u, samples %u, alpha to coverage %u, alpha to one %u, raster %u, topology %u",
				index, static_cast<u32>(properties.write_mask[0]), static_cast<u32>(properties.write_mask[1]),
				static_cast<u32>(properties.write_mask[2]), static_cast<u32>(properties.write_mask[3]),
				properties.attachment_count, properties.sample_count, properties.alpha_to_coverage ? 1u : 0u,
				properties.alpha_to_one ? 1u : 0u, properties.raster_enabled ? 1u : 0u,
				static_cast<u32>(properties.input_primitive_topology));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped blend mask 0x%x src %u/%u dst %u/%u equations %u/%u alpha test %u logic %u/%u shade %u",
				index, properties.blend.enable_mask,
				static_cast<u32>(properties.blend.source_rgb), static_cast<u32>(properties.blend.source_a),
				static_cast<u32>(properties.blend.destination_rgb), static_cast<u32>(properties.blend.destination_a),
				static_cast<u32>(properties.blend.equation_rgb), static_cast<u32>(properties.blend.equation_a),
				properties.blend.alpha_test_enabled ? 1u : 0u, properties.blend.logic_op_enabled ? 1u : 0u,
				static_cast<u32>(properties.blend.logic_operation), static_cast<u32>(properties.blend.shade_mode));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, rsx depth test %u func %u write %u, stencil test %u, cull %u/%u, front face %u, polygon %u/%u, clip %.4f..%.4f, point %.3f",
				index, m.depth_test_enabled() ? 1u : 0u, static_cast<u32>(m.depth_func()), m.depth_write_enabled() ? 1u : 0u,
				m.stencil_test_enabled() ? 1u : 0u, m.cull_face_enabled() ? 1u : 0u, static_cast<u32>(m.cull_face_mode()),
				static_cast<u32>(m.front_face_mode()), static_cast<u32>(m.polygon_mode_front()), static_cast<u32>(m.polygon_mode_back()),
				static_cast<f64>(m.clip_min()), static_cast<f64>(m.clip_max()), static_cast<f64>(m.point_size()));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, mapped cull %u winding %u fill %u, depth compare %u depth write %u",
				index, static_cast<u32>(capture_cull_mode(raster)), static_cast<u32>(capture_front_face(raster)),
				static_cast<u32>(capture_fill_mode(raster)), static_cast<u32>(depth.depth_compare), depth.depth_write ? 1u : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, scissor %d %d %ux%u, layout color 0x%x 0x%x 0x%x 0x%x pitch %u %u %u %u, zeta 0x%x pitch %u, target %u, aa %u",
				index, scissor.x1, scissor.y1, scissor.width(), scissor.height(),
				layout.color_addresses[0], layout.color_addresses[1], layout.color_addresses[2], layout.color_addresses[3],
				layout.color_pitch[0], layout.color_pitch[1], layout.color_pitch[2], layout.color_pitch[3],
				layout.zeta_address, layout.zeta_pitch, static_cast<u32>(layout.target), static_cast<u32>(layout.aa_mode));

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, pass colors %u samples %u size %ux%u depth %u, color0 pixel format %u %ux%u samples %u",
				index, attachments.color_count(), attachments.sample_count(), attachments.width(), attachments.height(),
				attachments.has_depth() ? 1u : 0u,
				attachments.color(0) ? static_cast<u32>(attachments.color(0)->pixel_format()) : 0u,
				attachments.color(0) ? attachments.color(0)->width() : 0u,
				attachments.color(0) ? attachments.color(0)->height() : 0u,
				attachments.color(0) ? attachments.color(0)->samples() : 0u);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: draw %u, clear color %.4f %.4f %.4f %.4f, clear depth %.4f stencil %u, clear rect %u %u %ux%u",
				index, static_cast<f64>(surface.clear_color.red), static_cast<f64>(surface.clear_color.green),
				static_cast<f64>(surface.clear_color.blue), static_cast<f64>(surface.clear_color.alpha),
				static_cast<f64>(surface.clear_depth), static_cast<u32>(surface.clear_stencil),
				static_cast<u32>(surface.clear_x), static_cast<u32>(surface.clear_y),
				static_cast<u32>(surface.clear_width), static_cast<u32>(surface.clear_height));

			for (u32 unit = 0; unit < 16; ++unit)
			{
				const auto& tex = m.fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 gcm_format = tex.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
				const auto swizzle = get_texture_component_layout(gcm_format);

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: draw %u, unit %u, format 0x%x, masked 0x%x, host pixel format %u, swizzle A%u R%u G%u B%u, %ux%u, mipmaps %u, dimension %u",
					index, unit, static_cast<u32>(tex.format()), gcm_format,
					static_cast<u32>(get_texture_format(gcm_format, supports_bc)),
					static_cast<u32>(swizzle[0]), static_cast<u32>(swizzle[1]), static_cast<u32>(swizzle[2]), static_cast<u32>(swizzle[3]),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), static_cast<u32>(tex.mipmap()),
					static_cast<u32>(tex.get_extended_texture_dimension()));
			}
		}
#endif // MTL_DEBUG_LOGS

		// The PSO key of one draw (HLD-05 §3.5).  Every field is a latch the encoder
		// state does not carry, so the key holds exactly the pipeline's inputs.
		pipeline_properties capture_pipeline_properties(const surface_state& surface, const RSXVertexProgram& vertex)
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
			const MTL::PrimitiveTopologyClass draw_class = get_primitive_topology_class(rsx::method_registers.current_draw_clause.primitive);
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

	MTLGSRender::MTLGSRender(utils::serial* ar) noexcept
		: GSRender(ar)
	{
		// The guest shaders are compiled from GLSL, so the glslang context belongs
		// to the renderer instance and comes up before the first one is compiled
		// (VKGSRender.cpp:704).  Without it every shader parses against zero limits.
		spirv::initialize_compiler_context();
	}

	// Out of line so the unique_ptr teardown sees the complete types
	// (MTLGSRender.h only forward-declares them).
	MTLGSRender::~MTLGSRender()
	{
		// The base owns the ZCULL control through a unique_ptr and the backend
		// hands it a member of frame_resources, so the base must not delete it.
		// on_exit releases it on the graceful path; a renderer that never reached
		// on_exit reaches the teardown with the pointer still aliased
		// (GLGSRender.cpp:69, VKGSRender.cpp:916).
		zcull_ctrl.release();
		spirv::finalize_compiler_context();
	}

	u64 MTLGSRender::get_cycles()
	{
		return thread_ctrl::get_cycles(static_cast<named_thread<MTLGSRender>&>(*this));
	}

	void MTLGSRender::on_init_thread()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "init");
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
		m_flush_queue = std::make_unique<flush_queue>();
		m_frame_resources->resources.init(*m_device, frame_slot_count);
		m_frame_resources->textures.set_bc_texture_support(m_frame_resources->resources.supports_bc_texture_compression());
		m_frame_resources->recorder.init(*m_device, m_frame_resources->resources);
		m_frame_resources->fill.init(*m_device, m_frame_resources->resources);
		m_frame_resources->constants.init(m_frame_resources->resources, frame_slot_count);
		m_frame_resources->depth_cache.init(m_device->dev());
		m_frame_resources->samplers.init(m_device->dev());

		// The occlusion counters, and every pass carries their buffer
		// (MTLFramebuffer.cpp).  The base class owns the pointer, not the object.
		m_frame_resources->zcull_control.init(*m_device, m_frame_resources->resources, m_frame_resources->recorder);
		m_frame_resources->attachments.set_visibility_buffer(m_frame_resources->zcull_control.visibility_buffer(0));
		zcull_ctrl.reset(&m_frame_resources->zcull_control);

		// A frame without a native view is legal: the presenter stays invalid and
		// the frame runs without a present (C7).
		if (GSFrameBase* frame = get_frame())
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

		if (!m_overlay_manager)
		{
			GSFrameBase* frame = get_frame();

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
			rsx::shader_loading_dialog_native dlg(this);
			m_frame_resources->shaders->load(&dlg, context);
		}

		GSRender::on_init_thread();
	}

	void MTLGSRender::on_exit()
	{
		// The tables are printed one last time, because a bounded run ends in a
		// signal and its periodic report is the only other chance to see them.
		// TODO(excise): level 1; the final aggregate report.
		MTL_PERF_REPORT(1);
		// TODO(excise): level 2; the final coverage table.
		MTL_PROBE_DUMP(2);

		if (m_frame_resources)
		{
			// The compiler is the first owner that can outlive the renderer.  A
			// timeout here leaves the frame resources and the device alive; the
			// completion handler may still hold a program address.
			if (!m_frame_resources->jobs.shutdown())
			{
				// TODO(excise): level 1; the shader registry did not drain.
				MTL_PROBE_LATCH(1, "session/archive skipped after abort");
				rsx_log.error("Metal: shader jobs did not drain; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				GSRender::on_exit();
				return;
			}

			if (m_device && m_device->lost_flag().load(std::memory_order_acquire))
			{
				// A removed device has no valid queue or compiler.  The archive flush
				// and the ring shutdown both touch it, so leave both objects alive.
				// TODO(excise): level 1; the device disappeared during the session.
				MTL_PROBE_LATCH(1, "device/removed at teardown");
				rsx_log.error("Metal: device removed; leaking the frame resources and device");
				m_frame_resources.release();
				m_device.release();
				GSRender::on_exit();
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
			// The base class must let go of the backend before the backend goes.
			zcull_ctrl.release();
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
		GSRender::on_exit();
	}

	void MTLGSRender::open_frame()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/open");
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

#if MTL_DEBUG_LOGS
		diagnostic_frame_opened();
#endif
	}

	void MTLGSRender::begin()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/begin");

		// Publish pipelines that finished while the previous frame presented.
		if (m_frame_resources)
		{
			m_frame_resources->jobs.advance();
		}

		open_frame();
		rsx::thread::begin();
	}

	bool MTLGSRender::update_scissor(bool clip_viewport)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "scissor");
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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/bind");
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
		reconcile_render_targets();

		// TODO(excise): level 2; the framebuffer bind stream and its formats.  A
		// bind runs near once per draw, so it is counted and classified here.
		MTL_PROBE_LATCH(2, "framebuffer/bind");
		MTL_PROBE_HISTOGRAM(2, "framebuffer color format", static_cast<u32>(m_framebuffer_layout.color_format), "gcm color format");
		MTL_PROBE_HISTOGRAM(2, "framebuffer depth format", static_cast<u32>(m_framebuffer_layout.depth_format), "gcm depth format");
		MTL_PROBE_HISTOGRAM(2, "framebuffer aa mode", static_cast<u32>(m_framebuffer_layout.aa_mode), "aa mode");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first binds of the run only.
		static u32 diagnostic_binds = 0;
		const bool diagnostic_bind_trace = diagnostic_binds++ < 8;

		if (diagnostic_bind_trace)
		{
			MTL_NOTE(3, "Metal 4: bind context %u, color 0x%x/0x%x/0x%x/0x%x fmt 0x%x pitch %u, depth 0x%x fmt 0x%x pitch %u, %ux%u, aa_mode %u",
				static_cast<u32>(context),
				m_framebuffer_layout.color_addresses[0], m_framebuffer_layout.color_addresses[1],
				m_framebuffer_layout.color_addresses[2], m_framebuffer_layout.color_addresses[3],
				static_cast<u32>(m_framebuffer_layout.color_format), m_framebuffer_layout.actual_color_pitch[0],
				m_framebuffer_layout.zeta_address, static_cast<u32>(m_framebuffer_layout.depth_format),
				m_framebuffer_layout.actual_zeta_pitch,
				static_cast<u32>(m_framebuffer_layout.width), static_cast<u32>(m_framebuffer_layout.height),
				static_cast<u32>(m_framebuffer_layout.aa_mode));
		}
#endif // MTL_DEBUG_LOGS

		update_scissor(clipped_scissor);
		// The occlusion result of the previous framebuffer is stale after a bind
		// (VKGSRender.cpp:2851, GLRenderTargets.cpp:321).
		check_zcull_status(true);
		return m_frame_resources->attachments.valid();
	}

	void MTLGSRender::reconcile_render_targets()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "framebuffer/reconcile");
		frame_resources& frame = *m_frame_resources;
		surface_cache& store = frame.cache;
		texture_cache& textures = frame.textures;
		command_recorder& cmd = frame.recorder;

		const u8 color_bpp = rsx::get_format_block_size_in_bytes(m_framebuffer_layout.color_format);
		const u8 samples = rsx::get_format_sample_count(m_framebuffer_layout.aa_mode);

		// A range that stops being a target is written back or dropped before the
		// record of it is replaced, because the cache no longer knows the address
		// afterwards (VKGSRender.cpp:2659-2692).
		for (u32 index = 0; index < rsx::limits::color_buffers_count; ++index)
		{
			if (m_surface_info[index].pitch && g_cfg.video.write_color_buffers)
			{
				const auto stale_range = m_surface_info[index].get_memory_range();
				textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
				textures.flush_if_cache_miss_likely(cmd, stale_range);
			}

			m_surface_info[index].address = 0;
			m_surface_info[index].pitch = 0;
			m_surface_info[index].width = m_framebuffer_layout.width;
			m_surface_info[index].height = m_framebuffer_layout.height;
			m_surface_info[index].color_format = m_framebuffer_layout.color_format;
			m_surface_info[index].bpp = color_bpp;
			m_surface_info[index].samples = samples;
		}

		if (m_depth_surface_info.pitch && g_cfg.video.write_depth_buffer)
		{
			const auto stale_range = m_depth_surface_info.get_memory_range();
			textures.set_memory_read_flags(stale_range, rsx::memory_read_flags::flush_once);
			textures.flush_if_cache_miss_likely(cmd, stale_range);
		}

		m_depth_surface_info.address = 0;
		m_depth_surface_info.pitch = 0;
		m_depth_surface_info.width = m_framebuffer_layout.width;
		m_depth_surface_info.height = m_framebuffer_layout.height;
		m_depth_surface_info.depth_format = m_framebuffer_layout.depth_format;
		m_depth_surface_info.bpp = rsx::get_format_block_size_in_bytes(m_framebuffer_layout.depth_format);
		m_depth_surface_info.samples = samples;

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

		for (u8 index : rsx::utility::get_rtt_indexes(m_framebuffer_layout.target))
		{
			surface* target = store.m_bound_render_targets[index].second;
			if (!target)
			{
				continue;
			}

			m_surface_info[index].address = m_framebuffer_layout.color_addresses[index];
			m_surface_info[index].pitch = m_framebuffer_layout.actual_color_pitch[index];

			// A target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2780).
			if (!m_surface_info[index].address || !m_surface_info[index].pitch)
			{
				continue;
			}

			ensure(target->get_rsx_pitch() == m_framebuffer_layout.actual_color_pitch[index]);

			const auto range = m_surface_info[index].get_memory_range(m_framebuffer_layout.aa_factors);

			// A cached view of this range, and every scaled or cropped view derived
			// from it, is stale the moment the range becomes an attachment.
			textures.notify_surface_changed(range);

			if (g_cfg.video.write_color_buffers)
			{
				// The section becomes the guest-side backing of the surface, so the
				// write-back at flip has somewhere to land (texture_cache.h:1410).
				textures.lock_memory_region(cmd, target, range, true,
					m_surface_info[index].width, m_surface_info[index].height,
					m_surface_info[index].pitch, target);
			}
			else
			{
				// The guest copy is never updated, so drop the cached section: the
				// sample must resolve to the surface instead (texture_cache.h:2046).
				textures.commit_framebuffer_memory_region(cmd, range);
			}
		}

		// TODO(excise): level 2; the render-target store stream and its turnover.  A
		// store runs near once per draw, so it is counted and classified here.
		MTL_PROBE_LATCH(2, "framebuffer/store");
		MTL_PROBE_HISTOGRAM(2, "store orphans", static_cast<u32>(store.orphaned_surfaces.size()), "orphaned surfaces");
		MTL_PROBE_HISTOGRAM(2, "store superseded", static_cast<u32>(store.superseded_surfaces.size()), "superseded surfaces");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first stores of the run only.
		static u32 diagnostic_stores = 0;

		if (diagnostic_stores++ < 8)
		{
			char held[256] = {};
			u32 held_len = 0;

			for (u32 i = 0; i < 4; ++i)
			{
				const auto& bound = store.m_bound_render_targets[i];

				if (!bound.second)
				{
					continue;
				}

				const u32 remaining = (held_len < sizeof(held)) ? static_cast<u32>(sizeof(held) - held_len) : 0u;

				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(held + held_len, remaining, " c%u=0x%x/%ux%u/p%u", i,
					bound.first, static_cast<u32>(bound.second->width()), static_cast<u32>(bound.second->height()),
					bound.second->get_rsx_pitch());

				if (written <= 0)
				{
					break;
				}

				held_len += static_cast<u32>(written);

				if (held_len >= sizeof(held) - 48)
				{
					break;
				}
			}

			MTL_NOTE(3, "Metal 4: store holds%s, depth 0x%x, orphans %u, superseded %u",
				held_len ? held : " nothing", store.m_bound_depth_stencil.first,
				static_cast<u32>(store.orphaned_surfaces.size()), static_cast<u32>(store.superseded_surfaces.size()));
		}
#endif // MTL_DEBUG_LOGS

		if (surface* depth = store.m_bound_depth_stencil.second)
		{
			m_depth_surface_info.address = m_framebuffer_layout.zeta_address;
			m_depth_surface_info.pitch = m_framebuffer_layout.actual_zeta_pitch;

			// A depth target with no address or no pitch has no guest range to lock
			// (VKGSRender.cpp:2796).
			if (m_depth_surface_info.address && m_depth_surface_info.pitch)
			{
				ensure(depth->get_rsx_pitch() == m_framebuffer_layout.actual_zeta_pitch);

				const auto range = m_depth_surface_info.get_memory_range(m_framebuffer_layout.aa_factors);
				textures.notify_surface_changed(range);

				if (g_cfg.video.write_depth_buffer)
				{
					textures.lock_memory_region(cmd, depth, range, true,
						m_depth_surface_info.width, m_depth_surface_info.height,
						m_depth_surface_info.pitch, depth);
				}
				else
				{
					textures.commit_framebuffer_memory_region(cmd, range);
				}
			}
		}
	}

	void MTLGSRender::clear_surface(u32 mask)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "clear");
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

		surface_state state = capture_surface_state();

		// The clear registers hold one A8R8G8B8 word that the hardware copies into the
		// surface, so the format decides the colour and the channels the clear writes
		// (S4, S3; VKGSRender.cpp:1526-1578).
		const resolved_clear resolved = resolve_surface_clear(state.color_format,
			rsx::method_registers.clear_color_r(), rsx::method_registers.clear_color_g(),
			rsx::method_registers.clear_color_b(), rsx::method_registers.clear_color_a(), mask);
		state.clear_color = resolved.color;
		state.clear_colormask = resolved.mask;
		state.clear_color_fast = resolved.fast;

		const u8 stencil_mask = rsx::method_registers.stencil_mask();
		const bool write_color = resolved.mask != 0;
		const bool write_depth = (mask & RSX_GCM_CLEAR_DEPTH_BIT) != 0;
		const bool write_stencil = (mask & RSX_GCM_CLEAR_STENCIL_BIT) != 0;

		// The stencil write mask below 0xff makes the stencil clear a partial write,
		// which MTLLoadActionClear cannot express.  The fill then writes the reference
		// through the mask (S5; VKGSRender.cpp:1624-1638).
		const bool partial_stencil = write_stencil && stencil_mask != 0xff;
		state.clear_stencil_fast = !partial_stencil;

		// A clear writes the attachments, so the store marks them newest for the same
		// reason a draw does: a later sample of the range must resolve to the surface
		// and not to the value the clear replaced (VKGSRender.cpp:1648-1651).
		m_frame_resources->cache.on_write({ write_color, write_color, write_color, write_color }, write_depth || write_stencil);

		framebuffer& attachments = m_frame_resources->attachments;

		if (state.clear_covers_attachment() && (!write_color || resolved.fast) && !partial_stencil)
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

		m_frame_resources->encoder = encoder;
		record_pass_hazards(false);

		u16 x = static_cast<u16>(m_scissor.x1);
		u16 y = static_cast<u16>(m_scissor.y1);
		u16 width = static_cast<u16>(m_scissor.width());
		u16 height = static_cast<u16>(m_scissor.height());

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
			const fill_stencil stencil = write_stencil ? fill_stencil{ state.clear_stencil, stencil_mask } : fill_stencil{};
			m_frame_resources->fill.fill(m_frame_resources->recorder.fragment_table(), encoder, values, color_format, attachments.sample_count(), to_color_write_mask(resolved.mask), write_depth, stencil);
		}

		m_frame_resources->encoder = nullptr;
		m_frame_resources->recorder.close_encoder();
	}

#if MTL_DEBUG_LOGS
	// TODO(excise): level 5; the sampled images of the present readback.
	MTL::Texture* diagnostic_sampled_image[8] = {};
	u32 diagnostic_sampled_address[8] = {};
	u32 diagnostic_sampled_context[8] = {};
	u32 diagnostic_sampled_count = 0;

	// TODO(excise): level 5; the bytes per pixel of one host format.
	u32 diagnostic_pixel_size(MTL::PixelFormat format)
	{
		switch (format)
		{
		case MTL::PixelFormat::PixelFormatRGBA8Unorm:
		case MTL::PixelFormat::PixelFormatRGBA8Unorm_sRGB:
		case MTL::PixelFormat::PixelFormatBGRA8Unorm:
		case MTL::PixelFormat::PixelFormatBGRA8Unorm_sRGB:
		case MTL::PixelFormat::PixelFormatRGB10A2Unorm:
		case MTL::PixelFormat::PixelFormatBGR10A2Unorm:
		case MTL::PixelFormat::PixelFormatRGBA32Uint:
		case MTL::PixelFormat::PixelFormatRGBA32Float:
			return 4;
		case MTL::PixelFormat::PixelFormatB5G6R5Unorm:
		case MTL::PixelFormat::PixelFormatA1BGR5Unorm:
		case MTL::PixelFormat::PixelFormatBGR5A1Unorm:
		case MTL::PixelFormat::PixelFormatR16Unorm:
		case MTL::PixelFormat::PixelFormatR16Float:
		case MTL::PixelFormat::PixelFormatRG8Unorm:
		case MTL::PixelFormat::PixelFormatRG16Unorm:
		case MTL::PixelFormat::PixelFormatRG16Float:
			return 2;
		case MTL::PixelFormat::PixelFormatR8Unorm:
			return 1;
		default:
			return 0;
		}
	}

	// TODO(excise): level 5; the block size of one host compressed format.  A
	// compressed readback is sized in blocks, not in pixels, so it cannot use the
	// scalar path above.
	u32 diagnostic_block_size(MTL::PixelFormat format, u32& edge)
	{
		switch (format)
		{
		case MTL::PixelFormat::PixelFormatBC1_RGBA:
		case MTL::PixelFormat::PixelFormatBC1_RGBA_sRGB:
			edge = 4;
			return 8;
		case MTL::PixelFormat::PixelFormatBC2_RGBA:
		case MTL::PixelFormat::PixelFormatBC2_RGBA_sRGB:
		case MTL::PixelFormat::PixelFormatBC3_RGBA:
		case MTL::PixelFormat::PixelFormatBC3_RGBA_sRGB:
			edge = 4;
			return 16;
		default:
			edge = 0;
			return 0;
		}
	}

	void dump_host_image(command_recorder& recorder, resource_manager& resources, MTL::Texture* image,
		const char* label, u32 serial, u32 address)
	{
		if (!image || !probe::at_least(5))
		{
			return;
		}

		const u32 width = static_cast<u32>(image->width());
		const u32 height = static_cast<u32>(image->height());
		const MTL::PixelFormat format = image->pixelFormat();
		const u32 pixel_size = diagnostic_pixel_size(format);
		u32 block_edge = 0;
		const u32 block_size = pixel_size ? 0u : diagnostic_block_size(format, block_edge);

		if (!width || !height || (!pixel_size && !block_size) || width > 4096 || height > 4096)
		{
			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: host image %s %u skipped, %ux%u format 0x%x",
				label, serial, width, height, static_cast<u32>(format));
			return;
		}

		// A blit needs an aligned row, so the rows are padded and the hash runs over
		// the row bytes only.  Padding is not initialised, and hashing it would make
		// an unchanged image look changed.  A compressed image is sized in blocks,
		// but the copy size stays in pixels because that is Metal's convention.
		const u32 row_units = pixel_size ? width : (width + block_edge - 1) / block_edge;
		const u32 row_bytes = row_units * (pixel_size ? pixel_size : block_size);
		const u32 row_count = pixel_size ? height : (height + block_edge - 1) / block_edge;
		const u32 row_pitch = utils::align(row_bytes, 0x100);
		const u64 length = static_cast<u64>(row_pitch) * row_count;
		MTL::Buffer* buffer = resources.new_buffer(utils::align(length, 0x1000), MTL::StorageModeShared);

		if (!buffer)
		{
			return;
		}

		resources.make_persistent(buffer);

		MTL4::ComputeCommandEncoder* encoder = recorder.open_compute();

		if (!encoder)
		{
			resources.defer_release(buffer);
			return;
		}

		emit_queue_barrier(encoder, barrier_spec{ MTL::StageAll, MTL::StageBlit, MTL4::VisibilityOptionDevice, true });
		encoder->copyFromTexture(image, 0, 0, MTL::Origin::Make(0, 0, 0), MTL::Size::Make(width, height, 1),
			buffer, 0, row_pitch, row_pitch * row_count);
		recorder.close_encoder();

		// The copy must be observable, so the frame commits early and waits.
		recorder.flush_for_readback();

		const u8* bytes = static_cast<const u8*>(buffer->contents());
		u64 hash = 1469598103934665603ull;
		u32 nonzero = 0;
		u32 words = 0;
		u32 distinct = 0;
		u32 first[4] = {};

		if (bytes)
		{
			words = row_bytes / 4u;

			for (u32 y = 0; y < row_count; ++y)
			{
				const u8* row = bytes + static_cast<u64>(y) * row_pitch;
				const u32* data = reinterpret_cast<const u32*>(row);

				for (u32 i = 0; i < words; ++i)
				{
					nonzero += data[i] ? 1u : 0u;
					hash = (hash ^ data[i]) * 1099511628211ull;
				}

				if (!y)
				{
					for (u32 i = 0; i < 4 && i < words; ++i)
					{
						first[i] = data[i];
					}
				}
			}

			// The distinct byte count separates a flat image from one with content.
			// A flat image is what a stale, zeroed, or never-refreshed copy looks
			// like, and that is the question this readback exists to answer.
			u8 seen_bytes[256] = {};

			for (u32 y = 0; y < row_count; ++y)
			{
				const u8* row = bytes + static_cast<u64>(y) * row_pitch;

				for (u32 i = 0; i < row_bytes; ++i)
				{
					if (!seen_bytes[row[i]])
					{
						seen_bytes[row[i]] = 1;
						distinct++;
					}
				}
			}

			char path[192];
			std::snprintf(path, sizeof(path), "/tmp/mtl4-dump/host-%s-%u-addr%x-%ux%u-fmt%x.bin",
				label, serial, address, width, height, static_cast<u32>(format));

			if (std::FILE* out = std::fopen(path, "wb"))
			{
				std::fwrite(bytes, 1, length, out);
				std::fclose(out);
			}
		}

		// TODO(excise): level 5; sampled byte dump.
		MTL_NOTE(5, "Metal 4: host image %s %u, address 0x%x, %ux%u format 0x%x, block %u/%u, rows %u, row bytes %u, hash 0x%llx, nonzero %u of %u words, distinct bytes %u, first %08x %08x %08x %08x",
			label, serial, address, width, height, static_cast<u32>(format),
			block_edge, block_size, row_count, row_bytes,
			static_cast<unsigned long long>(hash), nonzero, words, distinct, first[0], first[1], first[2], first[3]);

		resources.defer_release(buffer);
	}
#endif // MTL_DEBUG_LOGS

	void MTLGSRender::load_texture_env()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "textures/resolve");
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;

		// The stencil half of a redirected depth unit samples through its own
		// sampler (VKDraw.cpp:707-717), shared by every unit of the clause.
		MTL::SamplerState* stencil_sampler = frame.samplers.get(stencil_mirror_sampler());
		frame.fragment_stencil = {};

#if MTL_DEBUG_LOGS
		// The per-unit trace runs near once per draw per unit, so a bounded prefix of
		// the run carries it and the counters carry the rest (anti-flood rule).
		static u32 diagnostic_unit_traces = 0;
#endif

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

			// A disabled unit must not keep the image of the previous draw.  The
			// argument writer clears the table slot when the bind becomes null (B3).
			if (!tex.enabled())
			{
				// TODO(excise): level 2; a referenced unit was disabled.
				MTL_PROBE_LATCH(2, "texture/disabled");
				frame.fragment_textures[i] = {};
				frame.fragment_stencil[i] = {};
				continue;
			}

			const rsx::format_class previous_class = descriptor->format_class;
			const bool dirty = m_textures_dirty[i];
			m_textures_dirty[i] = false;
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;

				// TODO(excise): level 2; the format of an uploaded fragment texture.
				// An upload is far rarer than a draw, so the key is stable enough.
				MTL_PROBE_HISTOGRAM(2, "texture format",
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN), "gcm texture format");

				// TODO(excise): level 2; the host format the guest format resolved to,
				// and the context the upload came from.  A blit-engine upload is the
				// FMV/video path, which is one of the reported black screens.
				MTL_PROBE_HISTOGRAM(2, "texture host format",
					static_cast<u32>(mtl::get_texture_format(
						static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
						frame.resources.supports_bc_texture_compression())),
					"metal texture format");
				MTL_PROBE_HISTOGRAM(2, "texture upload context",
					static_cast<u32>(descriptor->upload_context), "upload context");

#if MTL_DEBUG_LOGS
				// TODO(excise): level 3; the first uploads of the run only.
				if (diagnostic_unit_traces++ < 16)
				{
					MTL_NOTE(3, "Metal 4: resolve fragment unit %u, address 0x%x, format 0x%x, %ux%u, dirty %u, class %u, ctx %u",
						i, rsx::get_address(tex.offset(), tex.location()), static_cast<u32>(tex.format()),
						static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), dirty ? 1u : 0u,
						static_cast<u32>(descriptor->format_class), static_cast<u32>(descriptor->upload_context));
				}
#endif // MTL_DEBUG_LOGS
			}

			if (!descriptor->validate())
			{
				// TODO(excise): level 2; a bound fragment texture that failed to
				// resolve, so the draw samples a null slot.
				MTL_PROBE_LATCH(2, "texture/bind null fragment");
				frame.fragment_textures[i] = {};
				continue;
			}

			descriptor->format_ex = tex.format_ex();

			if (descriptor->format_class != previous_class)
			{
				// The host image changed class under the program's feet.
				m_graphics_state |= rsx::fragment_program_state_dirty;
			}

			bool deferred = false;
			frame.fragment_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_fragment_sampler(static_cast<u8>(i))), deferred);

			// TODO(excise): level 2; the sampled unit, counted rather than printed.
			if (resampled)
			{
				MTL_PROBE_LATCH(2, "texture/resolved");
			}
			else
			{
				MTL_PROBE_LATCH(2, "texture/retained");
			}

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; one capture frame reaches the race units that
			// the bounded prefix of the whole run never reaches.
			if (diagnostic_capture_active())
			{
				const u32 guest_address = rsx::get_address(tex.offset(), tex.location());
				const auto& params = current_fragment_program.texture_params[i];
				const sampler_state state = capture_fragment_sampler(static_cast<u8>(i));

				MTL_NOTE(5, "Metal 4: capture frame %llu unit %u, guest 0x%x ref 0x%x, raw 0x%x gcm 0x%x %ux%u pitch %u mips %u, ctx %u class %u, image %p %ux%u fmt 0x%x, sampler wrap %u/%u/%u filter %u/%u unnorm %u lod %.3f..%.3f bias %.3f aniso %u border %u compare %u/%u nearest %u, params scale %.4f %.4f %.4f bias %.4f %.4f %.4f clamp %.4f %.4f %.4f %.4f remap 0x%x control 0x%x",
					static_cast<unsigned long long>(diagnostic_frame_index), i, guest_address, descriptor->ref_address,
					static_cast<u32>(tex.format()),
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), tex.pitch(),
					static_cast<u32>(tex.get_exact_mipmap_count()),
					static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
					static_cast<const void*>(descriptor->image_handle),
					descriptor->image_handle ? descriptor->image_handle->width() : 0u,
					descriptor->image_handle ? descriptor->image_handle->height() : 0u,
					descriptor->image_handle ? static_cast<u32>(descriptor->image_handle->pixel_format()) : 0u,
					static_cast<u32>(state.wrap_s), static_cast<u32>(state.wrap_t), static_cast<u32>(state.wrap_r),
					static_cast<u32>(state.min_filter), static_cast<u32>(state.mag_filter),
					state.unnormalized_coords ? 1u : 0u,
					static_cast<f64>(state.min_lod), static_cast<f64>(state.max_lod), static_cast<f64>(state.lod_bias),
					state.max_aniso, static_cast<u32>(state.border_color),
					state.depth_compare ? 1u : 0u, static_cast<u32>(state.compare), state.force_nearest ? 1u : 0u,
					static_cast<f64>(params.scale[0]), static_cast<f64>(params.scale[1]), static_cast<f64>(params.scale[2]),
					static_cast<f64>(params.bias[0]), static_cast<f64>(params.bias[1]), static_cast<f64>(params.bias[2]),
					static_cast<f64>(params.clamp_min[0]), static_cast<f64>(params.clamp_min[1]),
					static_cast<f64>(params.clamp_max[0]), static_cast<f64>(params.clamp_max[1]),
					params.remap, params.control);

				const u32 capture_address = descriptor->ref_address ? descriptor->ref_address : guest_address;

				if (descriptor->image_handle && !diagnostic_capture_address_seen(capture_address) &&
					diagnostic_capture_address_count < diagnostic_capture_address_limit)
				{
					diagnostic_capture_addresses[diagnostic_capture_address_count++] = capture_address;
					dump_host_image(frame.recorder, frame.resources, descriptor->image_handle->value(),
						"sample", static_cast<u32>(diagnostic_frame_index * 100u + i), capture_address);
				}
			}

			// TODO(excise): level 3; the first sampled units of the run only.
			if (diagnostic_unit_traces++ < 16)
			{
				MTL_NOTE(3, "Metal 4: fragment unit %u reads 0x%x, gcm 0x%x, %ux%u, pitch %u, ctx %u, class %u, ref 0x%x, %s, image %p %ux%u fmt 0x%x",
					i, rsx::get_address(tex.offset(), tex.location()),
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), tex.pitch(),
					static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
					descriptor->ref_address, resampled ? "resolved" : "retained",
					static_cast<const void*>(descriptor->image_handle),
					descriptor->image_handle ? descriptor->image_handle->width() : 0u,
					descriptor->image_handle ? descriptor->image_handle->height() : 0u,
					descriptor->image_handle ? static_cast<u32>(descriptor->image_handle->pixel_format()) : 0u);
			}
#endif // MTL_DEBUG_LOGS

			// The transfer that filled an uploaded image is a write, and the
			// scheduler orders only the edges it holds a write record for, so the
			// read this draw makes needs that record (HLD-03 §2.4).
			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.fragment_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.fragment_textures[i].texture,
					access_stages(rsx::surface_access::transfer_write, false));
			}

			// A redirected depth unit also reads the stencil half of its surface,
			// through a castable view (VKDraw.cpp:702-721).  The view is the surface
			// itself when the surface carries no stencil, and the slot then stays
			// unbound.
			if ((current_fragment_program.texture_state.redirected_textures & (1u << i)) &&
				stencil_sampler && descriptor->image_handle)
			{
				if (texture* stencil = descriptor->image_handle->stencil_view())
				{
					frame.fragment_stencil[i] = { stencil->resource_id(), stencil_sampler->gpuResourceID() };
				}
			}
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

			// A disabled unit binds null, not the image from the previous draw (B4).
			if (!tex.enabled())
			{
				// TODO(excise): level 2; a referenced unit was disabled.
				MTL_PROBE_LATCH(2, "texture/disabled");
				frame.vertex_textures[i] = {};
				continue;
			}

			const bool dirty = m_vertex_textures_dirty[i];
			m_vertex_textures_dirty[i] = false;
			bool resampled = false;

			if (dirty || frame.textures.test_if_descriptor_expired(frame.recorder, frame.cache, descriptor, tex))
			{
				*descriptor = frame.textures.upload_texture(frame.recorder, tex, frame.cache);
				resampled = true;

				// TODO(excise): level 2; the guest and host formats of an uploaded
				// vertex texture, and the upload context.
				MTL_PROBE_HISTOGRAM(2, "texture format",
					static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN), "gcm texture format");
				MTL_PROBE_HISTOGRAM(2, "texture host format",
					static_cast<u32>(mtl::get_texture_format(
						static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
						frame.resources.supports_bc_texture_compression())),
					"metal texture format");
				MTL_PROBE_HISTOGRAM(2, "texture upload context",
					static_cast<u32>(descriptor->upload_context), "upload context");

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: resolve vertex unit %u, address 0x%x, format 0x%x, %ux%u, dirty %u, ctx %u",
					i, rsx::get_address(tex.offset(), tex.location()), static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), dirty ? 1u : 0u,
					static_cast<u32>(descriptor->upload_context));
			}

			if (!descriptor->validate())
			{
				// TODO(excise): level 2; a bound vertex texture that failed to
				// resolve, so the draw samples a null slot.
				MTL_PROBE_LATCH(2, "texture/bind null vertex");
				frame.vertex_textures[i] = {};
				continue;
			}

			bool deferred = false;
			frame.vertex_textures[i] = frame.textures.resolve_texture_bind(
				frame.recorder, descriptor,
				frame.samplers.get(capture_vertex_sampler(static_cast<u8>(i))), deferred);

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: vertex unit %u reads 0x%x, gcm 0x%x, %ux%u, ctx %u, class %u, %s, image %p",
				i, rsx::get_address(tex.offset(), tex.location()),
				static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN),
				static_cast<u32>(tex.width()), static_cast<u32>(tex.height()),
				static_cast<u32>(descriptor->upload_context), static_cast<u32>(descriptor->format_class),
				resampled ? "resolved" : "retained", static_cast<const void*>(descriptor->image_handle));

			if (((resampled && descriptor->upload_context == rsx::texture_upload_context::shader_read) || deferred) &&
				frame.vertex_textures[i].texture._impl)
			{
				frame.scheduler.note_write(frame.vertex_textures[i].texture,
					access_stages(rsx::surface_access::transfer_write, false));
			}
		}

		m_frame_stats.textures_upload_time += m_profiler.duration();
	}

	void MTLGSRender::record_pass_hazards(bool sample_textures)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "pass/hazards");
		frame_resources& frame = *m_frame_resources;
		pass_scheduler& scheduler = frame.scheduler;

		scheduler.begin_pass();

		// The read set is the units the programs reference, not the whole resolved
		// array: an unreferenced unit still holds the previous clause's image, and a
		// barrier for it would consume a write record the frame still needs.
		if (sample_textures)
		{
			for (u32 referenced = current_fp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.fragment_textures[i].texture._impl)
				{
					scheduler.note_read(frame.fragment_textures[i].texture, access_stages(rsx::surface_access::shader_read, false));
				}
			}

			for (u32 referenced = current_vp_metadata.referenced_textures_mask, i = 0; referenced; referenced >>= 1, ++i)
			{
				if ((referenced & 1) && frame.vertex_textures[i].texture._impl)
				{
					scheduler.note_read(frame.vertex_textures[i].texture, access_stages(rsx::surface_access::shader_read, true));
				}
			}
		}

		// The attachment set is the pass's write set.
		for (u32 index = 0; index < frame.attachments.color_count(); ++index)
		{
			scheduler.note_write(frame.attachments.color(index)->resource_id(), access_stages(rsx::surface_access::shader_write, false));
		}

		if (frame.attachments.has_depth())
		{
			scheduler.note_write(frame.attachments.depth()->resource_id(), access_stages(rsx::surface_access::shader_write, false));
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
	void MTLGSRender::end()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "frame/end");

		// Publish a link that completed since the previous clause.  This is the
		// only drain point inside the draw loop, and it does not wait.
		if (m_frame_resources)
		{
			m_frame_resources->jobs.advance();
		}

		static u32 diagnostic_skip_count = 0;
		static bool diagnostic_bind_reported = false;
		static bool diagnostic_program_reported = false;
		static bool diagnostic_pipeline_reported = false;
		static bool diagnostic_pass_reported = false;

		if (should_skip_draw())
		{
			// TODO(excise): level 2; the first 64 refused draws.
			// The predicate has four independent causes; a bare latch cannot say
			// which one fires, and the log cannot be replayed.
			if (mtl::probe::at_least(2) && diagnostic_skip_count < 64)
			{
				MTL_NOTE(2, "Metal 4: should_skip_draw refuses the draw: skip frame %u, config valid %u, conditional render disabled %u, no attachments %u",
					skip_current_frame ? 1u : 0u,
					m_graphics_state.test(rsx::rtt_config_valid) ? 1u : 0u,
					cond_render_ctrl.disable_rendering() ? 1u : 0u,
					m_graphics_state.test(rsx::rtt_config_no_attachments) ? 1u : 0u);

				diagnostic_skip_count++;
			}

			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		if (!bind_framebuffer(rsx::framebuffer_creation_context::context_draw))
		{
			if (!diagnostic_bind_reported)
			{
				diagnostic_bind_reported = true;
				rsx_log.error("Metal 4: bind_framebuffer refuses the draw");
			}

			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		frame_resources& frame = *m_frame_resources;
		auto& draw_call = rsx::method_registers.current_draw_clause;

		static u32 diagnostic_draws = 0;

		// The programs of this draw.  analyse_current_rsx_pipeline fills the
		// metadata (RSXThread.cpp:2318).  The texture units resolve first, because
		// the program builders read the resolved descriptors (RSXThread.cpp:2488).
		analyse_current_rsx_pipeline();
		load_texture_env();

		// TODO(excise): level 2; the draw stream is counted and classified here.
		MTL_PROBE_LATCH(2, "draw/record");
		MTL_PROBE_HISTOGRAM(2, "draw primitive", static_cast<u32>(draw_call.primitive), "rsx primitive");

#if MTL_DEBUG_LOGS
		// The per-draw trace is the largest trace source of the backend, so it prints
		// for a bounded prefix of the run and never again.  The counters and the
		// histograms above carry the rest of the draw stream (anti-flood rule).
		if (diagnostic_draws < 16)
		{
			char units[512] = {};
			u32 units_len = 0;

			for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
			{
				const auto& tex = rsx::method_registers.fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(tex.offset(), tex.location());
				const bool is_target =
					guest_address == m_framebuffer_layout.color_addresses[0] ||
					guest_address == m_framebuffer_layout.color_addresses[1] ||
					guest_address == m_framebuffer_layout.color_addresses[2] ||
					guest_address == m_framebuffer_layout.color_addresses[3] ||
					guest_address == m_framebuffer_layout.zeta_address;

				const u32 remaining = (units_len < sizeof(units)) ? static_cast<u32>(sizeof(units) - units_len) : 0u;

				if (!remaining)
				{
					break;
				}

				const int written = std::snprintf(units + units_len, remaining,
					" u%u=0x%x/%x/%ux%u%s", unit, guest_address, static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), is_target ? "/target" : "");

				if (written <= 0)
				{
					break;
				}

				units_len += static_cast<u32>(written);

				if (units_len >= sizeof(units) - 64)
				{
					break;
				}
			}

			// TODO(excise): level 3; the first draws of the run.
			MTL_NOTE(3, "Metal 4: draw %u, command %u, primitive %u, inline %u, color 0x%x/0x%x/0x%x/0x%x fmt 0x%x, depth 0x%x fmt 0x%x, %ux%u, aa_mode %u, scissor (%u,%u,%u,%u), samples:%s",
				diagnostic_draws, static_cast<u32>(draw_call.command), static_cast<u32>(draw_call.primitive),
				::size32(draw_call.inline_vertex_array),
				m_framebuffer_layout.color_addresses[0], m_framebuffer_layout.color_addresses[1],
				m_framebuffer_layout.color_addresses[2], m_framebuffer_layout.color_addresses[3],
				static_cast<u32>(m_framebuffer_layout.color_format),
				m_framebuffer_layout.zeta_address, static_cast<u32>(m_framebuffer_layout.depth_format),
				static_cast<u32>(m_framebuffer_layout.width), static_cast<u32>(m_framebuffer_layout.height),
				static_cast<u32>(m_framebuffer_layout.aa_mode),
				m_scissor.x1, m_scissor.y1, m_scissor.width(), m_scissor.height(),
				units_len ? units : " none");
		}
#endif // MTL_DEBUG_LOGS

		if (diagnostic_draws == 0)
		{
			// TODO(excise): level 3; the texture barrier count of the first draw.
			MTL_NOTE(3, "Metal 4: stats at draw %u, texture barriers %u",
				diagnostic_draws, frame.textures.diagnostic_texture_barriers);
		}

		diagnostic_draws++;

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the texture units of the first draws, with a scan.
		if (diagnostic_draws < 24)
		{
			for (u32 unit = 0; unit < frame.fragment_textures.size(); ++unit)
			{
				const auto& tex = rsx::method_registers.fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(tex.offset(), tex.location());
				u32 nonzero = 0;

				for (u32 word = 0; word < 1024; ++word)
				{
					nonzero += (vm::read32(guest_address + word * 4) != 0u);
				}

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: unit %u, address 0x%x, location %u, format 0x%x, %ux%u, nonzero %u/1024",
					unit, guest_address, static_cast<u32>(tex.location()), static_cast<u32>(tex.format()),
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), nonzero);
			}
		}

		if (diagnostic_draws % 30 == 0)
		{
			static u32 diagnostic_samples[64] = {};
			static u32 diagnostic_sample_count = 0;

			for (u32 unit = 0; unit < frame.fragment_textures.size() && diagnostic_sample_count < 64; ++unit)
			{
				const auto& tex = rsx::method_registers.fragment_textures[unit];

				if (!tex.enabled())
				{
					continue;
				}

				const u32 guest_address = rsx::get_address(tex.offset(), tex.location());
				const u32 gcm_format = static_cast<u32>(tex.format()) & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
				const u32 block = rsx::get_format_block_size_in_bytes(gcm_format);
				// A compressed format stores a 4x4 block in `block` bytes, so the
				// extent is in blocks and the byte span is block_count * block_size.
				// Multiplying the texel extent by the block size over-reads by 16x.
				const u32 block_texels = rsx::get_format_block_size_in_texel(gcm_format);
				const u32 width_in_blocks = (static_cast<u32>(tex.width()) + block_texels - 1) / block_texels;
				const u32 height_in_blocks = (static_cast<u32>(tex.height()) + block_texels - 1) / block_texels;
				const u32 length = width_in_blocks * height_in_blocks * block;

				if (!guest_address || !block || !length)
				{
					continue;
				}

				if (fs_sampler_state[unit] && diagnostic_sampled_count <= 8)
				{
					auto* descriptor = static_cast<texture_cache::sampled_image_descriptor*>(fs_sampler_state[unit].get());

					if (descriptor && descriptor->image_handle)
					{
						u32 slot = diagnostic_sampled_count;

						for (u32 i = 0; i < diagnostic_sampled_count; ++i)
						{
							if (diagnostic_sampled_address[i] == guest_address)
							{
								slot = i;
								break;
							}
						}

						if (slot == diagnostic_sampled_count && diagnostic_sampled_count < 8)
						{
							diagnostic_sampled_count++;
						}

						if (slot < 8)
						{
							diagnostic_sampled_address[slot] = guest_address;
							diagnostic_sampled_image[slot] = descriptor->image_handle->value();
							diagnostic_sampled_context[slot] = static_cast<u32>(descriptor->upload_context);
						}
					}
				}

				bool seen = false;

				for (u32 i = 0; i < diagnostic_sample_count; ++i)
				{
					seen = seen || (diagnostic_samples[i] == guest_address);
				}

				if (seen)
				{
					continue;
				}

				diagnostic_samples[diagnostic_sample_count++] = guest_address;

				// The whole range is scanned, not just its head: a readable head says
				// nothing about the texels the shader actually reads.
				const u32 total_words = length / 4u;
				const u32 stride = std::max<u32>(1u, total_words / 8192u);
				u32 nonzero = 0;
				u32 scanned = 0;

				for (u32 word = 0; word < total_words; word += stride)
				{
					nonzero += (vm::read32(guest_address + word * 4u) != 0u);
					scanned++;
				}

				char path[160];
				std::snprintf(path, sizeof(path), "/tmp/mtl4-dump/sample-%u-draw%u-addr%x-fmt%x-%ux%u.bin",
					diagnostic_sample_count, diagnostic_draws, guest_address, gcm_format,
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()));

				if (std::FILE* out = std::fopen(path, "wb"))
				{
					std::fwrite(vm::_ptr<const std::byte>(guest_address), 1, length, out);
					std::fclose(out);
				}

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: sample %u at draw %u, address 0x%x, format 0x%x, %ux%u, bytes %u, nonzero %u of %u scanned (stride %u)",
					diagnostic_sample_count, diagnostic_draws, guest_address, gcm_format,
					static_cast<u32>(tex.width()), static_cast<u32>(tex.height()), length, nonzero, scanned, stride);

				// The region descriptor uses the effective pitch: a swizzled texture
				// has no guest pitch, and upload_texture substitutes the packed pitch
				// (texture_cache.h:3110-3127).  The width is expressed in block units
				// so the byte span is exact for a compressed format too.
				const bool swizzled = !(tex.format() & CELL_GCM_TEXTURE_LN);
				const u32 packed_pitch = rsx::get_format_packed_pitch(gcm_format,
					static_cast<u16>(tex.width()), !tex.border_type(), swizzled);
				const u32 effective_pitch = swizzled
					? packed_pitch
					: (tex.pitch() ? tex.pitch() : packed_pitch);

				const auto overlaps = frame.cache.get_merged_texture_memory_region(frame.recorder,
					guest_address, width_in_blocks, height_in_blocks,
					effective_pitch, static_cast<u8>(block), rsx::surface_access::shader_read);

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: store lookup for 0x%x, bound %u, overlaps %u",
					guest_address, frame.cache.address_is_bound(guest_address) ? 1u : 0u,
					static_cast<u32>(overlaps.size()));

				for (u32 slot = 0; slot < overlaps.size(); ++slot)
				{
					const auto& overlap = overlaps[slot];
					auto* surface = overlap.surface;

					if (!surface)
					{
						continue;
					}

					// The entry the search returned, with the state test that decides
					// whether the sampler is allowed to use it.
					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: overlap %u for 0x%x, base 0x%x, %ux%u pitch %u bpp %u, depth %u, clipped %u, reloaded %u, test %u, needs upload %u, write through %u, src %u,%u %ux%u, dst %u,%u %ux%u",
						slot, guest_address, overlap.base_address,
						static_cast<u32>(surface->width()), static_cast<u32>(surface->height()),
						surface->get_rsx_pitch(), surface->get_bpp(),
						overlap.is_depth ? 1u : 0u, overlap.is_clipped ? 1u : 0u, overlap.is_reloaded ? 1u : 0u,
						surface->test() ? 1u : 0u, surface->needs_cpu_upload() ? 1u : 0u, surface->write_through() ? 1u : 0u,
						static_cast<u32>(overlap.src_area.x), static_cast<u32>(overlap.src_area.y),
						static_cast<u32>(overlap.src_area.width), static_cast<u32>(overlap.src_area.height),
						static_cast<u32>(overlap.dst_area.x), static_cast<u32>(overlap.dst_area.y),
						static_cast<u32>(overlap.dst_area.width), static_cast<u32>(overlap.dst_area.height));
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		get_current_vertex_program(vs_sampler_state);
		get_current_fragment_program(fs_sampler_state);
		if (current_vertex_program.data.empty() || !current_fragment_program.ucode_length)
		{
			if (!diagnostic_program_reported)
			{
				diagnostic_program_reported = true;
				rsx_log.error("Metal 4: the draw has no program (vertex bytes %u, fragment words %u)",
					static_cast<u32>(current_vertex_program.data.size()), current_fragment_program.ucode_length);
			}

			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		const surface_state surface = capture_surface_state();
		pipeline_properties properties = capture_pipeline_properties(surface, current_vertex_program);

		// TODO(excise): level 2; the classified draw state, one row per draw.
		MTL_PROBE_HISTOGRAM(2, "draw topology", static_cast<u32>(properties.input_primitive_topology), "topology");
		MTL_PROBE_HISTOGRAM(2, "draw sample count", properties.sample_count, "samples");
		MTL_PROBE_HISTOGRAM(2, "draw color format", static_cast<u32>(surface.color_format), "gcm color format");
		MTL_PROBE_HISTOGRAM(2, "draw depth format", static_cast<u32>(surface.depth_format), "gcm depth format");
		MTL_PROBE_HISTOGRAM(2, "draw write mask", static_cast<u32>(properties.write_mask[0]), "write mask 0");
		// TODO(excise): level 3; the programs of this draw.  A program changes more
		// often than a format does, so the lookup stays off the level-2 path, which
		// is the performance path.
		MTL_PROBE_HISTOGRAM(3, "draw vertex program", current_vp_metadata.ucode_hash, "vertex ucode hash");
		MTL_PROBE_HISTOGRAM(3, "draw fragment program", current_fp_metadata.ucode_hash, "fragment ucode hash");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the draw-state block, for the first draws and the
		// capture frame.
		if (diagnostic_draws < 9 || diagnostic_capture_active())
		{
			dump_draw_state(diagnostic_draws - 1, surface, properties, frame.attachments, m_framebuffer_layout, m_scissor,
				frame.resources.supports_bc_texture_compression());
		}
#endif // MTL_DEBUG_LOGS

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
			interpreter_cache::build_request(current_vertex_program, current_fragment_program,
				current_vp_metadata, current_fp_metadata, interpreted_request))
		{
			// The interpreter declares only the outputs it writes.  The real
			// pipeline still sees the unmodified mask, so the trim is a copy.
			pipeline_properties interpreter_properties = properties;

			for (u32 index = interpreted_request.output_count; index < interpreter_properties.write_mask.size(); ++index)
			{
				interpreter_properties.write_mask[index] = MTL::ColorWriteMaskNone;
			}

			interpreted = frame.interpreter.get(interpreted_request, interpreter_properties);

			// TODO(excise): level 2; the source is eligible but its pipeline is not
			// ready yet, so this draw takes the real-pipeline path or the skip.
			if (!interpreted)
			{
				MTL_PROBE_LATCH(2, "interpreter/pipeline not ready");
			}
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
				&m_program_cache_hint,
				current_vertex_program,
				current_fragment_program,
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
				const bool failed = compiled_vertex && compiled_fragment &&
					frame.jobs.permanently_failed(compiled_vertex->id, compiled_fragment->id, properties);

				// TODO(excise): level 2; distinguish an in-flight link from a failed one.
				if (failed)
				{
					MTL_PROBE_LATCH(2, "draw/skipped pipeline failed");
				}
				else
				{
					MTL_PROBE_LATCH(2, "draw/skipped pipeline pending");
				}

				if (failed && !diagnostic_pipeline_reported)
				{
					diagnostic_pipeline_reported = true;
					rsx_log.error("Metal 4: the program cache returned no pipeline");
				}

				// The host miss contract yields once; the link is on Metal's compiler
				// threads.  Do not drain here: the next end()/begin() publishes.
				std::this_thread::yield();
				execute_nop_draw();
				rsx::thread::end();
				return;
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

		// TODO(excise): level 2; which cover this draw used.  One latch per branch,
		// because a latch registers its name once per call site.
		if (use_interpreter)
		{
			MTL_PROBE_LATCH(2, "draw/interpreter");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/recompiler");
		}

		if (use_interpreter)
		{
			frame.compiled_vertex = nullptr;
			frame.compiled_fragment = nullptr;
			frame.interpreter_mapping = interpreted->mapping;
			frame.interpreter_output_count = interpreted->output_count;
			frame.interpreter_texture_count = 0;
			for (u32 unit = 0; unit < rsx::limits::fragment_textures_count; ++unit)
			{
				if (current_fp_metadata.referenced_textures_mask & (1u << unit))
				{
					frame.interpreter_texture_count = unit + 1;
				}
			}

			// The instruction blocks of this clause.  A block is 16 header bytes
			// followed by the guest ucode; the header carries the guest addresses
			// and the per-clause state the body switches on
			// (VKShaderInterpreter.cpp:2255-2293).
			byte_ring& ring = frame.constants.instruction_ring();

			const u64 vertex_key = static_cast<u64>(current_vp_metadata.ucode_hash) ^
				(static_cast<u64>(current_vertex_program.base_address) << 32) ^
				(static_cast<u64>(current_vertex_program.entry) << 16) ^
				static_cast<u64>(current_vertex_program.output_mask) ^
				(static_cast<u64>(current_vp_metadata.ucode_length) << 48);

			if (auto found = frame.interpreter_vertex_blocks.find(vertex_key); found != frame.interpreter_vertex_blocks.end())
			{
				frame.interpreter_vertex_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + current_vp_metadata.ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter vertex instruction ring exhausted");
					execute_nop_draw();
					rsx::thread::end();
					return;
				}

				u32* header = static_cast<u32*>(block);
				header[0] = current_vertex_program.base_address;
				header[1] = current_vertex_program.entry;
				header[2] = current_vertex_program.output_mask;
				header[3] = rsx::method_registers.two_side_light_en() ? 1u : 0u;
				std::memcpy(static_cast<std::byte*>(block) + 16, current_vertex_program.data.data(), current_vp_metadata.ucode_length);

				frame.interpreter_vertex_instructions = ring.address_of(offset);
				frame.interpreter_vertex_blocks.emplace(vertex_key, frame.interpreter_vertex_instructions);
			}

			const u64 fragment_key = static_cast<u64>(current_fp_metadata.ucode_hash) ^
				(static_cast<u64>(rsx::method_registers.shader_control()) << 32) ^
				(static_cast<u64>(current_fragment_program.texture_state.texture_dimensions) << 48) ^
				(static_cast<u64>(current_fp_metadata.program_ucode_length) << 24);

			if (auto found = frame.interpreter_fragment_blocks.find(fragment_key); found != frame.interpreter_fragment_blocks.end())
			{
				frame.interpreter_fragment_instructions = found->second;
			}
			else
			{
				const u64 block_size = 16 + current_fp_metadata.program_ucode_length;
				u32 offset = 0;
				void* block = ring.alloc(block_size, offset, 256);

				if (!block)
				{
					rsx_log.error("Metal: interpreter fragment instruction ring exhausted");
					execute_nop_draw();
					rsx::thread::end();
					return;
				}

				u32 resident = 0;

				for (u32 unit = 0; unit < 16; ++unit)
				{
					if ((current_fp_metadata.referenced_textures_mask & (1u << unit)) &&
						frame.fragment_textures[unit].texture._impl)
					{
						resident |= 1u << unit;
					}
				}

				u32* header = static_cast<u32*>(block);
				header[0] = rsx::method_registers.shader_control();
				header[1] = current_fragment_program.texture_state.texture_dimensions;
				header[2] = resident;
				header[3] = 0;
				std::memcpy(static_cast<std::byte*>(block) + 16, current_fragment_program.get_data(), current_fp_metadata.program_ucode_length);

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
		if ((current_fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE) && frame.attachments.has_depth())
		{
			const MTL::ResourceID mirror = frame.attachments.depth()->mirror_depth(frame.recorder);
			MTL::SamplerState* sampler = frame.samplers.get(stencil_mirror_sampler());
			if (sampler && mirror._impl)
			{
				frame.depth_input = { mirror, sampler->gpuResourceID() };
			}
		}

		MTL4::RenderCommandEncoder* encoder = frame.attachments.open_pass(frame.recorder, 0, surface);
		if (!encoder)
		{
			if (!diagnostic_pass_reported)
			{
				diagnostic_pass_reported = true;
				rsx_log.error("Metal 4: the pass of the draw could not open");
			}

			// Every other failure path of this function ends in a NOP draw, and the
			// shared draw bookkeeping runs the same way (VKDraw.cpp:1283-1288).
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		frame.encoder = encoder;
		record_pass_hazards(true);

		// The mirror copy ran in the encoder that open_pass closed, so the pass reads
		// the mirror across a queue-scope edge.
		if (frame.depth_input.texture._impl)
		{
			emit_queue_barrier(encoder, barrier_spec{ MTL::StageBlit, MTL::StageFragment, MTL4::VisibilityOptionDevice, true });
		}

		encoder->setRenderPipelineState(use_interpreter ? interpreted->state : program->state);

		const auto diagnostic_encoder_state = capture_encoder_state(!!(current_fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE));
		MTL::ScissorRect scissor = build_scissor_rect(m_scissor.x1, m_scissor.y1, m_scissor.x1 + m_scissor.width(), m_scissor.y1 + m_scissor.height(), diagnostic_encoder_state.clip_height);
		apply_encoder_state(encoder, diagnostic_encoder_state, scissor, frame.depth_cache);

		// TODO(excise): level 2; the coordinate audit.  The clip size against the
		// attachment, and the raster modes, decide whether geometry lands as the
		// title drew it (area A and area C of the design).
		MTL_PROBE_LATCH(2, diagnostic_encoder_state.clip_width == m_framebuffer_layout.width &&
			diagnostic_encoder_state.clip_height == m_framebuffer_layout.height
			? "audit/clip matches target" : "audit/clip mismatches target");
		MTL_PROBE_HISTOGRAM(2, "audit fill mode", static_cast<u32>(diagnostic_encoder_state.fill_mode), "triangle fill mode");
		MTL_PROBE_HISTOGRAM(2, "audit cull mode", static_cast<u32>(diagnostic_encoder_state.cull_mode), "cull mode");
		MTL_PROBE_HISTOGRAM(2, "audit winding", static_cast<u32>(diagnostic_encoder_state.winding), "winding");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the viewport audit of the first draws of the run.
		static u32 diagnostic_viewports = 0;

		if (diagnostic_viewports++ < 8)
		{
			const MTL::Viewport viewport = build_viewport(diagnostic_encoder_state.clip_width, diagnostic_encoder_state.clip_height,
				diagnostic_encoder_state.has_clip_range ? diagnostic_encoder_state.znear : 0.0,
				diagnostic_encoder_state.has_clip_range ? diagnostic_encoder_state.zfar : 1.0);

			MTL_NOTE(3, "Metal 4: viewport %u, origin %.1f %.1f size %.1f %.1f z %.4f %.4f, scissor %u %u %ux%u, clip %ux%u, cull %u winding %u fill %u depthclip %u",
				diagnostic_viewports, viewport.originX, viewport.originY, viewport.width, viewport.height, viewport.znear, viewport.zfar,
				static_cast<u32>(scissor.x), static_cast<u32>(scissor.y), static_cast<u32>(scissor.width), static_cast<u32>(scissor.height),
				static_cast<u32>(diagnostic_encoder_state.clip_width), static_cast<u32>(diagnostic_encoder_state.clip_height),
				static_cast<u32>(diagnostic_encoder_state.cull_mode), static_cast<u32>(diagnostic_encoder_state.winding),
				static_cast<u32>(diagnostic_encoder_state.fill_mode), static_cast<u32>(diagnostic_encoder_state.depth_clip_mode));
		}
#endif // MTL_DEBUG_LOGS

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

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the target and the resolve readback of the first
		// draws, and a few samples of the capture frame.
		if (diagnostic_draws < 24 || (diagnostic_capture_active() && (diagnostic_draws % 200 == 0)))
		{
			if (auto* target = frame.attachments.color(0))
			{
				dump_host_image(frame.recorder, frame.resources, target->value(), "target", diagnostic_draws, target->base_addr);

				const u32 guest_address = target->base_addr;
				u32 nonzero = 0;

				for (u32 word = 0; word < 1024; ++word)
				{
					nonzero += (vm::read32(guest_address + word * 4) != 0u);
				}

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: target %u guest bytes, address 0x%x, %ux%u, nonzero %u/1024",
					diagnostic_draws, guest_address, static_cast<u32>(target->value()->width()), static_cast<u32>(target->value()->height()), nonzero);

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: target %u, address 0x%x, resolve surface %s",
					diagnostic_draws, guest_address, target->resolve_surface ? "present" : "absent");

				if (target->resolve_surface)
				{
					dump_host_image(frame.recorder, frame.resources, target->resolve_surface->value(), "resolve", diagnostic_draws, guest_address);
				}
			}
		}
#endif // MTL_DEBUG_LOGS

		// The draw wrote the attachments, so the surfaces now hold the newest pixels
		// for their guest ranges.  A later sample resolves to them only if the store
		// says so: its write tag is what wins the tie against a cached copy
		// (texture_cache.h:2680, VKDraw.cpp:1424).
		frame.cache.on_write(m_framebuffer_layout.color_write_enabled, m_framebuffer_layout.zeta_write_enabled);

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

		rsx::thread::end();
	}

	void MTLGSRender::emit_geometry(u32 sub_index)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "draw");
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "draw/subdraws", 1);
		m_profiler.start();
		frame_resources& frame = *m_frame_resources;
		MTL4::RenderCommandEncoder* encoder = frame.encoder;
		auto& draw_call = rsx::method_registers.current_draw_clause;

		// The vertex state of this subdraw (GLDraw.cpp:730-760).
		const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
		const rsx::flags32_t vertex_state = (sub_index == 0)
			? rsx::vertex_arrays_changed
			: (draw_call.execute_pipeline_dependencies(m_ctx) & vertex_state_mask);

#if MTL_DEBUG_LOGS
		// The geometry stream of the run.  Every trace below is a bounded prefix of
		// it and every deep dump is bounded to it (anti-flood rule).
		static u32 diagnostic_geometry = 0;
		static u32 diagnostic_traces = 0;
		const bool diagnostic_trace = (diagnostic_traces < 20) || ((diagnostic_traces % 200) == 0);
		diagnostic_traces++;

		if (diagnostic_geometry < 20)
		{
			MTL_NOTE(3, "Metal 4: geometry %u, subdraw %u, first %u, count %u",
				diagnostic_geometry, sub_index, draw_call.min_index(), draw_call.get_elements_count());
		}

		diagnostic_geometry++;
#endif // MTL_DEBUG_LOGS

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
			// No vertex inputs are enabled.  Nothing reaches the GPU, but the
			// clause's pipeline dependencies still have to run
			// (GLDraw.cpp:755-766).
			execute_nop_draw();
			rsx::thread::end();
			return;
		}

		// The index array and the vertex range of this subdraw
		// (GLVertexBuffers.cpp:66-150).
		const vertex_input_state input = std::visit(
			draw_command_visitor{ frame.constants.index_ring(), frame.vertex_layout },
			GRAPH_frontend().get_draw_command(rsx::method_registers));

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "draw/vertices", input.vertex_draw_count);

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

		m_frame_stats.setup_time += m_profiler.duration();

		if (persistent_data || volatile_data)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "draw/vertex upload");
			GRAPH_frontend().write_vertex_data_to_memory(frame.vertex_layout, vertex_base, vertex_count, persistent_data, volatile_data);
		}

		m_frame_stats.vertex_upload_time += m_profiler.duration();

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the vertex upload of the first subdraws.
		{
			static u32 diagnostic_uploads = 0;

			if (diagnostic_uploads < 12)
			{
				const f32* stream = (persistent_data && required.first >= 24u) ? static_cast<const f32*>(persistent_data) : nullptr;

				MTL_NOTE(5, "Metal 4: upload %u, persistent %llu bytes base %u, volatile %llu bytes base %u, first %.6f %.6f %.6f %.6f %.6f %.6f",
					diagnostic_uploads,
					static_cast<unsigned long long>(required.first), persistent_base,
					static_cast<unsigned long long>(required.second), volatile_base,
					stream ? static_cast<f64>(stream[0]) : 0.0, stream ? static_cast<f64>(stream[1]) : 0.0,
					stream ? static_cast<f64>(stream[2]) : 0.0, stream ? static_cast<f64>(stream[3]) : 0.0,
					stream ? static_cast<f64>(stream[4]) : 0.0, stream ? static_cast<f64>(stream[5]) : 0.0);
			}

			diagnostic_uploads++;
		}
#endif // MTL_DEBUG_LOGS

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

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the transform constants of a bounded prefix.
			if (diagnostic_trace)
			{
				MTL_NOTE(5, "Metal 4: transform constants, index %u, reloc %u, vp base 0x%x, [0] %.4f %.4f %.4f %.4f [1] %.4f %.4f %.4f %.4f [2] %.4f %.4f %.4f %.4f [3] %.4f %.4f %.4f %.4f",
					frame.transform_constants_index, static_cast<u32>(reloc_table.size()), current_vertex_program.base_address,
					static_cast<f64>(block[0].v[0]), static_cast<f64>(block[0].v[1]), static_cast<f64>(block[0].v[2]), static_cast<f64>(block[0].v[3]),
					static_cast<f64>(block[1].v[0]), static_cast<f64>(block[1].v[1]), static_cast<f64>(block[1].v[2]), static_cast<f64>(block[1].v[3]),
					static_cast<f64>(block[2].v[0]), static_cast<f64>(block[2].v[1]), static_cast<f64>(block[2].v[2]), static_cast<f64>(block[2].v[3]),
					static_cast<f64>(block[3].v[0]), static_cast<f64>(block[3].v[1]), static_cast<f64>(block[3].v[2]), static_cast<f64>(block[3].v[3]));
			}
#endif // MTL_DEBUG_LOGS
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

#if MTL_DEBUG_LOGS
			// TODO(excise): level 5; the fragment constants of a bounded prefix.
			if (diagnostic_trace)
			{
				const auto& offsets = frame.compiled_fragment->constant_offsets;
				const auto offset_at = [&](usz index) { return (index < offsets.size()) ? offsets[index] : 0u; };
				const auto value_at = [&](usz index, usz channel) -> f64 {
					return (index < fragment_constant_count) ? static_cast<f64>(constants[index].v[channel]) : 0.0;
				};

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: fragment constants, count %u, index %u, fp id %u, ucode offsets %u %u %u, values [0] %.4f %.4f %.4f %.4f [1] %.4f %.4f %.4f %.4f [2] %.4f %.4f %.4f %.4f",
					static_cast<u32>(fragment_constant_count), params->fs_constants_offset,
					frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
					offset_at(0), offset_at(1), offset_at(2),
					value_at(0, 0), value_at(0, 1), value_at(0, 2), value_at(0, 3),
					value_at(1, 0), value_at(1, 1), value_at(1, 2), value_at(1, 3),
					value_at(2, 0), value_at(2, 1), value_at(2, 2), value_at(2, 3));
			}
#endif // MTL_DEBUG_LOGS
		}

		GRAPH_frontend().fill_vertex_layout_state(
			frame.vertex_layout,
			current_vp_metadata,
			vertex_base,
			vertex_count,
			&params->attrib_data[0][0],
			persistent_base,
			volatile_base);

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the full geometry dump of the first subdraws.
		if (diagnostic_geometry < 9)
		{
			const u32 geometry = diagnostic_geometry - 1;

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, primitive %u, min %u, max %u, base %u, count %u, draw count %u, rebase %u, index buffer %u type %u offset %u length %u, required %u/%u",
				geometry, static_cast<u32>(draw_call.primitive), input.min_index, input.max_index, vertex_base, vertex_count,
				input.vertex_draw_count, input.index_rebase ? 1u : 0u, input.has_index_buffer ? 1u : 0u,
				static_cast<u32>(input.index_type), input.index_offset, input.index_length, required.first, required.second);

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, attribute mask 0x%x, blocks %u, volatile blocks %u, registers %u, branch bits 0x%x, z %.4f..%.4f, point %.3f",
				geometry, frame.vertex_layout.attribute_mask, static_cast<u32>(frame.vertex_layout.interleaved_blocks.size()),
				static_cast<u32>(frame.vertex_layout.volatile_blocks.size()), static_cast<u32>(frame.vertex_layout.referenced_registers.size()),
				context->transform_branch_bits, static_cast<f64>(context->z_near), static_cast<f64>(context->z_far),
				static_cast<f64>(context->point_size));

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, scale offset %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
				geometry,
				static_cast<f64>(context->scale_offset_mat[0]), static_cast<f64>(context->scale_offset_mat[1]),
				static_cast<f64>(context->scale_offset_mat[2]), static_cast<f64>(context->scale_offset_mat[3]),
				static_cast<f64>(context->scale_offset_mat[4]), static_cast<f64>(context->scale_offset_mat[5]),
				static_cast<f64>(context->scale_offset_mat[6]), static_cast<f64>(context->scale_offset_mat[7]),
				static_cast<f64>(context->scale_offset_mat[8]), static_cast<f64>(context->scale_offset_mat[9]),
				static_cast<f64>(context->scale_offset_mat[10]), static_cast<f64>(context->scale_offset_mat[11]),
				static_cast<f64>(context->scale_offset_mat[12]), static_cast<f64>(context->scale_offset_mat[13]),
				static_cast<f64>(context->scale_offset_mat[14]), static_cast<f64>(context->scale_offset_mat[15]));

			for (usz block_index = 0; block_index < frame.vertex_layout.interleaved_blocks.size(); ++block_index)
			{
				const auto& block = *frame.vertex_layout.interleaved_blocks[block_index];

				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, block %u, stride %u, base offset %u, location %u, address 0x%x, interleaved %u, single vertex %u, range %u..%u, attributes %u",
					geometry, static_cast<u32>(block_index), static_cast<u32>(block.attribute_stride), block.base_offset,
					static_cast<u32>(block.memory_location), block.real_offset_address, block.interleaved ? 1u : 0u,
					block.single_vertex ? 1u : 0u, block.vertex_range.first, block.vertex_range.second,
					static_cast<u32>(block.locations.size()));

				for (const auto& attribute : block.locations)
				{
					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: geometry %u, block %u, attribute %u, modulo %u, frequency %u",
						geometry, static_cast<u32>(block_index), static_cast<u32>(attribute.index),
						attribute.modulo ? 1u : 0u, static_cast<u32>(attribute.frequency));
				}

				const u32 first = block.real_offset_address + (block.single_vertex ? 0 : vertex_base * block.attribute_stride);
				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, block %u, guest 0x%x: %s",
					geometry, static_cast<u32>(block_index), first, dump_guest(first, 16).c_str());
			}

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, persistent stream %u bytes at %u: %s",
				geometry, required.first, persistent_base, dump_bytes(persistent_data, std::min<usz>(required.first, 96)).c_str());
			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, volatile stream %u bytes at %u: %s",
				geometry, required.second, volatile_base, dump_bytes(volatile_data, std::min<usz>(required.second, 96)).c_str());

			for (u32 index = 0; index < 16; ++index)
			{
				// TODO(excise): level 5; sampled byte dump.
				MTL_NOTE(5, "Metal 4: geometry %u, attribute %u, placement %u, attrib_data %d %d",
					geometry, index, static_cast<u32>(frame.vertex_layout.attribute_placement[index]),
					params->attrib_data[index][0], params->attrib_data[index][1]);
			}

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, pass colors %u size %ux%u, base index %u, index offset %u, xform %u, vs context %u, fs context %u, fs texture base %u",
				geometry, frame.attachments.color_count(), frame.attachments.width(), frame.attachments.height(),
				params->vertex_base_index, params->vertex_index_offset, params->xform_constants_offset,
				params->vs_context_offset, params->fs_context_offset, params->fs_texture_base_index);

			// TODO(excise): level 5; sampled byte dump.
			MTL_NOTE(5, "Metal 4: geometry %u, vertex program base 0x%x output mask 0x%x ctrl 0x%x, fragment ctrl 0x%x ucode %u, compiled vertex %u mask 0x%x, compiled fragment %u mask 0x%x, rop control 0x%x, alpha ref %.4f",
				geometry,
				current_vertex_program.base_address, current_vertex_program.output_mask, current_vertex_program.ctrl,
				current_fragment_program.ctrl, current_fragment_program.ucode_length,
				frame.compiled_vertex ? frame.compiled_vertex->id : 0u,
				frame.compiled_vertex ? frame.compiled_vertex->color_output_mask : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->color_output_mask : 0u,
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref));
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3 and level 5; the bound units and the vertex dump of a
		// bounded prefix of the geometry stream.
		if (diagnostic_trace)
		{
			for (u32 unit = 0; unit < 16; ++unit)
			{
				if (!(current_fp_metadata.referenced_textures_mask & (1u << unit)))
				{
					continue;
				}

				const sampler_info& info = sampler[unit];

				// TODO(excise): level 3; per-event trace line.
				MTL_NOTE(3, "Metal 4: unit %u, base %u, scale %.6f %.6f %.6f, bias %.6f %.6f %.6f, clamp %.4f %.4f %.4f %.4f, remap 0x%x, control 0x%x",
					unit, params->fs_texture_base_index,
					static_cast<f64>(info.scale[0]), static_cast<f64>(info.scale[1]), static_cast<f64>(info.scale[2]),
					static_cast<f64>(info.bias[0]), static_cast<f64>(info.bias[1]), static_cast<f64>(info.bias[2]),
					static_cast<f64>(info.clamp_min[0]), static_cast<f64>(info.clamp_min[1]),
					static_cast<f64>(info.clamp_max[0]), static_cast<f64>(info.clamp_max[1]),
					info.remap, info.control);
			}

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: fragment context, rop 0x%x, alpha ref %.4f, fog %.4f %.4f mode %u, wpos %.4f %.4f %.4f, fs context %u, vs context %u, xform %u",
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref),
				static_cast<f64>(fragment->fog_param0), static_cast<f64>(fragment->fog_param1), fragment->fog_mode,
				static_cast<f64>(fragment->wpos_scale), static_cast<f64>(fragment->wpos_bias[0]), static_cast<f64>(fragment->wpos_bias[1]),
				params->fs_context_offset, params->vs_context_offset, params->xform_constants_offset);

			const u32 stride = frame.vertex_layout.interleaved_blocks.empty()
				? 0u
				: static_cast<u32>(frame.vertex_layout.interleaved_blocks[0]->attribute_stride);

			if (persistent_data && stride)
			{
				const u8* stream = static_cast<const u8*>(persistent_data);

				for (u32 vertex = 0; vertex < 4; ++vertex)
				{
					const u8* source = stream + static_cast<usz>(vertex) * stride;
					f32 in_pos[4] = {};

					for (u32 channel = 0; channel < 3; ++channel)
					{
						const u32 word = (static_cast<u32>(source[channel * 4 + 0]) << 24)
							| (static_cast<u32>(source[channel * 4 + 1]) << 16)
							| (static_cast<u32>(source[channel * 4 + 2]) << 8)
							| static_cast<u32>(source[channel * 4 + 3]);
						std::memcpy(&in_pos[channel], &word, 4);
					}

					in_pos[3] = 1.0f;

					// The generated vertex program scales and offsets the fetched
					// position by the context matrix and applies nothing else
					// (vs-0.metal:363-381), so the trace must do the same.
					f32 clip[4] = {};
					for (u32 column = 0; column < 4; ++column)
					{
						for (u32 row = 0; row < 4; ++row)
						{
							clip[column] += in_pos[row] * context->scale_offset_mat[row * 4 + column];
						}
					}

					const f64 w = static_cast<f64>(clip[3]);

					// TODO(excise): level 5; sampled byte dump.
					MTL_NOTE(5, "Metal 4: vertex %u, guest %.4f %.4f %.4f, clip %.4f %.4f %.4f %.4f, ndc %.4f %.4f %.4f",
						vertex,
						static_cast<f64>(in_pos[0]), static_cast<f64>(in_pos[1]), static_cast<f64>(in_pos[2]),
						static_cast<f64>(clip[0]), static_cast<f64>(clip[1]), static_cast<f64>(clip[2]), static_cast<f64>(clip[3]),
						static_cast<f64>(clip[0]) / w, static_cast<f64>(clip[1]) / w, static_cast<f64>(clip[2]) / w);
				}
			}
		}
#endif // MTL_DEBUG_LOGS

#if MTL_DEBUG_LOGS
		// TODO(excise): level 5; the draw summary of one capture frame.  The
		// fragment-constant count is here because a program that reads no
		// constants cannot explain a lit surface, and that is a question a black
		// frame raises immediately.
		if (diagnostic_capture_active())
		{
			const usz fragment_constant_count = frame.compiled_fragment ? frame.compiled_fragment->constant_offsets.size() : 0;

			MTL_NOTE(5, "Metal 4: capture geometry frame %llu, primitive %u count %u base %u, vp %u fp %u, fp constants %llu fs offset %u, textures dims 0x%x shadow 0x%x redirect 0x%x msaa 0x%x, rop 0x%x alpha %.4f, fog %.4f %.4f mode %u, wpos %.4f %.4f %.4f",
				static_cast<unsigned long long>(diagnostic_frame_index),
				static_cast<u32>(draw_call.primitive), vertex_count, vertex_base,
				frame.compiled_vertex ? frame.compiled_vertex->id : 0u,
				frame.compiled_fragment ? frame.compiled_fragment->id : 0u,
				static_cast<unsigned long long>(fragment_constant_count), params->fs_constants_offset,
				current_fragment_program.texture_state.texture_dimensions,
				current_fragment_program.texture_state.shadow_textures,
				current_fragment_program.texture_state.redirected_textures,
				current_fragment_program.texture_state.multisampled_textures,
				fragment->rop_control, static_cast<f64>(fragment->alpha_ref),
				static_cast<f64>(fragment->fog_param0), static_cast<f64>(fragment->fog_param1), fragment->fog_mode,
				static_cast<f64>(fragment->wpos_scale), static_cast<f64>(fragment->wpos_bias[0]), static_cast<f64>(fragment->wpos_bias[1]));
		}
#endif // MTL_DEBUG_LOGS

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

		if (current_fragment_program.ctrl & (RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING | RSX_SHADER_CONTROL_LOGIC_OP))
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

			blend->logic_op = static_cast<u32>(rsx::method_registers.logic_operation());

			binds.fragment_blend = frame.constants.blend_values_block_address();
		}

		// Metal 4 snapshots the argument table when the encoder binds it, and a draw
		// opens its own pass and therefore its own encoder (MTLCommandRecorder.cpp:174),
		// so the table is re-bound on every draw.  The binder writes only the slots
		// whose value changed (REVIEW-FINDINGS.md F19).
		bool vertex_written [[maybe_unused]] = false;
		bool fragment_written [[maybe_unused]] = false;

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "draw/argument binds");

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

		// TODO(excise): level 2; the argument-table bind stream.  A bind runs once per
		// subdraw, so it is counted here and the trace stays a bounded prefix.
		MTL_PROBE_LATCH(2, "draw/bind");
		// TODO(excise): level 2; the binder writes only the slots whose value changed,
		// so a written and a cached table are different signatures (area N).
		if (vertex_written)
		{
			MTL_PROBE_LATCH(2, "bind/vertex table written");
		}
		else
		{
			MTL_PROBE_LATCH(2, "bind/vertex table cached");
		}

		if (fragment_written)
		{
			MTL_PROBE_LATCH(2, "bind/fragment table written");
		}
		else
		{
			MTL_PROBE_LATCH(2, "bind/fragment table cached");
		}

#if MTL_DEBUG_LOGS
		static u32 diagnostic_binds = 0;
		static u32 diagnostic_vertex_writes = 0;
		static u32 diagnostic_fragment_writes = 0;

		diagnostic_vertex_writes += vertex_written ? 1u : 0u;
		diagnostic_fragment_writes += fragment_written ? 1u : 0u;

		if (diagnostic_binds < 24)
		{
			// TODO(excise): level 3; the first subdraw binds of the run only.
			MTL_NOTE(3, "Metal 4: bind %u, vertex written %u, fragment written %u, vertex texture 0 (0x%llx, 0x%llx), fragment unit 0 (0x%llx, 0x%llx), unit 1 (0x%llx, 0x%llx)",
				diagnostic_binds, vertex_written ? 1u : 0u, fragment_written ? 1u : 0u,
				static_cast<unsigned long long>(binds.vertex_textures[0].texture._impl),
				static_cast<unsigned long long>(binds.vertex_textures[0].sampler._impl),
				static_cast<unsigned long long>(binds.fragment_textures[0].texture._impl),
				static_cast<unsigned long long>(binds.fragment_textures[0].sampler._impl),
				static_cast<unsigned long long>(binds.fragment_textures[1].texture._impl),
				static_cast<unsigned long long>(binds.fragment_textures[1].sampler._impl));

			MTL_NOTE(3, "Metal 4: bind %u, push constant index %u (of %u), draw parameters base 0x%llx, vertex base %u offset %u, xform %u, vs context %u, fs context %u, fs constants %u, fs texture base %u, attrib0 %d %d, attrib8 %d %d",
				diagnostic_binds, *vs_params, frame.constants.draw_parameters_index(),
				static_cast<unsigned long long>(frame.constants.draw_parameters_address()),
				params->vertex_base_index, params->vertex_index_offset, params->xform_constants_offset,
				params->vs_context_offset, params->fs_context_offset, params->fs_constants_offset,
				params->fs_texture_base_index,
				params->attrib_data[0][0], params->attrib_data[0][1],
				params->attrib_data[8][0], params->attrib_data[8][1]);

			for (u32 unit = 0; unit < 4; ++unit)
			{
				const sampler_info& info = sampler[unit];

				MTL_NOTE(3, "Metal 4: bind %u, texture parameters %u, scale %.4f %.4f %.4f, bias %.4f %.4f %.4f, clamp %.4f %.4f %.4f %.4f, remap 0x%x, control 0x%x",
					diagnostic_binds, unit,
					static_cast<f64>(info.scale[0]), static_cast<f64>(info.scale[1]), static_cast<f64>(info.scale[2]),
					static_cast<f64>(info.bias[0]), static_cast<f64>(info.bias[1]), static_cast<f64>(info.bias[2]),
					static_cast<f64>(info.clamp_min[0]), static_cast<f64>(info.clamp_min[1]),
					static_cast<f64>(info.clamp_max[0]), static_cast<f64>(info.clamp_max[1]),
					info.remap, info.control);
			}

			MTL_NOTE(3, "Metal 4: bind %u, vertex writes %u, fragment writes %u (of %u binds)",
				diagnostic_binds, diagnostic_vertex_writes, diagnostic_fragment_writes, diagnostic_binds + 1);
		}

		diagnostic_binds++;
#endif // MTL_DEBUG_LOGS

		m_frame_stats.setup_time += m_profiler.duration();

		const MTL::PrimitiveType primitive = get_primitive_type(draw_call.primitive);

		// A ZCULL query that started or ended between draws takes effect on the
		// encoder that carries the following draws (MTLZCULL.h).
		frame.zcull_control.apply(encoder);

		// TODO(excise): level 2; the submitted geometry and its index form (area I,
		// area N).  A draw with no vertices and a draw with vertices are different
		// signatures, and so are an indexed and a non-indexed draw.
		MTL_PROBE_TOTAL(2, "draw/vertices submitted", input.vertex_draw_count);
		if (input.has_index_buffer)
		{
			MTL_PROBE_LATCH(2, "draw/indexed");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/not indexed");
		}

		if (input.vertex_draw_count)
		{
			MTL_PROBE_LATCH(2, "draw/nonempty");
		}
		else
		{
			MTL_PROBE_LATCH(2, "draw/empty");
		}
		MTL_PROBE_HISTOGRAM(2, "draw metal primitive", static_cast<u32>(primitive), "metal primitive type");

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

		m_frame_stats.draw_exec_time += m_profiler.duration();
		m_frame_stats.draw_calls++;
	}


	// Resolve the image that carries the RSX output.  A surface at the display
	// address wins, then a texture the cache already holds, then a copy of the guest
	// bytes.  `width` and `height` come back as the image's own extent.
	void MTLGSRender::get_present_source(u32 address, u32& width, u32& height, u32 pitch, u32 format, u8 eye,
		MTL::Texture*& image)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/source");
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

						// TODO(excise): level 3; per-event trace line.
						MTL_NOTE(3, "Metal 4: present source from the surface, address 0x%x, %ux%u", address, width, height);

						std::tie(width, height) = rsx::apply_resolution_scale<true>(
							resolution_scaling_config, std::min(surface_width, width), std::min(surface_height, height));
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

			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: present source from the texture cache, address 0x%x", address);
			return;
		}

		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: flip image was not in the cache, uploading the surface from the CPU (address 0x%x, format 0x%x)", address, format);

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
			std::memcpy(host + (static_cast<u64>(row) * row_bytes), guest + (static_cast<u64>(row) * pitch), copy_bytes);
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
	void MTLGSRender::flip(const rsx::display_flip_info_t& info)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "present/flip");

		// A flip always owns a frame.  The guest can flip with no draw since the
		// last flip, and the output pass still needs a command buffer, so the frame
		// opens here when it is not already open (VKPresent.cpp:471-493).  A flip
		// never returns with no present, because that is a frozen image
		// (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if (!m_frame_resources)
		{
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/no frame resources", 1);
			GSRender::flip(info);
			rsx::thread::flip(info);
			return;
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

		if (active_scaling_config != resolution_scaling_config)
		{
			m_frame_resources->cache.sync_scaling_config(m_frame_resources->recorder, active_scaling_config);
			resolution_scaling_config = active_scaling_config;
		}

		auto& resources = *m_frame_resources;
		const u32 slot = resources.recorder.slot();

		// TODO(excise): level 2; the flip stream and its correlation key.  The flip
		// index below is the frame correlation id: it joins the flip, the acquire and
		// the display stamp of one frame.
		if (info.skip_frame)
		{
			MTL_PROBE_LATCH(2, "present/flip skipped");
		}
		else
		{
			MTL_PROBE_LATCH(2, "present/flip delivered");
		}
		MTL_PROBE_HISTOGRAM(2, "present flip buffer", static_cast<u32>(info.buffer), "display buffer index");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first flips of the run.
		static u32 diagnostic_flips = 0;

		if (diagnostic_flips < 16)
		{
			MTL_NOTE(3, "Metal 4: flip %u, buffer %u, skip %u",
				diagnostic_flips, static_cast<u32>(info.buffer), info.skip_frame ? 1u : 0u);
		}

		diagnostic_flips++;
#endif // MTL_DEBUG_LOGS

		// The recorder waited for this slot when the frame opened, so a capture that
		// slot recorded its previous time round is readable now (HLD-06 §2.7).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/deliver");
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
		if (!info.skip_frame && info.buffer < display_buffers_count)
		{
			buffer_width = display_buffers[info.buffer].width;
			buffer_height = display_buffers[info.buffer].height;
			buffer_pitch = display_buffers[info.buffer].pitch;

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
				const u32 address = rsx::get_address(display_buffers[info.buffer].offset, CELL_GCM_LOCATION_LOCAL);
				get_present_source(address, buffer_width, buffer_height, buffer_pitch, av_format, 0, image);

				static u32 diagnostic_presents = 0;

				// TODO(excise): level 2; the present branch.  The source resolves here
				// or it does not, and the two outcomes are different defects.
				if (image)
				{
					MTL_PROBE_LATCH(2, "present/source resolved");
				}
				else
				{
					MTL_PROBE_LATCH(2, "present/source missing");
				}
				// TODO(excise): level 2; the output format of the present.
				MTL_PROBE_HISTOGRAM(2, "present format", static_cast<u32>(av_format), "rsx av format");

				// TODO(excise): level 3; the first presents of the run.
				if (diagnostic_presents < 12)
				{
					MTL_NOTE(3, "Metal 4: present %u, display index %u, address 0x%x, %ux%u, pitch %u, image %s",
						diagnostic_presents, info.buffer, address, buffer_width, buffer_height, buffer_pitch,
						image ? "resolved" : "none");
				}

				diagnostic_presents++;

#if MTL_DEBUG_LOGS
				// TODO(excise): level 5; the present readback of the first presents,
				// the capture frame, and a sparse sample of the whole run.  The
				// sparse sample is what reaches the menu video and the race start.
				if (image && (diagnostic_presents < 6 || diagnostic_capture_active() ||
					(diagnostic_presents < 20000 && (diagnostic_presents % 600) == 0)))
				{
					dump_host_image(resources.recorder, resources.resources, image, "present", diagnostic_presents, address);
				}

				if (diagnostic_presents < 4)
				{
					for (u32 sample = 0; sample < diagnostic_sampled_count; ++sample)
					{
						// TODO(excise): level 5; sampled byte dump.
						MTL_NOTE(5, "Metal 4: readback at present %u, sample %u, address 0x%x, context %u",
							diagnostic_presents, sample, diagnostic_sampled_address[sample],
							diagnostic_sampled_context[sample]);

						dump_host_image(resources.recorder, resources.resources, diagnostic_sampled_image[sample],
							"sample", diagnostic_presents * 16u + sample, diagnostic_sampled_address[sample]);
					}
				}
#endif // MTL_DEBUG_LOGS

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
					resolution_scaling_config, RSX_SURFACE_DIMENSION_IGNORED, static_cast<u32>(display_buffers[info.buffer].height) + 30);

				if (image->height() < min_expected_height)
				{
					const u32 image_offset = (display_buffers[info.buffer].height + 30) * buffer_pitch + display_buffers[info.buffer].offset;
					get_present_source(rsx::get_address(image_offset, CELL_GCM_LOCATION_LOCAL), buffer_width, buffer_height, buffer_pitch, av_format, 1, image2);
				}
			}
		}

		present_source source{ image, image2, buffer_width, buffer_height };

		// The capture reads the source before the output path scales it
		// (VKPresent.cpp:684).  Only a capture the user asked for happens in a
		// run that is not tracing; the periodic one is instrumentation.
#if MTL_DEBUG_LOGS
		// A bounded periodic capture runs beside the user's request, so a session
		// with no key press still yields frames to compare (anti-flood rule).
		static u32 diagnostic_captures = 0;
		const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false) ||
			(++diagnostic_captures % 6000 == 0 && diagnostic_captures < 30000);
#else
		const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false);
#endif
		const bool user_is_recording = (g_recording_mode != recording_mode::stopped && m_frame->can_consume_frame());

		if (source.image && (user_asked_for_screenshot || user_is_recording))
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/capture");
			resources.output.request_capture(resources.recorder, resources.constants, source, user_asked_for_screenshot,
				g_cfg.video.record_with_overlays.get(), m_overlay_manager);
		}

		// The CPU-reduction limits are re-evaluated on an emu flip only
		// (VKPresent.cpp:586-589, GLPresent.cpp:253-257).
		if (info.emu_flip)
		{
			evaluate_cpu_usage_reduction_limits();
			resources.present.update_vsync();
		}

		const u32 target_width = static_cast<u32>(std::max(0, m_frame->client_width()));
		const u32 target_height = static_cast<u32>(std::max(0, m_frame->client_height()));

		MTL::Texture* drawable = nullptr;

		// The drawable is acquired even when the present source is missing, so the
		// window shows this flip's black clear and not the previous image
		// (GLPresent.cpp:276-281, VKPresent.cpp:781-802).
		if (target_width && target_height &&
			resources.present.acquire(resources.recorder, target_width, target_height, drawable))
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/compose");
			const areau display_area{0, 0, target_width, target_height};
			const areau aspect = (g_cfg.video.stretch_to_display_area.get() || !source.width || !source.height)
				? display_area
				: avconfig.aspect_convert_region({ source.width, source.height }, { target_width, target_height });

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
					info.stats.framebuffer_stats.to_string(resolution_scaling_config, !backend_config.supports_hw_msaa),
					get_load(), info.stats.draw_calls, info.stats.submit_count,
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
					avconfig.gamma, !g_cfg.video.full_rgb_range_output.get(), avconfig.stereo_enabled, m_overlay_manager);
			}
			else
			{
				// The output pass could not open, so the drawable stays empty.  It is
				// still presented, because a drawable the queue signalled must reach
				// the display or the layer keeps it.
				rsx_log.error("Metal 4: the output pass could not open, so the frame presents empty");
			}

			pass->release();
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/composed", 1);
		}
		else
		{
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "present/flip/no acquire", 1);
		}

		// Present submits the frame and presents the drawable it holds, so a frame
		// that acquired one always presents it and the ring advances.
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/present");
			resources.present.present(resources.recorder);
		}

		// The overlays the frame retired lose their temporary images here, and the
		// manager then frees them (GLPresent.cpp:283).
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "present/flip/retire");
			resources.output.retire_overlays(m_overlay_manager);

			// The cache retires the temporary subresources and the predictor state of
			// the frame that just ended (GLPresent.cpp:535).
			resources.textures.on_frame_end();
		}

		m_frame_open = false;

		m_frame_stats.flip_time = m_profiler.duration();
		// TODO(excise): level 1; frame boundary.
		MTL_PERF_FRAME(1);
		GSRender::flip(info);

		// The base flip clears this frame's flip request and clears the display
		// interrupt, and it stamps the host flip time the overlay refresh gate reads.
		// Without it the overlay requests a flip on every local task, so the RSX
		// thread acquires and presents a drawable far more often than the guest
		// flips and stalls in the acquire (GLPresent.cpp:519, VKPresent.cpp:987).
		rsx::thread::flip(info);
	}

	// A mid-frame wait must submit: the guest can wait on a label that only the
	// GPU can release, and the GPU cannot progress on a frame that has not been
	// committed (HLD-04 §4.3).
	void MTLGSRender::on_semaphore_acquire_wait()
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/semaphore wait");

		// This path is a guest spin, so it fires millions of times a session.  It is
		// counted and classified and it never prints a line: a trace line here
		// produced three quarters of one measured log (anti-flood rule).
#if MTL_PERF_ENABLED
		// TODO(excise): level 2; the wait count and the flush depth at the wait.
		{
			const u32 pending_flushes = m_flush_queue ? static_cast<u32>(m_flush_queue->pending.size()) : 0u;

			MTL_PROBE_TOTAL(2, "sync/semaphore waits", 1);
			// The key is the flush depth, capped at four: key 4 means four or more.
			MTL_PROBE_HISTOGRAM(2, "sync/semaphore wait flush depth", pending_flushes < 4 ? pending_flushes : 4, "pending flushes");
			if (m_frame_open)
			{
				MTL_PROBE_LATCH(2, "sync/semaphore wait/frame open");
			}
			else
			{
				MTL_PROBE_LATCH(2, "sync/semaphore wait/frame closed");
			}
		}
#endif // MTL_PERF_ENABLED

		// A pending flush request, or an emu flip the guest is waiting on, runs the
		// local task here (GLGSRender.cpp:1328-1335, VKGSRender.cpp:1136-1144).  The
		// wait does not submit or close the frame: the frame stays open until its
		// flip presents it, because a flip that finds the frame closed is a frozen
		// image (GLPresent.cpp:518-519, VKPresent.cpp:983-988).
		if ((m_flush_queue && !m_flush_queue->pending.empty()) ||
			(async_flip_requested & flip_request::emu_requested))
		{
			do_local_task(rsx::FIFO::state::lock_wait);
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
	// TODO(excise): level 2; the hook and the print go with the probe work.
	void MTLGSRender::on_fifo_desync(const fifo_desync_info& info)
	{
		if (!mtl::probe::at_least(2))
		{
			return;
		}

		// The method index the offending word would name, and whether the method
		// table knows it.  A word that names an undefined method is a data word
		// that the host took for a command (L52), and the index says which
		// register range the host was walking when it went wrong.
		const u32 reg = (info.last_command & 0xffff) >> 2;

		std::string words;
		if (info.words_valid)
		{
			for (u32 index = 0; index < 8; ++index)
			{
				words += fmt::format(" %08x", info.words[index]);
			}
		}
		else
		{
			words = " <unmapped>";
		}

		std::string history;
		for (u32 index = 0; index < info.history_count; ++index)
		{
			const dispatch_entry& entry = info.history[index];
			history += fmt::format(" [reg 0x%x value 0x%x at 0x%x]", entry.reg, entry.value, entry.position);
		}

		MTL_NOTE(2, "Metal 4: FIFO desync capture: get=0x%x put=0x%x get_guest=0x%x restore_point=0x%x code_start=0x%x ret_addr=0x%x last_label=0x%x/0x%x cmd=0x%x method_index=0x%x method_known=%u words:%s",
			info.get, info.put, info.get_guest, info.restore_point, info.code_start, info.ret_addr,
			info.last_label_address, info.last_label_value, info.last_command, reg,
			info.last_command_method_known ? 1u : 0u, words.c_str());

		MTL_NOTE(2, "Metal 4: FIFO desync capture: last commands:%s", history.c_str());
	}

	bool MTLGSRender::release_GCM_label(u32 /*type*/, u32 /*address*/, u32 /*data*/)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/label release");

		if (!m_frame_resources)
		{
			return false;
		}

		if (m_frame_open)
		{
			m_frame_resources->recorder.flush_for_readback();
		}
		else if (!m_frame_resources->recorder.wait_idle())
		{
			rsx_log.error("Metal 4: a label release found GPU work in flight after the feedback timeout");
		}

		// TODO(excise): level 2; the label release that drained the GPU.
		MTL_PROBE_LATCH(2, "sync/label release drained");

		// False hands the write to the caller, after the GPU work above is done.
		return false;
	}

	// The flush runs here, on the RSX thread, because it needs an encoder and the
	// fault that asked for it runs on a guest thread (GLGSRender.cpp:1329-1350).
	// A request is released by the faulting thread once it has consumed its result,
	// so the list keeps the requests that are still in flight.
	void MTLGSRender::do_local_task(rsx::FIFO::state state)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "local task");
		if (m_flush_queue && m_frame_resources)
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "local task/drain queue");

			std::lock_guard lock(m_flush_queue->guard);

			// A request leaves the queue once its owner has stopped waiting and the
			// RSX thread has serviced it.  A request with no flush work is never
			// waited on, so its `received` is set at the post; the `processed` term
			// keeps it until the loop below has applied its sampler flags.
			m_flush_queue->pending.remove_if([](const flush_queue::request& q) { return q.received && q.processed; });
			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "local task/pending requests", m_flush_queue->pending.size());

			for (flush_queue::request& request : m_flush_queue->pending)
			{
				if (request.processed)
				{
					continue;
				}

				// The request may have arrived between two frames, and the recorder
				// has no command buffer open while the ring slot is idle.
				{
					// TODO(excise): level 1; block timer.
					MTL_PERF_SCOPE(1, "local task/drain/open frame");
					open_frame();
				}

				{
					// TODO(excise): level 1; block timer.
					MTL_PERF_SCOPE(1, "local task/drain/flush all");
					m_frame_resources->textures.flush_all(m_frame_resources->recorder, request.sections);
				}

				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "local task/requests serviced", 1);

				if (request.invalidate_samplers)
				{
					// A cached image became invalid, so every unit re-resolves on its
					// next draw (VKGSRender.cpp:1028).  The flags are the RSX thread's
					// to write, because the faulting thread cannot touch them.
					m_textures_dirty.fill(true);
					m_vertex_textures_dirty.fill(true);
					// TODO(excise): level 1; running total.
					MTL_PERF_COUNT(1, "local task/sampler invalidations", 1);
				}

				request.processed = true;
			}
		}

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "local task/gs");
			GSRender::do_local_task(state);
		}

		// The in-game interface refreshes on its own schedule, and it asks for a flip
		// when the guest is not flipping (overlays.cpp:532).  The output pass belongs
		// to a frame, so the flip opens one when the previous frame already ended
		// (VKGSRender.cpp:1911).
		if (m_overlay_manager)
		{
			const auto should_ignore = in_begin_end && state != rsx::FIFO::state::empty;

			if ((async_flip_requested & flip_request::native_ui) && !should_ignore && !is_stopped())
			{
				// TODO(excise): level 1; block timer.
				MTL_PERF_SCOPE(1, "local task/overlay flip");
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "local task/overlay flips", 1);
				open_frame();

				rsx::display_flip_info_t info{};
				info.buffer = current_display_buffer;
				flip(info);
			}
		}
	}

	// A guest write to a range a surface owns invalidates that surface, because
	// the guest bytes are canonical and the host copy is now stale
	// (VKGSRender.cpp:1483).
	bool MTLGSRender::scaled_image_from_memory([[maybe_unused]] const rsx::blit_src_info& src, [[maybe_unused]] const rsx::blit_dst_info& dst, [[maybe_unused]] bool interpolate)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "blit/scaled image");

		// TODO(excise): level 2; the transfer stream and its formats (area H).
		if (interpolate)
		{
			MTL_PROBE_LATCH(2, "transfer/interpolated");
		}
		else
		{
			MTL_PROBE_LATCH(2, "transfer/direct");
		}
		MTL_PROBE_HISTOGRAM(2, "transfer source format", static_cast<u32>(src.format), "gcm source format");
		MTL_PROBE_HISTOGRAM(2, "transfer destination format", static_cast<u32>(dst.format), "gcm destination format");

#if MTL_DEBUG_LOGS
		// TODO(excise): level 3; the first transfers of the run.
		static u32 diagnostic_transfers = 0;

		if (diagnostic_transfers < 16)
		{
			MTL_NOTE(3, "Metal 4: blit %u, src 0x%x %ux%u pitch %u bpp %u fmt %u dma 0x%x, dst 0x%x %ux%u pitch %u bpp %u fmt %u dma 0x%x, clip %ux%u, scale %.2f/%.2f, inter %u",
				diagnostic_transfers, src.rsx_address, src.width, src.height, src.pitch, src.bpp,
				static_cast<u32>(src.format), src.dma,
				dst.rsx_address, dst.width, dst.height, dst.pitch, dst.bpp,
				static_cast<u32>(dst.format), dst.dma,
				dst.clip_width, dst.clip_height, static_cast<f64>(dst.scale_x), static_cast<f64>(dst.scale_y),
				interpolate ? 1u : 0u);
		}

		diagnostic_transfers++;
#endif // MTL_DEBUG_LOGS

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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "sync/write barrier");
		if (!m_frame_resources)
		{
			return;
		}

		// TODO(excise): level 2; the write-barrier stream and a coarse size bucket
		// (area J).  The bucket is 0 for a range up to 256 bytes, 1 to 4 KiB, 2 to
		// 64 KiB and 3 above.
		MTL_PROBE_LATCH(2, "sync/write barrier");
		MTL_PROBE_HISTOGRAM(2, "sync/write barrier size",
			memory_range <= 256 ? 0 : memory_range <= 4096 ? 1 : memory_range <= 65536 ? 2 : 3, "barrier range bucket");

#if MTL_DEBUG_LOGS
		static u32 diagnostic_barriers = 0;

		// TODO(excise): level 3; the first write barriers of the run.
		if (diagnostic_barriers++ < 16)
		{
			MTL_NOTE(3, "Metal 4: write_barrier 0x%x + 0x%x", memory_address, memory_range);
		}
#endif // MTL_DEBUG_LOGS

		m_frame_resources->cache.invalidate_range(utils::address_range32::start_length(memory_address, memory_range));
	}

	// A guest access to a locked page invalidates the sections that overlap it
	// (VKGSRender.cpp:1277).  A fault on a guest thread is recorded and queued for
	// the RSX thread, which owns the encoders.  A fault on the RSX thread itself
	// cannot be queued for that thread, so it flushes in place
	// (GLGSRender.cpp:1281).
	bool MTLGSRender::on_access_violation(u32 address, bool is_writing)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "fault");
		if (!m_frame_resources)
		{
			return false;
		}

		// TODO: the RSX thread faults on this page while it owns it, every frame,
		// and the access that reaches a protected page is still to be found.  A
		// wait for this thread stalls the whole run ("Thread [rsx::thread] is too
		// sleepy"), so the flush runs in place until the access is known.
		const bool can_flush = is_current_thread();

		if (can_flush)
		{
			// The fault of the RSX thread is the one that stalls the run, so it is
			// always reported; a fault of a guest thread is routine and handled by
			// the deferred path below.
			rsx_log.notice("Metal 4: access violation 0x%x, writing %u, on the RSX thread", address, is_writing ? 1u : 0u);
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/on RSX thread", 1);
		}
		else
		{
			// TODO(excise): level 3; per-event trace line.
			MTL_NOTE(3, "Metal 4: access violation 0x%x, writing %u", address, is_writing ? 1u : 0u);
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/on guest thread", 1);
		}

		const rsx::invalidation_cause cause = is_writing
			? (can_flush ? rsx::invalidation_cause::write : rsx::invalidation_cause::deferred_write)
			: (can_flush ? rsx::invalidation_cause::read : rsx::invalidation_cause::deferred_read);

		if (can_flush)
		{
			if (is_writing)
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/direct write", 1);
			}
			else
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/direct read", 1);
			}
		}
		else
		{
			if (is_writing)
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/deferred write", 1);
			}
			else
			{
				// TODO(excise): level 1; running total.
				MTL_PERF_COUNT(1, "fault/deferred read", 1);
			}
		}

		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/mm flush");
			rsx::mm_flush(address);
		}

		auto result = [&] {
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/invalidate address");
			return m_frame_resources->textures.invalidate_address(m_frame_resources->recorder, address, cause);
		}();

		if (!result.violation_handled)
		{
			// The fault is not the texture cache's, so it belongs to a query.
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/unhandled", 1);
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/zcull");
			return zcull_ctrl->on_access_violation(address);
		}

		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/handled", 1);
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/flushable sections", result.num_flushable);
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/sections to flush", result.sections_to_flush.size());
		// TODO(excise): level 1; running total.
		MTL_PERF_COUNT(1, "fault/sections to unprotect", result.sections_to_unprotect.size());

		if (can_flush)
		{
			// The flush ran on this call, so nothing is left to wait for, and the
			// flags below are this thread's to write in any case.
			if (result.invalidate_samplers)
			{
				m_textures_dirty.fill(true);
				m_vertex_textures_dirty.fill(true);
			}

			return true;
		}

		const bool invalidate_samplers = result.invalidate_samplers;

		// The deferred path keeps sections_to_flush only when the RSX thread has to
		// write them back; every other handled fault unprotects in place, so an empty
		// set means the page is already writable and the faulting instruction can be
		// retried at once (texture_cache.h:1329-1351).
		const bool flush_pending = !result.sections_to_flush.empty();

		flush_queue::request* request = nullptr;
		{
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/enqueue");
			std::lock_guard lock(m_flush_queue->guard);

			auto& queued = m_flush_queue->pending.emplace_back();
			queued.sections = std::move(result);
			queued.invalidate_samplers = invalidate_samplers;
			queued.received = !flush_pending;
			request = &queued;

			// TODO(excise): level 1; instantaneous value.
			MTL_PERF_GAUGE(1, "fault/queue depth", m_flush_queue->pending.size());
		}

		m_eng_interrupt_mask |= rsx::backend_interrupt;
		vm::temporary_unlock();

		if (flush_pending)
		{
			// The page keeps its protection until the flush reaches guest memory, so
			// the faulting instruction must not be retried before the request
			// completes (GLGSRender.cpp:1298-1307).
			// TODO(excise): level 1; block timer.
			MTL_PERF_SCOPE(1, "fault/producer wait");
			request->producer_wait();
		}
		else
		{
			// Nothing is written back, so the request carries only the sampler flags,
			// which the RSX thread reads on its next pass.  This thread does not wait:
			// the wait would be a whole frame of the guest thread at a frame rate the
			// RSX thread already paces.
			// TODO(excise): level 1; running total.
			MTL_PERF_COUNT(1, "fault/not waited", 1);
		}

		return true;
	}

	// The guest unmapped a range, so every host copy of it is invalid
	// (VKGSRender.cpp:1312).
	void MTLGSRender::on_invalidate_memory_range(const utils::address_range32& range, rsx::invalidation_cause cause)
	{
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "invalidate range");
		if (!m_frame_resources)
		{
			return;
		}

		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: invalidate memory range");

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
		// TODO(excise): level 1; block timer.
		MTL_PERF_SCOPE(1, "tile unbound");
		// TODO(excise): level 3; per-event trace line.
		MTL_NOTE(3, "Metal 4: tile unbound, every unit re-resolves");

		m_textures_dirty.fill(true);
		m_vertex_textures_dirty.fill(true);
	}
}

#endif // RSX_ENABLE_METAL4
