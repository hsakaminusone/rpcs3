#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

namespace mtl
{
	bool stages_legal_intra_pass(MTL::Stages producer)
	{
		return (producer & (MTL::StageFragment | MTL::StageTile)) == 0;
	}

	bool emit_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec)
	{
		if (!stages_legal_intra_pass(spec.producer))
		{
			return false;
		}

		encoder->barrierAfterEncoderStages(spec.producer, spec.consumer, spec.visibility);
		return true;
	}
}

#endif // RSX_ENABLE_METAL4
