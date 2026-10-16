#include "MTLPipeline.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/gcm_enums.h"
#include "MTLFormats.h"

#include <algorithm>

namespace mtl
{
	namespace
	{
		// `blend_state` has no operator==, so compare it field by field; a memcmp
		// would read padding (MTLBlendState.h:15-40).
		bool same_blend_state(const blend_state& a, const blend_state& b)
		{
			return a.enable_mask == b.enable_mask
				&& a.source_rgb == b.source_rgb
				&& a.source_a == b.source_a
				&& a.destination_rgb == b.destination_rgb
				&& a.destination_a == b.destination_a
				&& a.equation_rgb == b.equation_rgb
				&& a.equation_a == b.equation_a
				&& a.alpha_test_enabled == b.alpha_test_enabled
				&& a.alpha_func == b.alpha_func
				&& a.alpha_ref == b.alpha_ref
				&& a.logic_op_enabled == b.logic_op_enabled
				&& a.logic_operation == b.logic_operation
				&& a.shade_mode == b.shade_mode;
		}
	}

	bool operator==(const pipeline_properties& a, const pipeline_properties& b)
	{
		return a.color_format == b.color_format
			&& a.attachment_count == b.attachment_count
			&& a.sample_count == b.sample_count
			&& a.alpha_to_coverage == b.alpha_to_coverage
			&& a.alpha_to_one == b.alpha_to_one
			&& a.raster_enabled == b.raster_enabled
			&& same_blend_state(a.blend, b.blend)
			&& a.write_mask == b.write_mask
			&& a.input_primitive_topology == b.input_primitive_topology;
	}

	MTL::PrimitiveTopologyClass get_primitive_topology_class(rsx::primitive_type mode)
	{
		switch (mode)
		{
		case rsx::primitive_type::points:
			return MTL::PrimitiveTopologyClassPoint;
		case rsx::primitive_type::lines:
		case rsx::primitive_type::line_loop:
		case rsx::primitive_type::line_strip:
			return MTL::PrimitiveTopologyClassLine;
		case rsx::primitive_type::triangles:
		case rsx::primitive_type::triangle_strip:
		case rsx::primitive_type::triangle_fan:
		case rsx::primitive_type::quads:
		case rsx::primitive_type::quad_strip:
		case rsx::primitive_type::polygon:
			return MTL::PrimitiveTopologyClassTriangle;
		default:
			return MTL::PrimitiveTopologyClassTriangle;
		}
	}

	void apply_pipeline_properties(MTL4::RenderPipelineDescriptor* descriptor, const pipeline_properties& properties)
	{
		if (!descriptor)
		{
			return;
		}

		descriptor->setRasterSampleCount(properties.sample_count);

		// Alpha-to-coverage and alpha-to-one are descriptor state in Metal 4, not
		// encoder state (CONFLICTS.md C7).
		descriptor->setAlphaToCoverageState(properties.alpha_to_coverage ? MTL4::AlphaToCoverageStateEnabled : MTL4::AlphaToCoverageStateDisabled);
		descriptor->setAlphaToOneState(properties.alpha_to_one ? MTL4::AlphaToOneStateEnabled : MTL4::AlphaToOneStateDisabled);

		descriptor->setRasterizationEnabled(properties.raster_enabled);
		descriptor->setInputPrimitiveTopology(properties.input_primitive_topology);

		// The count is caller-supplied and color_target_mask is an unclamped popcount
		// (MTLSurfaceState.cpp), so bound the walk by the write-mask array.  The blend
		// applier takes the same count, or the two would disagree.
		const u32 attachment_count = std::min<u32>(properties.attachment_count, static_cast<u32>(properties.write_mask.size()));
		const MTL::PixelFormat color_format = get_surface_color_format(properties.color_format);
		MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments = descriptor->colorAttachments();

		for (u32 index = 0; index < attachment_count; ++index)
		{
			MTL4::RenderPipelineColorAttachmentDescriptor* attachment = attachments->object(index);
			attachment->setPixelFormat(color_format);
			attachment->setWriteMask(properties.write_mask[index]);
		}

		// The blend state, the six factors and the operations are the landed
		// applier's job; it does not touch the pixel format or the write mask above.
		apply_blend_state(descriptor, properties.blend, attachment_count);
	}
}

#endif // RSX_ENABLE_METAL4
