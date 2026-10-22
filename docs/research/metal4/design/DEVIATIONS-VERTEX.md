# DEVIATIONS-VERTEX — the RSX vertex and index path on Metal 4

Scope: the vertex upload, the index upload, the index subrange, the element count and the vertex
addressing. The RSX is the specification. Vulkan, OpenGL, the shared `Emu/RSX/Common` code and the
RSX decode headers are evidence. KosmicKrisp is a second implementation of the same Metal 4 model,
not a specification.

## Decision

Metal 4 has no index-buffer offset argument. `drawIndexedPrimitives` takes the index buffer GPU
address, the byte length, the index type, the instance count, a base vertex and a base instance
(`MTL4RenderCommandEncoder.hpp:68`). The backend therefore cannot pass a subrange as an offset. It
advances the address by `index_offset * type_size`, shrinks the declared length by the same number
of bytes, and keeps `baseVertex = 0`. The first subrange already used the full address, so only the
second and later subranges were wrong.

A 16-bit strip cannot pass through unchanged. Metal restarts a strip at the largest index value and
has no restart toggle. RSX with restart disabled treats `0xFFFF` as a normal vertex index. The
backend widens such an array to `u32` before the draw; `0xFFFFFFFF` is then the only restart value,
and no RSX title can address that vertex.

The two byte rings reserve with one Metal-free helper. A block starts on a four-byte boundary, the
reservation consumes its aligned size, and one block larger than the ring grows the ring to the
block size. The earlier rule allocated the old capacity and then copied past the end.

## Confirmed deviations

| id | deviation | site | reference | severity | state |
|---|---|---|---|---|---|
| VX1 | A multi-subrange indexed draw passed `index_offset * type_size` as `baseVertex`. The second and later subranges drew from the wrong vertices. The address now advances and the length shrinks. | `MTLGSRender.cpp:2759-2776` | `VKDraw.cpp:1242-1252`; `MTL4RenderCommandEncoder.hpp:68` | H | fixed |
| VX2 | One allocation larger than the ring capacity allocated the old capacity, then copied past the buffer end. The reservation now grows to `max(capacity, aligned_size)`. | `MTLConstants.cpp:104-145`, `186-213`; `MTLConstants.h:228-247` | `VKVertexBuffers.cpp` ring reserve; `GLVertexBuffers.cpp` | H | fixed, unit-tested |
| VX3 | `byte_ring` placed each block at the previous unaligned end. Index, overlay vertex and overlay uniform blocks now reserve on a four-byte boundary. | `MTLConstants.cpp:186-213` | `VKVertexBuffers.cpp`; `GLVertexBuffers.cpp` | M | fixed, unit-tested |
| VX4 | The vertex upload writes the whole `[min_index, max_index]` range on every subdraw. OpenGL keeps `vertex_range.second` and uploads only the missing tail; Metal has no equivalent cache. | `MTLGSRender.cpp:2235-2360` | `GLVertexBuffers.cpp:160-210`; `VKVertexBuffers.cpp` | M | open (performance) |

## The VX1 argument order

| argument | Metal 4 meaning | value used |
|---|---|---|
| 1 | primitive type | translated RSX topology |
| 2 | index count | subrange count |
| 3 | index type | `UInt16` or `UInt32` |
| 4 | index buffer address | ring base + subrange byte offset |
| 5 | index buffer length | remaining bytes |
| 6 | instance count | 1, or the instanced count |
| 7 | base vertex | 0 |
| 8 | base instance | 0 |

The earlier code put the byte offset in argument 7. Metal read it as a vertex number, so a subrange
at byte 8 with 16-bit indices started at vertex 4 instead of index 4. A single-subrange draw was
unaffected because its offset is zero.

## No-deviation checks

These were read against the same reference and reproduce it. They need no change.

| check | result |
|---|---|
| base-index rebase | `vertex_base_index = index_rebase ? min_index : 0` matches the reference (`MTLGSRender.cpp:2359`). |
| inline-array extent | The clause's `min_index`/`max_index` and the expanded count match the shared writer. |
| expanded index output | `write_index_array_data_to_buffer` writes the widened array with the same count and type as the reference. |
| `vertex_index_offset` handoff | It reaches the shader constant block unchanged (`MTLGSRender.cpp:2360`, `:2495`). |
| attribute stride, offset and divisor | They come from the shared layout and reach the vertex descriptor without a second interpretation. |
| endianness | The guest window is little-endian and the host is little-endian; no swap is emitted, as in the reference. |

## Tests

`rpcs3/tests/test_rsx_metal4_state.cpp` covers the ring arithmetic without a GPU:

1. A block that fits keeps the capacity and starts on the alignment boundary.
2. A block whose aligned size exactly fills the remaining space fits.
3. A block larger than the capacity grows the ring to the block size.
4. A block that does not fit the remaining space moves to the start and grows when the block itself
   is larger than the capacity.

VX1 and VX4 need a device observation. VX1 is provable from the command stream: the address and
length of each subrange. VX4 needs a frame-time comparison.

## Open work

VX4 is the only open row. The design is a per-ring `vertex_range` cache keyed by the storage
address, the attribute layout and the frame slot, as OpenGL does. It must not cache across a guest
write to the same range. No title has been measured for this cost, so it stays open until a 3-D
frame shows the upload is material.

<!-- doc-end -->
