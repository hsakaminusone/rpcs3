# Plan — draw-time encoder lifecycle (2026-11-14)

Status: **A implemented (restart, L245); B planned; C rejected.**
Context: L243–L245 in `LOG-FINDINGS.md`. The `0x28a0` fault was a guest read
inside a draw ending the pass's render encoder before the draw bound its argument
tables.

The rule this plan is built on:

> If the draw holds an encoder, no call it makes may end it.  If a call does end
> it, the encoder is re-established at the point of use, never assumed.

## What the other backends do (measured, not inferred)

| backend | ordering | mid-clause split | reopen |
|---|---|---|---|
| **Vulkan** | `VKDraw.cpp:1040` uploads index/vertex data, then `begin_render_pass()` at `:1157`.  Comment at `:1049`: *"Faults are allowed during vertex upload. Ensure consistent CB state after uploads."* | `VKGSRender.cpp:1696` `flush_command_queue()` → `close_and_submit_command_buffer()`; `:2555` *"End any active renderpasses; the caller should handle reopening."* Always hard-syncs on a flushable fault. | `VKDraw.cpp:1111` `reload_state = (!m_current_draw.subdraw_id++)` → the next subdraw begins a new pass. |
| **KosmicKrisp (MTL4)** | pass is opened lazily at the first render command (`kk_cmd_buffer.c:298` `cs_get_render()` when `need_to_start_render_pass`). | `kk_cmd_buffer.c:314` `cs_get_compute()`: ends the open render encoder, `mtl_apply_attachment_store_ops`, `kk_cmd_buffer_dirty_all_gfx`, `need_to_start_render_pass = true`.  No GPU submission is added by the split. | the next render command re-opens and replays dirty state. |
| **OpenGL** | no pass object; a flush rebinds FBO/program. | n/a | n/a |

Two consequences:

1. **A mid-clause split is the reference design, not a workaround.**  KK does
   exactly what L245 implemented (end, mark dirty, replay), and VK does the same
   at subdraw granularity.  Our restart must stay.
2. **The commit is not an inefficiency to remove.**  Both VK and KK end the
   encoder without submitting; Metal only submits here because the guest's
   faulting instruction retries and needs canonical bytes on the CPU
   (`flush_for_readback`).  That is the fault contract, and the shared cache
   already gates it on `num_flushable > 0`.

## A. Restart at the point of use — implemented (L245)

`frame_resources` carries the pass record (`pass_surface`, `pass_pipeline`,
`pass_encoder_state`, `pass_scissor`, `pass_restartable`).  `emit_geometry`
compares its encoder with `recorder.current_encoder()` before the argument
tables; on a mismatch it reopens load-only (`clear_mask = 0`), re-emits the owed
hazards and replays pipeline + encoder state.  `command_recorder::table_generation()`
forces the binders to rewrite after a slot is reallocated.

Open question this plan closes: **is the reopen in the common path?**  Today the
pass opens in `end_draw()`, i.e. *before* the subdraw loop performs its guest
reads, so every fault pays a reopen, and a fault in a later subdraw can split a
pass that was already absorbing draws.

## B. Late pass open — planned (aligns with both oracles)

Move the open out of `end_draw()` to a deferred `ensure_draw_pass()` called at
the first encoder use in `emit_geometry()`.

Steps:

1. `end_draw()`: replace the `open_pass`/absorb block with the *decision* —
   bind, feedback split (`samples_open_pass()`), and record `frame.pass_*`.
   Do not touch the encoder.
2. `emit_geometry()`: immediately before the argument-table block, call
   `ensure_draw_pass()`:
   - if `frame.encoder` is the recorder's current encoder and
     `frame.attachments.can_absorb()` → absorb (same as today);
   - else `open_pass(recorder, 0, frame.pass_surface)`, `record_pass_hazards(true)`,
     `setRenderPipelineState`, `apply_encoder_state`.
3. The L245 restart becomes the fallback inside `ensure_draw_pass()` for an
   encoder that ended *after* the open (a read in the bind itself, or a binder
   path that allocates).
4. Keep the one-shot "pass could not open" report.  The failure mode moves from
   `end_draw() == false` (core NOP draw) to dropping the subdraw, which every
   other early return in `emit_geometry` already does.

Ordering proof: the index upload, the vertex upload, the constant fill, the
transform-constant upload and `execute_pipeline_dependencies()` all run before
step 2, so none of them can invalidate a pass that has not opened.  Everything
after step 2 is either encoded on the encoder (tables, zcull, draws) or is a
barrier.

Risks / checks:

- `record_pass_hazards()` emits on `frame.encoder`; it must run after the open.
- `samples_open_pass()` must be evaluated against the *previous* pass before it
  is closed; the feedback split therefore has to stay in `end_draw()` (it only
  closes, never opens).
- A clear is consumed by the first pass of a key.  The deferred open passes
  `clear_mask = 0`; clears keep their own path (`MTLGSRender.cpp:2149/2173`), so
  no clear can move later and wipe encoded draws.
- MSAA: every attachment stores (`MTLFramebuffer.cpp:251/283/295`), so a load
  after a split is always valid; a restart must never re-clear.

Verification: DS to the loading screen and RR7 to the boot card with
`MTL_PROBE=4`; `recorder/pass restarted` must be 0 on a fault-free run,
`recorder/pass absorbed` unchanged or higher, no `0x28a0`, `rpcs3_test`
687/687, release scrub `check` clean **and** the scrubbed tree built (L232).

## C. Conditional commit — rejected

The earlier idea was to commit+wait only when the flushed surface is an
attachment of the open pass.  Neither oracle does this: VK hard-syncs on every
flushable fault, and KK adds no submission at all (the copy shares the command
buffer, so there is nothing to commit).  In our design the submission exists
because the fault is synchronous and the guest retries; a surface that is not an
attachment still has to be readable on the CPU before that retry.  The existing
`num_flushable > 0` gate is the only legitimate narrowing.

## Follow-ups not in this plan

- L240: the whole-packet FIFO gate is still an unproven deviation.
- RR7 needs a frozen-process sample plus a pad to reach car selection.
