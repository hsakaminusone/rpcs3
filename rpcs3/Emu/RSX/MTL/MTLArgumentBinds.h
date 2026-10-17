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
		// slot.  It continues above the map, in the buffer space only.
		fill_values_slot = fragment_depth_input_slot + 1, // 51
	};

	// The argument-table shape follows the map, so a new slot cannot overflow the
	// table in silence (HLD-05 §4 invariant 3).  The counts are the highest index
	// in each space, plus one.
	constexpr u32 argument_buffer_count = fill_values_slot + 1;           // 52
	constexpr u32 argument_texture_count = fragment_depth_input_slot + 1; // 51
	constexpr u32 argument_sampler_count = argument_texture_count;

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
