#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

#include <unordered_map>

namespace mtl
{
	// One stencil face (LLD-depth-rop §3.1).  The reference is deliberately absent:
	// it is dynamic encoder state and never enters the cached object (invariant 3).
	struct stencil_face
	{
		rsx::comparison_function compare = rsx::comparison_function::always;
		rsx::stencil_op fail = rsx::stencil_op::keep;
		rsx::stencil_op zfail = rsx::stencil_op::keep;
		rsx::stencil_op zpass = rsx::stencil_op::keep;
		u8 read_mask = 0xff;
		u8 write_mask = 0xff;
	};

	// The latched `depth-rop` depth/stencil state (LLD-depth-rop §2.3).  This is the
	// key of the MTL::DepthStencilState, so it holds exactly the 19 registers that
	// build the object and nothing dynamic (HLD-05 §3.4/§3.5).
	struct depth_stencil_state
	{
		rsx::comparison_function depth_compare = rsx::comparison_function::always;
		bool depth_write = false;

		stencil_face front{};
		stencil_face back{};
	};

	// The per-face stencil references (LLD-depth-rop §3.1): dynamic encoder state.
	struct stencil_references
	{
		u8 front = 0;
		u8 back = 0;
	};

	depth_stencil_state capture_depth_stencil_state();
	stencil_references capture_stencil_references();

	// NV4097_SET_DEPTH_BOUNDS_MIN/MAX/TEST_ENABLE: dynamic encoder state.  The RSX
	// range is saturated into the depth format's range and, when the shader
	// compressed Z, encoded the same way (VKDraw.cpp:240-271; LLD-raster §3.2).
	struct depth_bounds
	{
		f32 min = 0.0f;
		f32 max = 1.0f;
	};

	// `emulate_depth_range` is the draw's RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE.
	depth_bounds capture_depth_bounds(bool emulate_depth_range);

	// The latch is the key of the cached MTL::DepthStencilState, so it needs value
	// semantics.  A memcmp would read the padding the members leave behind.
	bool operator==(const stencil_face& a, const stencil_face& b);
	bool operator==(const depth_stencil_state& a, const depth_stencil_state& b);
	u64 hash_depth_stencil_state(const depth_stencil_state& state);

	// NV4097_SET_ANTI_ALIASING_CONTROL (LLD-depth-rop §2.4).  Alpha-to-coverage and
	// alpha-to-one are descriptor state on Metal 4, not encoder state, so they are
	// captured here and consumed by the pipeline key (CONFLICTS.md C7;
	// M_NV4097_SET_ANTI_ALIASING_CONTROL).  The per-draw sample mask has no Metal
	// encoder setter and travels in the MSL as [[sample_mask]].
	struct anti_aliasing_state
	{
		bool alpha_to_coverage = false;
		bool alpha_to_one = false;
	};

	anti_aliasing_state capture_anti_aliasing_state();

	// The colour write mask of one attachment (LLD-depth-rop §3.2).  The RSX channel
	// mask is reconciled with the host channel order, as the surface format decides
	// the storage (color_utils.hpp:267,300; CONFLICTS.md C3).
	MTL::ColorWriteMask capture_color_write_mask(u32 index, rsx::surface_color_format format);

	// SET_ZMIN_MAX_CONTROL -> the encoder depth clip mode (LLD-depth-rop §3.3).
	MTL::DepthClipMode capture_depth_clip_mode();

	// Build the immutable object (LLD-depth-rop §3.1).  The caller owns the result.
	MTL::DepthStencilState* create_depth_stencil_state(MTL::Device* device, const depth_stencil_state& state);

	// The depth-stencil object cache (HLD-05 §3.4).  The latch has few bits and
	// the RSX cycles through a handful of states, so a state object is built once
	// and reused.  The object is never released while the cache lives, because a
	// Metal 4 command buffer holds no strong reference to the state it binds
	// (HLD-02 I7).
	class depth_stencil_cache
	{
	public:
		void init(MTL::Device* device) { m_device = device; }
		void shutdown();

		MTL::DepthStencilState* get(const depth_stencil_state& state);

	private:
		struct hasher
		{
			usz operator()(const depth_stencil_state& state) const { return hash_depth_stencil_state(state); }
		};

		MTL::Device* m_device = nullptr;
		std::unordered_map<depth_stencil_state, MTL::DepthStencilState*, hasher> m_cache;
	};

	// Bind the cached object for the latch.  This is the only path by which depth
	// state reaches the encoder (CONFLICTS.md C7).
	void apply_depth_stencil_state(MTL4::RenderCommandEncoder* encoder, depth_stencil_cache& cache, const depth_stencil_state& state);

	// `reverse` swaps the operand order.  The depth and stencil tests compare the
	// incoming value against the stored one, which is the Metal order; the shadow
	// sampler compares the reference against the texel, which is not
	// (REVIEW-FINDINGS.md, A7).
	MTL::CompareFunction get_compare_function(rsx::comparison_function op, bool reverse = false);
	MTL::StencilOperation get_stencil_operation(rsx::stencil_op op);
}

#endif // RSX_ENABLE_METAL4
