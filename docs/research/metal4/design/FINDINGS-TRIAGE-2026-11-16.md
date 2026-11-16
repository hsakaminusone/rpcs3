# Backend findings triage — 2026-11-16

Independent verification of the 9 findings supplied by an external reviewer.
Three subagents read the cited code in full. No files were modified by the
verification pass. Verdicts:

| # | Finding | Verdict | Notes |
|---|---------|---------|-------|
| 1 | OOB `dst[index_count]` write in shared `line_loop` (BufferUtils.cpp:1352-55) | **CONFIRMED** (2 corrections) | Real OOB, but GL treats line_loop natively so only VK/MTL hit it; over-aligned ring buffers hide it. Fix: write `dst[written]` from `upload_untouched`'s emitted count and return `written+1`, matching the :1291 sibling. |
| 2 | ZCULL deferred array overflow (MTLZCULL.cpp:384) | **DISPROVEN** | Counter index is always in exactly one of free/live/deferred; free list refuses allocation when empty, so `m_deferred_count <= 2048` is a structural invariant. The early-return stops draining, never adds. |
| 3 | Downscale read-back truncates rows (MTLSurface.cpp:660) | **CONFIRMED** | Clamp equates guest rows with host rows; config allows 25-800% so <100% is reachable and loses rows. Fix: delete `rows = std::min(rows, host_last-host_first);`; `host_row_offset()` already maps rows. |
| 4 | H3 half-fixed: unguarded `new_buffer` (MTLTextureCache.cpp:454, 638) | **CONFIRMED** | `new_buffer` returns null on OOM. Depth-plane `plane->contents()` and readback staging `m_staging` both deref unguarded while sibling sites are guarded. Fix: add null checks. |
| 5a | Async compile `NS::Error*` UAF after pool drain (MTLShader.cpp:219-233) | **CONFIRMED (failure paths)** | NSError out-param autoreleased into a pool drained before callers read it. Needs API-ownership confirmation but follows Cocoa convention. |
| 5b | `copy_error_text` at MTLProgramPipeline.cpp:143 | safe in itself | async/link paths read error before pool drain; only the sync path at :316 passes the dangling pointer (victim of 5a). |
| 5c | `&program` raw capture (MTLProgramPipeline.cpp:573) | **DISPROVEN as claimed** | Lifetime actively managed: program_cache outlives jobs, teardown leaks on timeout, advance() keeps jobs during compile. |
| 5d | `NS::String` sources in drained pool (MTLShader.cpp:273,289) | UNCERTAIN | Likely safe (Cocoa setSource retains); needs `LibraryDescriptor::setSource` semantics. |
| 6a | X1 FIFO gate unconditional + unsigned `<` on rewind | UNCERTAIN | Factual, but "fails open" is a documented design choice; project logs mark it the one open deviation. No VK/GL A/B exists. |
| 6b | Cache window frozen at fill | **DISPROVEN** | The freeze IS the fix (strict narrowing), not the bug. |
| 6c | `recovery_point` umax re-enters fault | **CONFIRMED (minor)** | Comment/behavior mismatch; 20-fault watchdog bounds it. Low severity. |
| 7 | MSAA old-contents -> resolve / `unresolve` no-op | UNCERTAIN | Both facts confirmed but intentional & documented (Metal forbids 1->N copy); no reproducer. |
| 8 | R1 (0x28a0) residual | reviewer's assessment **CONFIRMED** | Pass-restart at :6575 is the functional guard; no encoder-closing calls before setArgumentTable; final guard is diagnostic-only. Excision must keep the pass-restart and excise stamp+check together. |
| 9a | Blit extent uses far corner (MTLBlit.cpp:277-87) | **CONFIRMED (low)** | Over-cover, harmless under LoadActionLoad+StoreActionStore; contradicts own comment. |
| 9b | nv3089 GPU-blit early return bypasses watch hook | **CONFIRMED** | `scaled_image_from_memory` returns at :620 before the `note()` at :712. A patch delivered this way surfaces as `watch drift`, not a `watch nv3089` hit. **Relevant to DS bug** — likely the uninstrumented path that wrote the reversed literal. |

## DS bug connection

Of the confirmed findings, only **9b** is relevant: it identifies the fourth,
uninstrumented writer that explains why W2 saw nv308a writes at instruction
offsets but never at the trailing literal. The byte-order math
(`observed = bswap32(correct_mem_word)` for every trailing word) points at a
32-bit byte swap applied by an image-transfer path.

Not relevant: 1 (line topology), 3 (pixel rows), 4/5/8 (crash paths),
2/6b disproven, 7 uncertain, 6a uncertain.
