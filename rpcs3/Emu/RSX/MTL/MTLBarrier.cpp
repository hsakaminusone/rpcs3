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
		if (spec.cross_pass)
		{
			encoder->barrierAfterQueueStages(spec.producer, spec.consumer, spec.visibility);
			return true;
		}

		if (!stages_legal_intra_pass(spec.producer))
		{
			return false;
		}

		encoder->barrierAfterEncoderStages(spec.producer, spec.consumer, spec.visibility);
		return true;
	}

	void emit_queue_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec)
	{
		encoder->barrierAfterQueueStages(spec.producer, spec.consumer, spec.visibility);
	}
}

#endif // RSX_ENABLE_METAL4
