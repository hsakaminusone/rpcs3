#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "../Common/simple_array.hpp"
#include "../Overlays/overlay_controls.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace rsx::overlays
{
	struct overlay;
	class font;
}

namespace mtl
{
	class constants_heap;
	class resource_manager;

	// The output tier of the Metal backend (HLD-06 §2.6).  An overlay is not an RSX
	// draw: it is RPCS3's own output path drawing over the presented image, and the
	// calibration pass that finishes the same image.  Its shaders are the
	// backend-agnostic GLSL snippets (Program/GLSLSnippets), compiled by the same
	// glslang -> SPIR-V -> SPIRV-Cross route an RSX program takes (MTLShader.h), so
	// the geometry and the maths match the Vulkan and the OpenGL passes exactly.
	//
	// The pass owns its own argument indices, because the RSX map carries RSX state
	// the pass does not have.  MSL has no push constants, so the pass's uniform block
	// is a constant buffer at `overlay_uniform_slot` (MTLShader.h:rsx_push_slot); a
	// combined image sampler keeps its GLSL binding as both the texture and the
	// sampler index, so `fs0` is texture 0 / sampler 0.
	//
	// A pass opens nothing.  The present path opens one render pass on the drawable
	// and every output draw encodes into it, so the output is one Metal pass
	// (HLD-06 §2.4).
	class overlay_pass
	{
	public:
		virtual ~overlay_pass();

		static constexpr u32 overlay_uniform_slot = 0;
		static constexpr u32 overlay_vertex_slot = 1;
		static constexpr u32 overlay_texture_count = 2;

		// The largest uniform block of the output passes (the calibration pass at
		// 112 bytes, VKOverlays.cpp:233).  One ring block per draw, so the layout is
		// fixed and a shorter block writes only its own prefix.
		static constexpr u32 overlay_uniform_size = 128;

		// One vec4 per vertex (OverlayRenderVS.glsl).  The vertex data is read
		// through a vertex descriptor, because Metal 4 has no `setVertexBuffer`
		// (MTL4RenderCommandEncoder.hpp).
		static constexpr u32 overlay_vertex_stride = 16;

	protected:
		// The linked pipeline for one target shape.  The pipeline varies with the
		// drawable, which is only known at present time, so the pass keeps one per
		// key instead of one per pass.
		struct pipeline_entry
		{
			MTL::RenderPipelineState* state = nullptr;
		};

		// `vs`/`fs` are the GLSL the derived pass provides.  Both stages are
		// compiled once, here, because a pass's source never varies.  `linear`
		// selects the upscale filter.
		bool init(const device& dev, resource_manager& resources, std::string vs, std::string fs, bool linear = true);

		// The uniform block of the two output passes.  Every pass writes it whole,
		// because it is one ring block per draw and Metal 4 does not snapshot a
		// buffer's contents at encode time.
		struct output_config
		{
			f32 gamma;
			s32 limit_range;
			s32 stereo_display_mode;
			s32 stereo_image_count;
			f32 left_anaglyph_matrix[3][4];
			f32 right_anaglyph_matrix[3][4];
		};

		static_assert(sizeof(output_config) == 112);

		// One draw's arguments and geometry, encoded into an open pass.  `uniform`
		// is the pass's uniform block (overlay_uniform_size bytes at most) and
		// `vertices` the vertex data when the pass reads a vertex input.  `quad_list`
		// splits the vertices into independent 4-vertex triangle strips, which is how
		// the UI draws its quads (VKOverlays.cpp:616).  `src` are the combined image
		// samplers in index order.
		void encode(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, const areau& viewport,
			MTL::Texture* target, MTL::PrimitiveType topology, bool vertex_input, bool blend, bool quad_list,
			const void* uniform, const void* vertices, u32 vertex_bytes, u32 vertex_count,
			const std::vector<MTL::Texture*>& src);

		const device* m_device = nullptr;
		resource_manager* m_resources = nullptr;
		std::string m_vs_src;
		std::string m_fs_src;
		MTL::Library* m_vs_library = nullptr;
		MTL::Library* m_fs_library = nullptr;
		MTL::SamplerState* m_sampler = nullptr;
		// A pass always binds both samplers.  `fs0` and `fs1` declare different
		// texture types (OverlayRenderFS.glsl), so an unused slot needs a texture of
		// the declaration's own type.
		MTL::Texture* m_null_2d = nullptr;
		MTL::Texture* m_null_array = nullptr;
		// The pass's own tables: the RSX tables carry RSX state that the output path
		// does not have, and the output path runs after the frame's draws.
		MTL4::ArgumentTable* m_vertex_table = nullptr;
		MTL4::ArgumentTable* m_fragment_table = nullptr;
		std::unordered_map<u64, pipeline_entry> m_pipelines;

		// True when the pass's vertex stage emits Vulkan clip space, which is what the
		// backend's Vulkan-rule compile produces for the geometry snippets that carry
		// no orientation of their own (the passthrough quad).  Such a pass asks the
		// viewport to carry the Y flip.  The UI overlay geometry carries its own flip
		// in the snippet, so it leaves this false.
		bool m_vulkan_clip_space = false;

	private:
		MTL::RenderPipelineState* build_pipeline(MTL::PixelFormat format, u8 samples, MTL::PrimitiveType topology,
			bool vertex_input, bool blend);
	};

	// The RPCS3 UI renderer (the Metal tier of VKOverlays.cpp:284-710).  It owns the
	// font, the icon and the temporary-image textures the compiled UI references,
	// uploads them on first use, and keeps them until the owner that made a
	// temporary image goes away.
	class ui_overlay_renderer : public overlay_pass
	{
	public:
		bool init(const device& dev, resource_manager& resources);

		// Draw `ui` into the open pass, confined to `viewport`.
		void run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, const areau& viewport,
			MTL::Texture* target, rsx::overlays::overlay& ui);

		// Drop the temporary images an overlay owned (VKOverlays.cpp:410).
		void remove_temp_resources(u32 key);

		void destroy();

	private:
		MTL::Texture* find_font(resource_manager& resources, const rsx::overlays::font* font);
		MTL::Texture* find_temp_image(resource_manager& resources, const rsx::overlays::image_info_base* desc, u32 owner_uid);

		MTL::Texture* make_texture(resource_manager& resources, const void* pixels, u32 width, u32 height, u32 layers, bool font);
		void upload(MTL::Texture* texture, const void* pixels, u32 width, u32 height, u32 layers, bool font);

		std::unordered_map<u64, MTL::Texture*> m_view_cache; // icons and fonts, uploaded once
		std::unordered_map<u64, MTL::Texture*> m_temp_cache; // UI-owned images, keyed by the descriptor address
		std::unordered_map<u64, u32> m_temp_owner;           // the UI that owns each temporary image

		// The per-draw block laid out as OverlayRenderVS.glsl and
		// OverlayRenderFS.glsl read it (VKOverlays.cpp:517).
		alignas(16) u8 m_uniform[overlay_uniform_size]{};
	};

	// The calibration pass that finishes the presented image: gamma, limited range
	// and every stereo output mode (VKOverlays.cpp:880-966).
	class video_out_calibration_pass : public overlay_pass
	{
	public:
		bool init(const device& dev, resource_manager& resources);

		void run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, const areau& viewport,
			MTL::Texture* target, const rsx::simple_array<MTL::Texture*>& src, f32 gamma, bool limited_rgb,
			bool stereo_enabled);
	};

	// The plain upscale of the presented image, used when no calibration is needed.
	// One draw of the source over the aspect-ratio rect, filtered by `linear`.
	class present_scale_pass : public overlay_pass
	{
	public:
		bool init(const device& dev, resource_manager& resources, bool linear);

		void run(MTL4::RenderCommandEncoder* encoder, constants_heap& constants, const areau& viewport,
			MTL::Texture* target, MTL::Texture* src);

	private:
		bool m_linear = true;
	};
}

#endif // RSX_ENABLE_METAL4
