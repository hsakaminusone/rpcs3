#include "MTLDepthStencil.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Utils/color_utils.hpp"
#include "util/logs.hpp"

namespace mtl
{
	MTL::CompareFunction get_compare_function(rsx::comparison_function op)
	{
		switch (op)
		{
		case rsx::comparison_function::never: return MTL::CompareFunctionNever;
		case rsx::comparison_function::less: return MTL::CompareFunctionLess;
		case rsx::comparison_function::equal: return MTL::CompareFunctionEqual;
		case rsx::comparison_function::less_or_equal: return MTL::CompareFunctionLessEqual;
		case rsx::comparison_function::greater: return MTL::CompareFunctionGreater;
		case rsx::comparison_function::not_equal: return MTL::CompareFunctionNotEqual;
		case rsx::comparison_function::greater_or_equal: return MTL::CompareFunctionGreaterEqual;
		case rsx::comparison_function::always: return MTL::CompareFunctionAlways;
		default:
			rsx_log.error("Metal 4: unknown compare function 0x%x", static_cast<u32>(op));
			return MTL::CompareFunctionAlways;
		}
	}

	MTL::StencilOperation get_stencil_operation(rsx::stencil_op op)
	{
		switch (op)
		{
		case rsx::stencil_op::keep: return MTL::StencilOperationKeep;
		case rsx::stencil_op::zero: return MTL::StencilOperationZero;
		case rsx::stencil_op::replace: return MTL::StencilOperationReplace;
		case rsx::stencil_op::incr: return MTL::StencilOperationIncrementClamp;
		case rsx::stencil_op::decr: return MTL::StencilOperationDecrementClamp;
		case rsx::stencil_op::invert: return MTL::StencilOperationInvert;
		case rsx::stencil_op::incr_wrap: return MTL::StencilOperationIncrementWrap;
		case rsx::stencil_op::decr_wrap: return MTL::StencilOperationDecrementWrap;
		default:
			rsx_log.error("Metal 4: unknown stencil op 0x%x", static_cast<u32>(op));
			return MTL::StencilOperationKeep;
		}
	}

	depth_stencil_state capture_depth_stencil_state()
	{
		depth_stencil_state state;

		// SET_DEPTH_TEST_ENABLE has no Metal counterpart: a disabled test becomes
		// compare Always and stops the depth write (CONFLICTS.md C2).
		if (rsx::method_registers.depth_test_enabled())
		{
			state.depth_compare = rsx::method_registers.depth_func();
			state.depth_write = rsx::method_registers.depth_write_enabled();
		}

		if (rsx::method_registers.stencil_test_enabled())
		{
			auto& front = state.front;
			front.compare = rsx::method_registers.stencil_func();
			front.fail = rsx::method_registers.stencil_op_fail();
			front.zfail = rsx::method_registers.stencil_op_zfail();
			front.zpass = rsx::method_registers.stencil_op_zpass();
			front.read_mask = rsx::method_registers.stencil_func_mask();
			front.write_mask = rsx::method_registers.stencil_mask();

			// Metal always carries both faces; a disabled two-sided test mirrors the
			// front (LLD-depth-rop §2.3).
			if (rsx::method_registers.two_sided_stencil_test_enabled())
			{
				auto& back = state.back;
				back.compare = rsx::method_registers.back_stencil_func();
				back.fail = rsx::method_registers.back_stencil_op_fail();
				back.zfail = rsx::method_registers.back_stencil_op_zfail();
				back.zpass = rsx::method_registers.back_stencil_op_zpass();
				back.read_mask = rsx::method_registers.back_stencil_func_mask();
				back.write_mask = rsx::method_registers.back_stencil_mask();
			}
			else
			{
				state.back = front;
			}
		}

		return state;
	}

	stencil_references capture_stencil_references()
	{
		return { rsx::method_registers.stencil_func_ref(), rsx::method_registers.back_stencil_func_ref() };
	}

	MTL::ColorWriteMask capture_color_write_mask(u32 index, rsx::surface_color_format format)
	{
		bool red = rsx::method_registers.color_mask_r(index);
		bool green = rsx::method_registers.color_mask_g(index);
		bool blue = rsx::method_registers.color_mask_b(index);
		bool alpha = rsx::method_registers.color_mask_a(index);

		// The single- and two-channel formats store the RSX blue channel in the host
		// red slot (color_utils.hpp:267,275).
		switch (format)
		{
		case rsx::surface_color_format::b8:
			rsx::get_b8_colormask(red, green, blue, alpha);
			break;
		case rsx::surface_color_format::g8b8:
			rsx::get_g8b8_r8g8_colormask(red, green, blue, alpha);
			break;
		default:
			break;
		}

		// Drop the channels the format does not store (color_utils.hpp:300).
		const auto stored = rsx::get_write_output_mask(format);
		MTL::ColorWriteMask mask = MTL::ColorWriteMaskNone;
		if (red && stored[0]) mask |= MTL::ColorWriteMaskRed;
		if (green && stored[1]) mask |= MTL::ColorWriteMaskGreen;
		if (blue && stored[2]) mask |= MTL::ColorWriteMaskBlue;
		if (alpha && stored[3]) mask |= MTL::ColorWriteMaskAlpha;
		return mask;
	}

	MTL::DepthClipMode capture_depth_clip_mode()
	{
		// SET_ZMIN_MAX_CONTROL carries clip (b0) and clamp (b4) independently
		// (rsx_decode.h:634-647); Metal carries one bit.  Clip only when the RSX
		// clips and does not clamp, so the "both" and "neither" cases draw instead
		// of discarding (LLD-depth-rop §3.3 leaves those two cases open).
		const bool clip = rsx::method_registers.depth_clip_enabled() && !rsx::method_registers.depth_clamp_enabled();
		return clip ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp;
	}

	static void fill_stencil_descriptor(MTL::StencilDescriptor* descriptor, const stencil_face& face)
	{
		descriptor->setStencilCompareFunction(get_compare_function(face.compare));
		descriptor->setStencilFailureOperation(get_stencil_operation(face.fail));
		descriptor->setDepthFailureOperation(get_stencil_operation(face.zfail));
		descriptor->setDepthStencilPassOperation(get_stencil_operation(face.zpass));
		descriptor->setReadMask(face.read_mask);
		descriptor->setWriteMask(face.write_mask);
	}

	MTL::DepthStencilState* create_depth_stencil_state(MTL::Device* device, const depth_stencil_state& state)
	{
		MTL::StencilDescriptor* front = MTL::StencilDescriptor::alloc()->init();
		MTL::StencilDescriptor* back = MTL::StencilDescriptor::alloc()->init();
		fill_stencil_descriptor(front, state.front);
		fill_stencil_descriptor(back, state.back);

		MTL::DepthStencilDescriptor* descriptor = MTL::DepthStencilDescriptor::alloc()->init();
		descriptor->setDepthCompareFunction(get_compare_function(state.depth_compare));
		descriptor->setDepthWriteEnabled(state.depth_write);
		descriptor->setFrontFaceStencil(front);
		descriptor->setBackFaceStencil(back);

		MTL::DepthStencilState* result = device->newDepthStencilState(descriptor);

		descriptor->release();
		back->release();
		front->release();
		return result;
	}
}

#endif // RSX_ENABLE_METAL4
