R"(

#ifdef _ENABLE_POLYGON_STIPPLE
	// Convert x,y to linear address
	const uvec2 stipple_coord = uvec2(gl_FragCoord.xy) % uvec2(32, 32);
	const uint address = stipple_coord.y * 32u + stipple_coord.x;
	const uint bit_offset = (address & 31u);
#ifdef VULKAN
	// In vulkan we have a unified array with a dynamic offset
	const uint word_index = _get_bits(address, 7, 3) + _fs_stipple_pattern_array_offset;
#else
	const uint word_index = _get_bits(address, 7, 3);
#endif
	const uint sub_index = _get_bits(address, 5, 2);

	if (!_test_bit(stipple_pattern[word_index][sub_index], int(bit_offset)))
	{
		_kill();
	}
#endif

#if _MRT_BUFFERS_COUNT >= 1
  #define MRT0_DO(expr) expr
#else
  #define MRT0_DO(expr)
#endif
#if _MRT_BUFFERS_COUNT >= 2
  #define MRT1_DO(expr) expr
#else
  #define MRT1_DO(expr)
#endif
#if _MRT_BUFFERS_COUNT >= 3
  #define MRT2_DO(expr) expr
#else
  #define MRT2_DO(expr)
#endif
#if _MRT_BUFFERS_COUNT == 4
  #define MRT3_DO(expr) expr
#else
  #define MRT3_DO(expr)
#endif

#if _MRT_BUFFERS_COUNT >= 1

#ifdef _ENABLE_ROP_CHANNEL_REMAPPING
const uint ROP_remap = get_ROP_channel_remap();
#endif

#if defined(_ENABLE_PROGRAMMABLE_BLENDING) || defined(_ENABLE_LOGIC_OP)

#ifdef _ENABLE_ROP_OUTPUT_MULTISAMPLED
#define FRAG_LOAD(n) mrt_color##n = subpassLoad(frag_src_##n, gl_SampleID)
#else
#define FRAG_LOAD(n) mrt_color##n = subpassLoad(frag_src_##n)
#endif

#ifdef _ENABLE_PROGRAMMABLE_BLENDING
#define IS_MRT_BLEND_ENABLED(n) _test_bit(rop_control, (n) + MRT_BLEND_TARGETS_OFFSET)

#define MRT0_BLEND_DO(expr) MRT0_DO(if (IS_MRT_BLEND_ENABLED(0)) expr)
#define MRT1_BLEND_DO(expr) MRT1_DO(if (IS_MRT_BLEND_ENABLED(1)) expr)
#define MRT2_BLEND_DO(expr) MRT2_DO(if (IS_MRT_BLEND_ENABLED(2)) expr)
#define MRT3_BLEND_DO(expr) MRT3_DO(if (IS_MRT_BLEND_ENABLED(3)) expr)
#endif

// The destination load is gated on the ROP blend-target bits only for blending.
// The logic op applies to every output and must not read those bits: the RSX
// clears them whenever the op is enabled, because the op replaces blending
// (REVIEW-FINDINGS.md C1).
#ifdef _ENABLE_LOGIC_OP
#define MRT0_LOAD_DO(expr) MRT0_DO(expr)
#define MRT1_LOAD_DO(expr) MRT1_DO(expr)
#define MRT2_LOAD_DO(expr) MRT2_DO(expr)
#define MRT3_LOAD_DO(expr) MRT3_DO(expr)
#else
#define MRT0_LOAD_DO(expr) MRT0_BLEND_DO(expr)
#define MRT1_LOAD_DO(expr) MRT1_BLEND_DO(expr)
#define MRT2_LOAD_DO(expr) MRT2_BLEND_DO(expr)
#define MRT3_LOAD_DO(expr) MRT3_BLEND_DO(expr)
#endif

MRT0_DO(vec4 mrt_color0;)
MRT1_DO(vec4 mrt_color1;)
MRT2_DO(vec4 mrt_color2;)
MRT3_DO(vec4 mrt_color3;)

MRT0_LOAD_DO(FRAG_LOAD(0);)
MRT1_LOAD_DO(FRAG_LOAD(1);)
MRT2_LOAD_DO(FRAG_LOAD(2);)
MRT3_LOAD_DO(FRAG_LOAD(3);)

#ifdef _ENABLE_ROP_CHANNEL_REMAPPING
	MRT0_LOAD_DO(mrt_color0 = _mrt_color_t(remap_ROP_input(mrt_color0, ROP_remap));)
	MRT1_LOAD_DO(mrt_color1 = _mrt_color_t(remap_ROP_input(mrt_color1, ROP_remap));)
	MRT2_LOAD_DO(mrt_color2 = _mrt_color_t(remap_ROP_input(mrt_color2, ROP_remap));)
	MRT3_LOAD_DO(mrt_color3 = _mrt_color_t(remap_ROP_input(mrt_color3, ROP_remap));)
#endif // _ENABLE_ROP_CHANNEL_REMAPPING

#undef FRAG_LOAD
#undef MRT0_LOAD_DO
#undef MRT1_LOAD_DO
#undef MRT2_LOAD_DO
#undef MRT3_LOAD_DO
#endif // _ENABLE_PROGRAMMABLE_BLENDING || _ENABLE_LOGIC_OP
#endif // _MRT_BUFFERS_COUNT >= 1
)"
