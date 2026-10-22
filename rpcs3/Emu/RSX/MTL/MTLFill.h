#pragma once

#include "MTLCommandRecorder.h"
#include "MTLDepthStencil.h"

#ifdef RSX_ENABLE_METAL4

#include <unordered_map>

namespace mtl
{
	// The built-in full-screen fill (C1).  Metal has no scissored clear, so a clear
	// that does not cover its attachment is a full-screen triangle confined by the
	// scissor.  It is the Metal form of vkCmdClearAttachments, and the same
	// primitive serves every backend-side full-screen pass.
	//
	// Two Metal 4 facts shape it.  There is no setBytes, so the fill values come
	// from a buffer at the ABI's fill_values_slot.  And the depth-stencil object is
	// not in the pipeline: MTL4::RenderCommandEncoder::setDepthStencilState carries
	// it, and MTL4::RenderPipelineDescriptor has no depth attachment format, so a
	// fill that writes depth is a different fragment output set, not a different
	// attachment format.
	// Metal writes stencil through the depth-stencil object, not a shader output, so
	// a masked stencil clear is a full-screen draw with a Replace pass operation and
	// the write mask.  `write_mask == 0` leaves the stencil plane alone.
	struct fill_stencil
	{
		u8 reference = 0;
		u8 write_mask = 0;
	};

	class fill_pass
	{
	public:
		void init(const device& dev, resource_manager& resources);
		void shutdown();

		// Fill the open encoder.  `values` is the address of one fill-value block
		// (MTLConstants.h fill_values), because Metal 4 has no setBytes.  The caller
		// packs a fresh block per fill: a command buffer does not snapshot buffer
		// contents.  `color_format` is the pass's colour attachment format, or
		// MTL::PixelFormatInvalid for a depth-only pass.  `color_mask` is the
		// per-channel write mask: a partial clear writes only the channels the
		// format stores, and ColorWriteMaskNone leaves colour alone.  The caller
		// sets the scissor, opens the pass and passes its sample count.
		void fill(MTL4::ArgumentTable* table, MTL4::RenderCommandEncoder* encoder, MTL::GPUAddress values, MTL::PixelFormat color_format, u8 sample_count, MTL::ColorWriteMask color_mask, bool write_depth, fill_stencil stencil = {});

	private:
		// The output set of the fragment shader, which is what a Metal 4 pipeline
		// still has to declare.
		enum class variant : u32
		{
			color = 0,
			depth = 1,
			color_depth = 2,
		};

		MTL::RenderPipelineState* pipeline(MTL::PixelFormat color_format, variant kind, u8 sample_count, MTL::ColorWriteMask color_mask);

		MTL::Device* m_device = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		resource_manager* m_resources = nullptr;
		MTL::Library* m_library = nullptr;
		MTL::DepthStencilState* m_depth_write = nullptr;
		MTL::DepthStencilState* m_depth_read = nullptr;
		// The stencil-write states, keyed by the write mask and the depth flag.  A
		// partial stencil clear is rare, so the cache stays tiny.
		depth_stencil_cache m_stencil_states;
		std::unordered_map<u64, MTL::RenderPipelineState*> m_pipelines;
	};
}

#endif // RSX_ENABLE_METAL4
