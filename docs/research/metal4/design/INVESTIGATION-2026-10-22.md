# INVESTIGATION — 2026-10-21/22, Metal 4 backend, Ridge Racer 7 (BLUS30001)

Status: live document. Every entry is a measurement or a code citation; a statement that is an
inference says so and names the measurement it rests on. Entries are appended as runs complete.

Scope of this window: the black video / menu / minigame / car, the RSX FIFO desync and the
resulting freeze, and the texture-cache and present-path divergences found while chasing them.

Related: `LOG-FINDINGS.md` (L-numbers, the running finding register), `RR7-FREEZE-FORENSICS.md`
(the freeze RCA and its H1–H6 hypotheses), `VIDEO-TEXTURE-PARIALITY.md` / `VIDEO-TEXTURE-PARITY.md`
(the video path audit), `METAL4-TODO.md` (the priority queue), `HANDOFF.md` (state of the port).

---

## 0. Method and artifacts

- One emulator instance at a time; every run's `RPCS3.log` and probe file is preserved.
- Probe levels: `MTL_PROBE=1` counters, `2` + coverage latches/histograms, `3` + per-event text,
  `4` + stall/heartbeat, `5` + every dump, shader source and per-draw state. The probe file is
  `$TMPDIR/rpcs3-mtl-probe.log`; bulk text goes there, warnings and errors stay in `RPCS3.log`.
- Preserved artifacts: `~/Library/Caches/rpcs3/metal4-artifacts/20261022-023046-probe2-ebootscreen/`
  (probe level 2, `RPCS3.log`, `probe-level2.log`, 57 in-app screenshots) and
  `~/Library/Caches/rpcs3/metal4-artifacts/20261022-023406-probe5-level5/` (probe level 5,
  `RPCS3.log`, `probe-level5.log`, 150 guest/converted texture pairs, 64 sampled textures,
  28 host-image readbacks, 26 `fp-*.metal` + 38 `vs-*.metal` shader sources).
- The in-app screenshots live in `~/Library/Application Support/rpcs3/screenshots/BLUS30001/`.

### 0.1 Run inventory

| id | date/time | probe level | binary | game reached | outcome |
|---|---|---|---|---|---|
| R1 | 2026-10-21 20:55 | 2 | pre-instrumentation | mode select | ran 2:19, exited |
| R2 | 2026-10-21 22:28 | 3 | pre-instrumentation | FMV handoff | `Dead FIFO` |
| R3 | 2026-10-21 23:04 | 3 | pre-instrumentation | title screen | desync 0:00:36, ran 3:01 |
| R4 | 2026-10-21 23:06 | none | pre-instrumentation | title screen | `Dead FIFO` 0:01:12 |
| R5 | 2026-10-22 02:31 | 2 | + texture probes | EBOOT screen | 702 frames, 669 draws, hung |
| R6 | 2026-10-22 02:35 | 5 | + texture probes | EBOOT screen | 456 dump files, hung |
| R7 | 2026-10-22 02:55 | 3 | + fixes (F1–F4) | EBOOT screen | 399 frames, 0 draws, black |
| R8 | 2026-10-22 03:05 | 2 | + fixes (F1–F4) | race loading | reached race loading screen |
| R9 | 2026-10-22 03:20 | 2 | + desync capture | menu select | hung at menu, 14 capture lines |

---

## 1. The FIFO desync

### 1.1 The desync word is always a data word

Every recorded desync, with the word the FIFO decoder rejected:

| run | word | value |
|---|---|---|
| R2 | `0xbf800000` | IEEE-754 −1.0 |
| R3 | `0xbf800000` | IEEE-754 −1.0 |
| R3 | `0x10000` | `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` label value (L13) |
| R6 | `0x80078000` | `RSX_SHADER_CONTROL_EMULATE_DEPTH_RANGE` (0x80000000) OR 0x78000 |
| R9 | `0x02062000` | — |
| R9 | `0xbf800000` | IEEE-754 −1.0 |
| R9 | `0x0` | — |

`FIFO_ERROR` is raised from four sites: `RSXFIFO.cpp:115` (unmapped address, `read_from_ptr`),
`:460` (unmapped address, `FIFO::read`), `:492` (malformed word), `:526`/`:551` (argument fetch).
A word with `cmd & RSX_METHOD_NON_METHOD_CMD_MASK != 0` takes the malformed path (`:492`).

### 1.2 The desync capture

`thread::recover_fifo` now logs, on the error path only, GET, PUT, the restore point, the code
start, the guest address of the GET, the last label write and the eight FIFO words at the error
(RR7-FREEZE-FORENSICS.md §8.1, which had not been run before this window). R9 produced 14
capture lines; three are distinct:

```
get=0x10ae8ac put=0xcbf0dc get_guest=0x511ae8ac restore_point=0x10ae8ac code_start=0x10ae510 ret_addr=0x3 last_label=0x60300800/0x1 words: 02062000 00041a68 10030303 00041a7c 00000000 00041a6c 80078000 00041a74

get=0x1490000 put=0x10af74c get_guest=0x51590000 restore_point=0x10af74c code_start=0x10af3b0 ret_addr=0x3 last_label=0x60300800/0x1 words: bf800000 3f800000 3f800000 3f800000 3f800000 3f800000 bf800000 3f800000

get=0x3100000 put=0xcbf0dc get_guest=0xffffffff restore_point=0x10ae8ac code_start=0x3020300 ret_addr=0x10ae8b0 last_label=0x60300800/0x1 words: <unmapped>
```

R9 FIFO error counts: `last cmd = 0x2062000` ×7, `last cmd = 0x0` ×6, `last cmd = 0xbf800000` ×1.

### 1.3 What the captures establish

1. **The words at and around the error are data, not commands.** Read as headers they would name
   method indices 0x69A, 0x69B, 0x69D, 0x69F, and **none of those is a defined method**:
   RPCS3's NV4097 map ends at `NV4097_SET_VERTEX_DATA4S_M = 0x00001980 >> 2` (index 0x660) and the
   attribute masks are at 0x1ff0/0x1ff4 (`gcm_enums.h:692`, `:748-749`); registers 0x1A68–0x1A7C
   occur nowhere in `gcm_enums.h`. In the same window `0x10030303` and `0x80078000` are register
   values, and the second capture is eight IEEE-754 floats alternating `0xbf800000` (−1.0) and
   `0x3f800000` (+1.0). **The host is executing a data block as commands.**
2. **The label write is not the corrupting event.** The guest made one mapping,
   `sys_rsx_context_iomap(io=0x0, ea=0x50100000, size=0x3100000)`, so the ring's guest window is
   `0x50100000..0x53200000`; the arithmetic agrees, `0x50100000 + 0xcb5814 = 0x50db5814` is the
   capture's `get_guest` and `0x50100000 + 0x1490000 = 0x51590000` is the other capture's. The
   label address `0x60300800` is **outside** that window. The capture's `label_in_ring` and
   `label_offset` fields measure this directly.
3. **The dispatch history shows what the host executed last.** Before the error the host
   dispatched: `NV4097_SET_ZCULL_STATS_ENABLE` 0x0, `NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE` 0x0,
   `NV406E_SEMAPHORE_OFFSET` 0x10, `NV406E_SEMAPHORE_ACQUIRE` 0x0, `NV4097_SET_SEMAPHORE_OFFSET`
   0x800, `NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE` 0x10000, `NV406E_SEMAPHORE_OFFSET` 0x800,
   `NV406E_SEMAPHORE_ACQUIRE` 0x0 — method indices 0x601, 0x5f3, 0x19, 0x1a, 0x75b, 0x75c, 0x19,
   0x1a (`gcm_enums.h`). The last command is the label release; the guest stopped there.
4. **The corruption is a fixed distance after the jump target.** In three of the four captures the
   error is exactly **0x39c bytes (231 words)** after `code_start`, the last jump target:
   `code_start=0x10ae510` / `get=0x10ae8ac`, `code_start=0xcbf13c` / `get=0xcbf4d8`,
   `code_start=0xcb5478` / `get=0xcb5814`. The fourth (`code_start=0x10af3b0`, `get=0x1490000`) is
   0x3e050 bytes after its target, and its window is the eight floats. **The same jumped-to block
   is mis-parsed at the same word every time**, so the fault is in how the host reads that block.
   The capture's dispatch history is widened to 256 entries and filtered to the commands from the
   jump target onwards, so the command at the fault can be named.
5. **The freeze is a recovery loop that makes no progress.** Every capture has
   `restore_point == get`, so `recover_fifo()`'s `set_get(restore_point)`
   (`RSXThread.cpp:3090`) returns the host to the same word. After the retries the GET reaches
   `0x3100000` — the exact end of the iomap window — where `get_guest = 0xffffffff` and the
   address is unmapped, and the FIFO error becomes fatal (`Dead FIFO commands queue state`).

### 1.4 Hypotheses ruled out, with the measurement that ruled each out

| hypothesis | ruled out by |
|---|---|
| H4 — GET off the header boundary | the word spacing in the capture is 8 bytes per command, matching a count of 1 argument + 1 header; the stream is aligned |
| H3 — torn or partial guest write | the words either side of the error are complete command-shaped values with complete arguments |
| H1 — the guest stopped producing | the guest was writing commands normally up to the error; the host left the command stream into data |
| H2 — a GPU label was never released | the label is released and written (`last_label=0x60300800/0x1`); the label address is outside the FIFO window |
| the host's label write corrupted the ring | **superseded** — the label address lies inside a page the FIFO maps, because the iomap maps per 1 MB page and the ring is not contiguous (§1.3.2) |
| a one-word drift in the FIFO's position accounting | `read_unsafe`'s fall-through (`m_fifo_pos += 4; return false;`, `RSXFIFO.cpp:383-384`) supplies the final word of a command; `read()` (`RSXFIFO.cpp:520-521`) consumes the header and the first argument; `skip_methods` (`RSXFIFO.cpp:389-403`) lands on the last argument. The count field is the argument count, so a command occupies `count + 1` words, which the capture's 8-byte spacing confirms |
| `FIFO_PACKET_BEGIN` block accounting | `FIFO_PACKET_BEGIN` (`RSXFIFO.h:23`) has no reader; the "fast read" comment at `RSXFIFO.cpp:337` is stale |
| the non-accurate FIFO path | `RSX FIFO Fetch Accuracy` is absent from both `config.yml` and `config_BLUS30001.yml`; the default is `rsx_fifo_mode::atomic` (`system_config.h:58`), which selects the accurate path |
| the probe causes the freeze | R4 ran with `MTL_PROBE` unset and produced `Dead FIFO` at 0:01:12 with `last cmd = 0xbf800000` |
| the interpreter-only mode causes the freeze | R4 was interpreter-only and still produced the `Dead FIFO` |
| R7's black screen was caused by the F1–F4 fixes | R7 used probe level 3 and R8 the same binary at level 2; R8 reached the race loading screen. `texture/create`, `texture/reuse` and `texture/reuse rejected` are 0 in R7, so the changed texture-cache code did not execute |

### 1.5 Open, in the order they must be answered

1. **Which command carries the data block the host fell into.** The bulk commands are
   `NV4097_SET_TRANSFORM_PROGRAM` and `NV4097_SET_TRANSFORM_CONSTANT`
   (`nv4097.cpp:130`, `:144`, `:168`, `:202`, `:244`), which are the only FIFO commands that
   carry a variable-length data block, and they are the only ones that derive a skip count from
   something other than the header's count:
   `skip_methods((rcount < write_count ? rcount : count) - 1)`.
   The vertex-data path is the other candidate: the floats in the second capture are a position
   array, and the guest's inline/bulk vertex data is carried by `NV4097_INLINE_ARRAY`
   (0x1818>>2) and `NV4097_SET_VERTEX_DATA*_M` (0x1880–0x1980).
2. **Shared or Metal-specific.** The `RR7-FREEZE-FORENSICS.md` §8 companion control — the same
   scene on Vulkan — has not been run. If Vulkan freezes identically, the fault is in the shared
   FIFO/bulk-write path and Metal inherits it; if only Metal freezes, the fault is in the Metal
   backend's FIFO interaction.
3. The capture must additionally name the method index of the offending word
   (`cmd=0x%x reg=0x%x method_known=%u`) and the last commands the host dispatched
   (a dispatch history in `thread`). Both are implemented; the build carrying them is in progress.

---

## 2. The video path

### 2.1 The video surface is never written

- R6's `host-target` readbacks of the surface at guest `0xc0880000` are `0xff` for every pixel
  except one row: `nonzero 920320 of 921600` words, `distinct bytes 2`, `first ffffffff
  ffffffff ffffffff ffffffff`. A Metal private image that was never rendered into is not
  guaranteed zeroed.
- The blit that should fill it is `NV3089_IMAGE_IN`, and it NOPs. R9's log:
  `RSX: NV3089_IMAGE_IN: Operation NOPed out due to empty regions (clip 0x0, output 0x0)`.
- The register dump that accompanies the warning shows the guest's blit had no geometry:
  `Blit engine: clip x: 0 y: 0`, `clip width: 0 height: 0`, `output x: 0 y: 0`,
  `output width: 0 height: 0`, `NV3089: DS DX: 0`, `NV3089: DT DY: 0`. The context, colour
  conversion, colour format and operation registers of the same module *were* set
  (`NV3089: source fmt: a8r8g8b8`, `NV3089: op: srccopy`, `NV3089: context surface: surface 2d`).
  The register file is the executing context's own file (`REGS(ctx)` is `ctx->register_state`,
  `NV47/HW/context_accessors.define.h:2`; the single static context owns the global
  `method_registers`, `RSXThread.cpp:125`), so the dump reflects the context that ran the command.
- The NOP is reached at `nv3089.cpp:43-47`, which tests the clip before the scale. The scale
  registers were 0, and `ds_dx()` returns 0 for a zero register
  (`rsx_decode.h:2044-2052`) with the comment "Will get reported in image_in".

### 2.2 The video plane is sampled as a render target

R5's coverage:

```
family texture host format      446  metal texture format (key 134)   BC3
                                 223  metal texture format (key 80)    BGRA8Unorm
family texture bind context      446  upload context (key 1)            shader_read
                                 223  upload context (key 8)            framebuffer_storage
family texture readback format    14  gcm readback format (key 133)   A8R8G8B8
family texture readback context   14  readback context (key 8)         framebuffer_storage
```

All 14 readbacks in the run were A8R8G8B8 from a `framebuffer_storage` context, and the
`framebuffer_storage` binds number 223.

### 2.3 The decoded frames do reach guest memory

R6's sampled textures include 640×360 and 1280×720 single-channel images at guest addresses
`0x5258ec80`, `0x525c7080`, `0x526e0480`, `0x52718880`, `0x52750c80`, all 100 % non-zero. The
1280×720 one renders as a recognisable scene when drawn as ASCII. The 640×360 ones render as a
flat field with one blob.

### 2.4 The black rectangle's texture is correct

The quad that matches the black rectangle in the screenshots (guest x −0.3992..0.4008, y ±0.71,
so screen x 415..860, y 103..615 of 1280×720) samples an 8×8 DXT45 texture. Its guest bytes
are `ffff 0000 0000 0000 ffff ffff 0000 0000` repeated — colour0 white, alpha0 0xff, all indices
zero, i.e. opaque white — and the converted bytes are identical to the guest bytes
(`guest-0-addrc483e080-fmt88-sw1-8x8-sz64.bin` vs `conv-0-…`). **The blackness is not in the
upload.** The draw's own state is also correct: `write mask 0xf`, `color format 8` (A8R8G8B8),
`depth format 2` (Z24S8), `samples 1`, `alpha test 0`, `blend enable mask 0x0`.

### 2.5 Open

The NV3089 blit's geometry registers are zero while the same module's context registers are set.
Combined with §1.3.1 — the host executing a data block as commands — the hypothesis is that the
blit's setup commands were consumed as part of a bulk data block. Not yet measured.

---

## 3. The menu text

R9's menu screenshot (23:25:20) shows dropped and substituted glyphs:
`R dg e St te G AND PRIX` (Ridge State Grand Prix), `ON_INE BATTLE` (ONLINE BATTLE),
`UFR A S C!AL ENT` (UFR A SPECIAL EVENT), `RANKING L..O S E`, `X CON IRM` (X CONFIRM).
The title-screen line of the same run renders correctly
(`Press the START button to proceed to the main game.`), so two text paths differ.

The two runs' screenshots are preserved in the artifact directories.

---

## 4. The race scene

R2/R3/R5-era screenshots (`22_57_02`, `22_57_05`, `20_58_50`, `20_58_57`) show the environment
rendering (buildings, lit windows, wet-road reflections) and the car body dark, with emissive
detail (tail lights, red trim, interior) present. The car is the only 3D element that does not
render.

The black rectangle of §2.4 and the car are different symptoms: the first is a 2D quad with a
verified-correct texture; the second is a 3D object.

---

## 5. The texture cache

### 5.1 Measurements (R5, level 2, new probes)

```
latch    3  texture/create
latch    0  texture/reuse
latch    0  texture/reuse rejected
latch    0  texture/reuse cleared
latch    0  texture/upload partial
latch    0  texture/upload cleared levels
family texture upload declared mips     3  declared mip count (key 1)
family texture upload subresources      3  uploaded subresource count (key 1)
family texture upload format            3  gcm upload format (key 136)   DXT45
```

**In R5's EBOOT path the texture cache performed 3 CPU uploads, all 1-mip DXT45, and no reuse
and no partial upload.** The reuse, mismatch and coverage hypotheses are not exercised by that
path and remain unproven.

### 5.2 Divergences from the OpenGL reference

| id | OpenGL | Metal | status |
|---|---|---|---|
| T1 | clears a reused image that is not uploaded wholly from the CPU — `GLTextureCache.h:751-761`, `fast_clear_image` | does not clear | **fixed** (F2) |
| T2 | does not check the extent, the mip count or the format of a reused image — `GLTextureCache.h:735-745` | does not check | **fixed** (F3); OpenGL still has the hole |
| T3 | ignores `rsx::flags32_t flags` in `create_new_texture` — `GLTextureCache.h:724` | ignores them | **not a divergence**; only Vulkan uses them (`VKTextureCache.cpp:1013-1112`) |
| T4 | ignores `memory_load` and the tile in `create_nul_section` — `GLTextureCache.h:833` | ignores them | **not a divergence**; Vulkan's use of them is the host-GPU DMA path (`VKTextureCache.cpp:1218-1222`) |
| T5 | does not override `on_section_destroyed`; the section releases its own image — `GLTextureCache.h:446-466` | does not override it either | **not a divergence**; Vulkan overrides it because Vulkan's image is owned by the cache (`VKTextureCache.cpp:393-400`) |
| T6 | — | — | OpenGL also lacks `handle_memory_pressure`; Vulkan has it (`VKTextureCache.h:541`) |

T1 is the divergence that produces the `0xff` surface of §2.1: a reused Metal image keeps the
content of the previous occupant of the address, and an image that was never rendered into is
not zeroed.

---

## 6. Performance (R6, level 5, 300-frame interval)

| row | total | calls | avg | max |
|---|---|---|---|---|
| `sync/label release` | 17 381.114 ms | 300 | 57 937.05 µs | 470 078.71 µs |
| `sync/semaphore wait` | 1 926.634 ms | 65 297 798 | 0.03 µs | 2 991.67 µs |
| `fault` | 428.126 ms | 713 | 600.46 µs | 136 499.33 µs |
| `texture/readback wait` | 134.776 ms | 1 | 134 776.29 µs | 134 776.29 µs |
| `present/display gap` | 24 934 744 µs | 301 | 82 839.68 µs | 650 112 µs |
| `frame/gpu us` | 2 612 762 µs | 301 | 8 680.27 µs | 18 582 µs |
| `draw/subdraws` | 63 546 | 63 546 | 1.00 | 1 |
| `local task` | 611.084 ms | 3 885 726 | 0.16 µs | 9 482.62 µs |

Derived: the frame period is 82.8 ms (present/display gap) and the GPU is busy 8.68 ms of it.
`sync/label release` runs once per frame and costs 57.9 ms. The 8.59 ms difference between the
label release and the frame period is the rest of the CPU path.

`sync/label release` is `MTLGSRender::release_GCM_label` (`MTLGSRender.cpp:4011-4039`), which
drains the GPU with `recorder.wait_idle()` and then returns false, so the shared
`util::write_gcm_label` writes the label on the CPU (`NV47/HW/nv47_sync.hpp:62`). Vulkan, with
`Allow Host GPU Labels: false`, does not drain at all
(`VKGSRender.cpp:1710-1713` returns false when `supports_host_gpu_labels` is false, and the flag
follows the setting at `VKGSRender.cpp:801`), and writes the label on the GPU timeline with
`vkCmdUpdateBuffer` when it is enabled (`VKGSRender.cpp:1733-1756`).

The 713 faults are all on guest threads (`fault/on guest thread` 712, `fault/on RSX thread` 1) and
all are handled (`fault/handled` 713, `fault/unhandled` 0). Their addresses are
`0xc1bd0008` and `0xc2fd0008`, written with the value 1, in R6.

---

## 7. Code changes made in this window

| id | change | file | rationale |
|---|---|---|---|
| F1 | a zero `DS_DX`/`DT_DY` is the power-on value; it is now the default scale 1.0, reported, and the division by it at `nv3089.cpp:180`/`:186` is gone. The empty-region warning now names the geometry | `rpcs3/Emu/RSX/NV47/HW/nv3089.cpp:34-66` | a zero scale is not a request, and the old code divided by it |
| F2 | a reused image is cleared when the section is not uploaded wholly from the CPU, and a level that no upload covers is cleared | `rpcs3/Emu/RSX/MTL/MTLTextureCache.h:474-524`, `:566-580`; `MTLTextureCache.cpp:276-343` | T1; a Metal image that was never written is not zeroed |
| F3 | a reused image is discarded unless its format, extent and mip count match the new section | `rpcs3/Emu/RSX/MTL/MTLTextureCache.h:460-508` | T2; the old path only compared the type |
| F4 | the desync capture: GET, PUT, restore point, code start, guest address of the GET, last label write, eight FIFO words, the offending word's method index; plus a dispatch history in `thread` | `rpcs3/Emu/RSX/RSXThread.cpp:3061-3120`, `RSXThread.h:196-230` | RR7-FREEZE-FORENSICS.md §8.1, which had not been run |
| F5 | the label write is recorded so the capture can compare it with the FIFO window | `rpcs3/Emu/RSX/NV47/HW/nv47_sync.hpp:62` | the same capture |

F1–F3 did not execute in R7/R8's EBOOT path (`texture/create` 0), so their effect is unmeasured.

New probes added and their levels:

| probe | level | purpose |
|---|---|---|
| `texture/reuse`, `texture/reuse rejected`, `texture/reuse cleared` | 2 | the reuse decision and the clear |
| `texture/upload partial`, `texture/upload cleared levels` | 2 | the upload coverage against the declared chain |
| `texture upload declared mips`, `texture upload subresources`, `texture upload format` | 2 | the upload's own shape |
| `texture bind context`, `texture bind dimension`, `texture bind format class` | 2 | how each unit's image was classified |
| the desync capture and the dispatch history | unconditional, on the error path only | the FIFO |

---

## 8. Corrections to earlier statements

- The "one word early" reading of the capture is **wrong**: the position accounting is
  self-consistent (§1.4). The data words are *at* command boundaries, not one word before them.
- The four "divergences" L43/L45/L46 are **not Metal bugs**: OpenGL behaves the same way in all
  three cases (§5.2, T3–T5). The real divergence is T1, the missing clear.
- `NV3089_IMAGE_IN`'s "empty regions" is **not** a misleading message about the scale: the
  guest's clip and output areas are genuinely zero (§2.1).
- The interpreter never ran in the 22:28 run because the real pipeline won every draw
  (`draw/recompiler` 116 503, `draw/interpreter` 0); the interpreter is a first-use fallback, and
  with the asynchronous variant prebuild the fallback was never needed.
