#include "MTLDevice.h"

#include "MTLFill.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLArgumentBinds.h"
#include "MTLDepthStencil.h"

#include "util/logs.hpp"

#include <cstring>
#include <string>

namespace mtl
{
	namespace
	{
		// The full-screen triangle covers the clip square with three vertices, so no
		// vertex buffer is bound.  The three fragment entries are the output sets a
		// Metal 4 pipeline still has to declare: the depth side has no attachment
		// format in the descriptor, but a shader that writes depth still has to be a
		// different function.  FILL_VALUES_SLOT is replaced with the ABI index, so
		// the shader and the table cannot drift apart.
		constexpr const char* fill_msl = R"(#include <metal_stdlib>
using namespace metal;

struct fill_values
{
    float4 color;
    float depth;
};

vertex float4 fill_vertex(uint vertex_id [[vertex_id]])
{
    const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

fragment float4 fill_fragment_color(constant fill_values& values [[buffer(FILL_VALUES_SLOT)]])
{
    return values.color;
}

struct fill_depth_output
{
    float depth [[depth(any)]];
};

fragment fill_depth_output fill_fragment_depth(constant fill_values& values [[buffer(FILL_VALUES_SLOT)]])
{
    fill_depth_output out;
    out.depth = values.depth;
    return out;
}

struct fill_combined_output
{
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment fill_combined_output fill_fragment_color_depth(constant fill_values& values [[buffer(FILL_VALUES_SLOT)]])
{
    fill_combined_output out;
    out.color = values.color;
    out.depth = values.depth;
    return out;
}
)";

		// One fill value set: the colour and the depth, in the buffer the shader reads.
		struct fill_values
		{
			f32 color[4];
			f32 depth;
		};

		std::string build_fill_source()
		{
			std::string source = fill_msl;
			const std::string token = "FILL_VALUES_SLOT";
			const std::string slot = std::to_string(static_cast<u32>(fill_values_slot));

			for (usz pos = source.find(token); pos != std::string::npos; pos = source.find(token, pos + slot.size()))
			{
				source.replace(pos, token.size(), slot);
			}

			return source;
		}
	}

	void fill_pass::init(const device& dev, resource_manager& resources)
	{
		m_device = dev.dev();
		m_compiler = dev.compiler();
		m_resources = &resources;

		if (!m_device || !m_compiler)
		{
			return;
		}

		const std::string source = build_fill_source();
		MTL4::LibraryDescriptor* descriptor = MTL4::LibraryDescriptor::alloc()->init();
		descriptor->setSource(NS::String::string(source.c_str(), NS::UTF8StringEncoding));

		NS::Error* error = nullptr;
		m_library = m_compiler->newLibrary(descriptor, &error);
		descriptor->release();

		if (!m_library)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: fill library creation failed: %s", message);
			return;
		}

		// The fill values are written on the CPU and read by the GPU, so the buffer
		// is shared and stays resident for the recorder's lifetime (HLD-02 §2.6).
		m_values = m_resources->new_buffer(sizeof(fill_values), MTL::StorageModeShared);
		if (!m_values)
		{
			rsx_log.error("Metal: fill value buffer allocation failed");
			return;
		}

		m_resources->make_persistent(m_values);

		// Two immutable objects: a fill that writes depth and one that does not.  The
		// depth test always passes, because a clear must not be rejected by the depth
		// state it is clearing (CONFLICTS.md C2).
		depth_stencil_state write_state;
		write_state.depth_write = true;
		m_depth_write = create_depth_stencil_state(m_device, write_state);

		depth_stencil_state read_state;
		m_depth_read = create_depth_stencil_state(m_device, read_state);

		if (!m_depth_write || !m_depth_read)
		{
			rsx_log.error("Metal: fill depth-stencil state creation failed");
		}
	}

	void fill_pass::shutdown()
	{
		for (const auto& [key, state] : m_pipelines)
		{
			state->release();
		}

		m_pipelines.clear();

		if (m_depth_write)
		{
			m_depth_write->release();
			m_depth_write = nullptr;
		}

		if (m_depth_read)
		{
			m_depth_read->release();
			m_depth_read = nullptr;
		}

		if (m_values)
		{
			m_resources->defer_release(m_values);
			m_values = nullptr;
		}

		if (m_library)
		{
			m_library->release();
			m_library = nullptr;
		}

		m_resources = nullptr;
		m_compiler = nullptr;
		m_device = nullptr;
	}

	MTL::RenderPipelineState* fill_pass::pipeline(MTL::PixelFormat color_format, variant kind, u8 sample_count)
	{
		const u64 key = (static_cast<u64>(color_format) << 10) | (static_cast<u64>(sample_count) << 2) | static_cast<u64>(kind);

		if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
		{
			return it->second;
		}

		const char* fragment_name = "fill_fragment_color";
		if (kind == variant::depth)
		{
			fragment_name = "fill_fragment_depth";
		}
		else if (kind == variant::color_depth)
		{
			fragment_name = "fill_fragment_color_depth";
		}

		MTL4::LibraryFunctionDescriptor* vertex_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		vertex_function->setLibrary(m_library);
		vertex_function->setName(NS::String::string("fill_vertex", NS::UTF8StringEncoding));

		MTL4::LibraryFunctionDescriptor* fragment_function = MTL4::LibraryFunctionDescriptor::alloc()->init();
		fragment_function->setLibrary(m_library);
		fragment_function->setName(NS::String::string(fragment_name, NS::UTF8StringEncoding));

		MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
		descriptor->setVertexFunctionDescriptor(vertex_function);
		descriptor->setFragmentFunctionDescriptor(fragment_function);
		descriptor->setRasterSampleCount(sample_count);

		if (kind != variant::depth)
		{
			MTL4::RenderPipelineColorAttachmentDescriptor* attachment = descriptor->colorAttachments()->object(0);
			attachment->setPixelFormat(color_format);
			attachment->setWriteMask(MTL::ColorWriteMaskAll);
		}

		NS::Error* error = nullptr;
		MTL::RenderPipelineState* state = m_compiler->newRenderPipelineState(descriptor, nullptr, &error);

		vertex_function->release();
		fragment_function->release();
		descriptor->release();

		if (!state)
		{
			const char* message = (error && error->localizedDescription()) ? error->localizedDescription()->utf8String() : "unknown";
			rsx_log.error("Metal: fill pipeline creation failed: %s", message);
			return nullptr;
		}

		m_pipelines.emplace(key, state);
		return state;
	}

	void fill_pass::fill(MTL4::ArgumentTable* table, MTL4::RenderCommandEncoder* encoder, MTL::PixelFormat color_format, u8 sample_count, const std::array<f32, 4>& color, f32 depth, bool write_color, bool write_depth)
	{
		if (!table || !encoder || !m_library || !m_values)
		{
			return;
		}

		fill_values values;
		std::memcpy(values.color, color.data(), sizeof(values.color));
		values.depth = depth;
		std::memcpy(m_values->contents(), &values, sizeof(values));

		const variant kind = write_color ? (write_depth ? variant::color_depth : variant::color) : variant::depth;
		MTL::RenderPipelineState* state = pipeline(color_format, kind, sample_count);
		if (!state)
		{
			return;
		}

		// The values reach the fragment stage through the argument table: Metal 4 has
		// no setBytes (MTLArgumentBinds.h).
		table->setAddress(m_values->gpuAddress(), fill_values_slot);

		encoder->setRenderPipelineState(state);
		encoder->setArgumentTable(table, MTL::RenderStageFragment);
		encoder->setCullMode(MTL::CullModeNone);
		encoder->setTriangleFillMode(MTL::TriangleFillModeFill);
		encoder->setDepthStencilState(write_depth ? m_depth_write : m_depth_read);

		encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, 0, 3);
	}
}

#endif // RSX_ENABLE_METAL4
