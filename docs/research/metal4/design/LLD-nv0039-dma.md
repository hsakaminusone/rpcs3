# LLD — `nv0039-dma`: the NV0039 buffer-to-buffer DMA copy engine

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `nv0039-dma` (12 ids, `inventory/out/clusters/nv0039-dma.csv`). This LLD obeys
`HLD-02-resource-model.md` (guest memory is one `StorageModeShared` `MTL::Buffer`, `HLD-02` §2.1)
and `HLD-03-synchronization-model.md` (no driver hazard tracking; CPU↔GPU edges are `HLD-03` §2.3).

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`NV0039` is the RSX **buffer management** engine (`NV47/HW/nv0039.h:1`): a guest-issued
buffer-to-buffer DMA — a linear/pitched 2D block transfer between two guest memory ranges, with
independent input and output row pitch and an optional per-column byte stride. It is triggered by a
single method, `M_NV0039_BUFFER_NOTIFY` (handler `nv0039::buffer_notify`, `nv0039.cpp:69-177`);
every other register in the cluster is inert configuration read only at trigger time
(`nv0039.cpp:71-91`).

This LLD owns the **register model** of the twelve `NV0039_*` ids and the **Metal 4 realization of
the transfer**. It is a *guest-observable memory* operation: after `BUFFER_NOTIFY` returns, the
destination range must byte-match what the described transfer produces (`07`/`README` §4, layer A).

**Non-goals.** The surface/blit-engine path (`LLD-nv2d-surface`, `LLD-nv3089`, `nv3089-image`) and
the FIFO/notify protocol and ZCULL report interface (`HLD-03`, `sync-flip`). The `notify` argument
of `BUFFER_NOTIFY` is read (`nv0039.cpp:77`) but only logged; no guest-visible sink is written by
this cluster `[V]`.

---

## 2. Register model

Two DMA context locations, four offsets/pitches/lengths and one stride format; all plain value
registers (no dirty bit; the config registers are latched by the guest and consumed only when
`BUFFER_NOTIFY` fires). All twelve methods are installed as `nullptr` in the dispatch table
(`rsx_methods.cpp:1512-1524`) — the cluster is pure state, not event-driven.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV0039_SET_OBJECT` | 0x2000 | none |  | na | none (inert subchannel handle) |
| `M_NV0039_SET_CONTEXT_DMA_NOTIFIES` | 0x2180 | none |  | na | none (inert notify context) |
| `M_NV0039_SET_CONTEXT_DMA_BUFFER_IN` | 0x2184 | `CellGcmLocation input_dma` (`rsx_decode.h:3668`) |  | reshape | `MTL::Buffer` (guest window) read base |
| `M_NV0039_SET_CONTEXT_DMA_BUFFER_OUT` | 0x2188 | `CellGcmLocation output_dma` (`rsx_decode.h:3645`) |  | reshape | `MTL::Buffer` (guest window) write base |
| `M_NV0039_OFFSET_IN` | 0x230c | `u32 input_offset` (`rsx_decode.h:2220`) |  | reshape | `MTL::Buffer` read offset (host pointer) |
| `M_NV0039_OFFSET_OUT` | 0x2310 | `u32 output_offset` (`rsx_decode.h:2197`) |  | reshape | `MTL::Buffer` write offset (host pointer) |
| `M_NV0039_PITCH_IN` | 0x2314 | `input_pitch` (`rsx_decode.h:2105`) |  | reshape | source row stride of the host copy loop |
| `M_NV0039_PITCH_OUT` | 0x2318 | `output_pitch` (`rsx_decode.h:2128`) |  | reshape | destination row stride of the host copy loop |
| `M_NV0039_LINE_LENGTH_IN` | 0x231c | `input_line_length` (`rsx_decode.h:2151`) |  | reshape | row byte width of the host copy loop |
| `M_NV0039_LINE_COUNT` | 0x2320 | `line_count` (`rsx_decode.h:2174`) |  | reshape | row count of the host copy loop |
| `M_NV0039_FORMAT` | 0x2324 | `input_format[0:8]` / `output_format[8:8]` (`rsx_decode.h:3809-3831`) |  | emulate | MSL compute kernel or host strided-gather loop |
| `M_NV0039_BUFFER_NOTIFY` | 0x2328 | none (trigger; `arg` = notify value) | `nv0039::buffer_notify` | emulate | host `memcpy`/`memmove` + `MTL4VisibilityOption` barrier |

**Decode facts.** `input_format`/`output_format` are **column byte strides**, not pixel formats:
only the first byte of each column is read/written (`nv0039.cpp:75-76`). `validate_buffer_notify`
rejects a zero 2D area and any stride outside `1..4` (`nv0039.cpp:46-67`). A transfer is a
*block transfer* iff both strides are 1 and both pitches equal `line_length × stride`
(`nv0039.cpp:93-98`). Read and write ranges are derived from the pitched extents, not from the
texture pitches (`nv0039.cpp:101-102`).

**Lifetime.** `SET_OBJECT` (default `0x313371c3`, `rsx_methods.cpp:1178`) and
`SET_CONTEXT_DMA_NOTIFIES` are stored and never read. The remaining registers are read exactly once
per trigger, before any locking (`nv0039.cpp:71-91`). There is no register-level state that
survives a trigger.

---

## 3. Metal 4 mapping

**The load-bearing fact: guest memory is host memory.** Under `HLD-02` §2.1 the whole guest window is
one `StorageModeShared` `MTL::Buffer` created with `newBufferWithBytesNoCopy`; on Apple silicon it is
`hasUnifiedMemory()` and CPU-visible (`README` §4). The handler already operates on raw host
pointers: `vm::_ptr<u8>(write_address)` / `vm::_ptr<u8>(read_address)` (`nv0039.cpp:126-127`).

**Decision (forks stated, one chosen):**

- **Chosen — CPU copy.** Realise the transfer as a host `memcpy`/`memmove`/`block2d_copy` over the
  guest-window bytes, exactly the existing code (`nv0039.cpp:153-176`). Because source and
  destination are already host-resident, a CPU copy is correct and cheap, and it is *the only* path
  that makes the destination CPU-visible without a round-trip. Consequence: the operation is
  `emulate` (host code), not `direct`.
- **Rejected — `MTL4ComputeCommandEncoder.copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:`**
  (or a compute kernel for the pitched/strided cases). It would require the CPU-written source to be
  made visible to the GPU *and* the GPU-written destination to be made visible back to the CPU before
  any guest read — two extra synchronisation round-trips on data that never needed to leave the CPU
  (`README` §4.4: the blit encoder is gone; copies ride on the compute encoder, but that is a
  capability, not an obligation). CPU copy is not-recommended to replace with GPU copy.

**Sub-areas.**
1. *Block transfer* (`is_block_transfer`, `nv0039.cpp:95-98`): a plain `memcpy` when ranges are
   disjoint (`:157`), `memmove` when they overlap (`:167`).
2. *Pitched 2D* (`block2d_copy`, `nv0039.cpp:36-44`): row loop over `line_length` bytes with
   `in_pitch`/`out_pitch` strides. This is the host-shaped equivalent of a
   `MTL4ComputeCommandEncoder.copyFromBuffer` with a pitch; the pitched copy has no single Metal
   method, so it stays a host loop.
3. *Overlapping pitched range* (`nv0039.cpp:136-176`): `memmove` if pitches match, else a
   temp-buffer double copy (`:172-176`).
4. *Strided channel gather* (`in_format > 1 || out_format > 1`, `block2d_copy_with_stride`,
   `nv0039.cpp:17-34,144-151`): rare (Destiny, `:147`). Either keep the host loop or, if it ever
   shows up hot, an `MTL4ComputeCommandEncoder` MSL kernel. Correctness is the host loop; the
   compute path is a `[U]` perf question (§6 Q3).

**Ordering (the part that is Metal, not memcpy).** The handler brackets the copy with two existing
RPCS3 hazards whose *meaning* changes under Metal 4:
- `read_barrier(read_address, read_length, !is_block_transfer)` (`nv0039.cpp:106`) — a GPU→CPU
  edge: the CPU must not read a range the GPU may still be writing. Under Metal 4 this is the
  producer side of `HLD-03` §2.3 Edge B (RTT/label readback): the writer's encoder must be ordered
  before the CPU read via a producer `barrierAfterStages:` **and** a queue-visibility point
  (`MTL4CommitFeedback`/`MTLSharedEvent`) — a barrier alone does not order a CPU load `[V]`.
- deferred `write_barrier(write_address, write_length)` (`nv0039.cpp:118-122`) — a CPU→GPU edge: a
  subsequent GPU read of the destination must observe the CPU stores. On unified shared memory this
  is a visibility/scope point (`MTL4VisibilityOption`) plus ordering against subsequent encoders;
  it maps to the `HLD-03` §3.4 barrier chokepoint, not to an inline barrier.
- `invalidate_fragment_program(dst_dma, dst_offset, write_length)` (`nv0039.cpp:104`) — if the CPU
  writes over the cached fragment ucode, the program must be reloaded (`RSXThread.cpp:2696-2721`).
- ZCULL overlap: if `read_barrier` reports `result_zcull_intr`, deferred stats copy via
  `copy_zcull_stats` may absorb the whole write (`nv0039.cpp:107-115`).
- `mm_flush` (`nv0039.cpp:129-134`) and `reservation_lock` (`:124`) are backend-agnostic host
  memory-manager / exclusion mechanics and survive unchanged (`11` §3.4).

**Objects used.** No new Metal object is created by this cluster. The only Metal surface touched is
the guest-window `MTL::Buffer` (`HLD-02` §2.1); the only Metal mechanisms are the visibility point
of a barrier/event and, in the rejected GPU variant, a `MTL4ComputeCommandEncoder`.

---

## 4. Impossible / not-recommended

- **Not-recommended: routing the copy through the GPU.** `supports_passthrough_dma` is forced
  `false` (`10` §9 R4); the guest bytes are host bytes; a GPU copy adds a round-trip and an
  ordering problem for zero benefit. The chosen fallback *is* the CPU copy.
- **Not-recommended: `MTL4VisibilityOptionResourceAlias`** for the source/destination overlap. The
  overlap is a byte-range alias inside one buffer, not a resource alias; it is handled by
  `memmove`/temp-buffer exactly as today (`nv0039.cpp:136-176`), consistent with `HLD-02` §2.5's
  rejection of heap aliasing for guest aliasing.
- **Not a mapping failure:** the strided channel-gather (`in_format>1||out_format>1`) has no single
  Metal method; that is why its disposition is `emulate` (host loop), not `impossible` — the
  semantics are reachable, just not natively.
- **Impossible:** a GPU transfer cannot be the *sole* realization while the destination must be
  immediately CPU-observable and the source is immediately CPU-written; forcing it would be a
  correctness trap (two visibility edges per transfer), so it is excluded by design rather than by
  absence of an API.

---

## 5. Invariants & failure modes

**Invariants.**
1. After `BUFFER_NOTIFY` returns, the destination region is a byte-exact image of the described
   transfer (`nv0039.cpp:144-176`); correctness is guest-observable and mandatory.
2. A read of the source range happens only after every GPU writer of that range is visible; a write
   of the destination range becomes visible to every subsequent GPU reader (`nv0039.cpp:106,118-122`).
3. Overlapping same-buffer transfers never self-corrupt (explicit `memmove`/temp buffer,
   `nv0039.cpp:136-176`).
4. An unvalidated stride (`<1` or `>4`) or zero area never copies (`validate_buffer_notify`,
   `nv0039.cpp:46-67,79`).
5. A write over the live fragment ucode forces a reload (`nv0039.cpp:104`).

**Failure modes.**
- **F1 missing write barrier:** the next draw/sample observes stale destination bytes — silent
  wrong content, not a crash. Detection: `11` H5 region diff after a replay containing a DMA write.
- **F2 missing read barrier:** the CPU copies a partially-written surface (RTT→DMA readback) —
  torn output. Detection: H5 diff on the DMA source region.
- **F3 ZCULL overlap not deferred:** report/stat corruption (`nv0039.cpp:107-115`); detection: the
  ZCULL report oracle (`HLD-03` §2.3 Edge C).
- **F4 guest remap during the copy:** `mm_flush`/`reservation_lock` exclusion is the guard
  (`nv0039.cpp:124-134`); removing it in a native backend reintroduces the existing race.

---

## 6. Open questions

- **Q1 `[U]` CPU→GPU visibility without a queue drain.** Does the Metal 4 model let a *CPU* store
  into the guest-window shared buffer be ordered against already-committed encoders without a full
  queue drain (and without a `waitUntilCompleted`)? This is the same shape as `HLD-03` §2.3 Edge B
  (label write) and decides whether `write_barrier` is a cheap scope point or a stall.
  *Experiment:* DMA into a region the very next draw samples, with no queue wait, and diff the
  destination across a replay.
- **Q2 `[I]` Conservative `memmove` policy.** The code already picks `memmove` only for main-memory
  destinations (`nv0039.cpp:99-108,153-169`). Decide whether the native backend keeps that
  conservatism or derives overlap from DMA location + range; the latter is a correctness-bearing
  bet on guest aliasing.
- **Q3 `[U]` GPU compute for the pitched/strided paths.** Only worth it if a title drives large
  non-block DMA copies each frame; correctness is the host loop either way, so this is a pure perf
  question (H7).

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`NV47/HW/nv0039.h:1` (module title); `NV47/HW/nv0039.cpp:17-44` (`block2d_copy_with_stride`,
`block2d_copy`), `:46-67` (`validate_buffer_notify`), `:69-177` (`buffer_notify`; reads `:71-91`,
`invalidate_fragment_program` `:104`, `read_barrier` `:106-115`, `copy_zcull_stats` `:110`,
deferred `write_barrier` `:118-122`, `reservation_lock` `:124`, host pointers `:126-127`,
`mm_flush` `:129-134`, overlap detection `:136-142`, strided gather `:144-151`, `memcpy` `:157`,
`block2d_copy` `:161`, `memmove` `:167`, temp double copy `:172-176`);
`rsx_decode.h:2105,2128,2151,2174,2197,2220,3645,3668,3809-3831`; `rsx_methods.h:1197-1245`
(`nv0039_*` accessors); `rsx_methods.cpp:1178` (reset default), `:1512-1524` (all methods
`nullptr`); `RSXThread.h:386,395-396` (`copy_zcull_stats`, `read_barrier`, `write_barrier`);
`RSXThread.cpp:2696-2721` (`invalidate_fragment_program`), `:3221-3225` (`read_barrier`).

**Inventory (facts).** `inventory/out/clusters/nv0039-dma.csv` (12 ids, `BUFFER_NOTIFY` the only
bound row); `inventory/out/SUMMARY.md:154-169,473-474`; `inventory/SCHEMA.md` §4 (disposition
vocab) §5 (CSV columns) §6 (LLD template).

**Sibling scopes `[V]`.** `HLD-02-resource-model.md` §2.1 (guest window), §2.5 (no heap aliasing
for guest aliasing), §3.1 (`mtl_guest_window`); `HLD-03-synchronization-model.md` §2.3 Edge B,
§3.4 (`mtl::emit_barrier`), §4 (invariants); `10-rsx-native-metal4-architecture.md` §4.1, §5
(DMA reshape, `−200` LOC), §9 R4 (`supports_passthrough_dma=false`, CPU copy);
`README.md` §4 shifts 1/4 (no hazard tracking; copies on the compute encoder).

<!-- doc-end -->
