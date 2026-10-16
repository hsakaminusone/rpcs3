#include "MTLArgumentBinds.h"

#ifdef RSX_ENABLE_METAL4

#include "Emu/RSX/Utils/rsx_utils.h"

namespace mtl
{
	// The counts are repeated in the header so it needs no RSX include; assert they
	// have not drifted from the RSX limits (rsx_utils.h:38-39).
	static_assert(static_cast<u32>(fragment_texture_count) == static_cast<u32>(rsx::limits::fragment_textures_count));
	static_assert(static_cast<u32>(vertex_texture_count) == static_cast<u32>(rsx::limits::vertex_textures_count));

	namespace
	{
		// A zero MTL::GPUAddress / MTL::ResourceID means "not bound this draw".  There
		// is no unbind operation, so a zero is never written: a stale valid binding is
		// safe because the shader only declares the units its program uses, whereas an
		// invalid zero binding is not.  MTL::ResourceID has no operator==, so compare
		// its _impl.
		void set_address(MTL4::ArgumentTable* table, MTL::GPUAddress value, NS::UInteger index)
		{
			if (value != 0)
			{
				table->setAddress(value, index);
			}
		}

		void set_texture(MTL4::ArgumentTable* table, MTL::ResourceID value, NS::UInteger index)
		{
			if (value._impl != 0)
			{
				table->setTexture(value, index);
			}
		}

		void set_sampler(MTL4::ArgumentTable* table, MTL::ResourceID value, NS::UInteger index)
		{
			if (value._impl != 0)
			{
				table->setSamplerState(value, index);
			}
		}
	}

	void argument_binder::reset()
	{
		m_bound[0] = {};
		m_bound[1] = {};
		m_has_bound[0] = false;
		m_has_bound[1] = false;
	}

	void argument_binder::bind(MTL4::ArgumentTable* table, bool vertex_stage, const argument_binds& binds)
	{
		if (table == nullptr)
		{
			return;
		}

		const u32 stage = vertex_stage ? 0u : 1u;
		const argument_binds& previous = m_bound[stage];
		// The first bind for a stage has nothing to compare against, so every
		// non-zero slot is written; afterwards only a changed value is written.
		const bool first_bind = !m_has_bound[stage];

		// A slot is written when it is bound and either the stage has never been
		// bound or the value changed.  set_* drops the unbound (zero) case.
		const auto address = [&](MTL::GPUAddress value, MTL::GPUAddress old, NS::UInteger index)
		{
			if (first_bind || value != old)
			{
				set_address(table, value, index);
			}
		};

		const auto stream = [&](MTL::ResourceID value, MTL::ResourceID old, NS::UInteger index)
		{
			if (first_bind || value._impl != old._impl)
			{
				set_texture(table, value, index);
			}
		};

		const auto unit = [&](const texture_bind& value, const texture_bind& old, NS::UInteger index)
		{
			if (first_bind || value.texture._impl != old.texture._impl)
			{
				set_texture(table, value.texture, index);
			}
			if (first_bind || value.sampler._impl != old.sampler._impl)
			{
				set_sampler(table, value.sampler, index);
			}
		};

		if (vertex_stage)
		{
			address(binds.vertex_context, previous.vertex_context, vertex_context_slot);
			address(binds.vertex_constants, previous.vertex_constants, vertex_constants_slot);
			stream(binds.vertex_persistent_stream, previous.vertex_persistent_stream, vertex_buffers_slot);
			stream(binds.vertex_volatile_stream, previous.vertex_volatile_stream, vertex_volatile_slot);
			address(binds.vertex_draw_parameters, previous.vertex_draw_parameters, vertex_draw_parameters_slot);
			address(binds.conditional_render_predicate, previous.conditional_render_predicate, conditional_render_predicate_slot);
			address(binds.instancing_lookup, previous.instancing_lookup, instancing_lut_slot);
			address(binds.instancing_constants, previous.instancing_constants, instancing_constants_slot);
			address(binds.vertex_draw_params, previous.vertex_draw_params, vertex_draw_params_slot);

			for (u32 index = 0; index < vertex_texture_count; ++index)
			{
				unit(binds.vertex_textures[index], previous.vertex_textures[index], vertex_texture_slot(index));
			}
		}
		else
		{
			address(binds.fragment_constants, previous.fragment_constants, fragment_constants_slot);
			address(binds.fragment_state, previous.fragment_state, fragment_state_slot);
			address(binds.fragment_texture_params, previous.fragment_texture_params, fragment_texture_params_slot);
			address(binds.fragment_stipple, previous.fragment_stipple, fragment_stipple_slot);
			address(binds.fragment_blend, previous.fragment_blend, fragment_blend_slot);

			for (u32 index = 0; index < fragment_texture_count; ++index)
			{
				unit(binds.fragment_textures[index], previous.fragment_textures[index], texture_slot_base + index);
			}
		}

		m_bound[stage] = binds;
		m_has_bound[stage] = true;
	}
}

#endif // RSX_ENABLE_METAL4
