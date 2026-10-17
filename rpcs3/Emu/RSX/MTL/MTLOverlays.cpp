#include "MTLOverlays.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLConstants.h"
#include "MTLResourceManager.h"
#include "MTLShader.h"

#include "../Overlays/overlay_fonts.h"
#include "../Overlays/overlays.h"
#include "../Program/GLSLTypes.h"
#include "../Program/RSXOverlay.h"
#include "../Program/SPIRVCommon.h"

#include "Utilities/stereo_config.h"
#include "Emu/Cell/timers.hpp"
#include "util/logs.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace mtl
{
	namespace
	{
		std::string replace_all(std::string text, std::string_view from, std::string_view to)
		{
			for (usz pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
			{
				text.replace(pos, from.size(), to);
			}

			return text;
		}

		// OverlayRenderVS.glsl reads `gl_VertexIndex` for its geometry and takes the
		// per-draw block from the push constant; the Vulkan substitution is what this
		// backend needs too (VKOverlays.cpp:295).
		std::string overlay_vertex_source()
		{
			std::string src =
			#include "../Program/GLSLSnippets/OverlayRenderVS.glsl"
			;

			src = replace_all(std::move(src), "%preprocessor", "// %preprocessor");
			src = replace_all(std::move(src), "%push_block", "push_constant");
			return src;
		}

		std::string overlay_fragment_source()
		{
			std::string src =
			#include "../Program/GLSLSnippets/OverlayRenderFS.glsl"
			;

			src = replace_all(std::move(src), "%preprocessor", "// %preprocessor");
			// The fragment half of the block starts where the vertex half ends, so
			// both stages address one 128-byte block (VKOverlays.cpp:304).
			src = replace_all(std::move(src), "%push_block_offset", "layout(offset=68)");
			src = replace_all(std::move(src), "%push_block", "push_constant");
			return src;
		}

		std::string calibration_fragment_source()
		{
			std::string src =
			#include "../Program/GLSLSnippets/VideoOutCalibrationPass.glsl"
			;

			src = replace_all(std::move(src), "%sampler_binding", "x");
			src = replace_all(std::move(src), "%set_decorator", "set=0");
			return src;
		}

		std::string passthrough_vertex_source()
		{
			return
			#include "../Program/GLSLSnippets/GenericVSPassthrough.glsl"
			;
		}

		MTL::PrimitiveTopologyClass topology_class(MTL::PrimitiveType type)
		{
			switch (type)
			{
			case MTL::PrimitiveTypePoint:
				return MTL::PrimitiveTopologyClassPoint;
			case MTL::PrimitiveTypeLine:
			case MTL::PrimitiveTypeLineStrip:
				return MTL::PrimitiveTopologyClassLine;
			default:
				return MTL::PrimitiveTopologyClassTriangle;
			}
		}

		// Metal needs every address 16-byte aligned for a float4 read, and the rings
		// hand back a competely packed offset.  The caller reserves the slack.
		constexpr u32 ring_alignment = 16;

		u32 align_offset(u32 offset)
		{
			return (offset + (ring_alignment - 1)) & ~(ring_alignment - 1);
		}
	}

	overlay_pass::~overlay_pass()
	{
		for (auto& [key, entry] : m_pipelines)
		{
			if (entry.state)
			{
				entry.state->release();
			}
		}

		if (m_vs_library)
		{
			m_vs_library->release();
		}

		if (m_fs_library)
		{
			m_fs_library->release();
		}

		if (m_sampler)
		{
			m_sampler->release();
		}

		if (m_vertex_table)
		{
			m_vertex_table->release();
		}

		if (m_fragment_table)
		{
			m_fragment_table->release();
		}
	}

	bool overlay_pass::init(const device& dev, resource_manager& resources, std::string vs, std::string fs, bool linear)
	{
		m_device = &dev;
		m_resources = &resources;
		m_vs_src = std::move(vs);
		m_fs_src = std::move(fs);

		std::vector<u32> spv;
		std::string msl;

		if (!spirv::compile_glsl_to_spv(spv, m_vs_src, ::glsl::glsl_vertex_program, ::glsl::glsl_rules_vulkan)
			|| !spv_to_msl(spv, true, msl, nullptr, overlay_uniform_slot))
		{
			rsx_log.error("Metal: overlay vertex shader translation failed");
			return false;
		}

		m_vs_library = compile_msl(dev.compiler(), msl, nullptr);

		spv.clear();
		msl.clear();

		if (!spirv::compile_glsl_to_spv(spv, m_fs_src, ::glsl::glsl_fragment_program, ::glsl::glsl_rules_vulkan)
			|| !spv_to_msl(spv, false, msl, nullptr, overlay_uniform_slot))
		{
			rsx_log.error("Metal: overlay fragment shader translation failed");
			return false;
		}

		m_fs_library = compile_msl(dev.compiler(), msl, nullptr);

		if (!m_vs_library || !m_fs_library)
		{
			rsx_log.error("Metal: overlay library creation failed");
			return false;
		}

		MTL::SamplerDescriptor* sampler_descriptor = MTL::SamplerDescriptor::alloc()->init();
		sampler_descriptor->setMinFilter(linear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		sampler_descriptor->setMagFilter(linear ? MTL::SamplerMinMagFilterLinear : MTL::SamplerMinMagFilterNearest);
		sampler_descriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
		sampler_descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
		sampler_descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
		m_sampler = dev.dev()->newSamplerState(sampler_descriptor);
		sampler_descriptor->release();

		// One null texture per declared type, because FS0 and FS1 do not share one.
		MTL::TextureDescriptor* null_descriptor = MTL::TextureDescriptor::alloc()->init();
		null_descriptor->setPixelFormat(MTL::PixelFormatBGRA8Unorm);
		null_descriptor->setWidth(1);
		null_descriptor->setHeight(1);
		null_descriptor->setUsage(MTL::TextureUsageShaderRead);
		null_descriptor->setTextureType(MTL::TextureType2D);
		m_null_2d = resources.new_texture(null_descriptor, MTL::StorageModeShared);
		null_descriptor->setTextureType(MTL::TextureType2DArray);
		null_descriptor->setArrayLength(1);
		m_null_array = resources.new_texture(null_descriptor, MTL::StorageModeShared);
		null_descriptor->release();

		if (m_null_2d && m_null_array)
		{
			resources.make_persistent(m_null_2d);
			resources.make_persistent(m_null_array);
		}

		MTL4::ArgumentTableDescriptor* table_descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
		table_descriptor->setMaxBufferBindCount(overlay_vertex_slot + 1);
		table_descriptor->setMaxTextureBindCount(overlay_texture_count);
		table_descriptor->setMaxSamplerStateBindCount(overlay_texture_count);
		table_descriptor->setInitializeBindings(true);
		m_vertex_table = dev.dev()->newArgumentTable(table_descriptor, nullptr);
		m_fragment_table = dev.dev()->newArgumentTable(table_descriptor, nullptr);
		table_descriptor->release();

		if (!m_sampler || !m_vertex_table || !m_fragment_table || !m_null_2d || !m_null_array)
		{
			rsx_log.error("Metal: overlay argument table allocation failed");
			return false;
		}

		return true;
	}

	MTL::RenderPipelineState* overlay_pass::build_pipeline(MTL::PixelFormat format, u8 samples,
		MTL::PrimitiveType topology, bool vertex_input, bool blend)
	{
		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(m_vs_library);
		vertex_function->setName(NS::String::string(msl_entry_point, NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(m_fs_library);
		fragment_function->setName(NS::String::string(msl_entry_point, NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);
		descriptor->setRasterSampleCount(samples);
		descriptor->setInputPrimitiveTopology(topology_class(topology));

		if (vertex_input)
		{
			// Metal 4 has no setVertexBuffer (MTL4RenderCommandEncoder.hpp), so the
			// vertex data is an argument-table buffer and the descriptor names the
			// index it lives at.
			MTL::VertexDescriptor* layout = MTL::VertexDescriptor::alloc()->init();
			layout->attributes()->object(0)->setFormat(MTL::VertexFormatFloat4);
			layout->attributes()->object(0)->setOffset(0);
			layout->attributes()->object(0)->setBufferIndex(overlay_vertex_slot);
			layout->layouts()->object(overlay_vertex_slot)->setStride(overlay_vertex_stride);
			layout->layouts()->object(overlay_vertex_slot)->setStepFunction(MTL::VertexStepFunctionPerVertex);
			descriptor->setVertexDescriptor(layout);
			layout->release();
		}

		MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments = descriptor->colorAttachments();
		MTL4::RenderPipelineColorAttachmentDescriptor* attachment = attachments->object(0);
		attachment->setPixelFormat(format);
		attachment->setWriteMask(MTL::ColorWriteMaskAll);
		attachment->setBlendingState(blend ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);

		if (blend)
		{
			// The UI blend the Vulkan pass configures (VKOverlays.cpp:315): the
			// colour is src * srcA + dst * (1 - srcA) and the alpha is left alone.
			attachment->setSourceRGBBlendFactor(MTL::BlendFactorSourceAlpha);
			attachment->setDestinationRGBBlendFactor(MTL::BlendFactorOneMinusSourceAlpha);
			attachment->setRgbBlendOperation(MTL::BlendOperationAdd);
			attachment->setSourceAlphaBlendFactor(MTL::BlendFactorZero);
			attachment->setDestinationAlphaBlendFactor(MTL::BlendFactorOne);
			attachment->setAlphaBlendOperation(MTL::BlendOperationAdd);
		}

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = m_device->compiler()->newRenderPipelineState(descriptor, nullptr, &error);

		vertex_function->release();
		fragment_function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: overlay pipeline creation failed: %s", message);
			return nullptr;
		}

		return state;
	}

	void overlay_pass::encode(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, const areau& viewport,
		MTL::Texture* target, MTL::PrimitiveType topology, bool vertex_input, bool blend, bool quad_list,
		const void* uniform, const void* vertices, u32 vertex_bytes, u32 vertex_count,
		const std::vector<MTL::Texture*>& src)
	{
		if (!encoder || !target)
		{
			return;
		}

		const u64 key = (static_cast<u64>(target->pixelFormat()) << 24)
			| (static_cast<u64>(topology) << 8)
			| (vertex_input ? 2u : 0u)
			| (blend ? 1u : 0u);

		pipeline_entry& entry = m_pipelines[key];

		if (!entry.state)
		{
			entry.state = build_pipeline(target->pixelFormat(), 1, topology, vertex_input, blend);

			if (!entry.state)
			{
				return;
			}
		}

		const u32 uniform_reserved = overlay_uniform_size + ring_alignment;
		u32 uniform_raw = 0;
		void* uniform_dst = constants.overlay_uniform_ring().alloc(uniform_reserved, uniform_raw);

		if (!uniform_dst)
		{
			return;
		}

		const u32 uniform_offset = align_offset(uniform_raw);
		std::memcpy(static_cast<u8*>(uniform_dst) + (uniform_offset - uniform_raw), uniform, overlay_uniform_size);
		const MTL::GPUAddress uniform_address = constants.overlay_uniform_ring().address() + uniform_offset;

		MTL::GPUAddress vertex_address = 0;

		if (vertex_input && vertex_count && vertices && vertex_bytes)
		{
			u32 vertex_raw = 0;
			void* vertex_dst = constants.overlay_vertex_ring().alloc(vertex_bytes + ring_alignment, vertex_raw);

			if (!vertex_dst)
			{
				return;
			}

			const u32 vertex_offset = align_offset(vertex_raw);
			std::memcpy(static_cast<u8*>(vertex_dst) + (vertex_offset - vertex_raw), vertices, vertex_bytes);
			vertex_address = constants.overlay_vertex_ring().address() + vertex_offset;
		}

		encoder->setRenderPipelineState(entry.state);
		// The drawable pass carries no depth and the UI geometry winds both ways,
		// so the encoder's cull state is turned off for the draw (CONFLICTS.md C7:
		// cull mode is encoder state, and the RSX draw path sets it every draw).
		encoder->setCullMode(MTL::CullModeNone);

		MTL::Viewport metal_viewport{};
		metal_viewport.originX = static_cast<f64>(viewport.x1);
		metal_viewport.originY = static_cast<f64>(viewport.y1);
		metal_viewport.width = static_cast<f64>(viewport.width());
		metal_viewport.height = static_cast<f64>(viewport.height());
		metal_viewport.znear = 0.0;
		metal_viewport.zfar = 1.0;
		encoder->setViewport(metal_viewport);

		MTL::ScissorRect scissor{};
		scissor.x = viewport.x1;
		scissor.y = viewport.y1;
		scissor.width = viewport.width();
		scissor.height = viewport.height();
		encoder->setScissorRect(scissor);

		m_vertex_table->setAddress(uniform_address, overlay_uniform_slot);
		m_fragment_table->setAddress(uniform_address, overlay_uniform_slot);

		if (vertex_address)
		{
			m_vertex_table->setAddress(vertex_address, overlay_vertex_slot);
		}

		for (u32 index = 0; index < overlay_texture_count; ++index)
		{
			MTL::Texture* texture = (index < src.size()) ? src[index] : nullptr;

			if (texture)
			{
				m_fragment_table->setTexture(texture->gpuResourceID(), index);
			}

			m_fragment_table->setSamplerState(m_sampler->gpuResourceID(), index);
		}

		encoder->setArgumentTable(m_vertex_table, MTL::RenderStageVertex);
		encoder->setArgumentTable(m_fragment_table, MTL::RenderStageFragment);

		if (quad_list)
		{
			// The UI emits disjoint quads, and Vulkan emulates them with one
			// 4-vertex strip per quad (VKOverlays.cpp:618).  Metal keeps the
			// vertices and the indices the same.
			for (u32 first = 0; first + 4 <= vertex_count; first += 4)
			{
				encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, first, 4, 1, 0);
			}
		}
		else if (vertex_input)
		{
			encoder->drawPrimitives(topology, 0, vertex_count, 1, 0);
		}
		else
		{
			encoder->drawPrimitives(MTL::PrimitiveTypeTriangleStrip, 0, 4, 1, 0);
		}
	}

	// -- ui_overlay_renderer ----------------------------------------------------

	bool ui_overlay_renderer::init(const device& dev, resource_manager& resources)
	{
		if (!overlay_pass::init(dev, resources, overlay_vertex_source(), overlay_fragment_source()))
		{
			return false;
		}

		// The icons the UI references by resource id (VKOverlays.cpp:382).
		rsx::overlays::resource_config configuration;
		configuration.load_files();

		u64 storage_key = 1;

		for (const auto& resource : configuration.texture_raw_data)
		{
			MTL::Texture* texture = make_texture(resources, resource->get_data(), resource->w, resource->h, 1, false);

			if (texture)
			{
				resources.make_persistent(texture);
				m_view_cache[storage_key] = texture;
			}

			storage_key++;
		}

		configuration.free_resources();
		return true;
	}

	void ui_overlay_renderer::destroy()
	{
		m_temp_cache.clear();
		m_temp_owner.clear();
		m_view_cache.clear();
	}

	MTL::Texture* ui_overlay_renderer::make_texture(resource_manager& resources, const void* pixels,
		u32 width, u32 height, u32 layers, bool font)
	{
		MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
		descriptor->setTextureType(layers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D);
		descriptor->setPixelFormat(font ? MTL::PixelFormatR8Unorm : MTL::PixelFormatBGRA8Unorm);
		descriptor->setWidth(std::max(width, 1u));
		descriptor->setHeight(std::max(height, 1u));
		descriptor->setArrayLength(std::max(layers, 1u));
		descriptor->setUsage(MTL::TextureUsageShaderRead);
		MTL::Texture* texture = resources.new_texture(descriptor, MTL::StorageModeShared);
		descriptor->release();

		if (!texture)
		{
			return nullptr;
		}

		upload(texture, pixels, width, height, layers, font);
		return texture;
	}

	void ui_overlay_renderer::upload(MTL::Texture* texture, const void* pixels, u32 width, u32 height,
		u32 layers, bool font)
	{
		if (!texture || !width || !height || !layers)
		{
			return;
		}

		const u32 pitch = font ? width : width * 4;
		const u32 layer_size = pitch * height;

		if (!pixels)
		{
			return;
		}

		// The source is tightly packed and the destination rows are the same width,
		// so one region per layer carries it (VKOverlays.cpp:321).
		for (u32 layer = 0; layer < layers; ++layer)
		{
			const MTL::Region region = MTL::Region::Make2D(0, 0, width, height);
			texture->replaceRegion(region, 0, layer, static_cast<const u8*>(pixels) + (layer * layer_size), pitch, layer_size);
		}
	}

	MTL::Texture* ui_overlay_renderer::find_font(resource_manager& resources,
		const rsx::overlays::font* font)
	{
		const auto size = font->get_glyph_data_dimensions();
		const u64 key = reinterpret_cast<u64>(font);
		const auto found = m_view_cache.find(key);

		if (found != m_view_cache.end())
		{
			MTL::Texture* texture = found->second;

			if (texture->width() == size.width && texture->height() == size.height && texture->arrayLength() == size.depth)
			{
				return texture;
			}

			resources.defer_release(texture);
			m_view_cache.erase(found);
		}

		const std::vector<u8>& bytes = font->get_glyph_data();
		MTL::Texture* texture = make_texture(resources, bytes.data(), size.width, size.height, size.depth, true);

		if (texture)
		{
			// A font is rebuilt every time its atlas changes, so its residency is
			// per frame slot and the deferred release above already covers the old
			// one.
			resources.make_persistent(texture);
			m_view_cache[key] = texture;
		}

		return texture;
	}

	MTL::Texture* ui_overlay_renderer::find_temp_image(resource_manager& resources,
		const rsx::overlays::image_info_base* desc, u32 owner_uid)
	{
		const bool dirty = std::exchange(desc->dirty, false);
		const u64 key = reinterpret_cast<u64>(desc);
		const auto cached = m_temp_cache.find(key);

		if (cached != m_temp_cache.end())
		{
			if (dirty)
			{
				upload(cached->second, desc->get_data(), desc->w, desc->h, 1, false);
			}

			return cached->second;
		}

		MTL::Texture* texture = make_texture(resources, desc->get_data(), desc->w, desc->h, 1, false);

		if (texture)
		{
			resources.make_persistent(texture);
			m_temp_cache[key] = texture;
			m_temp_owner[key] = owner_uid;
		}

		return texture;
	}

	void ui_overlay_renderer::remove_temp_resources(u32 key)
	{
		for (auto it = m_temp_cache.begin(); it != m_temp_cache.end();)
		{
			const auto owner = m_temp_owner.find(it->first);

			if (owner != m_temp_owner.end() && owner->second == key)
			{
				m_resources->defer_release(it->second);
				m_temp_owner.erase(owner);
				it = m_temp_cache.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void ui_overlay_renderer::run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants,
		const areau& viewport, MTL::Texture* target, rsx::overlays::overlay& ui)
	{
		ui.set_render_viewport(
			static_cast<u16>(std::min<u32>(viewport.width(), std::numeric_limits<u16>::max())),
			static_cast<u16>(std::min<u32>(viewport.height(), std::numeric_limits<u16>::max())));

		if (ui.status_flags & rsx::overlays::status_bits::invalidate_image_cache)
		{
			remove_temp_resources(ui.uid);
			ui.status_flags.clear(rsx::overlays::status_bits::invalidate_image_cache);
		}

		const f32 virtual_width = static_cast<f32>(ui.get_virtual_width());
		const f32 virtual_height = static_cast<f32>(ui.get_virtual_height());

		for (auto& command : ui.get_compiled().draw_commands)
		{
			const auto& config = command.config;
			const u32 vertex_count = static_cast<u32>(command.verts.size());

			if (!vertex_count)
			{
				continue;
			}

			// Resolve the command's source first: its sampling mode belongs to the
			// uniform block below.
			MTL::Texture* texture = nullptr;
			bool font_array = false;

			switch (config.texture_ref)
			{
			case rsx::overlays::image_resource_id::font_file:
				texture = find_font(*m_resources, config.font_ref);
				font_array = texture && texture->arrayLength() > 1;
				break;
			case rsx::overlays::image_resource_id::raw_image:
				texture = find_temp_image(*m_resources, static_cast<const rsx::overlays::image_info_base*>(config.external_data_ref), ui.uid);
				break;
			case rsx::overlays::image_resource_id::game_icon:
			case rsx::overlays::image_resource_id::backbuffer:
			case rsx::overlays::image_resource_id::none:
				break;
			default:
			{
				const auto found = m_view_cache.find(config.texture_ref);
				texture = (found != m_view_cache.end()) ? found->second : nullptr;
				break;
			}
			}

			const bool sampled = texture != nullptr;

			// 00: ui_scale      16: albedo        32: viewport
			// 48: clip_bounds   64: vertex_config
			f32* head = reinterpret_cast<f32*>(m_uniform);
			head[0] = virtual_width;
			head[1] = virtual_height;
			head[2] = 1.0f;
			head[3] = 1.0f;
			std::memcpy(head + 4, config.color.rgba, sizeof(config.color.rgba));
			head[8] = static_cast<f32>(viewport.width());
			head[9] = static_cast<f32>(viewport.height());
			head[10] = static_cast<f32>(viewport.x1);
			head[11] = static_cast<f32>(viewport.y1);
			head[12] = config.clip_rect.x1;
			head[13] = config.clip_rect.y1;
			head[14] = config.clip_rect.x2;
			head[15] = config.clip_rect.y2;

			rsx::overlays::vertex_options vertex_options;
			head[16] = std::bit_cast<f32>(static_cast<u32>(vertex_options.disable_vertex_snap(config.disable_vertex_snap).get()));

			// 68: fragment_config  72: timestamp  76: blur_intensity
			// 80: sdf_params       96: sdf_origin  112: sdf_border_color
			rsx::overlays::texture_sampling_mode sampling_mode = rsx::overlays::texture_sampling_mode::none;

			if (sampled)
			{
				sampling_mode = font_array
					? rsx::overlays::texture_sampling_mode::font3D
					: (config.texture_ref == rsx::overlays::image_resource_id::font_file
						? rsx::overlays::texture_sampling_mode::font2D
						: rsx::overlays::texture_sampling_mode::texture2D);
			}

			rsx::overlays::fragment_options fragment_options;
			const u32 fragment_config = fragment_options
				.texture_mode(sampling_mode)
				.clip_fragments(config.clip_region)
				.pulse_glow(config.pulse_glow)
				.set_sdf(config.sdf_config.func)
				.get();

			f32* fragment = reinterpret_cast<f32*>(m_uniform + 68);
			fragment[0] = std::bit_cast<f32>(fragment_config);
			fragment[1] = config.get_sinus_value();
			fragment[2] = static_cast<f32>(config.blur_strength) * 0.01f;

			rsx::overlays::compiled_resource::sdf_config_t sdf = config.sdf_config;
			sdf.transform(static_cast<areaf>(viewport), { virtual_width, virtual_height });
			fragment[3] = sdf.hx;
			fragment[4] = sdf.hy;
			fragment[5] = sdf.br;
			fragment[6] = sdf.bw;
			fragment[7] = sdf.cx;
			fragment[8] = sdf.cy;
			fragment[9] = 0.0f;
			fragment[10] = 0.0f;
			std::memcpy(fragment + 11, sdf.border_color.rgba, sizeof(sdf.border_color.rgba));

			// FS0 is a 2D texture and FS1 is a 2D array, so the font is on FS1 and
			// every other source is on FS0 (OverlayRenderFS.glsl).
			const std::vector<MTL::Texture*> sources = font_array
				? std::vector<MTL::Texture*>{ m_null_2d, texture }
				: std::vector<MTL::Texture*>{ texture ? texture : m_null_2d, m_null_array };

			const MTL::PrimitiveType topology = config.primitives == rsx::overlays::primitive_type::line_list
				? MTL::PrimitiveTypeLine
				: (config.primitives == rsx::overlays::primitive_type::line_strip ? MTL::PrimitiveTypeLineStrip : MTL::PrimitiveTypeTriangleStrip);

			encode(encoder, constants, viewport, target, topology, true, true,
				config.primitives == rsx::overlays::primitive_type::quad_list, m_uniform,
				command.verts.data(), vertex_count * overlay_vertex_stride, vertex_count, sources);
		}

		ui.update(get_system_time());
	}

	// -- video_out_calibration_pass ---------------------------------------------

	bool video_out_calibration_pass::init(const device& dev, resource_manager& resources)
	{
		return overlay_pass::init(dev, resources, passthrough_vertex_source(), calibration_fragment_source());
	}

	void video_out_calibration_pass::run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants,
		const areau& viewport, MTL::Texture* target, const rsx::simple_array<MTL::Texture*>& src, f32 gamma,
		bool limited_rgb, bool stereo_enabled)
	{
		static stereo_config stereo(true);
		stereo.update_from_config(stereo_enabled);
		const auto& matrices = stereo.matrices();

		output_config config{};
		config.gamma = gamma;
		config.limit_range = limited_rgb ? 1 : 0;
		config.stereo_display_mode = static_cast<s32>(stereo.stereo_mode());
		config.stereo_image_count = static_cast<s32>(std::min<u32>(static_cast<u32>(src.size()), 2u));

		for (u32 row = 0; row < 3; ++row)
		{
			std::memcpy(config.left_anaglyph_matrix[row], matrices.left[row].rgb, sizeof(matrices.left[row].rgb));
			std::memcpy(config.right_anaglyph_matrix[row], matrices.right[row].rgb, sizeof(matrices.right[row].rgb));
		}

		// The fragment source declares two plain 2D samplers, and the shader may
		// reach FS1 in a stereo mode even with one image, so both slots stay bound.
		const std::vector<MTL::Texture*> sources
		{
			src.size() > 0 && src[0] ? src[0] : m_null_2d,
			src.size() > 1 && src[1] ? src[1] : m_null_2d,
		};

		encode(encoder, constants, viewport, target, MTL::PrimitiveTypeTriangleStrip, false, false, false,
			&config, nullptr, 0, 4, sources);
	}

	// -- present_scale_pass -----------------------------------------------------

	bool present_scale_pass::init(const device& dev, resource_manager& resources, bool linear)
	{
		m_linear = linear;
		return overlay_pass::init(dev, resources, passthrough_vertex_source(), calibration_fragment_source(), linear);
	}

	void present_scale_pass::run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants,
		const areau& viewport, MTL::Texture* target, MTL::Texture* src)
	{
		// Gamma 1 with full range is the calibration pass with nothing to do, so the
		// one pass serves both the scaled and the calibrated present (HLD-06 §2.4).
		output_config config{};
		config.gamma = 1.0f;

		const std::vector<MTL::Texture*> sources{ src ? src : m_null_2d, m_null_2d };

		encode(encoder, constants, viewport, target, MTL::PrimitiveTypeTriangleStrip, false, false, false,
			&config, nullptr, 0, 4, sources);
	}
}

#endif // RSX_ENABLE_METAL4
