# LLD — `sync-flip`: NV406E semaphores, reports, idle/wait, FIFO barriers & the flip hand-off

Owner/scope: `design/` cluster `LLD-sync-flip` (parent `HLD-03`) | Status: draft | Date: 2026-10-16

> **Read-only research.** No file under `rpcs3/**` was modified to produce this document. All
> `file:line` citations were read on 2026-10-16 on branch `metal4`; the tree is a moving branch, so
> **symbol names are the stable reference and line numbers are advisory**. Register facts are taken
> verbatim from `inventory/out/clusters/sync-flip.csv` (18 IDs); no register is invented here.
>
> **Legend.** `[V]` = verified against a primary source (this tree, or an Apple/vendor fact already
> verified by a sibling scope and cited); `[I]` = inference / engineering judgement; `[U]` = could not
> verify / needs a device spike. Difficulty scale (identical to the set): **1** trivial rename · **2**
> mechanical · **3** real work, well-trodden · **4** hard / needs design · **5** unknown/impossible.
> Metal 4 object names are the ones fixed by `HLD-03` §2.1/§3.3 and `02` §3.3; this document
> introduces no new Metal API surface.

---

## 1. Scope

This is the per-register low-level design for the **`sync-flip`** cluster
(`inventory/out/clusters/sync-flip.csv`): the guest-visible synchronization, report and
display-queue surface of RSX. It owns 18 FIFO method registers in three groups:

- **NV406E semaphore/reference** (`sync-flip.csv:2-6`): `M_NV406E_SET_REFERENCE`,
  `M_NV406E_SET_CONTEXT_DMA_SEMAPHORE`, `M_NV406E_SEMAPHORE_OFFSET`,
  `M_NV406E_SEMAPHORE_ACQUIRE`, `M_NV406E_SEMAPHORE_RELEASE`.
- **NV4097 idle/notify/report** (`sync-flip.csv:7-15`): `M_NV4097_NOTIFY`,
  `M_NV4097_WAIT_FOR_IDLE`, `M_NV4097_SET_CONTEXT_DMA_SEMAPHORE`, `M_NV4097_PIPE_NOP`,
  `M_NV4097_CLEAR_REPORT_VALUE`, `M_NV4097_GET_REPORT`, `M_NV4097_SET_SEMAPHORE_OFFSET`,
  `M_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE`, `M_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE`.
- **GCM flip/display** (`sync-flip.csv:16-19`): `M_GCM_FLIP_HEAD`, `M_GCM_DRIVER_QUEUE`,
  `M_GCM_SET_USER_COMMAND`, `M_GCM_FLIP_COMMAND`.

Plus one **internal, non-register** construct that belongs to this cluster's semantics and is
analysed in §3.4: `FIFO::FIFO_DRAW_BARRIER` (`RSXFIFO.h:25`). It is a FIFO command, not a guest
method, so it has no disposition row.

**Framing corrections, recorded so no register is mapped that does not exist** (carried from
`HLD-03` §2.2):

1. **`NV4097_SET_REFERENCE` is not a register.** The reference write is `M_NV406E_SET_REFERENCE`
   (`gcm_enums.h:533`; handler `nv406e::set_reference`, `nv406e.cpp:14`). `[V]`
2. **There is no `NV4097_SET_REPORT_*` register.** The report machinery is exactly
   `M_NV4097_GET_REPORT` and `M_NV4097_CLEAR_REPORT_VALUE` (`gcm_enums.h` has no `SET_REPORT`;
   grep `[V]`). Apple's "report" is not an RSX register.
3. **`RSXSync` is not a symbol.** The topic phrase denotes `rsx::thread::sync()`
   (`RSXThread.cpp:2917`), the `sync_point_request` protocol (`RSXThread.h:324`), and
   `reports::ZCULL_control` (`RSXZCULL.h:98`). `[V]`

**Non-goals.** ZCULL *pixel* semantics (only its report-retirement synchronization surface is
modelled, per `HLD-03` §1.2); the barrier scheduler's internals (`HLD-03` §2.4 owns those; this doc
calls into `mtl::emit_barrier`); the draw-clause batcher (`10` §9 RN2); shader/binding/format/present
design owned by `03`/`04`/`05`.

---

## 2. Register model

### 2.1 Decode, binding and lifetime

All 18 IDs are 32-bit method arguments. Decode is from `inventory/out/rsx_decode_fields.csv`; **none
of the 18 carries a declared bitfield** — the cluster is action/selector state, not packed
descriptive state. Lifetime is the property that matters: these registers are **actions** (consumed
at the FIFO position where they appear) or **selector state** (persist until rewritten), never
dirty-bits.

| id | addr | decode | kind | lifetime |
|---|---|---|---|---|
| `M_NV406E_SET_REFERENCE` | 0x0050 | u32 (opaque) | action | consumed once; writes `RsxDmaControl.ref` |
| `M_NV406E_SET_CONTEXT_DMA_SEMAPHORE` | 0x0060 | u32 (opaque) | selector | persists; semaphore DMA context |
| `M_NV406E_SEMAPHORE_OFFSET` | 0x0064 | u32 (opaque) | selector | persists; semaphore word offset |
| `M_NV406E_SEMAPHORE_ACQUIRE` | 0x0068 | u32 (opaque) | action | blocking (may stall FIFO) |
| `M_NV406E_SEMAPHORE_RELEASE` | 0x006c | u32 (opaque) | action | write-once guest label |
| `M_NV4097_NOTIFY` | 0x0104 | — | action | write-once `RsxNotify` block |
| `M_NV4097_WAIT_FOR_IDLE` | 0x0110 | — | action | flush/serialize point |
| `M_NV4097_SET_CONTEXT_DMA_SEMAPHORE` | 0x01a4 | u32 (opaque) | selector | persists; 4097 semaphore DMA context |
| `M_NV4097_PIPE_NOP` | 0x1718 | — | no-op | none |
| `M_NV4097_CLEAR_REPORT_VALUE` | 0x17c8 | — | action | resets report stat block |
| `M_NV4097_GET_REPORT` | 0x1800 | — | action | reads/retires report to guest |
| `M_NV4097_SET_SEMAPHORE_OFFSET` | 0x1d6c | u32 (opaque) | selector | persists; 4097 semaphore offset |
| `M_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` | 0x1d70 | — | action | full-pipeline label write |
| `M_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE` | 0x1d74 | — | action | shader-read-stage label write |
| `M_GCM_FLIP_HEAD` | 0xe920 | — | action | 2-word range; display flip |
| `M_GCM_DRIVER_QUEUE` | 0xe940 | — | action | 8-word range; display queue |
| `M_GCM_SET_USER_COMMAND` | 0xeb00 | — | action | 2-word range; user interrupt |
| `M_GCM_FLIP_COMMAND` | 0xfeac | — | action | HLE flip path |

The `decode` column reads the `has_decoder`/`n_fields` fields of `sync-flip.csv`; `u32 (opaque)`
means `has_decoder=1, n_fields=0` and `—` means `has_decoder=0`. `[V]`

### 2.2 The guest-visible state these registers touch

Four structs in guest memory are the sinks and sources; they are **not** backend state and are the
whole correctness obligation of this cluster (`sys_rsx.h`):

```
RsxDmaControl  { put, get, ref }            sys_rsx.h:80-88
RsxSemaphore   = be_t<u32>                  sys_rsx.h:90
RsxNotify      { be_t<u64> timestamp, zero} sys_rsx.h:92-96
RsxReport      { be_t<u64> timestamp, val, pad }  sys_rsx.h:98-103
RsxReports     { semaphore[1024], notify[64], report[2048] }  sys_rsx.h:105-110
```

Address composition is shared code and survives unchanged: `get_address(offset, location)`
(`RSXThread.cpp:139`) resolves the semaphore/report/device DMA contexts
(`RSXThread.cpp:190-260`; `CELL_GCM_CONTEXT_DMA_SEMAPHORE_RW/R`, `..._REPORT_LOCATION_LOCAL/MAIN`,
`..._DEVICE_RW/R`). `M_NV406E_SET_REFERENCE` writes `RsxDmaControl::ref` directly
(`nv406e.cpp:19-21`); `thread::sync()` (`RSXThread.cpp:2917`) is a **CPU-side** flush — `mm_flush()`
(`:2921`), ZCULL retire (`:2923-2926`), fragment-constant invalidation (`:2929`) — and does **not**
wait for the GPU. That fact is load-bearing for §3.3 and §6.1. `[V]`

---

## 3. Metal 4 mapping

### 3.1 The full table (every id, exactly once)

| id | addr | decode | binding | disposition | metal4 target |
|---|---|---|---|---|---|
| `M_NV406E_SET_REFERENCE` | 0x0050 | u32 (opaque) | `nv406e::set_reference` | emulate | shared `MTL::Buffer` over `RsxDmaControl`; host `contents()` ref write |
| `M_NV406E_SET_CONTEXT_DMA_SEMAPHORE` | 0x0060 | u32 (opaque) | — | reshape | `mtl::label_sink` address decode |
| `M_NV406E_SEMAPHORE_OFFSET` | 0x0064 | u32 (opaque) | — | reshape | `mtl::label_sink` slot offset |
| `M_NV406E_SEMAPHORE_ACQUIRE` | 0x0068 | u32 (opaque) | `nv406e::semaphore_acquire` | reshape | `MTLSharedEvent.waitUntilSignaledValue` / `MTL4CommandQueue.waitForEvent` |
| `M_NV406E_SEMAPHORE_RELEASE` | 0x006c | u32 (opaque) | `nv406e::semaphore_release` | reshape | `mtl::label_sink` write + `signalEvent` |
| `M_NV4097_NOTIFY` | 0x0104 | — | `nv4097::set_notify` | emulate | host `RsxNotify` write into shared `MTL::Buffer` |
| `M_NV4097_WAIT_FOR_IDLE` | 0x0110 | — | `nv4097::sync` | reshape | scheduler flush + `barrier(afterStages:beforeQueueStages:)` |
| `M_NV4097_SET_CONTEXT_DMA_SEMAPHORE` | 0x01a4 | u32 (opaque) | — | reshape | `mtl::label_sink` address decode |
| `M_NV4097_PIPE_NOP` | 0x1718 | — | — | na | none |
| `M_NV4097_CLEAR_REPORT_VALUE` | 0x17c8 | — | `nv4097::clear_report_value` | emulate | visibility-result reset / shared report `MTL::Buffer` clear |
| `M_NV4097_GET_REPORT` | 0x1800 | — | `nv4097::get_report` | emulate | visibility-result readback into shared `MTL::Buffer` |
| `M_NV4097_SET_SEMAPHORE_OFFSET` | 0x1d6c | u32 (opaque) | — | reshape | `mtl::label_sink` slot offset |
| `M_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` | 0x1d70 | — | `nv4097::back_end_write_semaphore_release` | reshape | `barrier(...visibilityOptions:.device)` + label write |
| `M_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE` | 0x1d74 | — | `nv4097::texture_read_semaphore_release` | reshape | read-stage barrier + label write |
| `M_GCM_FLIP_HEAD` | 0xe920 | — | `gcm::driver_flip` (x2) | reshape | `waitForDrawable`/`signalDrawable` + `drawable.present()` |
| `M_GCM_DRIVER_QUEUE` | 0xe940 | — | `gcm::queue_flip` (x8) | reshape | frame-ring `MTL4CommandAllocator` + `MTLSharedEvent` present value |
| `M_GCM_SET_USER_COMMAND` | 0xeb00 | — | `user_command` (x2) | reshape | commit + `MTL4CommitFeedback` drain, then host handler |
| `M_GCM_FLIP_COMMAND` | 0xfeac | — | `flip_command` | reshape | present hand-off (HLE) + flip-semaphore reset in shared `MTL::Buffer` |

### 3.2 The semaphore / reference protocol (guest values written to guest memory)

**RSX behaviour.** `M_NV406E_SET_REFERENCE` calls `thread::sync()` then stores the argument into
`RsxDmaControl::ref` (`nv406e.cpp:14-22`). `RsxDmaControl.put/get` are the FIFO pointers; `ref` is
the guest's reference channel. `M_NV406E_SEMAPHORE_ACQUIRE` resolves
`get_address(semaphore_offset_406e(), semaphore_context_dma_406e())` (`nv406e.cpp:27`), raises
`sync_point_request` (`:26`), fences seq-cst (`:33`), and **spins on the `RsxSemaphore` word until it
equals the argument** (`:60`), with a driver-recovery timeout that force-exits the wait (`:68-88`) and
a `spin_on_cacheline_once` backoff (`:100`). `M_NV406E_SEMAPHORE_RELEASE` raises
`sync_point_request` unless it is the flip semaphore (`:121-125`), rewrites offset `0x10` on the flip
DMA context, and calls `util::write_gcm_label<false, true>` (`:143`). The label path itself
(`nv47_sync.hpp:14-63`) does: seq-cst fence (`:17`), the host-GPU-label fast path when
`supports_host_gpu_labels` (`:23`), a same-value no-op elision (`:25-30`), the `FlushDMA`/`FlushPipe`
MM and pipe flushes (`:32-53`), and finally the raw `vm::write<atomic_t<RsxSemaphore>>` (`:62`). `[V]`

**Metal 4 shape.** These are **guest-memory writes and a guest-memory wait**, not GPU commands. The
sink is a `StorageModeShared` `MTL::Buffer` mapped over the `RsxReports` / `RsxDmaControl` block
(`HLD-03` §3.3 `mtl::label_sink`), so a host `contents()` store reaches guest memory with no
`didModifyRange:` dance on Apple silicon (unified memory; `README.md` §4). The **ordering** is the
only Metal-4 part:

- **Release (producer).** `write_gcm_label<true,true>` requires the label to become visible *after*
  all prior GPU work. That is exactly a producer barrier —
  `MTL4CommandEncoder.barrier(afterStages:beforeQueueStages:visibilityOptions:.device)` — followed by
  the label store. The store may be emitted by a compute encoder or, when the backend is about to
  wait anyway, written by the host after the covering `MTL4CommitFeedback` fires. `mtl::label_sink`
  write ownership sits with the RSX thread under `thread::sync()` (`HLD-03` §3.4). `[I]`
- **Acquire (consumer).** The guest's expected value can be produced by the GPU (a label release) or
  by the CPU (a PPU store). GPU-produced values map to
  `MTL4CommandQueue.waitForEvent(_:value:)` on the monotonic `MTLSharedEvent`; CPU-produced values
  are already in shared memory, so acquire degenerates to the existing spin. The **TDR escape**
  (`nv406e.cpp:68-88`) must be preserved verbatim for either form — `MTLSharedEvent` has no
  "wait forever"; `waitUntilSignaledValue(_:timeoutMS:)` is the host wait with a bounded timeout
  (`HLD-03` §2.3 Edge C, invariant 8). `[I]`
- **Reference (`SET_REFERENCE`).** A pure host write; it does not wait on the GPU. It is `emulate`
  because there is no Metal object — the value is a guest protocol word. If the design later decides
  `sync()` must become GPU-observable (§6.1), `SET_REFERENCE` gains a preceding visibility point. `[V]`/`[I]`

The **flip semaphore** is special: offset `0x10` on `CELL_GCM_CONTEXT_DMA_SEMAPHORE_R`
(`nv406e.cpp:121-122`; `RSXThread.cpp:2738`) is the `label_addr + 0x10` word that
`sys_rsx.cpp:1044` and `flip_command` (`rsx_methods.cpp:65`) reset to zero each flip. It bypasses the
`sync_point_request` raise and maps to the presentation hand-off (§3.5), not to a draw-stream label.

### 3.3 Report mechanism (guest-readable state — a correctness obligation)

**RSX behaviour.** `M_NV4097_GET_REPORT` (`nv4097.cpp:644-675`) resolves the report DMA context
(`util::get_report_data_impl`, `common.cpp:59-63`), and for `ZPASS_PIXEL_CNT`/`ZCULL_STATS*` calls
`thread::get_zcull_stats` (`RSXThread.cpp:2846`) → `ZCULL_control::read_report`
(`RSXZCULL.h:166`), otherwise writes a timestamp-only `CellGcmReportData`
(`nv4097.cpp:668-672`). `M_NV4097_CLEAR_REPORT_VALUE` (`:677-690`) calls `clear_zcull_stats` →
`ZCULL_control::clear` (`RSXZCULL.h:169`). The ZCULL unit *queues* report writes
(`queued_report_write`, `RSXZCULL.h:45-55`), retires them after `max_zcull_delay_us = 300` µs or when
forced by a read (`:89-96`, `:152`), and `read_barrier` (`:175-176`) syncs a range on access. The
report block is `RsxReports::report[2048]` (`sys_rsx.h:105-110`). `[V]`

**Metal 4 shape.** There is **no Metal report primitive**; a report is guest-readable memory, so it
is `emulate`. The backend has a real GPU mechanism only for the *pixels*: occlusion queries map to
`renderPassDescriptor.visibilityResultBuffer` + `setVisibilityResultMode(_:offset:)` (`README.md`
§3.3 #64; `02` #33). The report value the guest reads is then produced by reading the visibility
buffer (a `StorageModeShared` `MTL::Buffer`, `HLD-03` §3.3 `mtl::visibility_buffer`) after the commit
that produced it — i.e. after `MTL4CommitFeedback` or the frame `MTLSharedEvent` value fires — and
writing it into the `RsxReport` slot. The timestamp report needs no GPU at all: a host write of
`thread::timestamp()` (`RSXThread.cpp` report path). `[I]`

Two consequences the implementer must hold:

1. **A report read is an ordering point.** `read_report` retires queued writes and the guest may
   immediately read the sink; a stale `RsxReport` is a correctness bug, not a perf one. The design
   must gate the read on the covering commit's completion (`HLD-03` §2.3 Edge C), which is the same
   GPU→CPU visibility point as the label sink.
2. **`CLEAR_REPORT_VALUE` resets guest state, and `visibilityResultBuffer` reset is pass-scoped.**
   Vulkan's `vkCmdResetQueryPool` has no exact Metal analogue (`README.md` §6 R12; `02` §7 Q4), so
   the reset point and the pass-spanning accumulation are **unverified** — flagged `[U]` in §6.2.
   The RSX-first answer cannot reuse the ZCULL 300 µs / 1792-deep retirement policy (`RSXZCULL.h:89-96`)
   unchanged until the reset semantics are pinned (`HLD-03` §6 Q9). `[U]`

### 3.4 Idle / notify / wait semantics

**RSX behaviour.** `M_NV4097_WAIT_FOR_IDLE` is bound to `nv4097::sync` (`rsx_methods.cpp:1706`) →
`thread::sync()` (`nv4097.cpp:844-847`). `M_NV4097_NOTIFY` (`nv4097.cpp:762-785`) calls
`thread::sync()`, resolves the notify block from `context_dma_notify`, and stores an `RsxNotify`
timestamp (`:780-784`). `M_NV4097_PIPE_NOP` is unhandled (`rsx_methods.cpp:1425`). None of these
waits for the GPU: `thread::sync()` (`RSXThread.cpp:2917-2937`) is a CPU flush. `[V]`

**Metal 4 shape.** The Metal 4 backend has **no queue-idle wait** (`MTL4CommandQueue` exposes no
`waitUntilIdle`), so:
- `M_NV4097_WAIT_FOR_IDLE` is a **flush condition** in the scheduler (`HLD-03` §2.4): close the open
  encoder(s), emit a producer barrier for the stages that wrote, and (only if the guest can observe
  the result, which it cannot here) obtain a completion point. It is `reshape`, not `direct`. `[I]`
- `M_NV4097_NOTIFY` is `emulate`: a host `RsxNotify` store into a shared `MTL::Buffer`, ordered after
  the preceding GPU work only if the guest later reads it against GPU output — see §6.1. `[V]`/`[I]`
- `M_NV4097_PIPE_NOP` is `na`: no state, no action. `[V]`

**Die-boundary caution.** `M_GCM_SET_USER_COMMAND` (`rsx_methods.cpp:68-91`) calls `thread::sync()`
then dispatches a host handler (0xFEF, `sys_rsx.cpp:1086-1099`). Its comment claims "a full drain of
the backend and frontend", but `thread::sync()` does not drain the GPU. Under Metal 4 the correct
order is commit → `MTL4CommitFeedback` (or `MTLSharedEvent` value) → handler, so the callback cannot
observe pre-drain contents. This is the same visibility argument as §3.2's release. `[I]`

### 3.5 FIFO barriers (`FIFO::FIFO_DRAW_BARRIER`) — not a register

`FIFO_DRAW_BARRIER` (`RSXFIFO.h:25`) is emitted by the flattener at the end of a primitive block
(`RSXFIFO.cpp:612`) and turned by `run_FIFO` into `SET_BEGIN_END(0)` + `SET_BEGIN_END(primitive)`
(`:890-896`). **It is a draw-clause boundary, not a memory barrier, and must not be mapped to a
`Metal` barrier.** Under RSX-first it stays a host-side batch split (`10` §4.5; `09` §4), i.e. it
ends one `draw*(...)` run and starts another inside the same encoder. Mis-mapping it to a producer
barrier or a pass boundary would manufacture tile flushes (`HLD-03` §5 "over-splitting"). The
flattener also ignores pure invalidation ranges (`RSXFIFO.h:83-105`) — those are dirty-bit signals,
never ordering. `[V]` Separately, the FIFO desync alarm (`FIFO_ERROR`, `RSXFIFO.cpp:708-711`) and the
jump-to-self synchronization point (`:729`) are unchanged and remain the loop's FIFO-healthy signal
(`11` §3.4). `[V]`

### 3.6 Flip / display hand-off

**RSX behaviour.** `M_GCM_FLIP_HEAD` → `gcm::driver_flip` → `sys_rsx_context_attribute(..., 0x102, ...)`
(`rsx_methods.cpp:96-102`; `sys_rsx.cpp:706-759`) → `request_emu_flip` (`RSXThread.cpp:3605`) →
`handle_emu_flip` (`:3635`) → `flip()` (`:2776`). `M_GCM_DRIVER_QUEUE` → `gcm::queue_flip` → `0x103`
(`rsx_methods.cpp:104-116`; `sys_rsx.cpp:760-779`): records `lastQueuedBufferId`, calls
`on_frame_end`, and signals `SYS_RSX_EVENT_QUEUE_BASE`. `M_GCM_FLIP_COMMAND` (`rsx_methods.cpp:40-66`)
is the **HLE-only** path (`ensure(isHLE)`, `:42`): bumps the flip count, dispatches the guest
`queue_handler`, `reset()`s, `on_frame_end`, `request_emu_flip`, and zeroes the flip semaphore
`label_addr + 0x10` (`:65`). `[V]`

**Metal 4 shape.** All three are `reshape`: they are *host* hand-offs into the frame tail, not GPU
commands. The frame tail is `waitForDrawable(drawable)` → encode → `signalDrawable(drawable)` →
`drawable.present()` (`HLD-03` §2.3 Edge D; `02` #10/#11), with `maximumDrawableCount ≤ 3`
(`README.md` §6 R21) and a frame ring of `#frames-in-flight` `MTL4CommandAllocator`s + one
`MTLSharedEvent` value (`HLD-03` §2.3 Edge D). The `VkSwapchainKHR` object does not exist to
recreate (`README.md` §4 shift 3). `m_queued_flip`/`flip_status` (`RSXThread.h:113`, `:241`) are
unchanged shared state; the backend only supplies the drawable ring and the present call. The
`GCM_FLIP_COMMAND` flip-semaphore zeroing is a shared-memory write into `mtl::label_sink` and must be
ordered after the presented frame's GPU work if the guest polls it. `[I]`

---

## 4. Impossible / not-recommended

| Item | Status | Chosen fallback |
|---|---|---|
| Stage-gated queue wait (`pWaitDstStageMask` analogue) | **impossible** (`README.md` §6 R7; `02` §4 item 2) | `MTL4CommandQueue.waitForEvent` gates the whole queue; re-anchor `texture_read_semaphore_release` on the surface hazard (`HLD-03` §2.5) |
| Queue-idle wait (`WAIT_FOR_IDLE` → "GPU is idle") | **not-recommended** (no `waitUntilIdle`) | scheduler flush + producer barrier; only add a commit-feedback wait when a guest observable depends on it |
| GPU writes directly into guest memory (labels) without a host copy | **not-recommended** | `StorageModeShared` `MTL::Buffer` + `mtl::label_sink` host write after a visibility point; do not rely on `supports_passthrough_dma` (`10` §2.2 forces it `false`) |
| Binary semaphore / `VkEvent`-as-object for acquire/release | **impossible** (`README.md` §6 R8) | one monotonic `MTLSharedEvent` value + commit feedback (`HLD-03` §2.5) |
| `.resourceAlias` as the ordinary label visibility option | **not-recommended** (`02` §4 NR2) | `.device` for ordinary ordering; `.resourceAlias` only for simultaneously aliased virtual addresses |
| Reusing `vkCmdBlitImage`-style copy for report readback | **not-recommended** (`README.md` §4 shift 4) | compute-encoder copy from the visibility buffer into the shared report buffer |

**The chosen fallback is stated, not hidden:** the report and label paths are CPU-emulated
(shared-buffer writes) with GPU ordering supplied by explicit barriers and one monotonic event; the
only "real Metal primitive" in the cluster is the occlusion *pixel* counter
(`visibilityResultBuffer` + `setVisibilityResultMode`), and the semaphore *wait*, which maps to
`MTLSharedEvent`/`waitForEvent`.

---

## 5. Invariants & failure modes

### 5.1 Invariants

1. **A guest-visible semaphore/reference/report is written only after the GPU work it observes is
   visible** — via a producer barrier and, for the CPU path, after `MTL4CommitFeedback` / the frame
   `MTLSharedEvent` value (`HLD-03` invariant 7). `[I]`
2. **No unbounded wait.** `semaphore_acquire`'s TDR escape (`nv406e.cpp:68-88`) is preserved for any
   Metal completion wait (`HLD-03` invariant 8). `[V]`
3. **`FIFO_DRAW_BARRIER` never becomes a Metal barrier.** It is a batch split, not a hazard. `[V]`
4. **`WAIT_FOR_IDLE`/`NOTIFY`/`USER_COMMAND` are flush conditions**, not GPU-idle guarantees;
   `thread::sync()` semantics are not upgraded implicitly (`HLD-03` §2.4 flush conditions 1–4). `[V]`
5. **Every label/report store goes through `mtl::label_sink`** — one recordable chokepoint
   (`11` §9.1), so H2/H3 can diff it. `[I]`
6. **The flip semaphore is not a draw-stream label.** Offset `0x10` on the semaphore-R context
   bypasses `sync_point_request` and belongs to the present hand-off. `[V]`

### 5.2 Failure modes

| Failure | Mechanism | Detection |
|---|---|---|
| **Stale report read by guest** | `GET_REPORT` retires to a shared buffer before the producing commit completes | H3 per-command localisation; replay `is_fifo_idle()` stop-and-apply (`11` §3.3) |
| **Label written before GPU visible** | host store beats the producer barrier / feedback | H5 guest-memory differential (`11` §4) |
| **Deadlock / hang on acquire** | `waitForEvent` never satisfied, no TDR | desync counter; TDR escape log (`nv406e.cpp:85`) |
| **Over-serialization** | `WAIT_FOR_IDLE`/`NOTIFY` treated as full GPU drains → encoder churn | H7 perf band; `frame_statistics_t` (`RSXDisplay.h:50-68`) |
| **Report/visibility reset wrong** | `CLEAR_REPORT_VALUE` reset point ≠ pass boundary | `[U]` — needs a device spike (`HLD-03` §6 Q4/Q9) |
| **Commit-array order assumed** | `cb_ordinal` used to elide a barrier that is not guaranteed | `[U]` — needs a device spike (`HLD-03` §6 Q2) |
| **Flip semaphore desync** | `label_addr + 0x10` reset unordered vs presented frame | H3/H6 on a flip-heavy capture (`11` §9 corpus item (a)) |
| **`write_gcm_label` no-op elision misfires** | same-value skip (`nv47_sync.hpp:25-30`) when real HW delays | H5 guest readback |

---

## 6. Open questions

### 6.1 Highest-value (decide before implementation)

1. **`[U]` Commit-array ordering guarantee.** Is
   `MTL4CommandQueue.commit(commandBuffers, count, options)` a *documented* serialization of the
   array, or merely conventional? This decides whether `M_NV4097_WAIT_FOR_IDLE` and
   `M_GCM_SET_USER_COMMAND` need an explicit barrier before their visibility point, and whether
   `mtl::writer_record.cb_ordinal` can elide intra-commit barriers (`HLD-03` §2.1 item 6, §6 Q2).
   **The only `[U]` item this cluster cannot proceed without.** *(row `M_NV4097_WAIT_FOR_IDLE`)*
2. **`[U]` Visibility-buffer reset semantics.** Confirm the reset point of
   `MTL4RenderPassDescriptor.visibilityResultBuffer` (pass start vs per-query) and whether
   accumulation can span passes; this fixes `CLEAR_REPORT_VALUE` and the `GET_REPORT` retirement
   policy (`README.md` §6 R12; `02` §7 Q4; `HLD-03` §6 Q4/Q9). *(row `M_NV4097_CLEAR_REPORT_VALUE`)*
3. **`[U]` Does `thread::sync()` become GPU-observable?** Today it is a CPU flush
   (`RSXThread.cpp:2917`). If labels/reports must be correct against GPU output, `sync()` may need to
   return a token the guest-visible write waits on — a change to shared code that the RSX-first plan
   otherwise avoids (`HLD-03` §6 Q10, rated highest-risk there). This cluster cannot fully decide it,
   but it owns the labels/reports that depend on it.
4. **`[U]` Label path: compute-written or CPU-written?** `supports_host_gpu_labels` is rated
   `reshape` (`10` §2.2); decide whether `mtl::label_sink` writes are issued from an
   `MTL4ComputeCommandEncoder` (batched) or by the host after feedback (`HLD-03` §6 Q11). This
   chooses `M_NV406E_SEMAPHORE_RELEASE`'s concrete target.
5. **`[U]` `updateFence`/`waitForFence` vs producer/consumer barriers for the split release.** For
   the same-command-buffer, cross-encoder case (`BACK_END_WRITE_SEMAPHORE_RELEASE`),
   `MTL4RenderCommandEncoder.updateFence(_:afterEncoderStages:)` + `waitForFence` may be cheaper
   and more precise than a full barrier pair (`HLD-03` §6 Q3).

### 6.2 Lower-order

6. **`[I]` `MTL4VisibilityOptions` selection table.** Is `.device` sufficient for *every* ordinary
   RSX label/report dependency, including RTT→texture? `HLD-03` Q5; `02` §4 NR2 says yes for normal
   placement-heap aliasing.
7. **`[I]` ZCULL retirement under Metal.** Can the 300 µs / 1792-deep retirement (`RSXZCULL.h:89-96`)
   be preserved when the report sink is a shared `MTL::Buffer` read after feedback, or does the
   pass-scoped visibility reset force a different policy? (`HLD-03` §6 Q9.)
8. **`[U]` `M_GCM_SET_USER_COMMAND` drain depth.** Does the guest require a *real* GPU drain before
   the 0xFEF handler (`sys_rsx.cpp:1086-1099`), or is the current CPU-only `sync()` sufficient?
   LLE path (`rsx_methods.cpp:75`) vs HLE path (`:79-90`).

---

## 7. Provenance

### 7.1 Primary — verified against this tree `[V]`

- `rpcs3/Emu/RSX/NV47/HW/nv406e.cpp` — `set_reference` `:14` (`sync()` `:16`, `ref.store` `:19-21`);
  `semaphore_acquire` `:24` (`sync_point_request` `:26`, `get_address` `:27`, seq-cst `:33`,
  `spin` `:60`, TDR `:68-88`, `spin_on_cacheline_once` `:100`); `semaphore_release` `:107`
  (`sync_point_request` `:124`, `get_address` `:127`, flip word `:137-141`, `write_gcm_label` `:143`).
- `rpcs3/Emu/RSX/NV47/HW/nv47_sync.hpp` — `write_gcm_label` `:14-63` (seq-cst `:17`, flip-sema `:19`,
  `release_GCM_label` `:23`, no-op elision `:25-30`, MM/pipe flush `:32-53`, raw write `:62`).
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — `get_report` `:644` (`get_report_data_impl` `:649`, ZCULL
  `:663`, timestamp `:668-672`); `clear_report_value` `:677`; `set_notify` `:762` (`sync` `:778`,
  store `:780-784`); `texture_read_semaphore_release` `:787` (strict `:810-817`);
  `back_end_write_semaphore_release` `:820` (`write_gcm_label<true,true>` `:841`); `sync` `:844`.
- `rpcs3/Emu/RSX/NV47/HW/common.cpp` — `get_report_data_impl` `:59-63`.
- `rpcs3/Emu/RSX/RSXThread.cpp` — `get_address` `:139`; report/semaphore/device DMA decode
  `:190-260`; `sync()` `:2917-2937`; `is_fifo_idle` `:2944`; `flip` `:2776`; `check_zcull_status`
  `:2806`; `clear_zcull_stats` `:2841`; `get_zcull_stats` `:2846`; `request_emu_flip` `:3605`;
  `handle_emu_flip` `:3635`.
- `rpcs3/Emu/RSX/RSXThread.h` — `m_queued_flip` `:113`; `sync_point_request` `:324`; `flip_status`
  `:241`; `flip` `:372`; `write_barrier` `:396`; `release_GCM_label` `:398`; surface info `:145-147`.
- `rpcs3/Emu/RSX/RSXZCULL.h` — `queued_report_write` `:45-55`; constants `:89-96`; `ZCULL_control`
  `:98`; `read_report` `:166`; `clear` `:169`; `sync` `:172`; `read_barrier` `:175-176`; `update`
  `:179`; `has_pending` `:188`; backend virtuals `:212-216`.
- `rpcs3/Emu/RSX/RSXFIFO.h` — `FIFO_DRAW_BARRIER` `:25`; `interrupt_hint` `:45-49`; ignorable
  invalidation ranges `:83-105`.
- `rpcs3/Emu/RSX/RSXFIFO.cpp` — `FIFO_DRAW_BARRIER` emit `:612`; `EMIT_BARRIER` handling `:890-896`;
  `FIFO_ERROR` desync `:708-711`; jump-to-self sync `:729`.
- `rpcs3/Emu/RSX/rsx_methods.cpp` — `flip_command` `:40`; `user_command` `:68`; `driver_flip` `:96`;
  `queue_flip` `:104`; `fifo::draw_barrier` `:121`; `PIPE_NOP` unbound `:1425`;
  `GET_REPORT`/`NOTIFY`/`WAIT_FOR_IDLE` binds `:1645`,`:1660`,`:1706`.
- `rpcs3/Emu/RSX/gcm_enums.h` — `NV406E_SET_REFERENCE` `:533`; 4097 notify/idle `:542-543`;
  `NV4097_PIPE_NOP` `:666`; GCM `:825-829`.
- `rpcs3/Emu/Cell/lv2/sys_rsx.h` — `RsxDmaControl` `:80-88`; `RsxSemaphore` `:90`; `RsxNotify`
  `:92-96`; `RsxReport` `:98-103`; `RsxReports` `:105-110`.
- `rpcs3/Emu/Cell/lv2/sys_rsx.cpp` — 0x102 display flip `:706-759`; 0x103 display queue `:760-779`;
  0xFEC flip event `:1035-1048`; 0xFEF user command `:1086-1099`.
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp` — `release_GCM_label` `:1710` (global barrier `:1741`,
  `vkCmdUpdateBuffer` `:1743-1747`).

### 7.2 Sibling scope `[V]`

- `docs/research/metal4/design/HLD-03-synchronization-model.md` — §2.1 (ordering primitives,
  commit-array `[U]`, no stage-gated wait, TBDR constraint); §2.2 (RSX sync surface, register
  corrections); §2.3 (edges A–E); §2.4 (flush conditions); §2.5 (VK divergence); §3.3 (`mtl::`
  objects); §4 (invariants); §6 (open questions, esp. Q2/Q4/Q9/Q10/Q11); §7 (provenance).
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` — §4.1 (shared memory), §4.3–§4.4
  (pass/hazard model), §4.5 (draw/submission), §4.7 (present/frame); §2.2
  (`supports_host_gpu_labels`, `supports_passthrough_dma`); §9 (RN1–RN3).
- `docs/research/metal4/11-verification-loop.md` — §3 (RRC substrate, stop-and-apply), §4 (H0–H7),
  §5 (recording seam, null device), §9 (design-for-verifiability; flip-heavy corpus item).
- `docs/research/metal4/README.md` — §3.3 rows #33–#55; §4 shifts 1–4; §6 R7/R8/R9/R12/R21/R43/R47;
  RN1–RN3.
- `docs/research/metal4/02-submission-sync.md` — §3.3 (metal-cpp MTL4 barrier/event/feedback
  signatures); §4 (impossible/not-recommended); §7 (open questions Q1/Q2/Q4/Q6/Q9).
- `docs/research/metal4/05-presentation-and-impossible-register.md` — acquire/present (R21/R29).
- `docs/research/metal4/inventory/SCHEMA.md` §4/§6; `inventory/out/SUMMARY.md` §`sync-flip`;
  `inventory/out/clusters/sync-flip.csv` (18 IDs); `inventory/out/dispositions/sync-flip.csv`.

### 7.3 Apple / Metal 4 (as fixed by `HLD-03`/`02`; not re-fetched here) `[V]` via those docs

- `MTL4CommandEncoder.barrier(afterStages:beforeQueueStages:visibilityOptions:)` /
  `barrier(afterQueueStages:beforeStages:...)` / `barrier(afterEncoderStages:beforeEncoderStages:...)`;
  `MTL4::VisibilityOptions` (`None`/`Device`/`ResourceAlias`).
- `MTL4CommandQueue.signalEvent(_:value:)` / `waitForEvent(_:value:)`;
  `MTLSharedEvent.signaledValue` / `waitUntilSignaledValue(_:timeoutMS:)`.
- `MTL4CommitOptions.addFeedbackHandler` → `MTL4CommitFeedback` (`gpuStartTime`/`gpuEndTime`/`error`).
- `MTL4RenderPassDescriptor.visibilityResultBuffer` + `setVisibilityResultMode(_:offset:)`.
- `MTL4CommandQueue.waitForDrawable` / `signalDrawable` + `CAMetalDrawable.present`.
- `MTLResidencySet` (reachability, not ordering).

<!-- doc-end -->
