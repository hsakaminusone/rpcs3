#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

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

	MTL::CompareFunction get_compare_function(rsx::comparison_function op);
	MTL::StencilOperation get_stencil_operation(rsx::stencil_op op);
}

#endif // RSX_ENABLE_METAL4
