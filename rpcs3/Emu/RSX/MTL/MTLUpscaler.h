#pragma once

#include "MTLCommandRecorder.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	class constants_heap;

	// The FSR 1 upscale of the presented image (VK/upscalers/fsr1/fsr_pass.cpp,
	// GL/GLPresent.cpp:428).  It is the Metal form of `output_scaling = fsr` and of
	// the "RCAS sharpening" slider, both of which had no reader on this backend.
	//
	// The reference implementation is AMD's ffx_fsr1.h, and the Vulkan and OpenGL
	// passes compile it from the shared GLSL ubershader.  This backend does not: it
	// carries the float path of the same two filters as MSL compute kernels, so the
	// pass needs no GLSL compiler, no SPIR-V, and no sampled-image write descriptor.
	// The constants are still produced by the reference CPU helpers (FsrEasuCon /
	// FsrRcasCon), which is what keeps the image the same as the other backends:
	// only the arithmetic is restated, and it is restated instruction for
	// instruction, including the two exponent bit-tricks the reference uses in place
	// of a reciprocal.
	//
	// EASU enlarges the guest image to the output rectangle and RCAS sharpens the
	// result, so the caller's output pass receives an image at its own size and
	// draws it one-to-one.  Both stages are one dispatch over a 16x16 tile per
	// threadgroup, which is the ubershader's `local_size_x = 64` four-texel swizzle.
	class fsr_upscale_pass
	{
	public:
		bool init(const device& dev, resource_manager& resources);
		void shutdown();

		// Encode EASU then RCAS from `source` into an image of `width` x `height` and
		// return it, or null when the pass cannot run and the caller keeps its own
		// filter.  `input_width`/`input_height` are the guest rectangle the source
		// carries inside the image, which FSR needs separately from the image's own
		// size (dynamic resolution).  The returned image belongs to this pass and
		// stays valid until the next call with a different size.
		MTL::Texture* run(command_recorder& recorder, constants_heap& constants, MTL::Texture* source,
			u32 input_width, u32 input_height, u32 width, u32 height);

	private:
		MTL::ComputePipelineState* build_pipeline(const char* name);
		MTL::Texture* ensure_image(MTL::Texture*& image, u32 width, u32 height);
		void dispatch(MTL4::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* state,
			MTL4::ArgumentTable* table, MTL::Texture* source, MTL::Texture* destination,
			MTL::GPUAddress constants, u32 width, u32 height);

		MTL::Device* m_device = nullptr;
		MTL4::Compiler* m_compiler = nullptr;
		resource_manager* m_resources = nullptr;

		MTL::Library* m_library = nullptr;
		MTL::ComputePipelineState* m_easu = nullptr;
		MTL::ComputePipelineState* m_rcas = nullptr;

		// One sampler, bound where EASU gathers.  EASU needs the clamp-to-edge
		// address mode the reference sampler uses; the filter itself is unused by
		// `gather` and is set to linear to match it.
		MTL::SamplerState* m_sampler = nullptr;

		// One table per stage: Metal 4 reads an argument table when the command
		// executes, so the two dispatches cannot share one that is rewritten between
		// them (MTL4ArgumentTable.h).
		MTL4::ArgumentTable* m_easu_table = nullptr;
		MTL4::ArgumentTable* m_rcas_table = nullptr;

		// The EASU result and the RCAS result.  Both are the output size; only the
		// final one is returned.
		MTL::Texture* m_intermediate = nullptr;
		MTL::Texture* m_output = nullptr;
	};
}

#endif // RSX_ENABLE_METAL4
