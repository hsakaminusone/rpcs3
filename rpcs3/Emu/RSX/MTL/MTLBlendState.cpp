#include "MTLBlendState.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/rsx_utils.h"
#include "util/logs.hpp"

namespace mtl
{
	bool blend_state::blend_enabled(u32 index) const
	{
		return (enable_mask >> index) & 1u;
	}

	blend_state capture_blend_state()
	{
		blend_state state;

		state.enable_mask = rsx::method_registers.blend_enabled_mask();
		state.source_rgb = rsx::method_registers.blend_func_sfactor_rgb();
		state.source_a = rsx::method_registers.blend_func_sfactor_a();
		state.destination_rgb = rsx::method_registers.blend_func_dfactor_rgb();
		state.destination_a = rsx::method_registers.blend_func_dfactor_a();
		state.equation_rgb = rsx::method_registers.blend_equation_rgb();
		state.equation_a = rsx::method_registers.blend_equation_a();

		state.alpha_test_enabled = rsx::method_registers.alpha_test_enabled();
		state.alpha_func = rsx::method_registers.alpha_func();
		state.alpha_ref = rsx::method_registers.alpha_ref();

		state.logic_op_enabled = rsx::method_registers.logic_op_enabled();
		state.logic_operation = rsx::method_registers.logic_operation();
		state.shade_mode = rsx::method_registers.shade_mode();

		// SET_DITHER_ENABLE is dropped: Metal has no dither state and the effect
		// is not guest-observable (LLD-blend §2.7).

		return state;
	}

	std::array<f32, 4> capture_blend_color()
	{
		return rsx::get_constant_blend_colors();
	}

	MTL::BlendFactor get_blend_factor(rsx::blend_factor op)
	{
		switch (op)
		{
		case rsx::blend_factor::zero: return MTL::BlendFactorZero;
		case rsx::blend_factor::one: return MTL::BlendFactorOne;
		case rsx::blend_factor::src_color: return MTL::BlendFactorSourceColor;
		case rsx::blend_factor::one_minus_src_color: return MTL::BlendFactorOneMinusSourceColor;
		case rsx::blend_factor::src_alpha: return MTL::BlendFactorSourceAlpha;
		case rsx::blend_factor::one_minus_src_alpha: return MTL::BlendFactorOneMinusSourceAlpha;
		case rsx::blend_factor::dst_alpha: return MTL::BlendFactorDestinationAlpha;
		case rsx::blend_factor::one_minus_dst_alpha: return MTL::BlendFactorOneMinusDestinationAlpha;
		case rsx::blend_factor::dst_color: return MTL::BlendFactorDestinationColor;
		case rsx::blend_factor::one_minus_dst_color: return MTL::BlendFactorOneMinusDestinationColor;
		case rsx::blend_factor::src_alpha_saturate: return MTL::BlendFactorSourceAlphaSaturated;
		case rsx::blend_factor::constant_color: return MTL::BlendFactorBlendColor;
		case rsx::blend_factor::one_minus_constant_color: return MTL::BlendFactorOneMinusBlendColor;
		case rsx::blend_factor::constant_alpha: return MTL::BlendFactorBlendAlpha;
		case rsx::blend_factor::one_minus_constant_alpha: return MTL::BlendFactorOneMinusBlendAlpha;
		default:
			rsx_log.error("Metal 4: unknown blend factor 0x%x", static_cast<u32>(op));
			return MTL::BlendFactorOne;
		}
	}

	MTL::BlendOperation get_blend_operation(rsx::blend_equation op)
	{
		switch (op)
		{
		case rsx::blend_equation::add: return MTL::BlendOperationAdd;
		case rsx::blend_equation::subtract: return MTL::BlendOperationSubtract;
		case rsx::blend_equation::reverse_subtract: return MTL::BlendOperationReverseSubtract;
		case rsx::blend_equation::min: return MTL::BlendOperationMin;
		case rsx::blend_equation::max: return MTL::BlendOperationMax;
		case rsx::blend_equation::add_signed:
			rsx_log.error("Metal 4: blend equation add_signed has no MTLBlendOperation; using Add");
			return MTL::BlendOperationAdd;
		case rsx::blend_equation::reverse_add_signed:
			rsx_log.error("Metal 4: blend equation reverse_add_signed has no MTLBlendOperation; using Add");
			return MTL::BlendOperationAdd;
		case rsx::blend_equation::reverse_subtract_signed:
			rsx_log.error("Metal 4: blend equation reverse_subtract_signed has no MTLBlendOperation; using ReverseSubtract");
			return MTL::BlendOperationReverseSubtract;
		default:
			rsx_log.error("Metal 4: unknown blend equation 0x%x", static_cast<u32>(op));
			return MTL::BlendOperationAdd;
		}
	}

	void apply_blend_state(MTL4::RenderPipelineDescriptor* descriptor, const blend_state& state, u32 attachment_count)
	{
		MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments = descriptor->colorAttachments();

		const MTL::BlendFactor source_rgb = get_blend_factor(state.source_rgb);
		const MTL::BlendFactor source_a = get_blend_factor(state.source_a);
		const MTL::BlendFactor destination_rgb = get_blend_factor(state.destination_rgb);
		const MTL::BlendFactor destination_a = get_blend_factor(state.destination_a);
		const MTL::BlendOperation equation_rgb = get_blend_operation(state.equation_rgb);
		const MTL::BlendOperation equation_a = get_blend_operation(state.equation_a);

		for (u32 index = 0; index < attachment_count; ++index)
		{
			// Logic op and blend are mutually exclusive; logic op wins
			// (LLD-blend §3.6, invariant 2).
			const bool enabled = !state.logic_op_enabled && state.blend_enabled(index);

			MTL4::RenderPipelineColorAttachmentDescriptor* attachment = attachments->object(index);
			attachment->setBlendingState(enabled ? MTL4::BlendStateEnabled : MTL4::BlendStateDisabled);
			attachment->setSourceRGBBlendFactor(source_rgb);
			attachment->setSourceAlphaBlendFactor(source_a);
			attachment->setDestinationRGBBlendFactor(destination_rgb);
			attachment->setDestinationAlphaBlendFactor(destination_a);
			attachment->setRgbBlendOperation(equation_rgb);
			attachment->setAlphaBlendOperation(equation_a);
		}
	}
}

#endif // RSX_ENABLE_METAL4
