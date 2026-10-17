# LLD — `nv308a-image`: the NV308A "image from CPU" inline-upload engine

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Cluster `nv308a-image` (16 ids, `inventory/out/clusters/nv308a-image.csv`). This LLD obeys
`HLD-02-resource-model.md` (guest memory is one `StorageModeShared` `MTL::Buffer`; guest bytes are
canonical; surfaces/textures are projections kept coherent by copies, `HLD-02` §2.1/§2.4) and
`HLD-03-synchronization-model.md` (no driver hazard tracking; every CPU↔GPU edge is explicit,
`HLD-03` §2.3, and goes through the §3.4 chokepoint).

Reading conventions: `[V]` verified against the tree (`file:line`), `[I]` inference, `[U]`
unverified. IDs are taken verbatim from the cluster shard; **no register is invented.**

---

## 1. Scope

`NV308A` is the RSX **`NV30_IMAGE_FROM_CPU`** engine (`gcm_enums.h:782`): a host-driven pixel
upload. The guest delivers pixel data **inline in the FIFO** through the `M_NV308A_COLOR` method,
and the engine writes it into a 2D image whose destination is described by the **NV3062** 2D
context-surface registers (location/offset/pitch/format). It is the write-from-CPU counterpart of
the `NV3089` `IMAGE_IN` DMA blit: `NV3089` reads a source from guest memory and scales/converts it,
`NV308A` takes its source from the command stream and only stores or format-converts.

This LLD owns the **register model** of the sixteen `NV308A_*` ids and the **Metal 4 realization of
the upload**. It is a *guest-observable memory* operation: after a `COLOR` batch, the destination
range must byte-match what the described store/convert produces (`07`/`README` §4, layer A).
Correctness is mandatory, not best-effort.

**The module is not empty despite the inventory's "0 functions".** `nv308a.cpp` has exactly one
entry point, `nv308a::color::impl` (`nv308a.cpp:13`), which is a **nested static member** of
`struct nv308a::color` (`nv308a.h:10-13`); the generator's top-level-function counter does not see
it (`inventory/out/SUMMARY.md:518`). The engine is implemented, and it is registered as a
1792-entry bind array (`bind_array(NV308A_COLOR, 1, 256 * 7, nv308a::color::impl)`,
`rsx_methods.cpp:1723`), i.e. one handler for the whole `0xa400..0xbffc` method span
(`rsx_methods.cpp:1722`).

**Non-goals.** The destination descriptor objects `NV3062`/`NV309E` (`LLD-nv2d-surface`); the
`NV3089` scaled/format blit (`LLD-nv3089-image`); the `NV0039` buffer DMA (`LLD-nv0039-dma`); the
hazard/barrier placement (RN1/`HLD-03`); the FIFO/notify protocol and ZCULL report interface
(`HLD-03`, `sync-flip`). The `SET_CONTEXT_DMA_NOTIFIES` sink is never written by this engine `[V]`.

---

## 2. Register model

Nine context/handle registers, four scalar parameters and one streamed payload. All are plain
**value** registers: none has a dirty bit, none is event-driven, and the engine's semantics live
entirely in the trigger handler. Every `NV308A_*` method **except** `NV308A_COLOR` is installed as
`nullptr` (`rsx_methods.cpp:1544-1559`); only the payload method is bound.

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV308A_SET_OBJECT` | 0xa000 | none |  | na | none (inert subchannel handle) |
| `M_NV308A_SET_CONTEXT_DMA_NOTIFIES` | 0xa180 | none |  | na | none (inert notify context) |
| `M_NV308A_SET_CONTEXT_COLOR_KEY` | 0xa184 | none |  | moot | none (no colour-key op reachable) |
| `M_NV308A_SET_CONTEXT_CLIP_RECTANGLE` | 0xa188 | none |  | moot | none (no clip op reachable) |
| `M_NV308A_SET_CONTEXT_PATTERN` | 0xa18c | none |  | moot | none (pattern blit unreachable) |
| `M_NV308A_SET_CONTEXT_ROP` | 0xa190 | none |  | moot | none (ROP op unreachable) |
| `M_NV308A_SET_CONTEXT_BETA1` | 0xa194 | none |  | moot | none (blend op unreachable) |
| `M_NV308A_SET_CONTEXT_BETA4` | 0xa198 | none |  | moot | none (blend op unreachable) |
| `M_NV308A_SET_CONTEXT_SURFACE` | 0xa19c | none |  | reshape | destination-context select (NV3062 vs NV309E) |
| `M_NV308A_SET_COLOR_CONVERSION` | 0xa2f8 | none |  | emulate | host conversion matrix or `MTL4ComputeCommandEncoder` uniform `MTL::Buffer` |
| `M_NV308A_SET_OPERATION` | 0xa2fc | none |  | moot | none (only store/convert reachable) |
| `M_NV308A_SET_COLOR_FORMAT` | 0xa300 | none |  | reshape | destination `MTLPixelFormat` / host converter branch |
| `M_NV308A_POINT` | 0xa304 | `x[0:16]` / `y[16:16]` (`rsx_decode.h:468-494`) |  | reshape | destination byte origin in the guest-window `MTL::Buffer` |
| `M_NV308A_SIZE_OUT` | 0xa308 | none (raw `u32`; low 16 = width) |  | reshape | destination row width / per-batch write bound |
| `M_NV308A_SIZE_IN` | 0xa30c | none (raw `u32`; low 16 = width) |  | moot | none (source extent implicit in the FIFO stream) |
| `M_NV308A_COLOR` | 0xa400 | none (streamed payload) | `nv308a::color::impl` | emulate | host store/convert into the `mtl_guest_window` `MTL::Buffer` + `HLD-03` §3.4 barrier chokepoint |

**Decoder-coverage gap.** Only **1 of 16** registers has a decoded field: `NV308A_POINT` is the
lone `registers_decoder<>` specialisation (`rsx_decode.h:468-494`); the other fifteen are raw `u32`
(`has_decoder=0`, `n_fields=0` for each in the shard). The engine nevertheless reads two more raw
values through hand-rolled accessors that mask the low 16 bits: `nv308a_size_in_x()` and
`nv308a_size_out_x()` (`rsx_methods.h:1257-1265`). The design must therefore treat the register
semantics of `SET_COLOR_FORMAT`, `SET_OPERATION`, `SET_COLOR_CONVERSION` and `SET_CONTEXT_SURFACE`
as **undecoded** `[U]` and not assume the low-16-bit conventions of the size registers generalise.

**What the handler actually reads.** `color::impl` reads only: `nv308a_size_out_x()` (`nv308a.cpp:15`),
`nv308a_x()`/`nv308a_y()` (`nv308a.cpp:49-50`), the FIFO argument span (`nv308a.cpp:28,52`), and the
**NV3062** destination descriptor via the shared `blit_engine_output_*` accessors —
`blit_engine_output_location_nv3062()` (`rsx_methods.h:1137-1140`), `..._offset_nv3062()`
(`:1142-1145`), `..._pitch_nv3062()` (`:1157-1160`) and `blit_engine_nv3062_color_format()`
(`:1147-1150`). It does **not** read the engine's own `SET_COLOR_FORMAT`, `SET_OPERATION`,
`SET_COLOR_CONVERSION`, `SET_CONTEXT_SURFACE`, `SIZE_IN`, or any `SET_CONTEXT_*` register.

**Streaming semantics.** `NV308A_COLOR` is not one register but a **payload stream**: the handler is
invoked once per word, `index = reg - NV308A_COLOR` (`nv308a.cpp:16`), and it consumes a run of
consecutive argument words in a single call — `count = min(fifo_args_cnt, fifo_read_limit,
method_range)` (`nv308a.cpp:28-36`) — advancing the FIFO by `count - 1` (`nv308a.cpp:60`). The
destination X is `nv308a_x() + index` (`nv308a.cpp:49`); `index >= nv308a_size_out_x()` is skipped
(`nv308a.cpp:18-22`). A non-incrementing command (`RSX_METHOD_NON_INCREMENT_CMD_MASK`) moves only
the last word (`nv308a.cpp:91-97,142-148`).

**Lifetime.** Value registers, consumed per batch. `NV308A_POINT`, `SIZE_OUT` and `SIZE_IN` are
reset to `0` at context init (`rsx_methods.cpp:688-690`). No register carries state across a batch:
the guest re-arms the point and format for each upload.

---

## 3. Metal 4 mapping

**The shape change.** RSX describes a destination *surface* by
`(base, offset, pitch, format)` in guest address space; Metal has no address-identity resource
(`HLD-02` §2.0). Under `HLD-02` §2.1 the whole guest window is one `StorageModeShared`
`MTL::Buffer` created with `newBufferWithBytesNoCopy:...`; on Apple silicon it is
`hasUnifiedMemory()` and CPU-visible (`README` §4). The handler already operates on raw host
pointers (`vm::_ptr<u8>(dst_address)`, `nv308a.cpp:86`). Therefore the natural realization of the
upload is a **host store/convert into the guest-window buffer** — exactly the existing code — with
Metal supplying only the ordering/visibility edge. The disposition is `emulate`, not `direct`:
there is no Metal object that "is" an inline CPU upload, and there is no blit encoder to reuse
(`README` §4.4, `04` §2.6).

### 3.1 Destination descriptor (sourced from NV3062, not from NV308A)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV308A_SET_CONTEXT_SURFACE` | 0xa19c | none |  | reshape | destination-context select (NV3062 vs NV309E) |
| `M_NV308A_SET_COLOR_FORMAT` | 0xa300 | none |  | reshape | destination `MTLPixelFormat` / host converter branch |
| `M_NV308A_SET_OPERATION` | 0xa2fc | none |  | moot | none (only store/convert reachable) |
| `M_NV308A_SET_COLOR_CONVERSION` | 0xa2f8 | none |  | emulate | host conversion matrix or compute-encoder uniform |
| `M_NV308A_POINT` | 0xa304 | `x[0:16]` / `y[16:16]` (`rsx_decode.h:468-494`) |  | reshape | destination byte origin |
| `M_NV308A_SIZE_OUT` | 0xa308 | none (low 16 = width) |  | reshape | destination row width / per-batch write bound |

The destination base, offset, pitch and pixel format are taken **exclusively from the NV3062
registers** (`nv308a.cpp:45-47,69`), i.e. the shared 2D context-surface descriptor owned by
`LLD-nv2d-surface`; the destination byte address is
`get_address(dst_offset + (x * bpp) + (out_pitch * y), dst_dma, data_length)` (`nv308a.cpp:78,117`).
Under Metal 4 this resolves to a **byte offset into the guest-window `MTL::Buffer`** (`HLD-02`
§2.1), never a new allocation. `bpp` is 4 for `a8r8g8b8`/`y32` and 2 for `r5g6b5`
(`nv308a.cpp:76,115`). `SET_COLOR_FORMAT` and `SET_CONTEXT_SURFACE` are `reshape` rather than `na`
because they are *real* selectors that the current path ignores: if hardware honours
`NV308A_SET_COLOR_FORMAT` over `NV3062_SET_COLOR_FORMAT`, the native backend must branch on the
former (§6 Q1). `SET_OPERATION`/`SET_COLOR_CONVERSION` are `moot`/`emulate` because the CPU path
realises only an unconditional store or ARGB8→RGB565 convert (`nv308a.cpp:69-160`).

### 3.2 Payload and conversion

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV308A_SIZE_IN` | 0xa30c | none (low 16 = width) |  | moot | none (source extent implicit in the FIFO stream) |
| `M_NV308A_COLOR` | 0xa400 | none (streamed payload) | `nv308a::color::impl` | emulate | host store/convert into the guest-window `MTL::Buffer` + `HLD-03` §3.4 barrier |

Two destination-format branches exist and only these two:

1. **`a8r8g8b8` / `y32` — bit-cast store** (`nv308a.cpp:71-112`): a straight 4-byte-per-pixel
   `memcpy` (non-overlapping, local memory) or `memmove` (overlapping, main memory)
   (`nv308a.cpp:99-108`), guarded by `reservation_lock` (`nv308a.cpp:89`). This is a host copy of
   `count * 4` bytes; on Metal 4 it is a write into `mtl_guest_window.buffer->contents() +
   offset`.
2. **`r5g6b5` — ARGB8→RGB565 convert** (`nv308a.cpp:113-157`): the reference rounding is
   `r = (r * 32) / 255; g = (g * 64) / 255; b = (b * 32) / 255;` packed `(r<<11)|(g<<5)|b`
   (`nv308a.cpp:129-140`), writing 2 bytes per pixel.

Any other NV3062 destination format reaches the `default` branch and **aborts** with
`"Unreachable"` (`nv308a.cpp:158-161`). `SIZE_IN` is `moot` because the input length is the FIFO
argument run, not a register (`nv308a.cpp:28-36`).

### 3.3 Ordering, invalidation and the CPU↔GPU edge

The handler brackets the store with the existing RPCS3 hazards whose *meaning* changes under
Metal 4 (`HLD-03` §2.3):

- `invalidate_fragment_program(dst_dma, dst_offset, length)` (`nv308a.cpp:110,155`) — if the CPU
  writes over the live fragment ucode, the program must reload (`RSXThread.cpp:2696-2721`). In the
  native backend this is the same **texture/surface-cache invalidation** seam as
  `LLD-nv0039-dma`/`LLD-nv3089`: the destination bytes are now canonical guest memory, so a
  `MTL::Texture` projection over that range must be invalidated or re-uploaded (`HLD-02` §2.5).
- `write_barrier` (deferred) and the subsequent reader edge — a **CPU→GPU** edge: a later draw or
  sample of the destination must observe the CPU stores. This is a visibility/scope point plus
  ordering against subsequent encoders, routed through the `HLD-03` §3.4 chokepoint — never an
  inline barrier (`HLD-03` §4). This is identical in shape to `LLD-nv0039-dma` §3.
- `mm_flush`/`reservation_lock` are backend-agnostic host-memory-manager and exclusion mechanics and
  survive unchanged (`11` §3.4).
- `strict_rendering_mode` with `count == 1` calls `RSX(ctx)->sync()` (`nv308a.cpp:62-67`):
  `NV308A_COLOR` can be used as a **custom sync primitive**. Under Metal 4 that maps to a
  `MTLSharedEvent`/commit-feedback point, not a Metal command.

### 3.4 The rejected GPU path

| Alternative | Verdict |
|---|---|
| `MTL4ComputeCommandEncoder` + MSL convert kernel over the guest buffer | **not-recommended** (same reasoning as `LLD-nv0039-dma` §4): the source is CPU-written and the destination is immediately CPU-observable; a GPU convert adds two visibility round-trips on data that need never leave the CPU. |
| `copyFromBuffer:...toBuffer:...` on the compute encoder for the bit-cast case | **not-recommended**: the bit-cast is already a host `memcpy`/`memmove`; a GPU copy buys nothing and costs an ordering edge. |
| A `MTL::Texture` destination projection + render/compute write | **only if** the destination is a live render target/sampled texture; then the projection is invalidated and the canonical bytes are written, not a parallel GPU path (`HLD-02` §2.5). |

**Objects used.** No new Metal object is created. The only Metal surface touched is the guest-window
`MTL::Buffer` (`HLD-02` §2.1); the only Metal mechanism is the visibility point of the `HLD-03`
§3.4 barrier chokepoint. A compute encoder appears only in the rejected alternative and in the
`emulate` disposition of `SET_COLOR_CONVERSION` *if* a non-RGB conversion is ever enabled.

---

## 4. Impossible / not-recommended

- **Not-recommended: routing the upload through the GPU.** `supports_passthrough_dma` is forced
  `false` (`10` §9 R4); guest bytes are host bytes; the CPU store is the correct and cheapest
  realization. The chosen fallback *is* the host store/convert.
- **Not-recommended: treating `NV308A_SET_COLOR_FORMAT` as authoritative over
  `NV3062_SET_COLOR_FORMAT`** (or vice versa) before the semantics are resolved. RPCS3 has always
  read the NV3062 descriptor (`nv308a.cpp:69`); changing the source of truth is a
  correctness-bearing bet, not a mapping choice. See §6 Q1.
- **Not-recommended: `MTL4VisibilityOptionResourceAlias`** for overlapping main-memory writes. The
  overlap is a byte-range alias inside one buffer, handled by `memmove`/non-increment handling
  exactly as today (`nv308a.cpp:99-108`), consistent with `HLD-02` §2.5's rejection of heap
  aliasing for guest aliasing.
- **Not a mapping failure:** no part of this cluster is `impossible`. The engine's semantics are
  fully reachable in host code; the risk is byte-correctness of the conversion, not absence of an
  API. The `default`-branch abort (`nv308a.cpp:158-161`) is an implementation limit (three
  destination formats), not a Metal limit.
- **Unsupported today:** a `swizzle2d` destination. `SET_CONTEXT_SURFACE` is ignored and the
  NV3062 linear path is hard-coded (`nv308a.cpp:45-47`); whether the swizzled NV309E destination is
  reachable through this engine is open (§6 Q2).

---

## 5. Invariants & failure modes

**Invariants.**

1. After a `COLOR` batch, the destination region is a byte-exact image of the described store or
   convert (`nv308a.cpp:99-157`); correctness is guest-observable and mandatory.
2. Destination X is `nv308a_x() + index` and Y is `nv308a_y()` (`nv308a.cpp:49-50`); the write is
   bounded by `nv308a_size_out_x()` (`nv308a.cpp:15,18`).
3. Only three destination formats are legal: `a8r8g8b8` and `y32` (bit-cast) and `r5g6b5`
   (convert); anything else aborts (`nv308a.cpp:69-161`).
4. The `r5g6b5` conversion uses the 5/6/5 rounding in `nv308a.cpp:129-140`; the MSL/host converter
   must reproduce it exactly.
5. A GPU reader of the destination observes the CPU stores (the `HLD-03` §2.3 Edge B visibility
   point); a write over the live fragment ucode forces a reload (`nv308a.cpp:110,155`).
6. A non-incrementing `COLOR` command moves only the last word of the batch (`nv308a.cpp:91-97`).

**Failure modes.**

- **F1 missing visibility edge:** the next draw/sample observes stale destination bytes — silent
  wrong content, not a crash. Detection: `11` H5 region diff after a replay containing an upload.
- **F2 wrong format source:** binding the destination layout from `NV308A_SET_COLOR_FORMAT` while
  the reference used `NV3062_SET_COLOR_FORMAT` (or the reverse) produces a wrong pixel layout.
  Detection: H3 state oracle + H5 diff on a 16-bit target.
- **F3 `r5g6b5` rounding drift:** a converter that rounds differently from `nv308a.cpp:129-140`
  diverges by ±1 LSB per channel. Detection: H5 on a small RGB565 target.
- **F4 streaming mis-count:** mis-deriving `count` (`nv308a.cpp:28-36`) desynchronises the FIFO or
  drops the tail of a batch. Detection: the built-in FIFO desync alarm (`11` §3.4) + H3.
- **F5 destination is a live texture projection:** writing guest bytes without invalidating the
  `MTL::Texture` over that range leaves the GPU sampling stale texels (`HLD-02` §2.5). Detection:
  H5 on an RTT→sample sequence.
- **F6 overlap policy:** choosing `memcpy` where `memmove` is required (or the reverse)
  self-corrupts main-memory uploads (`nv308a.cpp:99-108`).

---

## 6. Open questions

- **Q1 `[U]` Which register is the destination format of record?** `NV308A_SET_COLOR_FORMAT`
  (0xa300) is never read; the path uses `NV3062_SET_COLOR_FORMAT` (`nv308a.cpp:69`). Does hardware
  honour the engine's own format register over the NV3062 descriptor? *Experiment:* issue an upload
  with the two registers deliberately mismatched and diff the destination bytes against the
  reference backend. This decides whether `SET_COLOR_FORMAT` is `reshape` (honoured) or `moot`.
- **Q2 `[U]` Is a `swizzle2d` (NV309E) destination reachable through `NV308A_SET_CONTEXT_SURFACE`?**
  The path hard-codes NV3062 and ignores the selector (`nv308a.cpp:45-47`). *Experiment:* grep
  replays for a `NV308A_COLOR` batch whose `SET_CONTEXT_SURFACE` differs from the surface2d value,
  and for NV309E writes with no intervening NV3089 trigger.
- **Q3 `[U]` CPU→GPU visibility without a queue drain.** Identical to `LLD-nv0039-dma` Q1: can a
  CPU store into the guest-window shared buffer be ordered against already-committed encoders
  without a full queue drain? *Experiment:* upload into a region the next draw samples, with no
  queue wait, and diff across a replay.
- **Q4 `[I]` Is `NV308A_COLOR` ever used as a real sync primitive?** The handler has a strict-mode
  `sync()` for `count == 1` (`nv308a.cpp:62-67`). *Experiment:* instrument the replay corpus for
  single-word `COLOR` batches and check whether a `sync`/`label` edge follows. Decides whether the
  `HLD-03` Edge B path must special-case a one-word upload.
- **Q5 `[U]` Do `NV308A_SIZE_IN`/the high 16 bits of `SIZE_OUT` matter?** Neither is read
  (`rsx_methods.h:1257-1265`). *Experiment:* find a title that streams more words than
  `SIZE_OUT.width` and observe whether the reference backend truncates or wraps.
- **Q6 `[I]` Should the `r5g6b5` converter share one MSL/host kernel with the
  `LLD-nv2d-surface`/`texture_cache` converters?** They are the same 5/6/5 rounding
  (`nv308a.cpp:129-140`). One scalar CPU reference (H0) and one MSL kernel (H1) would test it once.

---

## 7. Provenance

**This tree `[V]` (paths under `rpcs3/Emu/RSX/`).**
`gcm_enums.h:782-798` (`NV30_IMAGE_FROM_CPU (NV308A)` method enum);
`NV47/HW/nv308a.h:10-13` (`struct nv308a::color`), `NV47/HW/nv47.h:6` (module include);
`NV47/HW/nv308a.cpp:13-163` (`color::impl`; `out_x_max`/`index` `:15-16`; skip `:18-22`; FIFO count
derivation `:28-36`; NV3062 destination `:45-47`; point `:49-50`; arg span `:52`; `skip_methods`
`:60`; strict `sync` `:62-67`; format switch `:69`; `a8r8g8b8`/`y32` `:71-112`; `r5g6b5` convert
`:113-157`; `default` abort `:158-161`; `reservation_lock` `:89`; `memcpy`/`memmove` `:99-108`;
`invalidate_fragment_program` `:110,155`);
`rsx_decode.h:468-494` (`registers_decoder<NV308A_POINT>`, `x[0:16]`/`y[16:16]`);
`rsx_methods.h:1247-1265` (`nv308a_x/y`, `nv308a_size_in_x`, `nv308a_size_out_x`),
`:1137-1160` (`blit_engine_output_location/offset/pitch_nv3062`, `blit_engine_nv3062_color_format`);
`rsx_methods.cpp:688-690` (reset defaults), `:1543-1559` (all methods `nullptr`),
`:1722-1723` (`bind_array(NV308A_COLOR, 1, 256 * 7, …)`);
`RSXThread.cpp:3204-3208` (pretty-printer skip to `NV3089_SET_OBJECT`),
`:2696-2721` (`invalidate_fragment_program`), `:3221-3225` (`read_barrier`);
`RSXFIFO.cpp:840-858` (capture range for the `NV308A_COLOR` stream);
`gcm_printing.cpp:856-871` (method names).

**Inventory (facts).** `inventory/out/clusters/nv308a-image.csv` (16 ids; `NV308A_POINT` the only
decoded row, `NV308A_COLOR` the only bound row); `inventory/out/SUMMARY.md:216-235,480-481,518`
(cluster table, decoded-field table, `nv308a.cpp` "0 functions" artifact);
`inventory/SCHEMA.md` §4 (disposition vocab) §5 (CSV columns) §6 (LLD template).

**Sibling scopes `[V]`.** `HLD-02-resource-model.md` §2.0 (shape change), §2.1 (guest window,
`mtl_guest_window`), §2.4 (swizzle/import-export, Option B), §2.5 (no heap aliasing for guest
aliasing), §2.7 (format table); `HLD-03-synchronization-model.md` §2.3 (CPU↔GPU edges), §3.4
(barrier chokepoint), §4 (invariants); `LLD-nv0039-dma.md` §3 (the identical host-copy + visibility
argument), §4 (GPU-path rejection); `LLD-nv2d-surface.md` §2/§3.1 (the NV3062 destination
descriptor this engine borrows); `LLD-nv3089-image.md` (the DMA counterpart);
`10-rsx-native-metal4-architecture.md` §3.1 (blit-engine semantics matter, not `vkCmdBlitImage`),
§4.1 (guest memory is unified/host), §9 R4 (`supports_passthrough_dma=false`, CPU copy);
`04-images-formats-samplers-rtt.md` §2.6 (copies ride on the compute encoder; no blit encoder);
`README.md` §4 shifts 1/4 (no hazard tracking; no blit encoder); `11-verification-loop.md` §3.4
(FIFO desync alarm), §9 (H0/H5 oracles).

<!-- doc-end -->
