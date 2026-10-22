#pragma once

#include "MTLDevice.h"
#include "MTLResourceManager.h"

#ifdef RSX_ENABLE_METAL4

#include "util/asm.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace mtl
{
	// The std430 layouts the Metal shaders declare.  The RSX shader ABI is shared
	// with the other backends, so these mirror the GLSL field for field
	// (RSXDefines2.glsl).  The static assertions are the guard against drift: a
	// mismatch would silently shift every field after it.
	struct vertex_context
	{
		f32 scale_offset_mat[16];
		u32 user_clip_configuration_bits;
		u32 transform_branch_bits;
		f32 point_size;
		f32 z_near;
		f32 z_far;
		f32 reserved[3];
	};
	static_assert(sizeof(vertex_context) == 96);

	struct draw_parameters
	{
		u32 vertex_base_index;
		u32 vertex_index_offset;
		u32 draw_id;
		u32 xform_constants_offset;
		u32 vs_context_offset;
		u32 fs_constants_offset;
		u32 fs_context_offset;
		u32 fs_texture_base_index;
		u32 fs_stipple_pattern_offset;
		u32 reserved;
		s32 attrib_data[16][2];
	};
	static_assert(sizeof(draw_parameters) == 168);

	struct fragment_context
	{
		f32 fog_param0;
		f32 fog_param1;
		u32 rop_control;
		f32 alpha_ref;
		u32 fog_mode;
		f32 wpos_scale;
		f32 wpos_bias[2];
	};
	static_assert(sizeof(fragment_context) == 32);

	// The same layout as rsx::fragment_program_texture_config::TIU_slot, which the
	// shared framework fills (program_util.h:15).  One slot is one texture unit.
	struct sampler_info
	{
		f32 scale[3];
		f32 bias[3];
		f32 clamp_min[2];
		f32 clamp_max[2];
		u32 remap;
		u32 control;
	};
	static_assert(sizeof(sampler_info) == 48);

	// The programmable-blend push constant (MTLFragmentProgram.cpp:183).  Bytes
	// 0..3 are the block's reserved word, so blend_eqn is at offset 4.  logic_op
	// carries the guest logic op for the emulation and is unused when the program
	// does not emulate it (REVIEW-FINDINGS.md C1).
	struct blend_values
	{
		u32 reserved;
		u32 blend_eqn;
		u32 blend_sfactors;
		u32 blend_dfactors;
		f32 blend_constants[4];
		u32 logic_op;
	};
	static_assert(sizeof(blend_values) == 36);

	// The built-in fill's values (MTLFill.h).  Bytes 0..15 are the colour and byte
	// 16 is the depth, which is the MSL struct the fill library declares.  A fill
	// reads one block, and a Metal 4 command buffer does not snapshot buffer
	// contents, so every fill takes its own block.
	struct fill_values
	{
		f32 color[4];
		f32 depth;
		f32 reserved[3];
	};
	static_assert(sizeof(fill_values) == 32);

	// A vec4 element.  The transform-constant file, the fragment constants, the
	// instancing constants and the stipple pattern are all addressed in vec4 units,
	// so the ring's index is the shader's index.
	struct vec4_block
	{
		f32 v[4];
	};
	static_assert(sizeof(vec4_block) == 16);

	// The transform-constant file (fill_vertex_program_constants_data).
	constexpr u32 vertex_constant_count = 468;

	// The polygon-stipple pattern is 128 bytes, read as eight uvec4.
	constexpr u32 stipple_block_count = 8;

	// One texture-unit slot per RSX unit; the shader indexes the block by unit
	// (MTLFragmentProgram.cpp:171, fs_texture_base_index).
	constexpr u32 sampler_info_count = 16;

	// One typed ring of per-draw blocks (HLD-04 §3.3).  A shader indexes a block by
	// its position in an unsized array, so the ring is bound once and only the index
	// travels per draw.  A block type the shader cannot offset (the instancing
	// tables) is bound by address instead, and its caller uses address().
	//
	// Metal 4 command buffers hold no strong reference, and the GPU reads a buffer
	// at execution time, not at encode time.  A ring is therefore per frame slot and
	// is reused only after that slot's frame has completed (HLD-02 I7).
	template <typename T>
	class block_ring
	{
	public:
		void init(resource_manager& resources, u64 capacity)
		{
			m_resources = &resources;
			m_capacity = capacity;
			m_buffer = resources.new_buffer(capacity * sizeof(T), MTL::StorageModeShared);
			m_count = 0;
		}

		void shutdown()
		{
			if (m_resources)
			{
				for (MTL::Buffer* buffer : m_retired)
				{
					m_resources->defer_release(buffer);
				}

				if (m_buffer)
				{
					m_resources->defer_release(m_buffer);
				}
			}

			m_retired.clear();
			m_buffer = nullptr;
			m_resources = nullptr;
		}

		// Start a frame on this slot.  The slot's previous frame has completed, so
		// the buffers retired during it are unreferenced.
		void begin_frame()
		{
			for (MTL::Buffer* buffer : m_retired)
			{
				m_resources->defer_release(buffer);
			}

			m_retired.clear();
			m_count = 0;
		}

		// Reserve one block of `count` elements (one, for the fixed types).  Null
		// when the ring cannot grow; the caller skips the draw rather than write past
		// the buffer.
		T* alloc(u64 count = 1)
		{
			if (!m_buffer || count == 0)
			{
				return nullptr;
			}

			if (m_count + count > m_capacity)
			{
				// The next buffer must hold the block that did not fit, so it is at
				// least as large as that block.
				const u64 capacity = std::max(m_capacity, count);
				MTL::Buffer* next = m_resources->new_buffer(capacity * sizeof(T), MTL::StorageModeShared);
				if (!next)
				{
					return nullptr;
				}

				// The retired buffer is still referenced by the draws already encoded
				// this frame, so the release is deferred.
				m_retired.push_back(m_buffer);
				m_buffer = next;
				m_capacity = capacity;
				m_count = 0;
			}

			T* block = &static_cast<T*>(m_buffer->contents())[m_count];
			m_last = static_cast<u32>(m_count);
			m_count += count;
			return block;
		}

		u32 index() const { return m_last; }
		bool valid() const { return m_buffer != nullptr; }
		MTL::GPUAddress address() const { return m_buffer ? m_buffer->gpuAddress() : 0; }

		// The address of one block.  A block the shader reads at offset zero (a
		// substituted push constant) is bound here, not at the ring base.
		MTL::GPUAddress address_of(u32 index) const { return m_buffer ? m_buffer->gpuAddress() + (index * sizeof(T)) : 0; }

	private:
		resource_manager* m_resources = nullptr;
		MTL::Buffer* m_buffer = nullptr;
		std::vector<MTL::Buffer*> m_retired;
		u64 m_capacity = 0;
		u64 m_count = 0;
		u32 m_last = 0;
	};

	// The arithmetic of a growable byte ring, kept free of Metal types so a unit
	// test can exercise it.  A block starts on an alignment boundary and consumes
	// its aligned size, so every later block stays aligned.  A block larger than
	// the buffer asks for a replacement buffer that holds it.
	struct ring_block
	{
		u64 offset;
		u64 end;
		u64 capacity;
		bool grow;
	};

	inline ring_block allocate_ring_block(u64 count, u64 size, u64 capacity, u64 alignment)
	{
		ensure(alignment != 0 && (alignment & (alignment - 1)) == 0);
		const u64 aligned_size = utils::align(size, alignment);

		if (count <= capacity && aligned_size <= capacity - count)
		{
			return { count, count + aligned_size, capacity, false };
		}

		return { 0, aligned_size, std::max(capacity, aligned_size), true };
	}

	// One vertex stream.  The shader byte-addresses the stream, so it is bound as an
	// R8Uint texture buffer and read with `texture_buffer<uint>`: one texel is one
	// byte, and the shared GLSL needs no change (HLD-05 §3.5).  One view spans the
	// whole ring, so a draw's data is a starting offset in `attrib_data`, not a new
	// view (RSXDrawCommands.cpp:338, :496).  The view is therefore rebuilt only when
	// the ring grows.
	class stream_ring
	{
	public:
		void init(resource_manager& resources, u64 capacity);
		void shutdown();
		void begin_frame();

		// Reserve `size` bytes and return the byte offset.  Null when the ring cannot
		// grow; the caller skips the draw.
		void* alloc(u64 size, u32& offset);

		bool valid() const { return m_buffer != nullptr && m_view != nullptr; }
		MTL::ResourceID resource_id() const { return m_view ? m_view->gpuResourceID() : MTL::ResourceID{}; }

	private:
		resource_manager* m_resources = nullptr;
		MTL::Buffer* m_buffer = nullptr;
		MTL::Texture* m_view = nullptr;
		std::vector<MTL::Buffer*> m_retired;
		std::vector<MTL::Texture*> m_retired_views;
		u64 m_capacity = 0;
		u64 m_count = 0;
	};

	// A per-frame ring of raw bytes, for the index buffer.  The index buffer is a
	// GPU address argument, not an argument-table entry, so it needs no view
	// (HLD-05 §3.5).
	class byte_ring
	{
	public:
		void init(resource_manager& resources, u64 capacity);
		void shutdown();
		void begin_frame();

		// Reserve `size` bytes and return the byte offset.  Null when the ring
		// cannot grow; the caller skips the draw.  The interpreter's instruction
		// blocks need a stronger alignment than the index data.
		void* alloc(u64 size, u32& offset, u64 alignment = 4);

		bool valid() const { return m_buffer != nullptr; }
		MTL::GPUAddress address() const { return m_buffer ? m_buffer->gpuAddress() : 0; }
		MTL::GPUAddress address_of(u32 offset) const { return address() + offset; }

	private:
		resource_manager* m_resources = nullptr;
		MTL::Buffer* m_buffer = nullptr;
		std::vector<MTL::Buffer*> m_retired;
		u64 m_capacity = 0;
		u64 m_count = 0;
	};

	// Every per-draw ABI buffer, one set per frame slot (HLD-04 §3.3).  The draw
	// path allocates a block, writes it, and passes the index to the shader through
	// the draw-parameters block.  The capacities are the number of draws (or, for
	// the transform constants, the number of changes) one frame can hold.
	class constants_heap
	{
	public:
		static constexpr u32 draw_capacity = 8192;
		static constexpr u32 constant_capacity = 65536;
		static constexpr u32 transform_capacity = 512;
		static constexpr u32 fill_capacity = 1024;

		// The vertex streams hold one frame's uploaded attribute data.  The
		// capacities are the usual working set; a ring that overflows chains to a
		// new buffer instead of failing (HLD-04 §3.3).
		static constexpr u64 stream_capacity = 8u * 1024u * 1024u;
		static constexpr u64 volatile_capacity = 2u * 1024u * 1024u;
		static constexpr u64 index_capacity = 2u * 1024u * 1024u;
		static constexpr u64 instruction_capacity = 4u * 1024u * 1024u;

		// The overlay passes are not RSX draws: their vertices and their uniform
		// block are per-frame CPU data with no RSX slot, so they take their own
		// rings (HLD-06 §2.6).
		static constexpr u64 overlay_vertex_capacity = 4u * 1024u * 1024u;
		static constexpr u64 overlay_uniform_capacity = 128u * 1024u;

		void init(resource_manager& resources, u32 frame_count);
		void shutdown();
		void begin_frame(u32 slot);

		vertex_context* alloc_vertex_context() { return m_sets[m_slot].vertex_context.alloc(); }
		draw_parameters* alloc_draw_parameters() { return m_sets[m_slot].draw_parameters.alloc(); }
		fragment_context* alloc_fragment_context() { return m_sets[m_slot].fragment_context.alloc(); }
		sampler_info* alloc_sampler_info() { return m_sets[m_slot].sampler_info.alloc(sampler_info_count); }
		vec4_block* alloc_stipple() { return m_sets[m_slot].stipple.alloc(stipple_block_count); }
		blend_values* alloc_blend_values() { return m_sets[m_slot].blend_values.alloc(); }
		u32* alloc_vertex_draw_params() { return m_sets[m_slot].vertex_draw_params.alloc(); }
		vec4_block* alloc_fragment_constants(u32 vec4_count) { return m_sets[m_slot].fragment_constants.alloc(vec4_count); }
		vec4_block* alloc_transform_constants() { return m_sets[m_slot].transform_constants.alloc(vertex_constant_count); }
		vec4_block* alloc_instancing_constants(u32 vec4_count) { return m_sets[m_slot].instancing_constants.alloc(vec4_count); }
		s32* alloc_instancing_lookup(u32 count) { return m_sets[m_slot].instancing_lookup.alloc(count); }

		u32 vertex_context_index() const { return m_sets[m_slot].vertex_context.index(); }
		u32 draw_parameters_index() const { return m_sets[m_slot].draw_parameters.index(); }
		u32 fragment_context_index() const { return m_sets[m_slot].fragment_context.index(); }
		u32 sampler_info_index() const { return m_sets[m_slot].sampler_info.index(); }
		u32 stipple_index() const { return m_sets[m_slot].stipple.index(); }
		u32 blend_values_index() const { return m_sets[m_slot].blend_values.index(); }
		u32 fragment_constants_index() const { return m_sets[m_slot].fragment_constants.index(); }
		u32 transform_constants_index() const { return m_sets[m_slot].transform_constants.index(); }

		// A shader cannot offset these, so they are bound by address.
		MTL::GPUAddress vertex_context_address() const { return m_sets[m_slot].vertex_context.address(); }
		MTL::GPUAddress draw_parameters_address() const { return m_sets[m_slot].draw_parameters.address(); }
		MTL::GPUAddress fragment_context_address() const { return m_sets[m_slot].fragment_context.address(); }
		MTL::GPUAddress sampler_info_address() const { return m_sets[m_slot].sampler_info.address(); }
		MTL::GPUAddress stipple_address() const { return m_sets[m_slot].stipple.address(); }
		MTL::GPUAddress fragment_constants_address() const { return m_sets[m_slot].fragment_constants.address(); }
		MTL::GPUAddress vertex_constants_address() const { return m_sets[m_slot].transform_constants.address(); }
		MTL::GPUAddress instancing_constants_address() const { return m_sets[m_slot].instancing_constants.address(); }
		MTL::GPUAddress instancing_lookup_address() const { return m_sets[m_slot].instancing_lookup.address(); }
		MTL::GPUAddress blend_values_address() const { return m_sets[m_slot].blend_values.address(); }

		// Pack one fill's values and return the address of its block, because the
		// fill shader reads the block at offset zero (MTLFill.cpp).
		MTL::GPUAddress alloc_fill_values(const std::array<f32, 4>& color, f32 depth)
		{
			fill_values* values = m_sets[m_slot].fill_values.alloc();
			if (!values)
			{
				return 0;
			}

			std::copy(color.begin(), color.end(), values->color);
			values->depth = depth;
			return m_sets[m_slot].fill_values.address_of(m_sets[m_slot].fill_values.index());
		}

		// A substituted push constant is read at offset zero, so it is bound at its
		// own block, not at the ring base.
		MTL::GPUAddress blend_values_block_address() const { return m_sets[m_slot].blend_values.address_of(m_sets[m_slot].blend_values.index()); }
		MTL::GPUAddress vertex_draw_params_block_address() const { return m_sets[m_slot].vertex_draw_params.address_of(m_sets[m_slot].vertex_draw_params.index()); }

		stream_ring& persistent_stream() { return m_streams[m_slot].persistent; }
		stream_ring& volatile_stream() { return m_streams[m_slot].volatile_stream; }
		byte_ring& index_ring() { return m_streams[m_slot].index; }
		byte_ring& instruction_ring() { return m_streams[m_slot].instructions; }
		byte_ring& overlay_vertex_ring() { return m_streams[m_slot].overlay_vertices; }
		byte_ring& overlay_uniform_ring() { return m_streams[m_slot].overlay_uniforms; }

		bool valid() const { return m_sets[m_slot].vertex_context.valid(); }

	private:
		struct ring_set
		{
			block_ring<vertex_context> vertex_context;
			block_ring<draw_parameters> draw_parameters;
			block_ring<fragment_context> fragment_context;
			block_ring<sampler_info> sampler_info;
			block_ring<vec4_block> stipple;
			block_ring<blend_values> blend_values;
			block_ring<fill_values> fill_values;
			block_ring<u32> vertex_draw_params;
			block_ring<vec4_block> fragment_constants;
			block_ring<vec4_block> transform_constants;
			block_ring<vec4_block> instancing_constants;
			block_ring<s32> instancing_lookup;
		};

		struct stream_set
		{
			stream_ring persistent;
			stream_ring volatile_stream;
			byte_ring index;
			byte_ring instructions;
			byte_ring overlay_vertices;
			byte_ring overlay_uniforms;
		};

		std::array<ring_set, frame_slot_count> m_sets;
		std::array<stream_set, frame_slot_count> m_streams;
		u32 m_slot = 0;
	};
}

#endif // RSX_ENABLE_METAL4
