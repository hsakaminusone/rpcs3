#include "MTLDepthStencil.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/rsx_methods.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "Emu/RSX/Utils/color_utils.hpp"
#include "util/fnv_hash.hpp"
#include "util/logs.hpp"

#include <algorithm>

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
		// A single-sided RSX draw applies the front reference to both faces
		// (GLDraw.cpp:148-156; VKGSRender.cpp:351-355).
		const u8 front = rsx::method_registers.stencil_func_ref();
		const u8 back = rsx::method_registers.two_sided_stencil_test_enabled()
			? rsx::method_registers.back_stencil_func_ref()
			: front;

		return { front, back };
	}

	depth_bounds capture_depth_bounds(bool emulate_depth_range)
	{
		if (!rsx::method_registers.depth_bounds_test_enabled())
		{
			// The disabled bounds are an open range, which on Metal is [0,1]: the VK
			// form (`VKDraw.cpp:251-256`) collapses into the clamp below.
			return {};
		}

		const auto format = rsx::method_registers.surface_depth_fmt();
		depth_bounds state;
		state.min = rsx::clamp_depth_bounds_value(format, rsx::method_registers.depth_bounds_min());
		state.max = rsx::clamp_depth_bounds_value(format, rsx::method_registers.depth_bounds_max());

		if (emulate_depth_range)
		{
			state.min = rsx::encode_emulated_depth(state.min);
			state.max = rsx::encode_emulated_depth(state.max);
		}

		state.min = std::clamp(state.min, 0.0f, 1.0f);
		state.max = std::clamp(state.max, 0.0f, 1.0f);
		return state;
	}

	anti_aliasing_state capture_anti_aliasing_state()
	{
		anti_aliasing_state state;
		state.alpha_to_coverage = rsx::method_registers.msaa_alpha_to_coverage_enabled();
		state.alpha_to_one = rsx::method_registers.msaa_alpha_to_one_enabled();
		return state;
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

	bool operator==(const stencil_face& a, const stencil_face& b)
	{
		return a.compare == b.compare
			&& a.fail == b.fail
			&& a.zfail == b.zfail
			&& a.zpass == b.zpass
			&& a.read_mask == b.read_mask
			&& a.write_mask == b.write_mask;
	}

	bool operator==(const depth_stencil_state& a, const depth_stencil_state& b)
	{
		return a.depth_compare == b.depth_compare
			&& a.depth_write == b.depth_write
			&& a.front == b.front
			&& a.back == b.back;
	}

	u64 hash_depth_stencil_state(const depth_stencil_state& state)
	{
		// The latch is 19 small fields.  Pack each face into one word (three bits
		// per operation, eight per mask) and hash the three words, so the hash stays
		// consistent with operator== without touching padding.
		struct packed
		{
			u32 depth;
			u32 front;
			u32 back;
			u32 reserved = 0;
		};

		auto pack_face = [](const stencil_face& face) -> u32
		{
			return (static_cast<u32>(face.compare) << 0)
				| (static_cast<u32>(face.fail) << 3)
				| (static_cast<u32>(face.zfail) << 6)
				| (static_cast<u32>(face.zpass) << 9)
				| (static_cast<u32>(face.read_mask) << 12)
				| (static_cast<u32>(face.write_mask) << 20);
		};

		const packed key = {
			(static_cast<u32>(state.depth_compare) << 1) | static_cast<u32>(state.depth_write),
			pack_face(state.front),
			pack_face(state.back),
		};

		return rpcs3::hash_struct<packed>(key);
	}

	void depth_stencil_cache::shutdown()
	{
		for (const auto& [key, object] : m_cache)
		{
			object->release();
		}

		m_cache.clear();
		m_device = nullptr;
	}

	MTL::DepthStencilState* depth_stencil_cache::get(const depth_stencil_state& state)
	{
		const auto found = m_cache.find(state);
		if (found != m_cache.end())
		{
			return found->second;
		}

		MTL::DepthStencilState* object = create_depth_stencil_state(m_device, state);
		if (object)
		{
			m_cache.emplace(state, object);
		}

		return object;
	}

	void apply_depth_stencil_state(MTL4::RenderCommandEncoder* encoder, depth_stencil_cache& cache, const depth_stencil_state& state)
	{
		if (MTL::DepthStencilState* object = cache.get(state))
		{
			encoder->setDepthStencilState(object);
		}
	}
}

#endif // RSX_ENABLE_METAL4
