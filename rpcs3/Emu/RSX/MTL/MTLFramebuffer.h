#pragma once

#include "MTLSurfaceCache.h"
#include "MTLSurfaceState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Core/RSXFrameBuffer.h"

#include <array>

namespace mtl
{
	// The attachment set of the current RSX framebuffer (C1).  The store owns the
	// surfaces; this unit owns the pass that draws into them.  Metal has no
	// framebuffer object, so the set *is* the pass descriptor, rebuilt for each pass
	// (HLD-01 C3).
	class framebuffer
	{
	public:
		// Bind the surfaces the layout names and make them ready for drawing.  The
		// store decides reuse, aliasing and allocation (HLD-02 §2.5).  An unchanged
		// layout keeps the current set, so the walk runs once per framebuffer change
		// and not once per draw.
		void bind(command_recorder& cmd, surface_cache& cache, const rsx::framebuffer_layout& layout, const rsx::surface_scaling_config_t& scaling);

		// Open a pass over the bound attachments.  `clear_mask` is the RSX clear
		// mask, or 0 for a load-only pass.  A set bit becomes a clear load action,
		// which clears the whole attachment, so the caller asks
		// surface_state::clear_covers_attachment() first and uses fill_pass when it
		// does not (LLD-surface invariant 4).
		MTL4::RenderCommandEncoder* open_pass(command_recorder& cmd, u32 clear_mask, const surface_state& state);

		// True when the bound set is byte-for-byte the one the open pass was built
		// from, so the next draw can be encoded into the same pass.  The pass key is
		// the descriptor's inputs, not the framebuffer layout alone: a surface swap,
		// a resolve-target change, a sample-count change or a zcull buffer change
		// all require a new descriptor (C1).
		bool can_absorb() const;

		// Forget the open pass.  Called whenever the bound set is rebuilt.
		void reset();

		void reset_pass_key() { m_has_pass_key = false; }

		// The occlusion-query counter buffer.  Every pass carries it, because a
		// counter must keep its value across the passes of one query
		// (MTLZCULL.h).  The zcull backend owns the buffer and sets it once.
		void set_visibility_buffer(MTL::Buffer* buffer) { m_visibility = buffer; }

		bool valid() const { return m_color_count != 0 || m_depth != nullptr; }
		u8 color_count() const { return m_color_count; }
		u8 sample_count() const { return m_sample_count; }
		bool has_depth() const { return m_depth != nullptr; }
		surface* color(u32 index) const { return index < 4 ? m_color[index] : nullptr; }
		surface* depth() const { return m_depth; }
		u16 width() const { return m_width; }
		u16 height() const { return m_height; }

	private:
		// The exact inputs of the open pass descriptor.  A draw that would rebuild
		// the same descriptor can share the encoder; anything else opens a new pass.
		struct pass_key
		{
			rsx::framebuffer_layout layout{};
			std::array<MTL::Texture*, 4> color{};
			std::array<MTL::Texture*, 4> resolve{};
			MTL::Texture* depth = nullptr;
			MTL::Texture* depth_resolve = nullptr;
			MTL::Buffer* visibility = nullptr;
			u8 color_count = 0;
			u8 sample_count = 1;
			u16 width = 0;
			u16 height = 0;

			bool matches(const pass_key& other) const;
		};

		pass_key current_key() const;

		std::array<surface*, 4> m_color{};
		surface* m_depth = nullptr;
		MTL::Buffer* m_visibility = nullptr;
		rsx::framebuffer_layout m_layout{};
		pass_key m_pass_key{};
		u8 m_color_count = 0;
		u8 m_sample_count = 1;
		u16 m_width = 0;
		u16 m_height = 0;
		bool m_has_layout = false;
		bool m_has_pass_key = false;
	};

	// True when two layouts name the same attachment set, so the bound set is still
	// current.  The serialization tag and the write-enable flags are not part of it:
	// the first changes with every store tag and the second is a property of the
	// surfaces, not of the pass.
	bool same_framebuffer(const rsx::framebuffer_layout& a, const rsx::framebuffer_layout& b);
}

#endif // RSX_ENABLE_METAL4
