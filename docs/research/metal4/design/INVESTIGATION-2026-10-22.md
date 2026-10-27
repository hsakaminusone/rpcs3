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

---

## 9. The 2026-10-22 02:5x level-5 run — objective data

Artifacts: probe file 25 064 lines, `RPCS3.log` 24 493 lines (E 115, W 1536, S 184, U 168, A 4, T 1, F 0).

### 9.1 The desync capture, in full

```
get=0x1f48c8c8 put=0xd3732c get_guest=0xffffffff restore_point=0x10a0e20
code_start=0x1f48c8c8 ret_addr=0x10a0e24 last_label=0x60300800/0x1
label_in_ring=0 label_offset=0x0 cmd=0x3f48c8ca method_index=0x3232 method_known=0
words: <unmapped>
```

Repeated 21 times with identical values. The measurements in it:

| field | value | what it is |
|---|---|---|
| `get` | 0x1f48c8c8 | the host's GET, 300 MB past the PUT and outside the ring's window `0x50100000..0x53200000` |
| `get_guest` | 0xffffffff | the address is unmapped, so the read that follows is the FIFO error |
| `put` | 0xd3732c | the guest's PUT, inside the ring |
| `restore_point` | 0x10a0e20 | where the recovery puts the GET back — inside the ring, and the host walks into the same word again because the shift that misaligned the stream is behind it |
| `code_start` | 0x1f48c8c8 | the last control-flow target, equal to the wild address, so the host followed a **call** *to* it |
| `cmd` | 0x3f48c8ca | the word read. The old-jump test does not match (`& 0xe0000003 = 0x20000002`); the **call** test does (`& 0x00000003 = 2`), and `& 0x1ffffffc = 0x1f48c8c8`, the address followed |
| `method_index` | 0x3232 | the method the word would name, and `method_known=0` — it names no method |
| `label_in_ring` | 0 | the label address is not in a page the FIFO maps, so the label write is not the corruption |

**Root cause of the shift (L70 in `LOG-FINDINGS.md`).** The accurate FIFO's
"first argument not yet published" path is off by one. `read()` advances the
position to the first argument and fetches it; when that fetch is empty it left
the position on the argument and raised `m_remaining_commands`. The retry then
enters `read_unsafe()`, which advances by four **before** it fetches, so the
retry reads the second argument as the first, consumes one word too many, reads
the next command's header as the last argument, and resumes the stream on that
command's first argument — **a data word parsed as a command**. A data word with
low bits `0b10` is a legal call encoding, which is why the next control-flow word
is a wild call. The 21 identical repeats follow: the recovery restores the GET to
a point behind the fault but *after* the shift, so the shifted stream is re-walked
to the same word. Fixed by rewinding the position to the header and clearing the
remaining count, so the next `read()` re-parses the command from its start.

### 9.2 The refused draws

```
[14.125] skip frame 0, config valid 1, conditional render disabled 0, no attachments 1,
         scissor 0x0+256+64, window clip 268369920x268369920, viewport 0x0+256+64,
         scissor invalid 1, scissor clipped 1
[47.042] skip frame 0, config valid 0, conditional render disabled 0, no attachments 0,
         scissor 0x0+1920+1080, window clip 268369920x268369960, viewport 0x0+4096+4096,
         scissor invalid 0, scissor clipped 1
```

The 64 samples split 63 `config valid 0` and 1 `no attachments 1`. The 63 are at 47.042–47.050 s and
55.461–55.528 s in two bursts; the desync capture is at 99.774 s. **The refusals and the desync are
44 s apart and are not the same event.**

`rtt_config_valid` is set inside the colour-attachment loop, after
`ensure(layout.color_addresses[index])` (`RSXThread.cpp:1794`), and the loop skips the attachment —
and so never sets the flag — when `color_buffer_unused` (`:1724`) or when the pitch check fails with a
pitch ≤ 64 (`:1740`). Only the pitch check logs, and the run's log has **no**
`Framebuffer setup error: Color target failed pitch check`, so the pitch check passed and the
colour target was dropped as unused. The scissor path is excluded by the numbers: the rectangle is
1920×1080 at the origin and `scissor invalid 0`. The silent early return of
`get_framebuffer_layout()` when the surface clip is zero (`RSXThread.cpp:1490-1494`) is the remaining
way to leave the flag clear without a log.

**Why 63 in a row (a Metal divergence, not shared behaviour).** The flag is recomputed by
`get_framebuffer_layout()`, which every draw calls through `bind_framebuffer()`. Vulkan's
`begin()` calls `init_buffers` → `prepare_rtts` and OpenGL's `begin()` calls `init_buffers`, both
*before* `end()` consults `should_skip_draw()`, so the next draw repairs an invalid layout as soon as
the guest writes a usable one. Metal's `begin()` did not bind, and `end()` skipped before it would
have bound, so a single invalid layout survived every later draw until an unrelated configuration
write re-armed it. That is the flashing the operator reported (see §10.2 for the fix).

### 9.3 The run's reach and its faults

| interval | `draw/subdraws` |
|---|---|
| 30.210 s | 16 389 |
| 35.502 s | 12 897 |
| 40.773 s | 14 782 |
| 98.994 s | 51 211 |

The 02:31 level-2 run had 669 draws over 702 frames; the 23:04 level-3 run had 116 503. The level-5
run reached real gameplay and froze there, so **the probe level changes how far the run gets** and
the runs are not comparable without that.

All 65 faults were on guest threads and all were handled, with `fault/sections to flush` 0 and
`fault/sections to unprotect` 0.

At the end the guest's threads were alive: `sys_cond_wait` aborted after 37.157 s
(`CPamfDemuxer::ReadThread`), `vdecEntry` after 37.876 s (`HLE Video Decoder`), and the audio and
input threads never blocked. **The RSX thread is stuck; the guest is not waiting on it.**

### 9.4 The log census

Errors (115): 54 `VM: Failed to lock sudo memory`, 21 `FIFO error: possible desync event`,
11 `SPU: Trampoline simplified`, 10 `SIG: Thread [cellAudio Thread] is too sleepy`,
6 `'_CellDmuxCoreOpReleaseAu' failed with 0x8061000a : CELL_EPERM`,
4 `'cellVdecGetPicItem' failed with 0x80610104`, 2 `'sys_cond_destroy' failed with CELL_ESRCH`, and
the rest are `ppu_loader` PRX-registration notices, a missing translation directory, and missing
`BLJS30001` game files and region fonts.

Warnings (1536): 819 SPU `MFC_EAH`/`MFC_Cmd` notices, 71
`ROP reads from rN without writing to it` (upstream debug noise,
`FragmentProgramDecompiler.cpp:849`), 65 `sys_memory_allocate`, 54 `_sys_prx_start_module`,
34 `sys_memory_free`, 7 `sys_event_queue_receive` aborts.

**Nothing in the shared log is Metal-specific.**

### 9.5 The measurement environment

`SYS: Low Power Mode is enabled, performance may be reduced` at 0:00:00.000312, and
`pmset -g batt` reports `Now drawing from 'Battery Power'` at 40 % with `lowpowermode 1`. Every
performance figure in this document was taken on a throttled, battery-powered machine.

## 10. The Metal 4 semantics experiment and the code fixes

No emulator run was made for this section. The measurements come from
`EXPERIMENT-metal4-semantics.mm` (this directory), a standalone Metal 4 program built with
`clang++ -std=c++20 -fobjc-arc -I3rdparty/metal-cpp -framework Metal -framework Foundation` and run
on the same device the emulator uses (**Apple A18 Pro**). It exercises the exact calls the backend
makes: `MTL4::Compiler::newLibrary` with an MSL source, `newRenderPipelineState`,
`MTL4::ArgumentTable` with `setInitializeBindings(true)`, `setAddress`/`setTexture`/`setSamplerState`,
`setArgumentTable`, `drawPrimitives`, and `MTL4::RenderPassDescriptor` with a render-target size
different from the attachment.

### 10.1 Results

| case | setup | result | conclusion |
|---|---|---|---|
| A | table `v=100`, bound, draw | `64 ff ff ff` | baseline |
| B | table mutated to `v=200`, **no** re-bind, draw | `c8 ff ff ff` | the capture is **not** at `setArgumentTable` |
| C | mutated to 200, re-bound, draw | `c8 ff ff ff` | re-bind is redundant but safe |
| D | `setAddress(0, 0)`, draw | `00 ff ff ff` | `setAddress(0)` is legal and reads 0 |
| E | `setTexture(ResourceID{}, 0)`, draw | `64 00 00 ff` | a null texture samples 0 (black) |
| F | `setSamplerState(ResourceID{}, 0)`, draw | `64 ff ff ff` | a null sampler falls back to a default; the texture still samples |
| P2 | 64×64 attachment, render target 128×128, viewport 64×64 | the whole attachment renders | an **oversized** render target is harmless |
| P3 | 64×64 attachment, render target 32×32, viewport 64×64 | **nothing renders** | a viewport that exceeds the render target **drops the whole draw** |
| P4 | 64×64 / 64×64, viewport 128×128 | the whole attachment renders | an oversized viewport is clipped, not dropped |
| P6 | 64×64 attachment, render target 32×32, viewport 32×32 | the left 32×32 renders | a fitting viewport renders inside the render area |

A and B together settle the capture point: the later mutation did not change A, so the table is not
read at GPU execution; and B saw the mutation with no re-bind, so the table is not captured at
`setArgumentTable`. The capture is per draw, at encode time. **The consequence is that an unwritten
slot keeps the previous draw's binding**, which makes writing a cleared slot as zero mandatory.

### 10.2 Fixes made in this window (not built, not run)

1. **`MTLGSRender::begin()` now binds the framebuffer** when the frame is not skipped and rendering
   is enabled, as Vulkan (`init_buffers` → `prepare_rtts`) and OpenGL (`init_buffers`) do before
   `end()` tests `should_skip_draw()`. This is the repair path for §9.2's 63-run refusal: the layout
   is recomputed before the skip test, so one invalid layout cannot poison the rest of the frame.
2. **`MTLGSRender::bind_framebuffer()`'s fast path now tests the context, `rtt_config_dirty`,
   `attachments.valid()` and `ignore_change`**, matching `VKGSRender.cpp:2618-2619` and
   `GLRenderTargets.cpp:107`. Without the context and dirty tests, a draw clause that changed the
   context (clear vs draw) with an unchanged layout could reuse the wrong attachments.
3. **`argument_binder::bind`'s `address()` writes zero**, i.e. whenever the stage is first bound or
   the value differs, zero included (L72, §10.1 A/D/E). Clearing was silently skipped, leaving the
   previous draw's address bound.
4. **`FIFO_control::read()` rewinds on an empty first argument** instead of advancing past it
   (L70, §9.1). This is the strongest candidate for the desync: one word of shift turns the next
   command's argument into a command, and a data word with low bits `0b10` is a legal call.

### 10.3 What the experiment rules out

- **A mis-indexed texture unit is not the black-object cause.** The fragment GLSL declares units at
  `texture_slot_base + index` and the mirror/depth/subpass images at their own slot bases; the
  binder writes the same constants (L76).
- **The 4096×4096 guest viewport does not drop the draw.** The Metal viewport is the surface clip
  (`build_viewport(clip_width, clip_height)`), not the guest viewport (L75).
- **A null sampler does not black out a draw** (F); only a null *texture* does (E), and the backend
  clears a texture unit on transition.
- **The render target may be larger than the attachment but never smaller than the viewport**
  (P2/P3). The backend sizes both from the same surface clip, so it satisfies this by construction.

### 10.4 Fix batch 2 — the decisive diagnostic

The five fixes of §10.2 hold the correctness line. They do not answer §9.3, which is now a binary
question: for draw 18896 the quad is encoded, every state field is right, and the target keeps the
clear colour (L101). Either the fragment ran and the alpha test discarded it, or no fragment ran.

`MTL_PROBE_NO_ALPHA_TEST=1` compiles the alpha test out of every fragment program. The intended
three-way reading was colour × depth (depth write is on, `LEQUAL`, `z = 0` against a `1.0` clear),
so that a fragment that ran and wrote the clear colour could be told from one that never ran.
**That plan did not survive run 2**: the depth readback returned nothing usable (§10.6), so the
colour probe alone reads two outcomes — "wrote `(0,0,0,0)`" and "no fragment" — and the
discriminator is now a forced output (§10.7). Two level-5 additions from this batch did hold and
stay: the **composed view swizzle** (`apply_swizzle_remap`, not the format's declared layout,
L103a), and the **argument-table addresses** (`vc`/`fc`/both contexts with the draw's index and the
resulting element address), so a GPU-reads-zero divergence is visible next to the block contents
the same draw printed (L102).

`dump_host_image` also prints the full-image word count now, not one row's, as the denominator
(L106), and the probe banner states every rendering-changing switch before the first frame, so a
run's own log says its rendering is not the shipping rendering.

The switch is read when a program is built, so the run needs a cold archive: `MTLDevice.cpp` gives
it `archive-…-noalpha.bin` so the real archive is neither read nor written by a diagnostic run.

### 10.5 Two candidates closed without a run

- **The argument binder's stage-only cache is sound.** The tables are per recorder *slot*, and both
  binders are reset in `begin()`, so a table change within a frame cannot leave a slot unwritten
  (L107).
- **Inverted culling is not the car's cause.** L98's mid-frame dumps show the car as a correct
  silhouette: geometry, cull and depth accepted it and the colour is lost in the fragment inputs.


### 10.6 Run 2 (2026-10-22 20:24) — the alpha test is excluded, and the probe wedged the queue

Run 2 carried the five fixes of §10.2 and `MTL_PROBE_NO_ALPHA_TEST=1`, so every fragment program was
built without the alpha test and the run resolved to a cold `archive-…-noalpha.bin` (the real
archive untouched). Probe log `/tmp/mtl4-noalpha.log`, 16.4 MB, three windows of the five:

| window | armed | frame | draws probed |
|---|---|---|---|
| 1 | 47.740 s, video-plane bind (B8 1280×720) | 879 | 33 |
| 2 | 84.587 s, cubemap bind | 2079 | 289 |
| 3 | 124.963 s, cubemap bind | 3279 | 670 (capped later, §10.6.2) |

**The video draw, measured.** Draw **12262** is the video quad: `vp 50 fp 100`, `primitive 8
count 4`, full-screen clip (−0.9992…1.0008 / −1.0014…0.9986), `scissor 0 0 1280x720`, cull state
`2/1` — identical to draws in the same run that *do* paint — blend `770/770 771/771`
(SRC_COLOR / ONE_MINUS_SRC_COLOR), clear `(0,0,0,0)`. Its three units are the video planes:

- unit 0 B8 1280×720, guest `0x52750c80`, `nonzero 230400 of 230400 words, distinct bytes 71`
- unit 1 B8 640×360, guest `0x52831c80`, `nonzero 57600 of 57600`
- unit 2 B8 640×360, guest `0x5286a080`, `nonzero 57600 of 57600`

with "applied swizzle A4 R4 G4 B4, remapped, a swizzled view is bound". The alpha test was compiled
out of this program for this run. All four colour patches after the draw are exactly
`0x00000000`, `distinct 1`. Under those blend factors a fragment that ran and sampled that luma
cannot leave exact zero, so the draw either did not reach the write stage or sampled a zero view.
**The alpha-test hypothesis is dead** (L110).

**The depth probe failed.** 2 924 lines, every one garbage: `min -3.7e19`, `written` equal to the
whole patch, so every word differed from the 1.0 clear and none was a depth value. Both
`Depth32Float` and `Depth32Float_Stencil8` paths were tried; neither produced a value that could be
read as depth. The function is removed rather than repaired (L112) — repairing a diagnostic that has
already cost a run and answered nothing is worse than moving to a discriminator that cannot be
misread (§10.7).

**The freeze was the capture, not the backend.** Window 3 (a race-start frame, 670 draws) probed
8 copies per draw (4 colour + 4 depth), each a synchronising readback:

| | run 1 | run 2 |
|---|---|---|
| `start lag` median | 0.147 s | 0.416 s |
| `start lag` max | **2.947 s** | **94.788 s** |
| `MTL4CommandQueueErrorDomain` commit failures | **0** | **588** |

The lag starts climbing in the same second the window arms (124.963 s) and rises monotonically;
the 588 feedback failures all begin at 2:46.3, at the end of the 670-draw capture frame, and the
game never advances past the race start. The user quit at 4:21; the renderer shut down at probe
219.6 and teardown then segfaulted on the main thread (`Deleting old game window` → `SYS: Segfault`,
the L2/I6 class). The first 125 s had no commit failures at all, so the five fixes did not wedge the
queue — the probe did. Two changes: the depth probe is gone, and a window now probes **at most 64
draws** (`diagnostic_window_probe_limit`), which is still 33/33 for the video window and 64/289,
64/670 for the others.

**One desync still occurs** with both FIFO fixes in: 53.451 s, `get=0x1490000 put=0x10a588c
get_guest=0x51590000 restore_point=0x10a5814 code_start=0x10a5478` (L115). The parked section
(§1.2) remains parked; nothing in this run changes its shape.

### 10.7 The next experiment — a forced output that cannot be misread

The remaining ambiguity is not resolvable from colour: "fragment wrote `(0,0,0,0)`" and "no
fragment" are the same bytes. The response is to make one program's write unmistakable instead of
inferring it.

`MTL_PROBE_FORCE_FP=<id>[,<id>…]` (new) compiles `_FORCE_OUTPUT` into the named fragment programs
only. The epilogue clears the deferred discard before its discard block and writes
`ocol0 = vec4(1, 0, 1, 1)` after every ROP stage, so the value cannot be turned into the clear
colour by rounding, remapping, the alpha test or a `kil`. The alpha test and alpha-to-coverage are
also cleared for that program. The run writes `archive-…-forcefp.bin`, so the real archive is
neither read nor written.

It is targeted by compiled-program id because the draw log already names it (`fp id %u`), and
because a global override is useless here: in run 2 the other draws of the frame already paint the
patches, so a screen-wide magenta would prove nothing about draw 12262. The id is the one number
that isolates it.

Reading, with the video window's four patches:

| patch after the video draw | meaning | next step |
|---|---|---|
| magenta | the draw reaches the write stage: attachment, pipeline, raster and the viewport are all sound | Test B: sample-only output (or `MTL_PROBE_REPLACE_TEXTURES` for the coarse answer) to separate the binding/view from the YUV math |
| unchanged clear | no fragment of that program wrote | raster/clip side: geometry, viewport, attachment of *this* draw; the state fields already look right (L101), so the next suspects are the vertex stream and the pass the draw lands in |

One caution recorded from run 1: the same patch can be repainted by a later draw of the frame. The
probe runs after *every* draw of the window, so the transition (clear → magenta) is what is read,
not the end-of-frame value.

### 10.8 Run 3 (2026-10-23 17:02) — the forced run was not forced, and the constants clear the alpha

Run 3 (`MTL_PROBE_FORCE_FP=98`, log `/tmp/mtl4-forcefp98.log`) put the video draw at `fp 98` and the
alpha test was compiled out of the forced variant (`fp-98.metal` has no `discard_fragment` and no
`comparison_passes`; its epilogue is the magenta store). The video window's patches were again
exactly zero. **The zero does not come from the forced program**, because the program that ran was
not the forced one:

- The archive name was `…-forcefp.bin` for *every* force configuration
  (`MTLDevice.cpp`), and the previous force run had left `fp 98`'s **unforced** pipeline in that
  file. `RPCS3.log` records the archive being loaded at 0:03:25 and `fp 98` first forced at
  0:03:26 — the pipeline for the video's program was available from disk before the forced
  source was ever compiled.
- The forced id is not the video's id on every route: race-first the video is `fp 98`, intro-first
  it is `fp 99`. A run that names an id can miss the video entirely (run 3's *intro* draw, frame
  831, is `fp 99` and was not forced).

Two facts survive the run:

- **The alpha test is not the cause, again.** The full constant file at frame 831
  (`fp 99 [0..6]`) is `(-0.5,…) (0.187,…) (0,1.8558,…) (-0.5,…) (0.4678,…) (1.575,…) (0,1,0,0)`.
  `fs_main` sets `r1.w = fc[6].y = 1.0` and multiplies by the clamped vertex colour
  (`vc[0] = (1,1,1,1)` from the transform-constant dump), so the alpha test's `greater, 0`
  passes. This agrees with run 2's compile-out (L110) and rules out a zero alpha constant.
- **The quad's stream and matrix are right; the descriptor is not proven.** The vertex trace reads
  the *host* persistent stream and **forces `in_pos[3] = 1.0`**. A descriptor that decodes four
  components takes its `w` from the fourth word (the trace's `tc0.x`), and a zero there clips the
  whole quad while the trace still prints a correct `(−1,1)` clip.

### 10.9 The next experiment — a selector the routes cannot move, and a descriptor that is read

Three changes, all diagnostic:

1. `MTL_PROBE_FORCE_FP_TEXTURES=<n>` forces every fragment program whose texture state enables
   exactly `n` units. The video enables three; the title frame's other programs enable one. The
   selector does not depend on an id, so the intro and the in-race video are caught on either route.
2. The archive name carries the selector's value (`-forcefp98`, `-forcefpt3`), so a run never loads
   a pipeline built under a different force configuration. The stale `-forcefp.bin` was removed.
3. The capture prints the descriptor (`attrib0/attrib8`, `size`/`type`/`stride`), the input state
   (`indexed`, `count`, `offset`, `length`, `rebase`), the vertex range and ring bases, the full
   `scale_offset_mat` and `z_near..z_far`, and the stream's fourth and fifth words next to the
   forced `w`.

Reading, with the video window's four patches:

| patch after the video draw | meaning | next step |
|---|---|---|
| magenta | the draw reaches the write stage under a program that cannot discard, so raster, pass, viewport and the fetched geometry are all sound | the colour the *unforced* program computes is then the whole question; test a sample-only output and the view swizzle |
| unchanged clear | the draw does not reach the write stage even with a forced output, and the descriptor line says why (`size 4` with a non-one fourth word, or a ring base that differs from the trace's) | fix the descriptor path as the native defect |

### 10.10 Run 4–6 (2026-10-24 19:07–19:50) — the video quad was culled, and the winding maps straight across

**Run 4** (`MTL_PROBE=5 MTL_PROBE_FORCE_FP_TEXTURES=3`, `/tmp/mtl4-forcefpt3.log`, 142 475 lines)
made the selector work as designed: the log shows `fp 105` and thirteen other programs forced at
3.475 s "because its state enables 3 textures", and the tyres and the track railings came out
magenta. The intro FMV did not. The captured video draw (draw 34093, frame 1427, 46.828 s) is a
fullscreen quad, `primitive 8 count 4`, `vp 21 fp 105`, `textures dims 0x15`, `rop 0x204000`,
`attr mask 0x101 stride 20`, indexed with six indices, `vertex base 0 count 4`. Its four colour
patches read exactly the pass clear value, and so did every later draw of the frame.

Two facts narrowed the remainder to the raster stage:

1. The frame's other 32 draws are quads at guest x 1.0004..1.6252 — entirely outside the clip
   volume, so they paint nothing and the video quad is the only draw that can cover the patches.
2. The video draw is the only draw of the frame in a depth-attached pass (`pass … depth 1`); the
   32-quad pass that follows has `depth 0` and clears the same colour surface.

Element by element the state was sound: `scissor 0 0 1280x720`, viewport `0,720,1280,-720`,
`topology 0`, `write mask 0xf`, depth `515` LEQUAL against a 1.0 clear with `z = 0`, blend
`770/771`, and the vertex trace a full-screen quad (clip −0.9992..1.0008 / −1.0014..0.9986, `w = 1`).
The forced program contains no branch that can reach black (`fp-105.metal:192` is an unconditional
`out.ocol0 = float4(1.0, 0.0, 1.0, 1.0)`), so the fragment stage was never entered.

**Run 5** repeated run 4 with `MTL_PROBE_NO_CULL=1` (a capture-window-only encode override, no shader
change, so no archive name changes). `/tmp/mtl4-nocull.log`, frame 795 at 62.636 s: all four patches
went to `0xffff00ff`, `mean b 255.0 g 0.0 r 255.0`, and the user confirmed the screen flashed a full
magenta frame. Culling was the rejection.

The winding arithmetic explains it. `build_viewport` negates the height (`MTLRasterState.cpp:104`)
so that the guest's y-down screen space lands in Metal's y-down framebuffer space; that mirror is
already the whole compensation. `capture_front_face` then inverted the guest's `cw` to
`MTL::WindingCounterClockwise` as well, so the mirror was counted twice and the face test ran
backwards. The Vulkan backend does it once, with a positive rectangle and a direct
`front_face::cw → VK_FRONT_FACE_CLOCKWISE` (`VKGSRender.cpp:205`). All 351 traced quads in run 4 are
CCW in NDC, so every quad the app draws with culling enabled was rejected; quads with `cull 0`
(the boot logo, the HUD) were the only ones that painted. This is DEVIATIONS-DRAW **D5**, which
already recorded the defect, the reference comparison and the exact fix.

**The fix.** `capture_front_face` now maps `cw → MTL::WindingClockwise` and
`ccw → MTL::WindingCounterClockwise` (`MTLRasterState.cpp:66`), with no other change.

**Run 6** (`MTL_PROBE=5 MTL_PROBE_FORCE_FP_TEXTURES=3`, culling **enabled**, `/tmp/mtl4-wind.log`,
frame 683 at 66.251 s): the video patches read `0xffff00ff` again, this time without any override, so
the mapping is the fix and not the switch. The Xevious mini-game panel, black in every previous run
(L11), rendered correctly in the same run.

What remains is the colour the unforced program computes, which is the subject of the run that
follows.

### 10.11 The FIFO desync (2026-10-24, after run 6) — a data word read as a header, at a fixed 0x39C

The user asked for the desync to be root-caused rather than parked. This section records the
forensics and what they leave open; the findings are L117–L121.

**Artifacts.** `/tmp/mtl4-run.log` (the `Atomic` run, 28 desyncs) and `/tmp/mtl4-fifo.log` (the
`Ordered & Atomic` run, 24 desyncs), both with the shared capture that prints `get`, `put`,
`code_start`, `restore_point`, the 8-word window at GET, the 16-word cache snapshot and the 256-word
dispatch history.

**1. The reported word is a non-method word, and the report comes from the malformed branch.**
`read()` gates on `m_cmd & RSX_METHOD_NON_METHOD_CMD_MASK` (`0xa0030003`). For episode B's
`0x00010000` that is `0x10000 ≠ 0`, and the old-jump, new-jump, call and return tests all fail, so
`data.reg = FIFO_ERROR` at `RSXFIFO.cpp` and `run_FIFO` logs "possible desync". L37 had claimed the
opposite (that `0x10000` is a method and desyncs silently); that was wrong, and the corrected row is
in `LOG-FINDINGS.md`.

The capture proves which path produced it. Its cache line reads
`addr=0x10a28cc size=0x4 next=0xffffffff`. Only the control-command shortcut in `fetch_u32` (the
`addr == m_next_cmd_entry` fast path) stores `m_cache_addr = addr` with an unaligned address and
`m_cache_size = 4`; `next = umax` is then written by the malformed branch. On the unmapped-page path
`get_addr` returns `umax` and the shortcut never runs. `sys_rsx_context_iomap(io=0x50100000,
size=0x3100000)` happens once at init and there are no `iounmap` calls, so the page is mapped in
every capture: the unmapped-page theory is closed.

**2. GET is exactly 231 words past the last jump, in both episodes, on the same block.**
Episode A: `get=0xcb0e20`, `code_start=0xcb0a84`, `cmd=0x10030303`, `put=0x10a3b00`. Episode B:
`get=0x10a28cc`, `code_start=0x10a2530`, `cmd=0x00010000`, `put=0xcb28cc`. `0xcb0e20-0xcb0a84 =
0x10a28cc-0x10a2530 = 0x39C`. The last six dispatches are identical in both: `reg 0x19 = 0x10`,
`reg 0x1A = 0`, `reg 0x75B = 0x800`, `reg 0x75C = 0x10000`, `reg 0x19 = 0x800`, `reg 0x1A = 0`
(`cmd 0x40064/0x40068/0x41D6C/0x41D70/0x40064/0x40068`). So the parser consumes the same block
correctly in both runs and then reads a *data* word where the next header belongs. L58 recorded the
`0x39C` constant; this is what it is.

Episode B's ring at the failure is a perfectly regular two-word stream —
`0x10a28b0 00040064`, `b4 00000010`, `b8 00040068`, `bc 00000000`, `c0 00041d6c`, `c4 00000800`,
`c8 00041d70`, `cc 00010000` — so GET lands on the argument of `reg 0x75C`, one word inside a valid
command. The parser's own history is internally consistent for all 256 entries (`d=+8` for
one-argument commands, `d=+4` inside multi-argument groups, `m_next_cmd_entry = H + 4 + 4*count`),
so the bookkeeping did not drift; the position did.

**3. The region the parser read is being rewritten while it reads it.** Episode A's history says the
block at `0xcb0a84…0xcb0e1c` was `reg 0x19`, `reg 0x1B`, `reg 0x3A49`, `reg 0x764`, `reg 0xC1`…;
the ring captured at the same addresses microseconds later holds the vertex-attribute block
(`00041aac`, `00041ab4`, `00041ac8`, `00041adc`, …). Episode B's dump changed between the twenty
retries. The producer is therefore recycling the buffer the parser is walking, which closes L85
(it was never a remap or a cache/live discrepancy).

**4. There is no guard once GET passes PUT.** `fetch_u32` treats only `put == addr` as EMPTY, and the
`addr < put` trim (which ends the cache exactly at PUT) does not apply when GET is ahead. In that
state the cache is filled to the end of the 128-byte line that contains PUT and everything past PUT
is served as if published. `nv4097.cpp`'s `fifo_read_limit` already has the matching comment — "if
put is behind get it will result in a number ignored by min" — so the layer knows the state exists
and has no recovery for it; the 4.07 MB overrun in L83 is that walk continuing until a word happens
to be off the iomap.

**5. Recovery cannot make progress.** `recover_fifo` sets GET back to `restore_point`, which equals
GET, so the malformed word is fetched again; the retry path sleeps 2 ms, so twenty recoveries take
~50 ms and the queue is declared dead. Episode A repeats 7 times and episode B 21 times, all at one
address, which is exactly the shape of "nothing advances".

**What is still open, and the probes that close it.** Two events can move a correct parser off its
boundary: (a) the control-command shortcut runs at a position that is *not* a command boundary
(`m_next_cmd_entry` wrong, or the guest rewriting the word between the trim scan and the fetch);
(b) a command's count carries GET over PUT, after which (4) makes the overrun unstoppable. Both are
momentary, so the logs only ever contain the symptom. Four level-1 probes were added to the shared
FIFO code (all `TODO(excise)`, rate-limited to 20 ms, counting suppressed events), and the tree with
them builds:

- `FIFO diag: header phase slip pos=… expected=… delta=…` — fires in `read()` when the position
  about to be read as a header is not the one the previous command predicted. This is event (a) at
  the instant it happens. The empty-argument retry sets a flag so its deliberate rewind is not
  reported.
- `FIFO diag: fetch at/after PUT fetch=… put=… delta=…` — fires in `fetch_u32` when the parser is at
  or past the producer frontier, i.e. (b) has already happened.
- `FIFO diag: jump past PUT target=… put=…` and `FIFO diag: jump high-water target=…` — a jump is the
  only way the parser reaches an unpublished region deliberately; the high-water mark also bounds
  the guest's command-buffer ring, which nothing else logs.
- `FIFO diag: PUT rewind prev=… put=… max=…` — the producer pointer moving backwards is a buffer
  recycle; its size and spacing are the missing ring-extent datum.

The next run needs no game action beyond the current route: the desyncs arrive on their own at 44 s
and 85 s. Reading `~/Library/Caches/rpcs3/RPCS3.log` in time order then gives either the phase-slip
line (a) immediately before the first `FIFO error`, or the read-ahead line (b) and the jump target
that produced it.

### 10.12 The 2026-10-25 run — neither (a) nor (b): the frontier moves under a waiting parser

One approved run with the four probes, `MTL_PROBE` unset, PID 62650, log
`~/Library/Caches/rpcs3/RPCS3.log` (the previous log is at `/tmp/RPCS3-prev-2026-10-24.log`). 104
diag lines, 21 `FIFO error` lines, `Dead FIFO` at `0:01:05.467572`. The desync reproduced after the
title video closed at 1:04.99; the user quit.

**The two candidates from §10.11 are both excluded.**

- `header phase slip` never printed. The parser's `m_next_cmd_entry` agreed with `m_fifo_pos` at
  every single header for the whole 65 s, so (a) — the parser reading a header off its own boundary
  — did not happen.
- `jump past PUT` never printed either, and the only two `jump high-water` lines are `0xcb0000`
  (from `0x154c`, 34.259 s) and `0x10a0000` (from `0xcb09d4`, 34.876 s). So no jump carried the
  parser out of the published region.

So the parser did not leave its boundary; it stayed on it and read a word that is not a command.
The position was wrong from the start of the episode, not produced by a mis-step inside it. That
moves the question one level up: what put the parser there, and why did nothing stop it.

**The producer's geometry (§10.11 question, answered).** The two jump targets are the two command
buffers, and every published pointer sampled in this run sits in the first 64 KB above one of them
(the buffers are larger; §10.14 measures at least 136 KB each):

```
0xcb0000  <- buffer base 0        (jump target, from 0x154c)
0x10a0000 <- buffer base 1        (jump target, from 0xcb09d4)
```

`put` takes only these values in the run: `0xcb13bc`, `0xcb6264`, `0xcbca74`, `0x10a6264`,
`0x10af0dc` — all within `base+0x10000`. The two bases are `0x3F0000` apart, so this is a
ping-pong pair of command buffers, and the spacing is where the app allocated them, not a
ring. This is L122. (L115's `get=0x1490000` is then simply an address the parser walked to outside
both buffers, not a ring extent.)

**`put` names the buffer the parser is not in.** Every sample is taken at the resume that follows an
app move, so it always shows the pair at the same offset:

```
get=0x10a13bc put=0xcb13bc   get-put=+0x3F0000   low16 equal
get=0x10a3d08 put=0xcb3d08   get-put=+0x3F0000   low16 equal
get=0x10a6264 put=0xcb6264   get-put=+0x3F0000   low16 equal
get=0x10a3774 put=0xcb3774   get-put=+0x3F0000   low16 equal
```

The move itself is where it was logged: `prev=0x10a6264 put=0xcb6264` at 63.354 and 64.522, and at
that moment the parser was exactly on `A+0x6264`. So the equality test does fire — what the pair
shows is that the app immediately hands `put` to the other buffer. Comparing addresses across the
two is meaningless: the buffer being filled has the lower address, so `addr > put` is false for a
whole walk in the other one. This is L123.

**The parser waits on `put` and is moved off it, 50 times benignly.** From 36.26 s to 42.58 s the
game sits idle and the parser is caught up at one fixed address, `get=0x10a13bc`, with
`put=0xcb13bc`:

```
0:00:36.260509 PUT move prev=0xcb13bc put=0xcb13bc ... get=0x10a13bc
0:00:36.260565 fetch at/after PUT fetch=0x10a13bc put=0xcb13bc delta=0x3f0000 next=0x10a13bc last_cmd=0x40068
```

The same pair repeats at 36.6, 36.9, 37.3, 37.9, … 42.5 s. Each one is a resume: the parser had
stopped because it reached the producer, the producer's pointer then moved, and the parser re-read
the word at its stopped position. It is benign every time because the word there is still the
guest's own last command (or a jump that re-parks it). This is L125, and it is the state the whole
failure hinges on: **it is the only moment the parser reads memory it does not have a published
right to.**

**The failing frame.** The parser resumed at `A+0x6264` after the 64.522 move and consumed the
chunk the app published up to `A+0xF0DC` — `0x8E78` bytes, one chunk, no overrun. It then went into
buffer B and walked to `B+0xF57C`; the fetch that broke it was the next word:

```
PUT move prev=0x10af0dc put=0xcbca74 get=0xcbf580
fetch at/after PUT fetch=0xcbf580 put=0xcbca74 delta=0x2b0c next=0xcbf580 last_cmd=0x0
FIFO error ... (last cmd = 0x10030303)  at get=0xcbf580
```

Two readings of `next=0xcbf580` are possible, and the `last_cmd` value picks one. A jump to
`B+0xF580` would leave no header read there, but `last_cmd=0x0` means a header *was* read at
`B+0xF57C` and came out `0x00000000` — a count-0 NOP, whose `m_next_cmd_entry` is `0xF580`. So the
parser **walked** to `0xF57C`; it did not land there. That fixes the size of the overrun: the
furthest B was ever published in the run is `B+0xCA74` (every `put` into B is `0xcb13bc`,
`0xcb6264` or `0xcbca74`), so `0xF57C` is `0x2B08` past it. The `put` move in the same millisecond
is the app starting to append to B; it is not the trigger.

That also explains why the bookkeeping stayed consistent (L124) while the content was wrong: the
11 KB the parser consumed in B is the buffer's **pre-run** content, which is a well-formed old
stream, so every header/argument count agreed with the next address. Where the parser entered B, and
with which link word, is what the new `jump cross-buffer` line records. This is L126.

**The NOP path has no bound.** It is

```cpp
if (!count)
{
    m_fifo_pos += 4;
    sync_get();
    data.reg = FIFO_NOP;
    return;
}
```

— no `put` check, no cache trim, just advance. Zero is a legal count-0 NOP, so zeroed or unwritten
space parses as an unbounded NOP stream, and the walk stops only at the first non-NOP word. In this
episode the walk was short (the words just below `B+0xF580`), but nothing in the parser limits its
length. This is L127.

**A second way the guest can be misled.** `sync_get()` releases `m_cache_addr + m_cache_size`
whenever that covers `m_fifo_pos`, so the guest can see GET up to 1 KB *ahead* of what was parsed.
Across a region the parser is walking without publication, that tells the producer the region is
free. This is L128 and it is the one mechanism here that can also make the app recycle early.

**Why Vulkan and OpenGL do not show it.** The FIFO code is shared, so the parser is identical; the
difference is how often each backend parks the parser on the producer. `VKGSRender::release_GCM_label`
issues the label write GPU-side and returns true, `GLGSRender` enqueues a copy, so both keep GPU work
in flight and the parser stays behind the producer. `MTLGSRender::release_GCM_label` drains the GPU
on the RSX thread (`flush_for_readback`/`wait_idle`) and returns false so the CPU writes the label —
so the RSX catches the producer 50 times in 6 idle seconds (L125), and every one of those catch-ups
is a window in which the next producer write is to a word the parser is about to re-read. The label
design is why the race is *reachable* on this backend; it is not the mechanism of the desync itself.
This is L129.

**Why the run produced 104 clean-looking diag lines and still no chain.** Every test in the probes
is a numeric comparison against `put`, and this layout defeats all three: `addr > put` (read-ahead)
is false for a whole walk in the buffer that has the lower address, `target > put` (jump past PUT)
is false for a jump from A down into B, and the rewind test only fires when `put` decreases by more
than 64 KB. The hop from A into B, with `put=A+0xF0DC`, satisfied *all* of "not past PUT", so nothing
logged it. The discriminating test is a slab comparison,
`(addr ^ put) & ~0xffff`, not an address comparison. That is L130, and it is a defect in the probes,
not in the guest.

**Probes added for the next run** (all level 1, all `TODO(excise)`, in shared FIFO code, independent
of `MTL_PROBE`):

- `FIFO diag: jump cross-buffer from=… target=… put=… cmd=…` — every jump whose
  target leaves the source's 64 KB slab, logged unconditionally (capped at 4096 for the run). This is
  the A↔B hop that every earlier probe was blind to (L130); it names the link word and the
  frontier at the moment the parser entered the other buffer.
- `FIFO diag: desync word=… pos=… put=… delta=… nops=…` followed by 32 lines
  `desync jump[i] from=… target=… put=… cmd=…` — a 32-entry in-memory jump ring, dumped once per
  failing word. This is the chain that landed the parser where it was; it is absent from every log
  so far because the log is too slow to hold every jump of a running parser.
- `FIFO diag: resume at former PUT addr=… put=… delta=…` — fires when `fetch_u32` misses at exactly
  the address where it last returned EMPTY *and* the producer is not a sane distance ahead
  (`put <= addr`, or `put - addr > 0x10000`). This isolates the L125/L126 event and says whether the
  resumed word was the guest's or unwritten.
- `FIFO diag: NOP run start=… cur=… len=… put=… delta=…` — fires at 256 consecutive count-0
  headers. This is the L127 walk caught at its entry point, with the last jump's `from`/`target` in
  the same line, so the jump that entered the unpublished region is identified even if the jump ring
  has rolled.
- `FIFO diag: PUT move prev=… put=… parked=… delta=…` — every frontier move that either ends a
  catch-up (`parked=1`) or jumps by more than 64 KB sideways. This is the producer timeline the
  earlier rewind-only probe could not show.

**What the next run decides.** The `jump cross-buffer` line immediately before the first `FIFO
error` names the link that entered the wrong buffer, with the frontier at that moment. Two cases:

- **The link is fresh but premature** — the app wrote a jump to a block it has not appended yet.
  Then the app relies on the parser not reaching the link before the target is ready, and the fix
  belongs in the wait: the parser must not run ahead of the producer's *published* extent for the
  buffer it is in, which means the FIFO layer needs the per-buffer frontier the app already
  maintains and RPCS3 does not read.
- **The link is stale** — the word at the source predates this pass. Then the app published a chunk
  whose tail still holds a previous pass's link, and the fix belongs in what `put` is allowed to
  publish: a chunk whose last word is not part of this pass must not be consumed.

Either way the mechanism is the same one the rest of this section describes: **the parser has one
numeric frontier and the guest has two buffers**, so a `put` value cannot bound the buffer the
parser is actually in. `NOP run` and `resume at former PUT` say whether the entry was a link or a
resumed position; they no longer have to carry the diagnosis alone.


### 10.13 The label release moved to the GPU (2026-10-25)

**What changed.** `MTLGSRender::release_GCM_label` used to commit the frame and wait for its
feedback on every non-flip label, then let the shared code store the value on the CPU. It now
records the write:

- `command_recorder::write_label` copies the guest word into a per-frame-slot staging region and
  appends the guest address to a pending list. Recording a label costs a 4-byte store and an array
  append; no Metal call is made.
- `flush_label_writes` emits the whole batch as one compute encoder: one queue-scope
  `All → Blit` barrier, then one 4-byte `copyFromBuffer` per label into the guest `MTL::Buffer`
  at the guest address (the guest window is one buffer and a guest address is its offset).
  It runs at the next encoder boundary (`open_pass`, `open_compute`), before `submit`, and before
  `flush_for_readback`.
- The write is therefore ordered after everything encoded ahead of it. It is *deferred*, but a
  label is a release: the guest can only react to it, so no command after the release can depend
  on the value. Later is safe; earlier is not, and the barrier is what rules earlier out.
- `write_gcm_label` sees `handled == true` and skips its CPU store.

**Liveness.** A label only becomes visible when its command buffer is committed. Two paths force
that without waiting for the GPU:
`on_semaphore_acquire_wait` (the `nv406e` acquire spin, and FIFO idle through `cpu_wait`) sees a
pending write and calls `commit_pending_labels`, which ends and commits the buffer and leaves the
slot to be re-entered. The slot is re-entered lazily by the next `open_pass`/`open_compute`/
`write_label`/`flush_for_readback`, by which time the feedback has almost always arrived. The RSX
thread therefore never blocks on the GPU for a label; the guest's own wait is the only wait.

**Fallback.** A label outside the guest window (or a release with no frame resources yet) returns
false, and the caller takes the old path: drain, then CPU store. The old drain is now only the
fallback, not the common path.

**Resources.** One shared staging buffer of `3 × 4096 × 4` bytes. A region is reset only when its
slot has completed every commit that references it — at `begin_frame`, or at `reenter_slot` after a
waited commit — because the GPU reads the staged words at execution time. A frame that spends more
than 4096 labels commits and re-enters once, which is correct but is not expected in this game.

**What the next run decides.** `sync/label release` should stop appearing in the level-1 profile as
a 57.9 ms block and `sync/label write/gpu` should latch; `sync/label release drained` should stay at
zero unless a label is genuinely out of the window. The FIFO desync itself is a separate hole
(§10.12) and is expected to be *narrowed*, not closed, by this change: removing the drain changes
the RSX thread's pacing, which is what made the two-buffer race reachable (L129).


### 10.14 The frontier fixed in the parser itself (2026-10-25, after the second run)

The second 2026-10-25 run (`/tmp/RPCS3-run-2026-10-25-B.log`) gave the exact frontier shape. It
also corrects one number in §10.12: the buffers are at least 136 KB each, not 64 KB. The observed
spans are `0xccf9dc - 0xcb0000 = 0x1F9DC` and `0x10c1be0 - 0x10a0000 = 0x21BE0`, with the same
`0x3F0000` spacing.

**The event is a wait, not a walk.** At `0:02:36.587386`:

```
PUT move prev=0xcb2840 put=0x10a28cc parked=1 get=0xcb2840
         cache=0xcb2480+0x3c0 last_cmd=0x40068
```

The parser is parked at `A+0x2840`; the app moves `put` to `B+0x8CC`, the *other* stream. The word
at the parked position is still `0x00010000`, a non-method word that has not been written for this
pass. The old parser saw `put != addr` and read it; it should wait. This is the same state that is
healthy at `0:01:22.7`, where the word at `A+0x2840` is the link `0x210a0000` (a jump to B). So the
app sometimes publishes a mid-fill point before it writes the link, and sometimes writes the link
first.

**The second failure is the cache, not the frontier.** At `0:02:36.620748` the app moved `put` to
`A+0x28CC` in the same stream:

```
desync word=0x434c2587 pos=0xcb28cc put=0xcb28cc delta=+0 cache=0xcb2800+0xd0 next=0xffffffff
```

`pos == put`, so the `put == addr` guard in `fetch_u32` should have returned `EMPTY`. It did not,
because the word came from the cache. The fill was:

```cpp
m_cache_size = std::min<u32>((put | 0x7f) - m_cache_addr, ...) + 1;   // rounds up to a line
if (addr < put && put < m_cache_addr + m_cache_size)                   // only when addr < put
    m_cache_size = put - m_cache_addr;
```

At `addr == put` the trim never ran, so the cache held up to 31 words *past* PUT and
`fetch_u32`'s refill path — the only place the guard lives — was bypassed. That is L132. It also
explains the earlier §10.12 walk: once the parser is one word past the frontier, the cache can feed
it a stale-but-well-formed tail.

**The fix.** `FIFO_control` now owns a `stream_tracker`:

- `{base, frontier, linear}` per stream (4 entries). A `put` is attributed to the stream with the
  greatest base at or below it, within 1 MB; a `put` for a buffer not yet entered is held in a small
  ring and seeds that stream when the jump arrives. This works because the two buffers are 4 MB
  apart and each fill is local to its base.
- The boot stream is **linear**: the guest starts at `get=0x1000` and legitimately moves `put` to
  `0xcb09d4` before the parser reaches the jump at `0x154c`. Only jump targets become strict.
- `fetch_u32` computes `published_end` for the current stream (linear streams use `put`) and
  returns `FIFO_EMPTY` for `addr >= published_end`. At `addr == published_end`, one word is
  consumable **only** if it is a jump, call or return, and then as a single-word cache entry. A data
  word there is a later pass's command and is not read. The exception is measured, not assumed: at
  `0:00:23.220849` the parser reads the link at `A+0x9D4` with `cmd=0x210a0000`, and the last A PUT
  in force is exactly `0xcb09d4` (logged at `23.020203`). The app publishes a chunk by setting PUT
  to the **link's address**; every other word at PUT is data and waits.
- The cache is capped at `published_end` on every path: the initial fill, the control-word trim, the
  early-cut `bytes_read` path, and a final clamp after the fetch loop.
- A method packet is dispatched only when `header + count*4 + 4 <= published_end`; otherwise the
  parser restores `m_next_cmd_entry` to the header and returns `FIFO_EMPTY`, so a half-written
  packet is retried instead of parsed.
- `run_FIFO` feeds the jump, call, return and tail-call paths into the tracker. A call into static
  code below the buffers uses the linear rule; a call into a known stream switches to it and the
  return restores the caller. Recovery and savestate reset to a linear stream, so an unknown
  position still makes progress and the next jump recreates the strict streams.

`sync_get()` is unchanged. With the cache capped at the frontier, GET can no longer pass the
published end; the desync was the cache serving the word at PUT, not the GET value (L128 stays
`partial`).

**Evidence and tests.** Seven new cases in `rpcs3/tests/test_rsx_fifo_streams.cpp` model the boot
advance, the foreign mid-fill PUT, the stale tail, the pending-put entry, the static call/return, the
recycled-buffer rewind and the frontier-at-PUT link position. `rpcs3_test` is 511/511. The change is
built and uncommitted; the first device run is §10.15 and found one hole in the equality path.

### 10.15 Run C: the link at PUT (2026-10-25, after the third run)

The third run (`~/Library/Caches/rpcs3/RPCS3.log`, `MTL_PROBE=2`) is **inconclusive for the
desync** but decisive for one parser hole. The boot sequence is the same as run B, five seconds
later:

```
0:00:28.557308 PUT move prev=0x153c put=0xcb09d4 parked=1 ...
0:00:28.557567 jump cross-buffer from=0x154c target=0xcb0000 ...
0:00:28.631173 jump cross-buffer from=0xcb0634 target=0x0 cmd=0x2 count=2
```

Then nothing: no `FIFO error`, no `jump 0xcb09d4 -> 0x10a0000`, no further `PUT move` until the game
was closed at `0:01:09.3`. The guest was alive (Initialize Thread allocations at 30–32 s,
`title.pam` opened at 49.78 s, audio and input working), and the Metal profile presented 297 frames
with `sync/label write/gpu` 2 and `sync/label release` 0.966 ms. The black screen is the known FMV
(L12); the user quit 19 s into it.

The parser had **returned** from the `CALL 0`; run C's log had no park probe, so the first reading
assumed it stopped on the A chunk's final link at `A+0x9D4`. That address is both `put` and the
link, and the old `put == addr` guard returned `FIFO_EMPTY` before `fetch_u32` computed the
frontier, so the link exception could never run for that equality. The guard was a real hole for
the healthy link case (run B consumed `0x210a0000` there at 23.220849), and the fix stands. But it
was not the run-C stall: run D's park probe shows `word=0x0` at the same address, so the parser is
correctly waiting for a word the producer never wrote (L134, §10.16).

**The fix.** The guard is gone. `addr == put` now goes through
`published_end = frontier(put, addr)`, which is `put` for the linear boot stream and for a strict
stream whose PUT is its own; the `addr >= published_end` branch applies the link rule
(jump/call/return alone) and otherwise waits. The non-link park also sets `m_cache_size = 0`, because
the branch has already moved `m_cache_addr` to the new line and the retry address can otherwise fall
inside the old range and read a line that was never copied.

**Diagnostics.** Run C had no way to tell a parked link from a fill stall. A new level-1
`parked addr=… frontier=… put=… word=…` line records a new park; the fill loop logs `fetch wait` once
if one fill cannot commit for a second. `m_diag_parked_put` now uses `umax` as "not parked", so
address 0 is visible. These are `TODO(excise)`.

**Evidence and tests.** Seven cases in `test_rsx_fifo_streams.cpp` (the new
`FrontierAtPutIsTheLinkPosition`); `rpcs3_test` is 511/511. The change is built and uncommitted; the
next approved run must reach 3D and show `jump 0xcb09d4 -> 0x10a0000` immediately after the boot
chunk, then no `FIFO error` through the race.

### 10.16 Run D: the back-end label batch was never submitted (2026-10-25, after the fourth run)

Run D (`~/Library/Caches/rpcs3/RPCS3.log`; Metal profile `/tmp/rpcs3-mtl-probe-fifo-fix.log`)
repeated the boot shape and added the park word. The boot PUT moved to `0xcb09d4` at 30.225, the
parser entered the `CALL 0` at `0xcb0634` at 30.299, returned, and at 30.308 parked at `0xcb09d4`
with `frontier=0xcb09d4`, `put=0xcb09d4`, `word=0x0`. No further `PUT move` followed until the user
quit at 1:03.9; `title.pam` opened at 48.19 and closed at 56.63, and the Metal profile presented 137
frames with 1 draw / 3 vertices. The guest was alive; the producer was not writing.

The 3D never started because the guest was spinning on the boot chunk's back-end label. The label
write was **staged** in the open frame:

- `MTLGSRender::release_GCM_label` called `write_label` and returned true; the shared
  `write_gcm_label` then skipped its CPU store (`nv47_sync.hpp:23`).
- `write_label` only recorded the address/value in the staging ring and the pending batch
  (`MTLCommandRecorder.cpp:500`); `flush_label_writes` emitted the copies at the next encoder
  boundary, inside the current frame's command buffer.
- `commit_pending_labels` was the only path that ended and submitted that command buffer, and its
  only caller was `on_semaphore_acquire_wait` (`MTLGSRender.cpp:4644`).

The profile has the matching shape: `sync/label write/gpu` 2, `sync/label batch` 1,
`sync/label release` 2 calls / 0.805 ms total, and **zero** `sync/semaphore waits`. The guest never
performed an RSX acquire; it waited on the label from the producer side and the RSX thread parked
in `run_FIFO`/`FIFO_EMPTY`. There was no next encoder boundary and no guest wait, so the batch
stayed in the uncommitted frame and the GPU never wrote the label. The 2 latches count staging, not
visible writes; the "boot path device-confirmed" reading of run C was wrong.

**VK and GL do not stage the label into the frame.** VK's `release_GCM_label` writes the label and
the `commands_complete_event` into a **secondary command buffer and submits it immediately**
(`VKGSRender.cpp:1739-1760`); the main frame CB is not involved. Its texture-load branch uses the
current CB and calls `flush_command_queue()` when loads are unflushed, because that release *does*
gate texture data. GL writes the label and event into the host write buffer and calls
`copy_to`/`push_barrier`/`on_label_release` (`GLGSRender.cpp:1439-1459`); the `RSXDMAWriter` /
host context dispatches the copies asynchronously. In both, the label is in flight as soon as the
release is processed; neither `on_semaphore_acquire_wait` commits labels (VK:1136, GL:1328 only
handle flush requests, flips and local tasks). Their `write_gcm_label` sync does not submit the
frame.

**The fix.** `release_GCM_label` now calls `commit_pending_labels()` at a back-end
(`NV4097_BACK_END_WRITE_SEMAPHORE_RELEASE`) release, after the successful `write_label`, without
waiting for the slot feedback. This is the Metal 4 equivalent of VK's immediate submit: the
copies are flushed into the current command buffer with the queue-scope `All → Blit` barrier, the
buffer is ended and committed, and the next encoder re-enters a slot. The label is then ordered
after everything encoded ahead of it and is in flight while the guest spins. Texture-read labels
keep the staged path; their release gates texture data, and the frame's texture work is encoded
ahead of the copy.

**Tests and build.** The change is in `MTLGSRender.cpp`; `ninja rpcs3_emu rpcs3_test` is clean and
511/511; `ninja rpcs3` links. No new probe. The next approved run must show `recorder/commit
labels` at the back-end release before the park, then the producer's next `PUT move` and the
`jump cross-buffer from=0xcb09d4 target=0x10a0000`, then no `FIFO error` through the race. If the
commit is followed by a long `recorder/re-enter slot/wait`, the next design step is VK's dedicated
label command buffer instead of committing the whole frame; that cost is measured, not assumed.

### 10.17 Run K: the acquire skips the label wait (2026-10-26, after the fifth run)

Run K (`/tmp/RPCS3-run-2026-10-26-K.log`; Metal probe `/tmp/rpcs3-mtl-probe-K.log`) captured the
boot chunk tail before the producer recycled it. The tail from `0xcb05d4` ends with:

```
NV406E offset 0x10; acquire 0
NV4097 offset 0x800; BACK_END_WRITE_SEMAPHORE_RELEASE 0x00010000
NV406E offset 0x800; acquire 0
```

The boundary probe at 32.226 shows the release is **non-flip** (`type=0x75c`,
`addr=0x60300800`, `data=0x1`, `flip=0`), and the next acquire at the same address is
`arg=0x0 sema=0x0`. The acquire reads the pre-release word, takes the `sema == arg` fast path,
and never reaches `on_semaphore_acquire_wait`; the parser then parks at the genuinely unpublished
link `0xcb09d4` with `word=0x0`.

The Metal profile timestamps are offset by the emulator start; the two `sync/label release` calls
(18.7/19.96 s probe time) are the two boot releases (30.96/32.22 s shared-log time). Each stages
`value=0x1000000` and the source is correct. The release is submitted by the L134 immediate
commit; the missing piece is a wait on the **acquire** side, not another release submit.

**The fix (L135).** `semaphore_acquire` calls a new backend hook, `sync_label(address)`, when
`sema == arg`. The Metal recorder keeps the label addresses of each frame slot's command buffer.
`sync_label` finds the slot that carries the address, commits the open buffer if it is not yet
committed, and waits only for that slot's feedback; the acquire then re-reads the word and runs
its normal spin. If no label write to the address is pending, the hook returns false and the
fast path is unchanged. The per-address scan is the only new cost on a normal acquire; a label
with no waiter never pays a GPU wait. The release path still does not drain.

**Tests and build.** `ninja rpcs3_emu rpcs3_test rpcs3` is clean; `FifoStreams.*` 7/7 and the full
suite 511/511. The next run must show `sync/label wait`, a `sync/label verify` after the boot
label feedback, and then an acquire that re-reads the release; the park at `0xcb09d4` must be
followed by the next `PUT` and the link jump. If the label copy still reads `0` after feedback,
the problem is the copy destination, not the wait.

### 10.18 Run L: the wait fires, the word still reads zero (2026-10-26, after the sixth run)

Run L (`/tmp/RPCS3-run-2026-10-26-L.log`; Metal probe `/tmp/rpcs3-mtl-probe-L.log`) ran the L135
build with `MTL_PROBE=2`. The two labelled releases are at probe `14.385` and `15.548`
(shared `20.141669`/`20.145438` after the ≈4.593 s offset); both stage
`addr=0x60300800 value=0x1000000 open=1`, the source word is correct, and the batch is encoded.

The acquire now takes the new path: `sync/label wait` latched **2** times, the current slot was
committed and waited for, and `reenter_slot` ran the verify. But the verify read

```
sync/label verify addr=0x60300800 expected=0x1000000 sudo=0x0 guest=0x0
```

on both occasions. The second verify is the decisive one: it follows the boot release, so the GPU
copy had a committed command buffer and its feedback had been delivered, and the guest word is
still `0`. The parser then parks at `frontier=put=0xcb09d4` with `word=0x0` at `20.381254`, as
before. The guest itself is alive: the PAMF title FMV starts at 36 s and audio runs to ~44 s, but
the `Draw Thread` created at `18.989717` is silent until shutdown and there is no `PUT move` after
`19.961`.

The recorder timings rule out starvation: `recorder/re-enter slot/wait` max 235.7 ms,
`recorder/feedback lag` max 18.7 ms. The wait is real and short; the word is what is missing.

A standalone Metal 4 experiment (`/tmp/mtl4-label-copy.mm`) then reproduced the copy in isolation:
a placement-heap source into a 4 GiB `bytesNoCopy` `StorageModeShared` import, with and without
the barrier, lands `0x01000000` correctly. So the API mechanism is not the failure; the difference
must be in this backend's destination mapping or in the source word at GPU execution time.

**The next probe.** `flush_label_writes` now records the just-written word a second time, inside
the same command buffer: after the label copies and a `Blit → Blit` device barrier, it copies the
guest destination back into a per-slot `StorageModeShared` probe buffer. `reenter_slot` prints
`gpu=` beside `sudo=`/`guest=`, and `nv406e` prints the acquire's re-read word after the wait.
Reading `gpu=0x1000000` with `sudo=0x0` isolates the guest window alias as the fault; `gpu=0x0`
moves the fault to the destination write or the source word. The probe is diagnostic only.

### 10.19 The label destination is the wrong physical page (2026-10-26)

The run-L verify reads the application-lifetime guest import (`sudo`) and the CPU word (`guest`),
and both are `0`. The two timelines do not agree with the device's memory history: the Metal
device (and its 4 GiB no-copy import over `vm::g_sudo_addr`) is created at **4.582 s**, and the
RSX reports block that holds `0x60300800` is mapped at **18.958 s** by
`sys_rsx_context_allocate`. RPCS3 replaces guest page ranges with file-backed `MAP_FIXED`
mappings after the reservation (`util/vm_native.cpp:254`, `:858-869`).

Two standalone Metal 4 experiments measured the consequence:

| experiment | order | result |
|---|---|---|
| `/tmp/mtl4-label-remap.mm` | 4 GiB reserve → import → 64 KiB `MAP_FIXED` remap → GPU copy | CPU reads `0` |
| same | 4 GiB reserve → 64 KiB `MAP_FIXED` remap → import → GPU copy | CPU reads `0x01000000` |
| `/tmp/mtl4-label-window.mm` | 4 GiB import → remap → **second 64 KiB import over the page** → GPU copy | CPU reads `0x01000000` |

File-backed and anonymous remaps behave the same, and an overlapping sub-range import is legal.
The import is therefore fixed at creation; a page mapped later is not visible to the GPU, which
is L136. It also means the run-L fix's assumption ("if the copy reads `0`, the destination is at
fault") was right, but for a reason that no wait can repair.

**The fix (L137).** `resource_manager::label_buffer(address, offset)` keeps one 64 KiB no-copy
import per label page, created at the first release that targets it over `vm::g_sudo_addr + page`
and published to the residency set; `flush_label_writes` resolves the destination through it. The
RSX context block is allocated once at `sys_rsx_context_allocate`, so label, device and flip pages
do not move after that point; the cache holds one import per page and costs nothing per frame.
The flush, the queue barrier and the commit are unchanged, so the change is a destination fix,
not a scheduling change.

**What the confirming run must show.** `sync/label verify` should print `sudo=0x0`
(the application-lifetime import is expected to remain stale), `guest=0x1000000` (the CPU page)
and `gpu=0x1000000` (the same-commit readback). More importantly, the boot producer must move:
the `Draw Thread` should leave its silent state, the next `PUT move` must appear, and the parser
must consume the `0xcb09d4` link. With the release visible, the boot acquire (`arg=0`) will read
`1` and spin until the guest writes `0`; a TDR at that acquire would mean the guest's clear never
comes, and the acquire's role needs re-reading, not another destination change.

**Open scope.** L136 is general: any GPU access to a guest page mapped after 4.58 s sees the old
physical page. The only other current GPU access is the depth-plane load
(`MTLSurface.cpp:402`); the CPU decode (`:384`) is unaffected. That path needs the same per-page
import or a CPU copy when it is next touched; it is recorded in `METAL4-TODO.md`.




### 10.20 Run T: GET licensed the recycle, and the fix is the acknowledgement (2026-10-27)

Run T reproduced run N at the boot handover: `addr=0xcb09d4 frontier=0xcb09d4
put=0x10a0e20 word=0x30`, no redirect, no `Dead FIFO`. The new PUT history and the
chunk-tail dump show why, and it is not a lost link sample.

| time | event |
|---|---|
| 29.302805 | `PUT move prev=0x153c put=0xcb09d4`; the boot stream publishes A's end. |
| 29.302839 | the parser takes the boot link `0x20cb0000` and enters A. |
| 29.302876 | the chunk tail `cb05d4..cb09d4` ends in `0x0`: the jump at `A+0x9d4` is **not written yet**. |
| 29.490530 | the parser is still in A (`CALL 0` at `0xcb0634`, then the label release at `0x60000030`). |
| 29.490566 | the parser is at `A+0x9d4`; GET is `0xcb09d4`. |
| 29.520717 | `PUT move prev=0xcb09d4 put=0x10a0e20`; the parser parks at `A+0x9d4` with `0x30`. |
| 29.537045 | the word at the park changes `0x30 -> 0x2000`. |

The parser spent 188 ms inside A on the label acquire at `A+0x624`. During that window the guest
reused `A+0x9d4` for the next chunk's data. The licence was GET: `sync_get()` published
`m_cache_addr + m_cache_size`, the end of the prefetched block, not `m_fifo_pos`. The guest's
`cellGcmCallback` recycles a buffer only after GET has left it, so the guest had every right to
write A. The L141/L144 publish samples recorded the link at the instant PUT moved, but that only
narrows a window that is a whole chunk; the next miss would be a data word.

**The contract (L145).** GET is the consumer's acknowledgement, never a read-ahead hint:

1. the producer writes a chunk's final link and only then publishes PUT at it;
2. the consumer may consume the word at PUT only if it is a link, and GET acknowledges only the
   position the parser has reached;
3. the producer may recycle a buffer only after GET has left it.

`sync_get()` now releases `m_fifo_pos`. `cellGcmCallback` now writes the jump **before**
`ctrl.put.exchange`, so the parser can never see the published position before the link exists
(the run-T chunk tail is exactly that state: PUT at `0xcb09d4`, word `0x0`). The publish-sample
layer (`note_publish`/`take_publish`, the publish ring, `m_last_put`) is deleted; the stream
tracker keeps only `{base, frontier, linear}`, the frontier check and the call/return stack.

**The frontier is now mode-independent.** The fast path compared the position with the global PUT
only, so once PUT named the other buffer the parser could read the current stream's stale tail.
The accuracy mode chooses the copy strategy (direct read vs. the reservation-guarded line cache)
and whether a packet is dispatched atomically; it no longer chooses whether the stream boundary
is real. At `m_fifo_pos >= m_published_end` the fast path now enters the same single-word link
rule as the accurate path.

**Run U (2026-10-27) confirmed it.** With `MTL_PROBE=2`:

- the boot handover published `A+0x9d4` at `18.067752`; at `18.134687` the parser read the live
  word `0x210a0000` there and at `18.134689` entered B. Run T had parked on `0x30`;
- the run made 2,892 cross-buffer handovers through `2:23.810` with 0 `Dead FIFO`, 0 `FIFO error`,
  0 desync and 0 read-ahead;
- the 24 parks were all in the boot ring at `17.09–17.10`, before the stream started;
- two redirects (`55.122`, `1:08.269`) recovered a chunk the guest had already rewritten from base
  after GET left it. Both followed the producer's published stream; no command was lost;
- the run reached gameplay (about 1,011,128 draws and 3,000 flips) and quit normally. The visual
  defects of §1.1 are unchanged, so they are not a FIFO symptom.

The remaining performance limiter is presentation, not the FIFO: about 23 fps, `present/flip`
13.7 ms average with a 277 ms maximum, and 885 M `sync/semaphore waits`. That is the queued
presentation-decoupling work.
