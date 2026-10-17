#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"

#include <array>

namespace mtl
{
	// The latched `blend` cluster state (LLD-blend §2).  The latch is read once per
	// draw; the derivations below consume it instead of re-reading the register file.
	struct blend_state
	{
		// PSO material (LLD-blend §3.1, §3.8).  The blend colour is absent: it is
		// dynamic encoder state and never enters the cached object (§3.4).
		u32 enable_mask = 0;
		rsx::blend_factor source_rgb = rsx::blend_factor::one;
		rsx::blend_factor source_a = rsx::blend_factor::one;
		rsx::blend_factor destination_rgb = rsx::blend_factor::zero;
		rsx::blend_factor destination_a = rsx::blend_factor::zero;
		rsx::blend_equation equation_rgb = rsx::blend_equation::add;
		rsx::blend_equation equation_a = rsx::blend_equation::add;

		// Shader material (LLD-blend §3.5, §3.6, §3.7).  The alpha test and the
		// logic op change the MSL, so they key the shader, not the PSO.  The alpha
		// function and the alpha reference are runtime fragment-context uniforms
		// (MTLFragmentProgram.cpp:433-435), so the latch decodes them but the PSO
		// key does not carry them: keying on them would create a pipeline object per
		// reference value.  Dither is absent: Metal has no dither state and the
		// design drops it (§2.7).
		bool alpha_test_enabled = false;
		rsx::comparison_function alpha_func = rsx::comparison_function::always;
		f32 alpha_ref = 0.0f;
		bool logic_op_enabled = false;
		rsx::logic_op logic_operation = rsx::logic_op::logic_copy;
		rsx::shading_mode shade_mode = rsx::shading_mode::smooth;

		// The blend-enable bit of one attachment, after the MRT mask is applied
		// (rsx_methods.h:493-497).
		bool blend_enabled(u32 index) const;
	};

	blend_state capture_blend_state();

	// The constant blend colour, encoder dynamic state (LLD-blend §3.4).  It reads
	// SET_BLEND_COLOR and SET_BLEND_COLOR2 through the shared helper, which selects
	// the 8-bit or the fp16 path from the surface format (rsx_utils.cpp:82-103).
	std::array<f32, 4> capture_blend_color();

	// SET_BLEND_FUNC_SFACTOR / SET_BLEND_FUNC_DFACTOR -> MTL::BlendFactor
	// (LLD-blend §3.2).  All fifteen RSX factors map directly.
	MTL::BlendFactor get_blend_factor(rsx::blend_factor op);

	// SET_BLEND_EQUATION -> MTL::BlendOperation (LLD-blend §3.3).  The three signed
	// equations have no Metal equivalent and fall back to the unsigned operation.
	MTL::BlendOperation get_blend_operation(rsx::blend_equation op);

	// Fill the blend fields of every colour attachment (LLD-blend §3.1).  Logic-op
	// precedence disables every attachment (invariant 2).  The pixel format and the
	// write mask belong to `surface` and `depth-rop`, not here.
	void apply_blend_state(MTL4::RenderPipelineDescriptor* descriptor, const blend_state& state, u32 attachment_count);
}

#endif // RSX_ENABLE_METAL4
