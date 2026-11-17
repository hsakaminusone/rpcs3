# Metal 4 semantics: measured, 2026-11-17

Two standalone metal-cpp harnesses, run on the A18 Pro in this machine, against
the exact call shapes the RSX Metal 4 backend issues. They exist to answer one
question: **is any Metal 4 behaviour different from what the backend assumes?**
Read-only research; neither harness is part of the build.

- Harness #1 `metal4-constants.mm` — the constant path (binding, capture,
  address arithmetic, the real `fp-64` epilogue).
- Harness #2 `metal4-runtime-gaps.mm` — the runtime the backend runs on (heap
  suballocations, allocator reset, pass boundaries, in-flight ring).

Both live outside the tree (scratch), and every case runs in a re-executed
child so a driver fault in one case cannot hide the rest.

Build (either file):

```
clang++ -std=c++20 -fobjc-arc -O1 -I3rdparty/metal-cpp \
    -framework Metal -framework Foundation -framework QuartzCore \
    <file>.mm -o <out>
```

Do **not** also link `3rdparty/metal-cpp/metal.cpp`: the file defines the
`NS`/`MTL` private implementations itself, and the duplicate symbols fail the
link.

## Harness #2: runtime gaps — all ten cases pass

The single most important case is `table-after-reset`. The backend's readback
path (`command_recorder::reenter_slot`, MTLCommandRecorder.cpp:653-654) does

```
slot.allocator->reset();
slot.command_buffer->beginCommandBuffer(slot.allocator);
```

and then keeps using the **same** `MTL4::ArgumentTable` objects while
`argument_binder` keeps its "only write changed slots" cache
(MTLArgumentBinds.cpp:41-70). If the reset dropped the table, every draw after
the first readback of a frame would read null constants — and Demon's Souls'
world shader is a constant multiply, so that is exactly a black world. The
harness reproduces the sequence and measures it.

| case | result |
| --- | --- |
| `table-after-reset` | **TABLE SURVIVES `allocator->reset()`** — draw 2 with no slot written still reads 64 |
| `table-after-reset-rebind` | control ok |
| `sanity-standalone` | SANITY OK (standalone buffers, the harness itself) |
| `heap-suballoc` | heap suballocations are visible to the GPU; **contents are read at execution time**, not snapshotted at encode |
| `heap-oob` | - |
| `table-across-passes` | table persists across command buffers |
| `pipeline-switch` | pipeline change with no slot rewrite keeps the bound block |
| `pass-restart-load` | `LoadActionLoad` preserves the earlier pass' content |
| `two-in-flight` | commit 1, already queued but not run, **read the CPU rewrite** — the 3-slot ring is load-bearing and the ring does hold |
| `mrt-identity` | identity logical→physical colour attachment mapping holds without `setColorAttachmentMap` |

Conclusions that follow directly:

1. **The argument table is not per-allocator state.** The binder's changed-slot
   cache is correct across `reenter_slot()`.
2. **Every constants ring is a heap suballocation** (MTLResourceManager.cpp:353)
   and behaves exactly like a standalone buffer, with one exception below.
3. **Contents are read at execution**, so the frame ring depth is what makes
   per-draw constants safe. `two-in-flight` measures the hazard the ring exists
   for; the ring holds it.
4. **`setColorAttachmentMap` is not needed** for the identity mapping the
   backend relies on. Its absence is not a defect.
5. **`LoadActionLoad` on a restarted pass keeps the earlier content**, so the
   backend's pass-restart path does not lose the image.

### The one real hazard the harness found

`heap-oob`: a shader that declares `float4 fc[1]` and indexes `fc[160]` reads
the **neighbouring heap suballocation** and returns its data (0.5 → 128) with no
fault. The backend declares exactly that shape and indexes `67 + offset`, and
`fs_constants_offset` rides in a vertex varying.

The consequence is a failure mode, not a bug: if a draw's
`fs_constants_offset` is ever wrong, the shader reads plausible garbage from
the next ring block instead of faulting or reading zero. A wrong offset is
therefore a **silent wrong colour**, which is the same signature class as the
DS symptom. It also means "the value decoded to 4.49e-15" cannot by itself be
taken as "the shader read the block that was filled" — the bytes could come from
the neighbouring block.

## Harness #1: the constant path — every binding assumption holds

15 cases; the ones that matter for DS:

- Per-draw argument-table capture, in both draw orders and with the slot mutated
  after both draws were encoded: **captured at the draw**, not at
  `setArgumentTable` and not at `endEncoding`.
- Two table objects alternate correctly.
- An unwritten slot reads a deterministic null (`initializeBindings`).
- `setAddress` at skew 4 and 8 is byte-exact; past `maxBufferBindCount` is
  harmless.
- `float4 fc[1]` is **not** bounds-clamped.
- An integer `[[user(locnN)]]` varying is flat (the channel
  `_fs_constants_offset` rides in).
- No residency set is required for shared buffers.

### The real `fp-64` epilogue, evaluated on the GPU

Buffer block `[67]` = `a1273fbd 00400000 a1273fbd 803f0000` (the bytes the guest
image holds, after the emulator's fill):

| variant | lane values | output |
| --- | --- | --- |
| raw | `4.489e-15, 2, 4.489e-15, 1` | `0,0,0` — the black frames |
| 16-bit half swap, all four lanes | `1.48, 8.97e-44, 1.48, 4.6e-41` | `255,255,255` — the white frames |
| `.x`/`.z` 32-bit byte-reversed, `.y`/`.w` as-is | `-0.0466, 2, -0.0466, 1` | `1.48148` lane division |

Two facts follow:

1. The existing `epilogue-halves` diagnostic arm is **invalid**: a half swap on
   all four lanes manufactures the white frames rather than explaining them.
2. The visible-world value needs `.x`/`.z` alone to be byte-reversed relative to
   `.y`/`.w`. One 16-byte guest-authored block, two byte conventions.

## What no further metal-cpp test can settle

The byte chain is already proven byte-faithful end to end:

- `FIFO_control::m_cache` is `std::byte[8][128]` copied with `mov_rdata`, no
  endian swap (RSXFIFO.cpp), so the mixed block is authored upstream of the
  emulator's FIFO.
- `nv308a`'s a8r8g8b8/y32 path is a raw `memcpy`/`memmove`; every one of the
  22,802 writes of the trailing literal is `src == dst` byte-identical.

So the remaining question is **not** a Metal 4 behaviour. It is which bytes the
guest means by those four words — a question the GPU cannot answer, because
both decodings are legal floats and the harness shows both are computed exactly.
