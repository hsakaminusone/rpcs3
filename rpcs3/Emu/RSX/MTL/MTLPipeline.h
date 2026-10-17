#pragma once

#include "MTLBlendState.h"

#ifdef RSX_ENABLE_METAL4

#include "util/fnv_hash.hpp"

#include <array>

namespace mtl
{
	// The PSO-relevant state of one draw: the Metal backend's
	// `backend_traits::pipeline_properties` (Program/ProgramStateCache.h:165,174-179).
	// It holds exactly the fields an immutable `MTL4::RenderPipelineDescriptor` is
	// built from, so two draws that differ in any field need a different pipeline
	// object and two that agree produce the same object.  The key must stay complete:
	// a field left out here silently reuses a wrong pipeline.
	//
	// Metal 4 keeps the cull mode, the winding, the fill mode, the depth bias, the
	// clip mode, the viewport and the scissor on the `MTL4::RenderCommandEncoder`
	// (MTLEncoderState.h), and the depth and stencil attachment pixel formats on the
	// render pass, so none of them can enter a cached object (CONFLICTS.md C7;
	// HLD-05 §3.5).  The depth-stencil object, the stencil reference and the blend
	// colour are dynamic state too.
	struct pipeline_properties
	{
		// The colour attachment format, the attachment set and the MSAA count
		// (HLD-05 §3.5; surface_state::sample_count/color_attachment_count).
		rsx::surface_color_format color_format{};
		u8 attachment_count = 0;
		u8 sample_count = 1;

		// NV4097_SET_ANTI_ALIASING_CONTROL (MTLDepthStencil.h).  Metal 4 carries both
		// on the descriptor, so they key the object (CONFLICTS.md C7).
		bool alpha_to_coverage = false;
		bool alpha_to_one = false;

		// A cull-both draw has no Metal cull mode, so the rasteriser is dropped
		// (culls_both_faces(); MTLRasterState.h:52).
		bool raster_enabled = true;

		// The whole latched `blend` cluster (MTLBlendState.h:15-40).  The alpha test
		// and the shade mode travel with it: they change the MSL, so a different value
		// already yields a different program id, and carrying them here therefore
		// costs no extra pipeline object.  `enable_mask` already has the MRT mask
		// applied (rsx_methods.h:493-497), so the effective per-attachment blend bits
		// are complete and no separate colour-target mask is needed.
		blend_state blend{};

		// One entry per attachment.  A `std::array` of the `NS::UInteger`-based
		// `MTL::ColorWriteMask` has no padding, so it hashes by value.
		std::array<MTL::ColorWriteMask, 4> write_mask{};

		// The collapsed input-assembly family.  Metal 4 carries the class, not the
		// primitive list, on the descriptor (MTL4RenderPipeline.hpp:185).
		MTL::PrimitiveTopologyClass input_primitive_topology = MTL::PrimitiveTopologyClassUnspecified;
	};

	bool operator==(const pipeline_properties& a, const pipeline_properties& b);

	// The RSX primitive family -> MTL::PrimitiveTopologyClass (HLD-05 §3.5).  Point,
	// line and triangle cover every `rsx::primitive_type` (gcm_enums.h:1253-1265).
	MTL::PrimitiveTopologyClass get_primitive_topology_class(rsx::primitive_type mode);

	// Build the immutable object from the key.  Blending is the landed applier's job
	// (MTLBlendState.h:60); the pixel format and the write mask are set here because
	// `apply_blend_state` does not (MTLBlendState.h:57-59).
	void apply_pipeline_properties(MTL4::RenderPipelineDescriptor* descriptor, const pipeline_properties& properties);
}

namespace rpcs3
{
	template <>
	inline usz hash_struct<mtl::pipeline_properties>(const mtl::pipeline_properties& properties)
	{
		// Combine per-field hashes.  The struct has padding, so a whole-struct byte
		// hash would not be deterministic; XOR the individual fields instead.
		usz seed = hash_struct(properties.color_format);
		seed ^= hash_struct(properties.attachment_count);
		seed ^= hash_struct(properties.sample_count);
		seed ^= hash_struct(properties.alpha_to_coverage);
		seed ^= hash_struct(properties.alpha_to_one);
		seed ^= hash_struct(properties.raster_enabled);

		seed ^= hash_struct(properties.blend.enable_mask);
		seed ^= hash_struct(properties.blend.source_rgb);
		seed ^= hash_struct(properties.blend.source_a);
		seed ^= hash_struct(properties.blend.destination_rgb);
		seed ^= hash_struct(properties.blend.destination_a);
		seed ^= hash_struct(properties.blend.equation_rgb);
		seed ^= hash_struct(properties.blend.equation_a);
		seed ^= hash_struct(properties.blend.alpha_test_enabled);
		seed ^= hash_struct(properties.blend.alpha_func);
		// -0.0f and +0.0f compare equal, so the hash must agree: normalise the sign.
		seed ^= hash_struct(properties.blend.alpha_ref + 0.0f);
		seed ^= hash_struct(properties.blend.logic_op_enabled);
		seed ^= hash_struct(properties.blend.logic_operation);
		seed ^= hash_struct(properties.blend.shade_mode);

		seed ^= hash_struct(properties.write_mask);
		seed ^= hash_struct(properties.input_primitive_topology);
		return hash_base(seed);
	}
}

#endif // RSX_ENABLE_METAL4
