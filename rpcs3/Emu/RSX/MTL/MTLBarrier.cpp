#include "MTLBarrier.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLInstrumentation.h"

namespace mtl
{
	bool stages_legal_intra_pass(MTL::Stages producer)
	{
		return (producer & (MTL::StageFragment | MTL::StageTile)) == 0;
	}

	bool emit_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec)
	{
		// TODO(excise): level 2; the barrier stream and its stage pair (area J).
		if (spec.cross_pass)
		{
			MTL_PROBE_LATCH(2, "sync/barrier cross pass");
		}
		else
		{
			MTL_PROBE_LATCH(2, "sync/barrier intra pass");
		}
		MTL_PROBE_HISTOGRAM(2, "sync/barrier producer stage", static_cast<u32>(spec.producer), "producer stage");
		MTL_PROBE_HISTOGRAM(2, "sync/barrier consumer stage", static_cast<u32>(spec.consumer), "consumer stage");

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

	void emit_producer_barrier(MTL4::CommandEncoder* encoder, const barrier_spec& spec)
	{
		encoder->barrierAfterStages(spec.producer, spec.consumer, spec.visibility);
	}
}

#endif // RSX_ENABLE_METAL4
