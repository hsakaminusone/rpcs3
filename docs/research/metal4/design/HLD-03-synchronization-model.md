# HLD-03 — Synchronization & hazard model

Owner/scope: `metal4` design (RSX-first, native Metal 4) | Status: draft | Date: 2026-10-16

> **Scope of this document.** The synchronization and hazard model for a native Apple Metal 4
> backend for RSX: the Metal 4 ordering mechanism, the mapping of the RSX synchronization surface,
> the **hazard-derived pass scheduler**, the producer/consumer edges (RSX thread ↔ GPU, RSX thread ↔
> PPU, frame/flip), and why this diverges from the Vulkan backend. Read-only research: **no file
> under `rpcs3/**` was modified**; only this document was created. All `file:line` citations were read
> on 2026-10-16 on branch `metal4`; the tree is point-in-time and peers are editing concurrently, so
> **symbol names are the stable reference; line numbers are advisory.**
>
> **Verification legend.** `[V]` = verified this session against the RPCS3 tree or a primary source
> already fetched by a sibling scope (cited); `[I]` = inference from `[V]` facts; `[U]` = could not
> verify / needs a device spike. Difficulty scale (identical to the set): **1** trivial rename ·
> **2** mechanical · **3** real work, well-trodden · **4** hard / needs design · **5** unknown.

---

## 1. Purpose & non-goals

### 1.1 Purpose

Give the implementation one place to answer four questions, so the pass scheduler, the barrier
emitter and the report/label machinery cannot drift apart:

1. **What orders work on Metal 4?** The exact mechanism, and which `Vk*` synchronization vocabulary
   has no analogue (R7, R8, R9 — `README.md` §6).
2. **What must be ordered, in RSX terms?** The guest synchronization surface — NV406E semaphores,
   `NV4097_WAIT_FOR_IDLE`, `NV4097_NOTIFY`, the report block, FIFO draw barriers, ZCULL — mapped to
   Metal 4 objects.
3. **How are pass boundaries derived?** The hazard-derived pass scheduler (RN1; `10` §4.3/§4.4):
   detection, grouping, barrier insertion, and the conditions that force a flush/encoder boundary.
4. **Where are the producer/consumer edges?** RSX thread ↔ GPU, RSX thread ↔ PPU (report writes),
   frame/flip boundary, and the residency seam.

### 1.2 Non-goals

- **No source, CMake, test or `.ci` change; no build; no measurement.** (Matches `10` §1.4, `11`.)
- **Not the VK→Metal construct mapping.** Scope `02` owns `vkCmdPipelineBarrier` and friends; this
  document consumes `02` §2 and only re-derives the parts the RSX frame changes.
- **Not the shader, binding, format or present design.** `03`/`04`/`05`/`09` own those; this document
  names the barrier/event objects they must call into.
- **Not a ZCULL pixel-accuracy model.** ZCULL's *pixel* semantics are out of scope; only its
  *synchronization* surface (report retirement, read barriers, the `sync()` flush) is modelled.
- **Does not decide the strategic gate** (`10` §10.1) or the binding-model ceiling (`10` §10.3).
- **Does not specify the indirect batcher** (RN2) except where it changes a hazard decision.

---

## 2. Model — RSX → Metal 4

### 2.1 What Metal 4 no longer does: the hazard mechanism

**The driver tracks nothing.** Metal 4 removed automatic hazard tracking for `MTL4*` pipelines and
command encoders; explicit barrier methods replace it (`02` §3.5, MoltenVK #2560 [V]). The value of an
`MTLResource`'s `MTLHazardTrackingMode` **has no effect on work submitted to an `MTL4CommandQueue`**
(`README.md` §4 shift 1 [V]). The consequence is absolute for this design: **correctness rests on the
barriers and events this backend emits, and on nothing else.**

The four ordering primitives the backend has, all named concretely:

| Primitive | Metal 4 object / method | Orders | Cite |
|---|---|---|---|
| Producer barrier | `MTL4CommandEncoder.barrier(afterStages:beforeQueueStages:visibilityOptions:)` | this encoder's stages → later queue work | `02` §3.3 [V] |
| Consumer barrier | `MTL4CommandEncoder.barrier(afterQueueStages:beforeStages:visibilityOptions:)` | earlier queue work → this encoder's stages | `02` §3.3 [V] |
| Intra-pass barrier | `MTL4CommandEncoder.barrier(afterEncoderStages:beforeEncoderStages:visibilityOptions:)` | stages within one encoder | `02` §3.3 [V] |
| Encoder-scoped fence | `updateFence(_:afterEncoderStages:)` / `waitForFence(_:beforeEncoderStages:)` | stage → stage across encoders in one command buffer | `02` §3.3 [V] |

Every barrier takes an `MTL4::VisibilityOptions` value — `None` / `Device` / `ResourceAlias`
(`02` §3.3 [V]). **Use `.device` for ordinary ordering.** `.resourceAlias` is only for simultaneously
aliased virtual addresses and can flush to system memory; it is **not** the ordinary
placement-heap-aliasing option (`02` §4 NR2 [V]).

Cross-queue and CPU ordering:

| Need | Metal 4 | Cite |
|---|---|---|
| Queue A signals queue B | `MTL4CommandQueue.signalEvent(event, value:)` + `MTL4CommandQueue.waitForEvent(event, value:)` on a monotonic `MTLSharedEvent` | `02` #9/#13 [V] |
| CPU waits for GPU completion | `MTL4CommitOptions.addFeedbackHandler(...)` → `MTL4CommitFeedback` (`gpuStartTime`/`gpuEndTime`/`error`), or poll `MTLSharedEvent.signaledValue` | `02` #12 [V] |
| Drawable acquire / present | `MTL4CommandQueue.waitForDrawable(drawable)` / `signalDrawable(drawable)` + `drawable.present()` | `02` #10/#11 [V] |

**Facts that constrain the model:**

1. **There is no stage-gated queue wait.** `VkSubmitInfo.pWaitDstStageMask` (used at
   `VKGSRender.cpp:2597` to wait at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`) has no Metal 4 form —
   `waitForEvent` gates the *queue*, not a stage (**R7**; `02` §4 item 2 [V]). The RSX-first design
   does not reproduce this wait; it re-anchors the dependency on the surface hazard (§2.4).
2. **Binary semaphores and `VkEvent`-as-object do not exist.** Only monotonic `MTLEvent`/
   `MTLSharedEvent` values (**R8**; `02` §4 item 3 [V]). The `vk::event` backends
   (`events_v1`/`events_v2`/`gpu_label`, `vkutils/sync.h:38-56`) are deletable; a GPU-domain
   `VkEvent` lowers to producer/consumer barriers, a host-domain event to a `MTLSharedEvent`
   (`02` #14 [V]).
3. **Residency is not ordering.** "Residency sets don't track hazards, so you need to account for
   hazards with fences and events" (`README.md` §4 shift 2 [V]). A resource being in an
   `MTLResidencySet` makes it *reachable*, never *ordered*. `MTL4CommandBuffer.useResidencySet(_:)`
   and the queue's residency attachment are a separate, mandatory subsystem (`01` §1.4) that this
   model assumes but never uses as a barrier.
4. **The access-mask half of every RPCS3 barrier is dropped.** Only the stage half survives
   (`01` §1.5 [V]). Where VK wrote `srcAccessMask`/`dstAccessMask`, Metal 4 chooses *stages* plus a
   visibility option; there is no "shader resource" umbrella stage, and the source stage must be
   mapped literally (`02` §5 rank 1 [V]).
5. **The TBDR fragment constraint.** On a render encoder, `afterEncoderStages` **must not** contain
   `.fragment` or `.tile`; a fragment/tile producer therefore cannot be waited on inside the same
   pass. A read-back of a colour/depth attachment written earlier in the same pass forces either
   programmable blending (framebuffer fetch) or a **pass split** (`README.md` §4 shift 1; `02` §5
   rank 1; `10` §4.4 [V]). This single rule is why the scheduler (§2.4) exists.
6. **Ordering *within* one commit.** Command buffers passed to one
   `MTL4CommandQueue.commit(commandBuffers, count, options)` execute in the order of the array
   (`02` §3.4 [I]/[U]). This is the intra-commit backbone; explicit barriers are still required for
   resource visibility across encoders. **Marked `[U]`** — confirm on the target SDK whether the
   commit-array order is a documented serialization guarantee or merely conventional.

### 2.2 The RSX synchronization surface to map

The inventory spine (`inventory/out/clusters/sync-flip.csv`, `depth-rop.csv`, `surface.csv`) plus the
NV47 handlers. **Two corrections to the topic framing, recorded so nobody maps a register that does
not exist:**

- **`NV4097_SET_REFERENCE` is not a register.** The reference write is `M_NV406E_SET_REFERENCE`
  (`sync-flip.csv:2`; handler `nv406e::set_reference`). The 4097 semaphore machinery is
  `M_NV4097_SET_SEMAPHORE_OFFSET` (`sync-flip.csv:13`) plus the two release registers below. `[V]`
- **`RSXSync` is not a symbol.** No such identifier exists in `rpcs3/Emu/RSX/**` (grep `[V]`). The
  topic's phrase denotes the runtime synchronization behaviour: `rsx::thread::sync()`
  (`RSXThread.cpp:2917`), the `sync_point_request` protocol (`RSXThread.h:324`), and the
  `reports::ZCULL_control` machinery (`RSXZCULL.h:98`). This document maps those.

**NV406E semaphore / reference machinery** (`sync-flip.csv:2-6`):

| Register (ID) | Handler | RSX behaviour | Cite |
|---|---|---|---|
| `M_NV406E_SET_REFERENCE` | `nv406e::set_reference` | calls `thread::sync()`, then writes `RsxDmaControl::ref` | `nv406e.cpp:14-22` [V] |
| `M_NV406E_SEMAPHORE_ACQUIRE` | `nv406e::semaphore_acquire` | raises `sync_point_request`, spin-waits on a guest `RsxSemaphore` until it equals the argument; has a driver-recovery timeout (TDR) | `nv406e.cpp:24-105` [V] |
| `M_NV406E_SEMAPHORE_RELEASE` | `nv406e::semaphore_release` | raises `sync_point_request` (except the flip semaphore), then `write_gcm_label` to guest memory | `nv406e.cpp:107-144` [V] |

`RsxSemaphore` is a 4-byte big-endian word in the report block (`sys_rsx.h:90`, `:105-110`); the
release path writes it through `util::write_gcm_label` (`nv47_sync.hpp:14-63`), which first calls
`thread::sync()` (when `FlushPipe`) and then either asks the backend to emit the label
(`release_GCM_label`, `nv47_sync.hpp:23`) or writes the word directly (`nv47_sync.hpp:62`). `[V]`

**4097 idle / notify / semaphore / report** (`sync-flip.csv:7-15`):

| Register (ID) | Handler | RSX behaviour | Cite |
|---|---|---|---|
| `M_NV4097_WAIT_FOR_IDLE` | `nv4097::sync` | `thread::sync()` — flush MM, sync ZCULL, dirty fragment constants | `nv4097.cpp:844-847`, `RSXThread.cpp:2917-2937` [V] |
| `M_NV4097_NOTIFY` | `nv4097::set_notify` | `thread::sync()` then writes an `RsxNotify` timestamp | `nv4097.cpp:762-785` [V] |
| `M_NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` | `nv4097::back_end_write_semaphore_release` | full pipeline barrier; `write_gcm_label<true,true>` | `nv4097.cpp:820-842` [V] |
| `M_NV4097_TEXTURE_READ_SEMAPHORE_RELEASE` | `nv4097::texture_read_semaphore_release` | shader-read-stage barrier; `write_gcm_label<true,false>` (or `<true,true>` in strict mode) | `nv4097.cpp:787-818` [V] |
| `M_NV4097_GET_REPORT` | `nv4097::get_report` | ZCULL report read, or timestamp write | `nv4097.cpp:644-675` [V] |
| `M_NV4097_CLEAR_REPORT_VALUE` | `nv4097::clear_report_value` | ZCULL stats clear | `nv4097.cpp:677-690` [V] |

**FIFO barriers.** `FIFO::FIFO_DRAW_BARRIER` (`RSXFIFO.h:25`) is an **internal** command, not a guest
register. The flattener emits it at the end of a primitive block (`RSXFIFO.cpp:612`) so that a
`SET_BEGIN_END`-delimited run can be split into draw clauses; `run_FIFO` turns the resulting
`EMIT_BARRIER` into `SET_BEGIN_END(0)` followed by `SET_BEGIN_END(primitive)`
(`RSXFIFO.cpp:890-896`). It is a **draw-clause boundary**, not a memory barrier: it must not be
mistaken for a hazard. `[V]` The flattener also ignores pure invalidation registers
(`NV4097_INVALIDATE_VERTEX_FILE`, `NV4097_INVALIDATE_L2`, `NV4097_INVALIDATE_ZCULL`,
`RSXFIFO.h:85-92`) — those are dirty-bit signals, not ordering. `[V]`

**ZCULL** (`depth-rop.csv`, `RSXZCULL.h`). The ZCULL unit's synchronization surface is:
`ZCULL_control::sync` (`RSXZCULL.h:172`), `ZCULL_control::read_barrier` (`:175-176`),
`ZCULL_control::update` (`:179`, ticked from the FIFO loop at `RSXThread.cpp:1316`), the
`begin/end/check/get/discard_occlusion_query` backend virtuals (`RSXZCULL.h:212-216`), and
`has_pending()` (`:188`). Report writes are queued (`queued_report_write`, `:45-55`) and retired
after a delay or forced by `read_report`; the scheduling constants are
`max_zcull_delay_us = 300`, `min_zcull_tick_us = 100`, `max_safe_queue_depth = 1792`
(`RSXZCULL.h:89-96`). `[V]` A guest that reads a report (`M_NV4097_GET_REPORT`) or that
releases a label after ZCULL work depends on this retirement; the guest-visible sink is the report
block (`sys_rsx.h:98-110`). `[V]`

### 2.3 Producer/consumer edges

Four edges carry every dependency in the system. Each is stated with its RSX source and its Metal 4
realization.

**Edge A — RSX thread → GPU (the draw stream).** The RSX thread decodes the FIFO
(`thread::run_FIFO`, `RSXFIFO.cpp:668`) and encodes draws; on Metal 4 the encoding goes into
`MTL4CommandBuffer`s and one `MTL4CommandQueue.commit(...)`. Ordering *within* the stream is program
order; ordering *across* resources is the scheduler's job (§2.4). The one thing that must not happen
is the VK behaviour of ending a render pass at every barrier (`02` §1.4 [V]) — that flushes tile
memory (**R47**).

**Edge B — RSX thread → PPU (report/label writes).** The guest polls guest memory; RPCS3 writes
labels into `RsxReports` (`sys_rsx.h:105`) and reports into the same block. Today this is a CPU write
gated by `thread::sync()` (`nv4097.cpp:844`, `nv47_sync.hpp:51`) and, for the fast path, by
`release_GCM_label` (`nv47_sync.hpp:23`). On Metal 4 the label sink must be a **shared-storage
`MTLBuffer`** (unified memory), written either by a compute encoder or by the CPU after a
`MTL4CommitFeedback`/`MTLSharedEvent` fires. `[I]` This is where the monotonic `MTLSharedEvent`
replaces `VkFence`+`VkSemaphore`+`VkEvent` (**R8**). The `RsxDmaControl::ref`/`get` handshake
(`sys_rsx.h:80-88`; `nv406e.cpp:19-21`) and the `device_addr + 0x30` flip sync word
(`nv406e.cpp:137`, `RSXThread.cpp:2738`) are guest-visible CPU↔GPU handshakes and stay CPU writes,
but each must be preceded by a GPU→CPU visibility point (feedback or event), not by an implicit
driver flush. `[I]`

**Edge C — GPU → RSX thread (ZCULL results, fence waits).** Occlusion results are read back from the
visibility buffer into the report block; `check_occlusion_query_status`/`get_occlusion_query_result`
(`RSXZCULL.h:214-215`) are the backend seam. On Metal 4 this is a shared `MTLBuffer` read after the
commit that produced it (`02` #33/#35 [V]). The RSX thread must never busy-wait on a Metal object
without the TDR-style escape that `nv406e::semaphore_acquire` already implements
(`nv406e.cpp:68-88`) — that pattern generalizes to any Metal completion wait. `[I]`

**Edge D — frame / flip boundary.** `handle_emu_flip` (`RSXThread.cpp:3635`) drives `flip()`
(`RSXThread.cpp:2776`), which is pure-virtual (`RSXThread.h:372`). On Metal 4 the frame tail is
`waitForDrawable` → encode → `signalDrawable` → `drawable.present()` (`02` #10/#11; `10` §4.7 [V]),
with `maximumDrawableCount ≤ 3` (**R21**). The `m_queued_flip` protocol and `flip_status`
(`RSXThread.h:241`) are unchanged; what changes is that there is no `VkSwapchainKHR` to recreate
(**R29**) and no `vkDeviceWaitIdle` drain (`05` R29). The frame ring becomes `#frames-in-flight`
`MTL4CommandAllocator`s + one `MTLSharedEvent` value (`10` §4.7 [V]/[I]).

**Edge E (implicit) — residency.** Every resource bound into an `MTL4ArgumentTable` or used as an
attachment must be in an `MTLResidencySet` attached to the queue or the command buffer. This is a
*precondition* of every edge above, not an ordering edge (`README.md` §4 shift 2 [V]).

### 2.4 The hazard-derived pass scheduler (RN1)

This is the load-bearing component (`10` §4.3/§4.4, RN1; `README.md` §6). It replaces the VK
render-pass cache (`VKRenderPass.cpp:341-405`) and the packed-`u64` render-pass key with a scheduler
that derives encoder boundaries from **surface hazards**, not from attachment identity.

**Inputs.** Per draw, the scheduler already has, from code that exists today:

- the **target attachment set** — colour/depth surfaces from the framebuffer layout
  (`RSXThread.h:145-147`, `get_framebuffer_layout`); `[V]`
- the **texture read set** — the fragment/vertex samplers for the draw (`fs_sampler_state`,
  `vs_sampler_state`, `RSXThread.h:261-262`); `[V]`
- the **surface read/write set** — `rsx::surface_store` already computes R/W hazards, cyclic
  references and DMA-range tracking (`10` §2.3, `surface_store.h:229`/`:1114`/`:1388`/`:1566`); `[V]`
- the **access class** of each touch — `rsx::surface_access`
  (`shader_read`/`shader_write`/`transfer_read`/`transfer_write`/`memory_read`/`memory_write`/
  `gpu_reference`, `TextureUtils.h:49-66`); `[V]`
- the **draw-clause structure** — `draw_clause`, `draw_command_ranges`, `pass_count()`
  (`draw_call.hpp:19-223`). `[V]`

**Detection.** For each draw, build the ordered pair
`(attachments, reads) = (surface_access writes, surface_access reads)` and test against the
currently-open pass and against the last writer of each resource:

| Hazard | Condition | Action |
|---|---|---|
| RAW (RTT→texture) | a surface written earlier in the stream is now sampled | producer barrier after the writer, consumer barrier before the reader; **if the reader is the same attachment**, programmable blending or a pass split |
| WAR | a surface currently sampled as a texture is now written | consumer barrier before the writer (and a pass boundary if the sample is fragment/tile) |
| WAW | two passes write the same surface | store action + pass boundary; barrier only if the same surface is re-read |
| Cyclic ref / feedback | a surface is simultaneously attachment and sampled texture | intra-pass `[[color(N)]]` fetch if the hardware allows, else split pass + producer/consumer pair |
| Transfer alias | a blit-engine transfer reads/writes a surface that a draw also touches | `memory_barrier(access)` already emitted by `surface_store` (`surface_store.h:1114`, `:1389`, `:1618`) |

The mapping from `rsx::surface_access` to `MTLStages` is the one new table:

| `surface_access` | Producer stages (`afterStages`) | Consumer stages (`beforeStages`) |
|---|---|---|
| `shader_write` (attachment) | `.fragment` (or `.tile`), plus `.vertex` if VS writes | `.fragment`/`.vertex` |
| `shader_read` (texture) | — | `.fragment`/`.vertex` |
| `transfer_write` | `.blit`/`.dispatch` | — |
| `transfer_read` | — | `.blit`/`.dispatch` |
| `memory_write`/`memory_read` | `.all` (over-broad; narrow where known) | `.all` |
| `gpu_reference` | no barrier — reference only (`VKRenderTargets.cpp:946-956`) | — |

`[I]` — the stage names must be taken from the SDK's `MTLStages`; the mapping is the RSX-first
re-expression of `02` §2 rows #16–#20.

**Pass grouping.** Open a new encoder **only** when (a) the attachment set changes, or (b) a hazard
cannot be satisfied intra-pass (the fragment constraint, §2.1 item 5), or (c) an explicit flush
condition fires (§2.4 "Flush conditions"). Otherwise **keep one encoder open across draws**. Where a
boundary is unavoidable but the attachment configuration matches, link the encoders with
`MTL4RenderEncoderOptionSuspending` / `Resuming` so the GPU merges them into one pass (`10` §4.3,
WWDC25 254 [V]). This is the direct answer to **R47** and to `10` §7 item 2 (a naive scheduler opens
*more* encoders, each a store+load).

**Barrier insertion — one chokepoint.** All barriers go through a single emitter. This is an
architectural requirement from `11` §9.1: "a barrier computed inline and handed straight to an
encoder is invisible to the loop." The emitter is the only place that calls the three
`MTL4CommandEncoder.barrier*` methods and the only place that picks an `MTL4VisibilityOptions` value.

```
scheduler_step(draw, open_pass, hazard_map):
    writes, reads = access_sets(draw)
    if attachments(draw) != attachments(open_pass):
        close_and_store(open_pass)
        open_pass = new_pass(attachments(draw))
    for resource in reads:
        writer = last_writer(resource)
        if writer is in this pass:
            if stages_legal_intra_pass(writer, draw):
                emit_intra_pass_barrier(writer.stages, draw.stages)
            else:
                split_pass_with_producer_consumer(writer, draw)
        else if writer is not ordered_by_queue(writer, open_pass):
            emit_consumer_barrier(writer.stages, draw.stages)
    for resource in writes:
        if resource in open_pass.sampled_set:
            emit_consumer_barrier(sampler_stages, draw.stages)
        record_writer(resource, open_pass, draw.stages)
    encode(draw)
```

`[I]` — pseudocode; no comments; the two legality checks are `stages_legal_intra_pass` (no
`.fragment`/`.tile` in the after set on a render encoder) and `ordered_by_queue` (the writer's
command buffer precedes this one in the commit array, §2.1 item 6).

**Flush conditions.** The scheduler must close/serialize when any of these occur; each is an RSX
event, not a heuristic:

1. `thread::sync()` — reached by `M_NV4097_WAIT_FOR_IDLE` (`nv4097.cpp:844`),
   `M_NV4097_NOTIFY` (`nv4097.cpp:778`), `M_NV406E_SET_REFERENCE` (`nv406e.cpp:16`), the semaphore
   release path (`nv47_sync.hpp:51`), and the `pipe_flush_interrupt` local task
   (`RSXThread.cpp:1426-1429`). `[V]`
2. A label/semaphore write that the guest can observe (`write_gcm_label`,
   `nv47_sync.hpp:14-63`) — requires a GPU→CPU visibility point before the CPU write. `[V]`
3. ZCULL report retirement (`ZCULL_control::read_report`, `RSXZCULL.h:166`; `has_pending()`,
   `:188`). `[V]`
4. Frame end / `flip()` (`RSXThread.cpp:2776`, `:3635`). `[V]`
5. A hazard that cannot be satisfied intra-pass (fragment constraint). `[I]`
6. The end of a `draw_clause` batch where the indirect batcher needs a shared argument table
   (`09` §4 strategy C; RN2). `[I]`

### 2.5 Divergence from the VK backend — and why

| Concern | VK backend today | Metal 4 / RSX-first | Why the divergence is forced |
|---|---|---|---|
| Barrier → pass relationship | Every `vk::insert_*_memory_barrier` helper calls `vk::end_renderpass(cmd)` first unless `preserve_renderpass` (`02` §1.4 [V]) — **a barrier *is* a pass boundary** | keep the encoder open; intra-pass barrier when legal, split only on the fragment constraint | Metal 4 has explicit barriers *inside* an encoder (`02` §3.3 [V]); a VK-style break per barrier flushes tile memory (**R47**) |
| Pass identity | `VkRenderPass` + `VkFramebuffer` + packed-`u64` cache keyed on formats/samples/layouts/input-attachment mask (`VKRenderPass.cpp:341-405`, `:411-437`) | `MTL4RenderPassDescriptor` per encoder; boundaries derived from surface hazards (`10` §4.3) | the `VkRenderPass` object does not exist (`README.md` §4 shift 3 [V]); RSX has surfaces, not passes (`10` §1.1) |
| RTT→texture read-back | `render_target::texture_barrier` / `post_texture_barrier` with `VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT` or `GENERAL` (`VKRenderTargets.cpp:862-925`), plus a `VkSubpassDependency` for input attachments (`VKRenderPass.cpp:384-392`) | framebuffer fetch `[[color(N)]]` when same-pass, else split pass + producer/consumer barrier | `VK_DEPENDENCY_BY_REGION_BIT` and subpass self-deps are impossible on TBDR (**R9**); Apple prefers programmable blending over splitting (`02` #24 [V]) |
| Fence model | one `VkFence` per command buffer (`vkutils/commands.cpp:58`; `02` §1.1 [V]); binary `VkSemaphore`; `VkEvent` with three backends (`vkutils/sync.h:38-56`) | one monotonic `MTLSharedEvent` + `MTL4CommitFeedback`; `VkEvent` splits into barriers (GPU) / shared event (host) | no CPU-waitable fence, no binary semaphore, no event object (**R8**; `02` §4 [V]) |
| Async stream wait | secondary (transfer) queue chained by a semaphore waited at `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT` (`VKGSRender.cpp:2597`) | queue-level `waitForEvent`; re-anchored on the surface hazard | stage-gated waits are not expressible (**R7**; `02` §4 item 2 [V]) |
| Write barrier entry | `VKGSRender::write_barrier` → `m_rtts.invalidate_range(...)` (`VKGSRender.cpp:1778-1782`) | same surface-store invalidation, then a hazard re-check on the next touch | the *invalidation* logic is RSX, not Vulkan; it survives (`10` §2.3) |
| Conditional render | `_vkCmdBeginConditionalRenderingEXT` (`VKDraw.cpp:1164-1168`) | compute predicate-patch / host skip (**R6**) | no Metal command (**R6**; `02` §4 item 1 [V]) |

**The justification, stated once.** The VK barrier call sites are shaped by Vulkan objects RSX never
had; translating them 1:1 would inherit the "barrier ends the pass" behaviour that TBDR punishes
(`02` §5 rank 1 rates this the single hardest item). RSX-first starts from information RPCS3 already
computes — `surface_store`'s R/W/cyclic hazard set and `surface_access` (`10` §2.3, §4.4 [V]) — and
asks only *"does this dependency cross a tile?"* The TBDR constraint becomes a scheduler rule, not a
mapping failure (`10` §4.4 [V]).

---

## 3. Interfaces / data structures

New backend-side types. All live behind the backend's own seam so the null/recording device
(`11` §5.2) can exercise the decision path GPU-free.

### 3.1 Hazard description

| Type | Fields | Lifetime | Source |
|---|---|---|---|
| `mtl::resource_access` | `resource_id`, `rsx::surface_access` class | per draw | wraps `TextureUtils.h:49-66` |
| `mtl::hazard_set` | ordered `writes[]`, `reads[]` of `resource_access` | per draw, consumed by the scheduler | built from `fs_sampler_state`/`vs_sampler_state` (`RSXThread.h:261-262`) + attachment set (`RSXThread.h:145-147`) |
| `mtl::writer_record` | `resource_id`, `pass_id`, `MTLStages` stages, `cb_ordinal` | per frame | scheduler bookkeeping |

### 3.2 Pass node

| Type | Fields | Notes |
|---|---|---|
| `mtl::pass_node` | `attachments` (colour/depth `MTL::Texture`), per-attachment `MTLLoadAction`/`MTLStoreAction`, entry `barrier_spec`, exit `barrier_spec`, `draw_list` | the `MTL4RenderPassDescriptor` is built from this node; the node is the diffable unit for H2/H3 (`11` §9.2) |
| `mtl::barrier_spec` | `producer_stages` (`MTLStages`), `consumer_stages` (`MTLStages`), `MTL4VisibilityOptions` | the only thing the barrier chokepoint accepts |

### 3.3 Synchronization objects

| Type | Metal 4 object | RSX analogue |
|---|---|---|
| `mtl::frame_event` | one `MTLSharedEvent` per frame slot + a monotonic `u64` value | replaces `VkFence`+`VkSemaphore`+`VkEvent` (`02` #12/#13 [V]) |
| `mtl::completion` | `MTL4CommitOptions.addFeedbackHandler` → `MTL4CommitFeedback` | replaces per-CB `VkFence` (`vkutils/commands.cpp:58`) |
| `mtl::label_sink` | shared-storage `MTL::Buffer` over the `RsxReports` block (`sys_rsx.h:105-110`) | `write_gcm_label` target (`nv47_sync.hpp:62`) |
| `mtl::semaphore_point` | `RsxSemaphore` word + the event value that gates it | `M_NV406E_SEMAPHORE_ACQUIRE`/`_RELEASE` |
| `mtl::visibility_buffer` | shared `MTL::Buffer` + `setVisibilityResultMode(_:offset:)` | ZCULL report sink (`RSXZCULL.h:166`) |

### 3.4 The barrier chokepoint

One function, one call site per barrier, recordable:

| Signature | Effect |
|---|---|
| `mtl::emit_barrier(encoder, producer_stages, consumer_stages, visibility)` | dispatches to `barrierAfterEncoderStages:beforeEncoderStages:` if both stages are legal intra-pass; otherwise raises a pass-split request that the scheduler turns into a producer barrier on the closing encoder and a consumer barrier on the opening encoder |

Ownership: the scheduler owns `pass_node` and `writer_record`; the frame ring owns
`frame_event`/`completion`; the RSX thread owns `label_sink` writes (guarded by `thread::sync()`).
`[I]`

---

## 4. Invariants

1. **Every write→read on a surface or texture is ordered by an explicit barrier or a pass
   boundary.** No correctness argument may cite `MTLHazardTrackingMode`, residency, or "the driver
   serializes it" (`README.md` §4 shifts 1–2 [V]).
2. **Residency is never used as ordering.** Every argument-table-bound resource is in an
   `MTLResidencySet` (precondition), and every dependency has its own barrier/event (`README.md` §4
   shift 2 [V]).
3. **Every barrier is emitted through `mtl::emit_barrier`.** No inline `barrier*` call anywhere else
   (`11` §9.1 [I]).
4. **No `.fragment`/`.tile` in `afterEncoderStages` on a render encoder.** A fragment/tile producer
   forces a pass split (`README.md` §4 shift 1; `02` §5 rank 1 [V]).
5. **Pass boundaries are a pure function of `(attachments, hazard_set)`.** Given the same inputs the
   scheduler produces the same pass structure, and the structure is emitted as a recordable diff
   (`11` §9.2 [I]).
6. **One encoder stays open across draws unless a grouping rule or flush condition fires.** No
   per-draw encoder (`02` §4 NR1 [V]; **R47**).
7. **A guest-visible label is written only after the work it observes is visible** — i.e. after the
   matching producer barrier and, for the CPU path, after the `MTL4CommitFeedback`/`MTLSharedEvent`
   that covers it (`nv47_sync.hpp:22-62` [V]/[I]).
8. **No unbounded GPU wait.** Every wait on a Metal completion object carries the TDR-style escape
   that `nv406e::semaphore_acquire` already implements (`nv406e.cpp:68-88` [V]/[I]).
9. **Frames in flight are bounded** by the allocator ring and `maximumDrawableCount ≤ 3`
   (**R21**; `10` §4.7 [V]).
10. **Determinism in replay.** In replay mode the frame count, allocator behaviour, clock/vsync and
    the report-retirement timing are pinned so H2/H3 are stable run-to-run (`11` §9.5 [I]).

---

## 5. Failure modes & limits

| Failure | Mechanism | Detection |
|---|---|---|
| **Missing barrier → stale RTT texture** | a surface written in pass A is sampled in pass B with no producer/consumer pair | H5 guest-memory differential (`11` §4) — the only RSX-objective oracle |
| **Over-splitting → tile flush** | a pass boundary per barrier or per draw | H7 perf band (`11` §4); `frame_statistics_t` (`RSXDisplay.h:50-68`) |
| **Under-splitting / illegal intra-pass wait** | `.fragment`/`.tile` in `afterEncoderStages` | H4 `MTL_DEBUG_LAYER=1` validation (`11` §4) |
| **Queue/event deadlock** | `waitForEvent` never satisfied; `nextDrawable` blocking (**R21**) | desync counter / TDR escape; `is_fifo_idle()` (`RSXThread.cpp:2944`) |
| **ZCULL report retired late** | guest reads a stale `RsxReport` | replay's stop-and-apply quiesces at `is_fifo_idle()` (`rsx_replay.cpp:174`; `11` §3.3) — a divergence here is caught by H3 |
| **Stage-gated wait not reproducible** | async stream waited at a stage today (`VKGSRender.cpp:2597`) | by construction (**R7**); re-anchored on the surface hazard |
| **`MTLSharedEvent` value overflow** | monotonic `u64` reuse | `[U]` — at 64-bit width this is unreachable in practice; document, do not guard |
| **Visibility buffer reset semantics** | occlusion reset differs from `vkCmdResetQueryPool` | `[U]` — needs a device spike (`02` §7 Q4; **R12**) |
| **Conditional-render predicate latency** | host skip vs GPU predicate-patch | **R6**; correctness risk documented, not hidden |
| **Barrier visibility too weak** | `.none` where `.device` was required | H4 validation; `[I]` |
| **Barrier visibility too strong** | `.resourceAlias` used for ordinary aliasing (flushes to system memory) | perf only; not-recommended (`02` §4 NR2) |
| **Hazard bookkeeping leak** | `writer_record` not retired across a frame | memory growth; H7 soak |

**Hard limits carried forward.** `maximumDrawableCount ≤ 3` (**R21**); no stage-gated queue wait
(**R7**); no binary semaphore / event object (**R8**); no intra-pass fragment→fragment wait
(**R9**); no conditional-render command (**R6**); no local pixel oracle, so all of the above is
verified *differentially* until rung-3 guest readback exists (**R43**; `11` §6).

---

## 6. Open questions

1. **Hazard-tracking strategy — 1:1 barrier translation or a deferred resource-state pass?** `02` §7
   Q1 frames this; this document commits to the **deferred scheduler** for RSX-first (`10` §4.3),
   but the *sub-question* is whether a non-scheduler fallback (translate each call site) should ship
   first for bring-up. **Recommendation:** scheduler from the first iteration, because the
   suspend/resume pass merge is mandatory from iteration 1 (`10` §7 item 2). `[I]`
2. **Commit-array ordering guarantee.** Is `MTL4CommandQueue.commit(commandBuffers, count)` a
   documented serialization of the array, or conventional? §2.1 item 6. `[U]` — decides whether
   `mtl::writer_record.cb_ordinal` can elide barriers between CBs in one commit.
3. **`updateFence`/`waitForFence` vs producer/consumer barriers for same-CB, cross-encoder
   ordering.** Which is correct and cheaper for the split-pass case? `[U]` — needs the SDK spike.
4. **Visibility-buffer reset semantics.** Confirm the reset point and the pass-spanning accumulation
   (`02` §7 Q4; **R12**). `[U]`
5. **`MTL4VisibilityOptions` selection table.** Is `.device` sufficient for *every* ordinary RSX
   dependency, including the RTT→texture case? `[I]` pending a spike; `02` §4 NR2 says `.device`
   suffices for normal placement-heap aliasing.
6. **Fence replacement policy.** Per-commit feedback handler vs a per-frame `MTLSharedEvent` value
   (`02` §7 Q6). RPCS3 has one `VkFence` per CB today (`vkutils/commands.cpp:58`). `[U]`
7. **`MTL4CommandAllocator` reset cadence** given many CBs per frame (`02` §7 Q7). `[U]`
8. **Frame ring vs drawable ring.** One `MTL4CommandQueue` with batched CBs, or a second queue for
   the async stream (`02` §7 Q5/Q8; **R13**). This jointly decides `VKAsyncScheduler` and the
   semaphore→event change. `[U]`
9. **ZCULL retirement under Metal.** Can the 300 µs / 1792-deep queue (`RSXZCULL.h:89-96`) be
   preserved when the report sink is a shared `MTLBuffer` read after feedback? Or does the
   visibility buffer's pass-scoped reset force a different retirement policy? `[U]`
10. **Does `thread::sync()` need to become asynchronous?** Today it is a CPU-side flush that does not
    wait for the GPU (`RSXThread.cpp:2917-2937`). If the Metal label path requires a real
    GPU→CPU visibility point, `sync()` may need to return a token the caller waits on — a change to
    shared code, which the RSX-first plan otherwise avoids. `[U]` — **highest-risk open question.**
11. **`supports_host_gpu_labels` disposition.** `10` §2.2 rates it `reshape`; on unified memory the
    label workaround may be unnecessary (`02` #15). Decide whether the label path is compute-written
    or CPU-written. `[U]`

**Highest-value open questions (for the human), in order:** **Q10** (does `sync()` become
GPU-observable, and does that force a shared-code change?), **Q2/Q3** (commit/fence ordering
guarantees that decide how many barriers the scheduler emits), **Q4/Q9** (ZCULL/visibility-buffer
semantics that decide the query model), **Q8** (queue topology, which locks the async and present
sync).

---

## 7. Provenance

### 7.1 Primary — verified against this tree `[V]`

- `rpcs3/Emu/RSX/RSXThread.h` — `backend_configuration` `:80-98`; `sync_point_request` `:324`;
  `zcull_ctrl` `:142`; `is_fifo_idle` `:188`; `async_tasks_pending` `:333`; `send_event` `:247`;
  `write_barrier` `:396`; `sync_hint` `:397`; `flip` `:372`; `flip_status` `:241`; samplers
  `:261-262`; surface info `:145-147`; `m_graphics_state` `:254`.
- `rpcs3/Emu/RSX/RSXThread.cpp` — `get_report_data_impl` report/semaphore/device addresses
  `:190-253`; `post_vblank_event` `:1062`; FIFO loop + `sync_point_request` `:1300-1305`;
  `pipe_flush_interrupt` `:1426-1429`; `dma_control_interrupt` `:1409-1423`; `zcull_ctrl->update`
  `:1316`; `init` `:2731`; `flip` `:2776`; `check_zcull_status` `:2806`; `clear_zcull_stats`
  `:2841`; `get_zcull_stats` `:2846`; `copy_zcull_stats` `:2872`; `enable/disable_conditional_rendering`
  `:2877-2915`; `sync()` `:2917`; `sync_hint` `:2939`; `is_fifo_idle` `:2944`; `read_barrier` `:3221`;
  `on_notify_pre_memory_unmapped` `:3255`; `request_emu_flip` `:3605`; `handle_emu_flip` `:3635`.
- `rpcs3/Emu/RSX/NV47/HW/nv406e.cpp` — `set_reference` `:14`; `semaphore_acquire` `:24` (TDR
  `:68-88`); `semaphore_release` `:107`; flip-sync word `:137`.
- `rpcs3/Emu/RSX/NV47/HW/nv47_sync.hpp` — `write_gcm_label` `:14-63` (`release_GCM_label` `:23`,
  `mm_flush` `:38`, `sync` `:51`, raw write `:62`).
- `rpcs3/Emu/RSX/NV47/HW/nv4097.cpp` — `get_report` `:644`; `clear_report_value` `:677`;
  `set_notify` `:762`; `texture_read_semaphore_release` `:787`;
  `back_end_write_semaphore_release` `:820`; `sync` `:844`.
- `rpcs3/Emu/RSX/RSXFIFO.h` — `FIFO_DRAW_BARRIER` `:25`; `interrupt_hint` `:45-49`; ignorable
  invalidation ranges `:85-92`; flattener `:63-127`.
- `rpcs3/Emu/RSX/RSXFIFO.cpp` — flattener `FIFO_DRAW_BARRIER` `:612`; `EMIT_BARRIER` `:890-896`;
  jump-to-self `sync_point_request` `:729`; `FIFO_ERROR` desync `:710`; `run_FIFO` `:668`.
- `rpcs3/Emu/RSX/RSXZCULL.h` — `occlusion_query_info` `:32`; `queued_report_write` `:45`;
  `ZCULL_control` `:98`; constants `:89-96`; `sync` `:172`; `read_barrier` `:175-176`; `update`
  `:179`; `has_pending` `:188`; backend virtuals `:212-216`.
- `rpcs3/Emu/Cell/lv2/sys_rsx.h` — `RsxDmaControl` `:80-88`; `RsxSemaphore` `:90`; `RsxNotify`
  `:92-96`; `RsxReport` `:98-103`; `RsxReports` `:105-110`.
- `rpcs3/Emu/RSX/Common/TextureUtils.h` — `surface_access` `:49-66`.
- `rpcs3/Emu/RSX/Common/surface_store.h` — `read_barrier` `:229`; `memory_barrier` `:1114`,
  `:1389`; `prepare_transfer_target` `:1079-1114`; `clear_rw_barrier` `:1566`; `:1618`; `:1763`.
- `rpcs3/Emu/RSX/NV47/FW/draw_call.hpp` — `draw_command_ranges` `:22`; `draw_command_barriers`
  `:25`; `pass_count()` `:206`; `is_trivial_instanced_draw` `:79`.
- `rpcs3/Emu/RSX/RSXOffload.h` — `dma_manager` `:14-88` (`sync` `:80`).
- `rpcs3/Emu/RSX/VK/VKRenderTargets.h` — `memory_barrier` `:129`; `read_barrier` `:130`;
  `write_barrier` `:131`; surface `write_barrier` call `:456`.
- `rpcs3/Emu/RSX/VK/VKRenderTargets.cpp` — `texture_barrier` `:862-889`;
  `post_texture_barrier` `:891-925`; `memory_barrier` `:944-1014` (gpu_reference `:946-956`).
- `rpcs3/Emu/RSX/VK/VKRenderPass.cpp` — render-pass creation `:341-405`; input-attachment
  dependency `:377-393`; cache `:407`, `:423-437`; `begin_renderpass` `:439-473`;
  `end_renderpass` `:475`; `renderpass_op` `:486-490`.
- `rpcs3/Emu/RSX/VK/VKDraw.cpp` — `renderpass_op` `:1110-1128`; programmable-blending barrier
  `:1130-1149`; draw-time write barriers `:1330-1356`.
- `rpcs3/Emu/RSX/VK/VKCommandStream.cpp` — `queue_submit_impl` `:28-50` (`vkQueueSubmit` `:45`);
  `queue_submit` `:52-69`; `driver_manager_t` `:77-146`.
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp` — `on_semaphore_acquire_wait` `:1136`; `check_present_status`
  `:1282`; `write_barrier` `:1778`; `sync_hint` `:1784`; `close_and_submit_command_buffer`
  `:2510-2614` (async wait `:2597`).
- `rpcs3/Emu/RSX/VK/vkutils/sync.h` — `fence` `:21-36`; `event` `:38-69`; `semaphore` `:71-84`;
  `gpu_label_pool` `:87-103`; `gpu_label` `:105-126`.

### 7.2 Sibling scope `[V]`

- `docs/research/metal4/README.md` — §2 verdicts; §3.3 command/submission/sync rows #33–#55; §4
  shifts 1–5; §6 register R1/R6–R9/R12/R13/R21/R43/R47 and RN1–RN3; §7 phasing.
- `docs/research/metal4/02-submission-sync.md` — §1.1–§1.8 (RPCS3 inventory); §2 rows #1–#41; §3.3
  (metal-cpp MTL4 signatures); §3.5 (MoltenVK #2560); §4 impossible/not-recommended; §5 ranked
  difficulty; §7 open questions.
- `docs/research/metal4/10-rsx-native-metal4-architecture.md` — §2.2 (`backend_configuration`
  verdicts); §2.3 (generic caches as the design surface); §4.3 (pass/attachment model); §4.4
  (synchronization model); §4.5 (draw/submission); §4.7 (present/frame); §5 (delete/keep/reshape);
  §9 (RN1–RN3); §10 (open questions).
- `docs/research/metal4/11-verification-loop.md` — §3 (RRC substrate); §4 (H0–H7 ladder); §5
  (recording seam + null device); §9 (design-for-verifiability requirements).
- `docs/research/metal4/inventory/out/clusters/sync-flip.csv`, `depth-rop.csv`, `surface.csv`,
  `draw.csv`, `misc.csv`; `inventory/out/SUMMARY.md` §global counts; `inventory/SCHEMA.md` §2/§6.
- `docs/research/metal4/01-core-memory.md` — §1.4 (residency); §1.5 (access-mask drop, retention).
- `docs/research/metal4/05-presentation-and-impossible-register.md` — present sync (R21/R29).
- `docs/research/metal4/09-draw-submission-and-batching.md` — §4 (batching strategies), §6.2
  (predicate-patch), §7.2 (the wash verdict).

### 7.3 Apple / external (as cited by sibling scopes; not re-fetched here) `[V]` via `02`/`10`

- Metal 4 core API, `MTL4CommandQueue`/`MTL4CommandBuffer`/`MTL4CommandAllocator`/`MTL4CommandEncoder`/
  `MTL4RenderCommandEncoder`/`MTL4RenderPassDescriptor`/`MTL4VisibilityOptions`/`MTL4CommitOptions`/
  `MTL4CommitFeedback`/`MTL4CounterHeap` — `02` §8.
- Barrier methods (`afterStages:beforeQueueStages:`, `afterQueueStages:beforeStages:`,
  `afterEncoderStages:beforeEncoderStages:`) and `MTL4RenderCommandEncoder.updateFence`/
  `waitForFence` — `02` §3.3.
- `MTLSharedEvent`, `MTLResidencySet`, `MTLHazardTrackingMode` — `README.md` §9; `01`.
- WWDC25 205/254/211 (suspend/resume pass merging, colour-attachment map, allocator-per-frame);
  Apple game-porting-toolkit `managing-metal4-synchronization` and `tbdr-architecture` — `02` §8,
  `10` §11.
- Mesa KosmicKrisp `kk_queue.c`/`kk_event.c`/`kk_cmd_buffer.c`/`bridge/mtl_encoder.m` and
  `workarounds.rst`; MoltenVK #2560 — `02` §3.1, §3.5.

<!-- doc-end -->
