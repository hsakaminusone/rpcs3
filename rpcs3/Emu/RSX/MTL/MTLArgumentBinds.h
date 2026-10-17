#pragma once

#include "MTLDevice.h"

#ifdef RSX_ENABLE_METAL4

#include "MTLVertexState.h"

#include <array>

namespace mtl
{
	// The RSX shader ABI as a Metal 4 argument-table index map (HLD-05 §3.1-§3.2).
	// The vertex half that MTLVertexState.h already owns is reused from vertex_slot;
	// this enum adds the vertex entries that header omits, the fragment-stage
	// entries and the shared texture range.  The numbers are the RSX ABI
	// (inventory/out/rsx_binding_slots.csv), not the Vulkan header's per-program
	// packing: MSL bakes the index into the declaration, so a program's indices
	// never move (HLD-05 §2.7, §3.1).
	enum argument_index : u32
	{
		fragment_constants_slot = 2,      // the flat fragment constant file
		fragment_state_slot = 3,          // fragment context params
		fragment_texture_params_slot = 4, // per-unit texture parameters
		vertex_draw_parameters_slot = 7,  // the per-draw DrawParametersBuffer heap
		conditional_render_predicate_slot = 8, // the conditional-render predicate word
		fragment_stipple_slot = 9,        // the polygon-stipple rasteriser heap

		// Both stage tables start their texture range at 12.  The ranges do not
		// collide because each stage has its own MTL4::ArgumentTable and may share
		// an index (HLD-05 §3.1).
		texture_slot_base = 12,
		fragment_texture_count = 16, // rsx::limits::fragment_textures_count
		vertex_texture_count = 4,    // rsx::limits::vertex_textures_count

		// The two substituted push-constant slots.  MSL has no push constants, so
		// the VS draw-param offset and the FS blend config become buffer reads
		// (HLD-05 §3.3).  Their numeric value is not settled: HLD-05 §3.2 † and
		// §6 Q2 leave it open, and the doc's own stated safe default is to append
		// them above the texture range so the fixed map never moves.  These values
		// are that default, not a settled answer.
		vertex_draw_params_slot = texture_slot_base + fragment_texture_count, // 28
		fragment_blend_slot = vertex_draw_params_slot + 1,                    // 29

		// The derived fragment texture range.  HLD-05 §3.2 gives no index to the
		// stencil mirror, the depth-compare fetch or the programmable-blend inputs,
		// so they continue above the substituted slots.  A buffer and a texture may
		// share an index because each stage's MTL4::ArgumentTable keeps the two
		// spaces apart, but the range starts above both so the documented values of
		// the substituted slots never move.
		//
		// The programmable-blend inputs are an intermediate only: the MSL
		// cross-compiler turns each into a [[color(i)]] fragment input, so they
		// never reach the argument table.
		fragment_stencil_slot_base = fragment_blend_slot + 1,                             // 30
		fragment_subpass_slot_base = fragment_stencil_slot_base + fragment_texture_count, // 46
		fragment_depth_input_slot = fragment_subpass_slot_base + 4,                       // 50

		// The built-in full-screen fill is not an RSX draw, so its values have no RSX
		// slot.  It takes the last buffer index the MSL cap allows.  Index 30 is also
		// the first stencil-mirror texture, which is no collision: a buffer and a
		// texture are separate spaces (REVIEW-FINDINGS.md A9).
		fill_values_slot = 30,
	};

	// The MSL index of the sampler half of a texture unit.  MSL caps [[sampler(n)]]
	// at 15 (REVIEW-FINDINGS.md A9) and the RSX ABI puts every texture unit at 12 or
	// above, so a unit cannot reuse its texture index as its sampler index.  A unit
	// takes its position inside the stage's texture range instead.  The shader-side
	// remap (MTLShader.cpp) and the table writer below must agree, so both call
	// this.  Only a slot that slot_has_sampler accepts may be passed.
	constexpr u32 texture_sampler_index(u32 slot)
	{
		return slot - texture_slot_base;
	}

	// True when a slot's MSL declaration carries a sampler.  Only a texture unit
	// does: a unit is a combined image sampler written as two Metal entries, so its
	// image and its state both need an index.  The depth-as-texture inputs are
	// plain textures read with read() and carry no sampler in the A9 form, so one
	// index each.  The table writer uses this to decide whether a slot has a
	// sampler to write, which keeps a sampler bind inside the table's sampler space:
	// the validation layer aborts on a bind past the declared count (measured,
	// /tmp/flprobe/tbl.mm, exit 134).
	constexpr bool slot_has_sampler(u32 slot)
	{
		return slot >= texture_slot_base && slot < texture_slot_base + fragment_texture_count;
	}

	// The argument-table shape follows the map, so a new slot cannot overflow the
	// table in silence (HLD-05 §4 invariant 3).  The counts are the highest index
	// in each space, plus one.
	constexpr u32 argument_buffer_count = fill_values_slot + 1;           // 31
	constexpr u32 argument_texture_count = fragment_depth_input_slot + 1; // 51

	// The argument table has a sampler cap of its own, separate from the MSL
	// declaration cap and the same value (REVIEW-FINDINGS.md A9): the argument table
	// descriptor must not ask for more than 16 sampler entries, and the API
	// validation layer asserts it.  A stage needs one sampler per texture unit and no
	// more, because the depth mirror and the depth input become samplerless textures
	// in the A9 form, so the unit count is the whole requirement.
	constexpr u32 argument_sampler_count = 16;

	// MSL caps the shader-side index of each space (REVIEW-FINDINGS.md A9): a
	// declaration above its cap makes the library fail to build.  A slot the map
	// hands to a shader must stay inside the cap, so the map is asserted against
	// them here.  The stencil mirror and the depth input are separate textures in
	// the A9 form, so they leave the sampler space entirely and the 16 units are its
	// whole requirement.  The shader side declares them as such and reads them with
	// an unfiltered texel fetch (MTLFragmentProgram.cpp), and the SPIRV-Cross side
	// gives them no sampler index (MTLShader.cpp).
	static_assert(fill_values_slot <= 30, "the ABI map overflows the MSL buffer index cap");
	static_assert(fragment_depth_input_slot <= 127, "the ABI map overflows the MSL texture index cap");
	static_assert(argument_sampler_count <= 16, "the argument table overflows the MSL sampler index cap");
	static_assert(fragment_texture_count <= argument_sampler_count, "the texture units need more samplers than the table holds");
	static_assert(vertex_texture_count <= argument_sampler_count, "the texture units need more samplers than the table holds");
	static_assert(texture_sampler_index(texture_slot_base + fragment_texture_count - 1) < argument_sampler_count,
		"the last texture unit's sampler index leaves the argument table's sampler space");
	static_assert(!slot_has_sampler(fragment_stencil_slot_base), "a depth mirror must not carry a sampler");
	static_assert(!slot_has_sampler(fragment_depth_input_slot), "the depth input must not carry a sampler");

	// One RSX texture unit occupies one index in two tables because MSL has no
	// combined image sampler: the image is a texture entry and its state a sampler
	// entry (HLD-05 §3.2).  A zero _impl in either half means that half is unbound.
	struct texture_bind
	{
		MTL::ResourceID texture{};
		MTL::ResourceID sampler{};
	};

	// The value set of one draw, covering both stages.  A named buffer field is an
	// MTL::GPUAddress (a setAddress entry); a vertex stream is an MTL::ResourceID (a
	// setTexture entry).  A zero value means "not bound this draw".  Each field's
	// comment carries its argument-table index; the struct is grouped by the stage
	// half that argument_binder::bind selects.
	struct argument_binds
	{
		// Vertex stage (the vertex table).
		MTL::GPUAddress vertex_context = 0;                 // [0] vertex context params
		MTL::GPUAddress vertex_constants = 0;               // [1] the flat transform-constant file
		MTL::ResourceID vertex_persistent_stream{};         // [5] texture_buffer<uint>, persistent stream
		MTL::ResourceID vertex_volatile_stream{};           // [6] texture_buffer<uint>, volatile stream
		MTL::GPUAddress vertex_draw_parameters = 0;         // [7] the per-draw draw-parameter heap
		MTL::GPUAddress conditional_render_predicate = 0;   // [8] conditional-render predicate
		MTL::GPUAddress instancing_lookup = 0;              // [10] indexed-constant lookup table
		MTL::GPUAddress instancing_constants = 0;           // [11] indexed-constant vec4 array
		MTL::GPUAddress vertex_draw_params = 0;             // [28] substituted VS push constant

		// Fragment stage (the fragment table).
		MTL::GPUAddress fragment_constants = 0;          // [2] the flat fragment constant file
		MTL::GPUAddress fragment_state = 0;              // [3] fragment context params
		MTL::GPUAddress fragment_texture_params = 0;     // [4] per-unit texture parameters
		MTL::GPUAddress fragment_stipple = 0;            // [9] polygon-stipple rasteriser heap
		MTL::GPUAddress fragment_blend = 0;              // [29] substituted FS push constant

		// Texture units, at 12+id in each stage's table; the vertex range is the
		// first four indices and the fragment range the next sixteen.
		std::array<texture_bind, vertex_texture_count> vertex_textures{};     // [12..15]
		std::array<texture_bind, fragment_texture_count> fragment_textures{}; // [12..27]

		// The depth-as-texture inputs.  A redirected depth unit also reads the
		// stencil half of its surface at [30..45]; the emulated depth compare reads a
		// depth mirror at [50].  Each takes one index and no sampler: the A9 form
		// declares it as a plain texture read with read(), so the writer binds the
		// image only (slot_has_sampler).  The sampler half of the entry stays in the
		// type because the entry is shared with a texture unit.  Metal forbids reading
		// a texture that is an attachment in the open pass, so each mirror is a
		// distinct image, copied at the clause boundary (REALISATION.md, "The
		// depth/stencil-as-texture cluster").
		std::array<texture_bind, fragment_texture_count> fragment_stencil{}; // [30..45]
		texture_bind fragment_depth_input{};                                 // [50]
	};

	// The per-stage argument-table writer.  A bind is an encoder-visible write and
	// the table survives across draws, so re-writing unchanged slots every draw is
	// pure overhead.  This class owns the slot map, so it is the only thing that can
	// know what changed: it keeps the last-bound set per stage and writes only the
	// entries that differ (HLD-05 §4 invariant 3 permits leaving unused slots
	// unbound).
	class argument_binder
	{
	public:
		// Drop all knowledge of what is bound.  Call when the underlying table is
		// replaced (a new frame slot, or a new table object).
		void reset();

		// Bind the set into the stage's table.  Only slots whose value differs from
		// the last bind are written.  vertex_stage selects which half of the set
		// applies.  Returns true when a slot was written: Metal 4 snapshots the
		// table when the encoder binds it, so a write is invisible to a draw until
		// the caller re-binds the table (REALISATION.md, "Argument tables").
		bool bind(MTL4::ArgumentTable* table, bool vertex_stage, const argument_binds& binds);

	private:
		argument_binds m_bound[2];
		bool m_has_bound[2] = { false, false };
	};
}

#endif // RSX_ENABLE_METAL4
