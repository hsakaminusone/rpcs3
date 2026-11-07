#include "MTLDepthStencil.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Core/RSXDrawState.h"
#include "Emu/RSX/Common/TextureUtils.h"
#include "util/fnv_hash.hpp"
#include "util/logs.hpp"

#include <algorithm>

namespace mtl
{
	MTL::CompareFunction get_compare_function(rsx::comparison_function op, bool reverse)
	{
		switch (op)
		{
		case rsx::comparison_function::never: return MTL::CompareFunctionNever;
		case rsx::comparison_function::less: return reverse ? MTL::CompareFunctionGreater : MTL::CompareFunctionLess;
		case rsx::comparison_function::equal: return MTL::CompareFunctionEqual;
		case rsx::comparison_function::less_or_equal: return reverse ? MTL::CompareFunctionGreaterEqual : MTL::CompareFunctionLessEqual;
		case rsx::comparison_function::greater: return reverse ? MTL::CompareFunctionLess : MTL::CompareFunctionGreater;
		case rsx::comparison_function::not_equal: return MTL::CompareFunctionNotEqual;
		case rsx::comparison_function::greater_or_equal: return reverse ? MTL::CompareFunctionLessEqual : MTL::CompareFunctionGreaterEqual;
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

	depth_stencil_state capture_depth_stencil_state(const rsx::depth_stencil_state& decoded)
	{
		depth_stencil_state state;

		// SET_DEPTH_TEST_ENABLE has no Metal counterpart: a disabled test becomes
		// compare Always and stops the depth write (CONFLICTS.md C2).  The decoded
		// function is only valid while the test is enabled.
		if (decoded.depth_test_enabled)
		{
			state.depth_compare = decoded.depth_func;
			state.depth_write = decoded.depth_write_enabled;
		}

		if (decoded.stencil_test_enabled)
		{
			auto& front = state.front;
			front.compare = decoded.front.func;
			front.fail = decoded.front.op_fail;
			front.zfail = decoded.front.op_zfail;
			front.zpass = decoded.front.op_zpass;
			front.read_mask = decoded.front.func_mask;
			front.write_mask = decoded.front.write_mask;

			// Metal always carries both faces; a disabled two-sided test mirrors the
			// front (LLD-depth-rop §2.3).  The decoded back face is only valid for a
			// two-sided test.
			if (decoded.two_sided_stencil_test_enabled)
			{
				auto& back = state.back;
				back.compare = decoded.back.func;
				back.fail = decoded.back.op_fail;
				back.zfail = decoded.back.op_zfail;
				back.zpass = decoded.back.op_zpass;
				back.read_mask = decoded.back.func_mask;
				back.write_mask = decoded.back.write_mask;
			}
			else
			{
				state.back = front;
			}
		}

		return state;
	}

	stencil_references capture_stencil_references(const rsx::depth_stencil_state& decoded)
	{
		// A single-sided RSX draw applies the front reference to both faces
		// (GLDraw.cpp:148-156; VKGSRender.cpp:351-355).
		const u8 front = decoded.front.func_ref;
		const u8 back = decoded.two_sided_stencil_test_enabled
			? decoded.back.func_ref
			: front;

		return { front, back };
	}

	depth_bounds capture_depth_bounds(const rsx::depth_stencil_state& decoded, bool emulate_depth_range)
	{
		if (!decoded.depth_bounds_test_enabled)
		{
			// The disabled bounds are an open range, which on Metal is [0,1]: the VK
			// form (`VKDraw.cpp:251-256`) collapses into the clamp below.  The decoded
			// min/max are stale in this case and must not be read.
			return {};
		}

		// The core already saturated the bounds into the bound depth format's range.
		depth_bounds state;
		state.min = decoded.depth_bounds_min;
		state.max = decoded.depth_bounds_max;

		if (emulate_depth_range)
		{
			state.min = rsx::encode_emulated_depth(state.min);
			state.max = rsx::encode_emulated_depth(state.max);
		}

		state.min = std::clamp(state.min, 0.0f, 1.0f);
		state.max = std::clamp(state.max, 0.0f, 1.0f);
		return state;
	}

	anti_aliasing_state capture_anti_aliasing_state(const rsx::multisample_state& decoded)
	{
		anti_aliasing_state state;
		state.alpha_to_coverage = decoded.alpha_to_coverage_enabled;
		state.alpha_to_one = decoded.alpha_to_one_enabled;
		return state;
	}

	MTL::ColorWriteMask capture_color_write_mask(const rsx::color_write_mask& mask)
	{
		// The core remapped the channels to the host order and filtered them to the
		// channels the surface format stores; this is a direct translation.
		MTL::ColorWriteMask result = MTL::ColorWriteMaskNone;
		if (mask.r) result |= MTL::ColorWriteMaskRed;
		if (mask.g) result |= MTL::ColorWriteMaskGreen;
		if (mask.b) result |= MTL::ColorWriteMaskBlue;
		if (mask.a) result |= MTL::ColorWriteMaskAlpha;
		return result;
	}

	MTL::DepthClipMode capture_depth_clip_mode(const rsx::depth_stencil_state& decoded)
	{
		// SET_ZMIN_MAX_CONTROL carries clip (b0) and clamp (b4) independently
		// (rsx_decode.h:634-647); Metal carries one bit.  Clip only when the RSX
		// clips and does not clamp, so the "both" and "neither" cases draw instead
		// of discarding (LLD-depth-rop §3.3 leaves those two cases open).
		const bool clip = decoded.depth_clip_enabled && !decoded.depth_clamp_enabled;
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
		// The comparison and stencil-op enums are 16-bit GL constants, so each
		// field is hashed at its own width.  A packed bit field cannot hold them:
		// three bits per operation overlapped the mask fields, and `keep` and
		// `zero` share their low three bits, so distinct states collapsed.
		auto hash_face = [](usz hash, const stencil_face& face)
		{
			hash = rpcs3::hash64(hash, static_cast<u16>(face.compare));
			hash = rpcs3::hash64(hash, static_cast<u16>(face.fail));
			hash = rpcs3::hash64(hash, static_cast<u16>(face.zfail));
			hash = rpcs3::hash64(hash, static_cast<u16>(face.zpass));
			hash = rpcs3::hash64(hash, face.read_mask);
			return rpcs3::hash64(hash, face.write_mask);
		};

		usz hash = rpcs3::hash64(rpcs3::fnv_seed, static_cast<u16>(state.depth_compare));
		hash = rpcs3::hash64(hash, static_cast<u8>(state.depth_write));
		hash = hash_face(hash, state.front);
		return hash_face(hash, state.back);
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
