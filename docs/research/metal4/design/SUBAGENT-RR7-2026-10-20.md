# Ridge Racer 7 (BLUS30001) Metal 4 failures: forensic report and fix design

| Item | Value |
|---|---|
| Document ID | SUBAGENT-RR7-2026-10-20 |
| Date | 2026-10-20 |
| Author | Forensics subagent |
| Status | Final. Read-only analysis. |
| Build | RPCS3 v0.0.43-20383-17c3c332, `metal4`, local build |
| Host | Apple A18 Pro, 8 GB, macOS 27.0.1 (26A434) |
| Renderer | Metal 4, custom config `config_BLUS30001.yml` |
| Constraints | No build, no emulator launch, no source edit, no commit, no push. No change to `.kk-mesa` or `filesdump/`. |
| Deliverable | This file only. |

**Note on the evidence.** The evidence log was replaced at 23:05 on 2026-10-20 by a new RPCS3 start with the Vulkan renderer. This task did not start that process. The crash report is retained at `~/Library/Logs/DiagnosticReports/Retired/rpcs3-2026-10-20-220517.ips`. The log line numbers and the timestamps in this report were recorded from the original log before the replacement. The screenshots are still present.

**Note on the tree.** The working tree already contained uncommitted Metal 4 changes when this analysis started. This task read that tree. This task made no source edit. The tree changed again at 23:11 on 2026-10-20. The line numbers in this report are from the tree at 23:20. Re-check the anchors after a rebase.

---

## 1. Executive summary

The title has four failures. One failure has a confirmed cause. The other three share one suspect code path.

| ID | Failure | Status of the cause | Severity |
|---|---|---|---|
| R1 | SIGABRT during car selection, in `MTLTextureDescriptor validateWithDevice` | CONFIRMED. The cube descriptor has an illegal `arrayLength`. | H |
| R2 | Black Xevious panel. Input and audio work. | HYPOTHESIS. The sampled texture is not bound, or the slot is stale. | H |
| R3 | Black race-selection background video | HYPOTHESIS. Same sample path as R2. | H |
| R4 | Black FMVs | HYPOTHESIS. Same sample path as R2. | H |

The video decoder is healthy. Run 2 decoded 1280x720 YUV420 planar frames at about 30 frames per second into three rotating guest buffers. The black image is a Metal sample-path defect, not a decode defect.

### 1.1 Top findings

| ID | Finding | Severity | Status |
|---|---|---|---|
| F1 | `make_descriptor` sets `arrayLength = 6` for `MTLTextureTypeCube`. Metal requires `arrayLength = 1`. A cube has six implicit faces. | H | CONFIRMED by probe and crash stack |
| F2 | `texture::get_view` converts a cube to a 2D view and uses `arrayLength()` as the face count. After the F1 correction, this view has one face. | H (latent) | CONFIRMED by code. Effect is HYPOTHESIS. |
| F3 | `normalize_texture_extent` forces `depth = 1`, then `make_descriptor` compares that value with the input face count (6). The "not legal" message is a false positive. | L | CONFIRMED |
| F4 | The Metal bind path returns an empty bind for every deferred subresource (`image_handle == 0`, `validate() == true`). VK and GL materialize the subresource at bind time. Metal never does. | H | CONFIRMED code gap. Link to R2/R3/R4 is HYPOTHESIS. |
| F5 | `argument_binder::bind` never writes a zero or null value. The argument table keeps the previous draw's texture in that slot. | H (code) / M (effect) | CONFIRMED code defect. Effect is HYPOTHESIS. |
| F6 | `load_texture_env` does not check `tex.enabled()`. VK and GL do. A disabled unit can be uploaded with stale registers. | M | CONFIRMED divergence. Effect is HYPOTHESIS. |
| F7 | The video refresh path is not proven. No guest write fault was logged during video. Guest faults log only at probe level 3. | M | HYPOTHESIS |
| F8 | The FIFO desync at 0:01:24 and 0:01:53 is a separate stability defect. It is not the black-video cause. | M | HYPOTHESIS |
| F9 | A deferred copy that is materialized at bind time is not recorded as a scheduler write when the descriptor is retained. | M | HYPOTHESIS |
| F10 | The abort is not a memory failure. There is no jetsam, no memory pressure, and no heap growth failure. BC3 is supported. | - | CONFIRMED (negative) |

### 1.2 Recommended order

1. B0 and B1: add the descriptor gate and correct the cube descriptor. This stops the abort.
2. B2: materialize deferred subresources in the Metal bind path. This is the primary candidate for the black images.
3. B3: make the argument table write null transitions. This removes stale images.
4. B4: add the `tex.enabled()` guard.
5. B5: measure the video write path with probes. Change it only if the measurement shows a fault gap.
6. B6: correct cube views.
7. B7: add the regression probes and tests.

The detailed design is in section 7. The test plan is in section 8. All tests are **not run**.

---

## 2. Evidence and timeline

### 2.1 Artifacts

| Artifact | Path | Use |
|---|---|---|
| Crash report | `~/Library/Logs/DiagnosticReports/Retired/rpcs3-2026-10-20-220517.ips` | Exception, thread, stack, vmSummary |
| Emulator log | `~/Library/Caches/rpcs3/RPCS3.log` (original, 214,788 lines) | Timeline, video calls, FIFO events. Replaced at 23:05. |
| Screenshot 1 | `.../screenshots/BLUS30001/BLUS30001_screenshot_2026-10-20_21_14_46.png` | Black Xevious panel |
| Screenshot 2 | `.../screenshots/BLUS30001/BLUS30001_screenshot_2026-10-20_22_03_24.png` | Full black frame during video 2 |
| Screenshot 3 | `.../screenshots/BLUS30001/BLUS30001_screenshot_2026-10-20_22_03_26.png` | Main menu with black background video |
| Probe | `/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/opencode/argtable_probe.swift` | Descriptor legality and null table writes |
| Metal 4 docs | `https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api` | Paradigm-gap evidence |

### 2.2 Crash report

| Field | Value |
|---|---|
| Process | `rpcs3`, PID 75250 |
| Launch | 2026-10-20 22:01:36.7257 +0530 |
| Capture | 2026-10-20 22:05:13.3869 +0530 |
| Exception | `EXC_CRASH`, `SIGABRT` |
| Termination | `Abort trap: 6`, by `rpcs3` |
| Faulting thread | 34, name `rsx::thread` |
| vmSummary | No jetsam. No memory pressure. |

The faulting stack is:

```
 0  libsystem_kernel.dylib   __pthread_kill
 1  libsystem_pthread.dylib  pthread_kill
 2  libsystem_c.dylib        abort
 3  libsystem_c.dylib        __assert_rtn
 4  Metal                    MTLReportFailure.cold.1
 5  Metal                    MTLReportFailure
 6  Metal                    _MTLMessageContextEndNewNSErrorOrAbort
 7  Metal                    -[MTLTextureDescriptorInternal validateWithDevice:]
 8  AGXMetalG17P             -[AGXG17FamilyDevice heapTextureSizeAndAlignWithDescriptor:]
 9  rpcs3                    mtl::resource_manager::new_texture
10  rpcs3                    mtl::texture_cache::create_new_texture
11  rpcs3                    mtl::texture_cache::upload_image_from_cpu
12  rpcs3                    rsx::texture_cache<...>::upload_texture<rsx::fragment_texture, ...>
13  rpcs3                    mtl::MTLGSRender::load_texture_env
14  rpcs3                    mtl::MTLGSRender::end
15  rpcs3                    rsx::nv4097::set_begin_end
16  rpcs3                    rsx::thread::run_FIFO
17  rpcs3                    rsx::thread::on_task
18  rpcs3                    rsx::thread::cpu_task
19  rpcs3                    cpu_thread::operator()
```

The stack shows the failure is in a **fragment texture upload**. It is not a surface, overlay, or present allocation.

### 2.3 Timeline

Log time is the time since the process start. Host time is the wall clock.

| Host time | Log time | Event | Source |
|---|---|---|---|
| 22:01:36.7 | 0:00:00 | Process start | `.ips` `procLaunch` |
| - | 0:00:09.9 | `cellVdec` imports loaded | log 1286-1290 |
| - | 0:00:48.4 to 0:00:57.3 | 14 access violations. All are reads on the RSX thread at `0xc0880000`, `0x50110000`, `0xc0000000`. | log 21550-21666 |
| - | 0:01:11.60 | `cellVdecOpen`, handle `0xf0000000` | log 21728 |
| - | 0:01:11.63 | First `cellVdecGetPicture`, 1280x720, type 3 | log 21943 |
| - | 0:01:22.24 | `cellVdecClose`, handle `0xf0000000` | log 63341 |
| - | 0:01:22.86 | `cellVdecOpen`, handle `0xf0000100` | log 63486 |
| - | 0:01:24.48 | 8x `FIFO error: possible desync event (last cmd = 0x10030303)` | log 70131-70142 |
| - | 0:01:24.51 | `Metal 4: bind_framebuffer refuses the draw` (latched once) | log 70182 |
| 22:03:24 | ≈0:01:48 | Screenshot: full black frame | screenshot 22_03_24 |
| 22:03:26 | ≈0:01:50 | Screenshot: main menu, black background video, UI text visible | screenshot 22_03_26 |
| - | 0:01:52.81 | `cellVdecClose`, handle `0xf0000100` | log 136314 |
| - | 0:01:53.84 to 0:01:59.16 | 28x `FIFO error: possible desync event (last cmd = 0x10000)` | log 136370-136425 |
| - | 0:01:59.17 | SYS dump. Run 1 stalls. | log 136426 |
| - | 0:02:11.08 | Manual stop. RSX register dump. | log 137692+ |
| - | 0:02:35.13 | Second game start | log 138194 |
| - | 0:03:09.57 | `cellVdecOpen`, handle `0xf0000000` | log 159514 |
| - | 0:03:18.56 | `cellVdecClose`, handle `0xf0000000` | log 195248 |
| - | 0:03:18.77 | `cellVdecOpen`, handle `0xf0000100` | log 195342 |
| - | 0:03:26.52 | `cellVdecClose`, handle `0xf0000100` | log 214521 |
| 22:05:12.8 | 0:03:35.279 | `Metal 4: texture descriptor 256x256x6 with 7 mips is not legal; using 256x256x1 with 7 mips (type 5, format 134)` | log 214708 |
| 22:05:13.4 | - | SIGABRT, `rsx::thread` | `.ips` capture |

The first run stopped in a FIFO desync before car selection. The second run had no FIFO desync. It reached car selection and aborted. Therefore the desync is not a precondition for the abort.

### 2.4 Video decode evidence

| Item | Value |
|---|---|
| API | `cellVdecGetPicture` and `cellVdecGetPictureExt` |
| Picture format | `formatType = 3` = `CELL_VDEC_PICFMT_YUV420_PLANAR` |
| Size | 1280x720 |
| Output buffers | `0x524adc80`, `0x525ff480`, `0x52750c80` (three rotating buffers) |
| Call count, run 2 | about 1,960 `GetPicture`, about 1,716 `GetPictureExt` |
| Rate | about 30 frames per second (first three calls at 0:01:11.633, .666, .699) |
| Decode error | None. `cellVdecGetPicItem` returned `0x80610104` (`CELL_VDEC_ERROR_EMPTY`) at clip end and cleanup only. |

The decoder writes the frame to guest memory with `sws_scale` at `rpcs3/Emu/Cell/Modules/cellVdec.cpp:1954`. The three planes start at `outBuff`, `outBuff + w*h`, and `outBuff + w*h*5/4` (`cellVdec.cpp:1945-1946`).

The video stream is healthy. The black image is downstream of the decoder.

### 2.5 Negative evidence

| Check | Result | Meaning |
|---|---|---|
| Jetsam or memory pressure | None in `.ips` `vmSummary` | The abort is not an out-of-memory event. |
| `heap growth failed` | No log line | The heap manager did not fail. |
| `supportsBCTextureCompression` | `true` on Apple A18 Pro | BC3 is not the crash cause. |
| 2D BC3, 256x256, 7 mips | Allocates. `heapTextureSizeAndAlign` returns size 87808, align 128. | The format and size are legal. |
| Metal command buffer errors | None | The failure is descriptor validation, not command submission. |
| Access violations after 0:00:57 | None in the log | No guest write fault was recorded during video. This is weak evidence because guest faults log at level 3. |
| `MTL_PROBE` output | None | The probe level was 0. The per-draw traces were off. |

### 2.6 Evidence limits

1. The log was at the default level. It has no per-draw texture trace and no probe counters.
2. Guest-thread faults use `MTL_NOTE(3, ...)` (`MTLGSRender.cpp:3514`). They were not printed.
3. The unified log entry for the Metal assertion expired before this report was written. The message is recorded from the analysis of the run.
4. The log was replaced at 23:05. The line numbers in this report refer to the original log.
5. The screenshots show the result, not the cause. They cannot separate "no bind" from "null bind" from "black source".

---

## 3. Crash root cause (car selection)

### 3.1 The failing descriptor

The log gives this descriptor:

| Field | Guest value | Metal value after `make_descriptor` |
|---|---|---|
| Width | 256 | 256 |
| Height | 256 | 256 |
| Depth / faces | 6 | 1 (depth) |
| Mipmaps | 7 | 7 |
| Texture type | Cube (5) | `MTLTextureTypeCube` |
| Pixel format | BC3 (134) | `MTLPixelFormatBC3_RGBA` |
| Array length | - | **6 (illegal)** |

The Metal message is:

```
MTLTextureDescriptor has arrayLength (6) greater than the maximum allowed size of 1.
MTLTextureDescriptor requests an arrayLength of 6, but MTLTextureTypeCube requires that arrayLength is 1
```

This is the same message that the probe reproduces. F1 is **CONFIRMED**.

### 3.2 The code path

1. `MTLGSRender::load_texture_env` calls `frame.textures.upload_texture` (`rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1421`).
2. The base cache calls `upload_image_from_cpu` and then `create_new_texture` (`MTLTextureCache.h:438-499`).
3. `create_new_texture` calls `create_image` (`MTLTextureCache.h:468`).
4. `create_image` calls `make_descriptor` (`MTLTextureCache.h:237-245`).
5. `make_descriptor` sets `MTL::TextureTypeCube` and `arrayLength = 6` (`MTLTextureCache.h:265, 270`).
6. `resource_manager::new_texture` calls `heapTextureSizeAndAlign` (`MTLResourceManager.cpp:329`).
7. Metal validates the descriptor and aborts.

### 3.3 Why the diagnostic reports "not legal"

`normalize_texture_extent` (`MTLTextureCache.cpp:16-34`) does this:

- It forces `extent.depth = 1` for every type that is not 3D (line 21).
- It keeps the input `depth` value (6) unchanged.
- `make_descriptor` compares `extent.depth` (1) with the input `depth` (6) at line 251.
- The comparison fails. The code prints "not legal" and continues.
- It then sets `arrayLength = 6` at line 270.

Two defects are in this block:

- F3: the message is a false positive. The face count is not a depth.
- F1: the illegal `arrayLength` is not corrected. The diagnostic reports a problem that it does not fix, and it hides the real problem.

### 3.4 Why the abort happens at car selection

A car preview uses a compressed cube map for reflections. This is an inference from the format and from the screen. The first use of that cube uploads it. The upload creates the descriptor and aborts. The abort is at the first upload, not at the first draw. The car-selection screen is the first screen that needs this cube in this run.

The earlier part of the run does not upload this cube. Run 1 stopped in a FIFO desync before car selection. Run 2 reached car selection.

### 3.5 The correction

The design correction is:

```cpp
// MTLTextureCache.h, make_descriptor
descriptor->setDepth(type == MTL::TextureType3D ? extent.depth : 1);
descriptor->setArrayLength(1);   // A cube has six implicit faces, not six array elements.
                                 // A 2D array would use its layer count here.
```

No upload change is necessary. `upload_texture_data` already uses `layout.layer` as the slice index for Cube and 2DArray (`MTLTextureCache.cpp:127-129`). Slices 0 to 5 are the six faces.

`normalize_texture_extent` must compare the face count separately:

```cpp
struct texture_extent
{
    u16 width;
    u16 height;
    u16 depth;
    u16 faces;
    u8 mipmaps;
};

extent.faces = (type == MTL::TextureTypeCube) ? 6 : 1;
// The caller compares:
//   depth == extent.faces   (not extent.depth)
```

Severity: **H**. Status: **CONFIRMED**.

### 3.6 The second cube defect: views

`texture::get_view` (`rpcs3/Emu/RSX/MTL/MTLHandles.cpp:12-71`) does this:

- It accepts Cube as a source type (line 21).
- It changes a cube source to `MTL::TextureType2D` (line 56-59).
- It uses `value()->arrayLength()` as the slice count (line 65).

Today this "works" only because `arrayLength` is 6. After the F1 correction, `arrayLength` is 1. A 2D view with one slice shows face 0 only. A cube sample then reads one face. The result is a wrong or black cube.

This does not cause the observed abort. It is a latent H defect.

Two designs are possible:

| Option | Design | Risk |
|---|---|---|
| A | Keep the storage as a cube. Create the remap view with `MTL::TextureTypeCube` and a slice range of 6 faces. | Small change. Depends on Metal accepting a cube view with a 6-slice range. Verify with T-6. |
| B | Store the six faces as a 2D array. Create a cube view for sampling. | Matches KosmicKrisp practice. Larger change. |

This report recommends option A for the first fix, with T-6 as the gate. If T-6 fails, use option B.

Severity: **H (latent)**. Status: **CONFIRMED by code. Effect is HYPOTHESIS.**

---

## 4. Black Xevious panel

### 4.1 Observation

The screenshot `BLUS30001_screenshot_2026-10-20_21_14_46.png` shows the Xevious menu. The portrait panel is a uniform black rectangle. The surrounding background, the menu text, and the START prompt render. The user reports that input and audio work.

A uniform black result is consistent with one of these conditions:

1. The texture unit is not bound. A null texture reads as zero. The shader outputs zero. (The zero-read behavior needs test T-11.)
2. The unit is bound to a texture whose content is black.
3. The unit is bound to an RTT that was cleared to black and never filled.

The screenshot cannot separate these conditions. The code analysis below narrows the possibilities.

### 4.2 The sample path

`MTLGSRender::load_texture_env` (`MTLGSRender.cpp:1378-1560`) does this for each referenced fragment unit:

1. It skips units that the program does not reference.
2. It uploads the texture if it is dirty or expired (`line 1419-1421`).
3. It clears `frame.fragment_textures[i]` if `descriptor->validate()` is false (`line 1441-1445`).
4. It calls `resolve_texture_bind` and stores the result (`line 1455-1457`).
5. It records a scheduler write only if the unit was resampled (`line 1482-1487`).

The vertex path is the same pattern (`line 1503-1557`).

### 4.3 F4: the deferred subresource is dropped

`resolve_texture_bind` (`MTLTextureCache.cpp:454-470`) does this:

```cpp
const auto* desc = static_cast<const sampled_image_descriptor*>(descriptor);
if (!desc->image_handle)
{
    return {};   // <-- drops every deferred subresource
}
return { desc->image_handle->resource_id(), sampler->gpuResourceID() };
```

But `sampled_image_descriptor::validate()` is true when **either** an image handle exists **or** a deferred operation is pending (`Common/texture_cache.h:570-573`). A legal sampled descriptor can have `image_handle == 0` and `external_subresource_desc.op != nop`.

The base cache produces exactly this state in two important cases:

- Any deferred copy, atlas gather, cubemap gather, 3D gather, or blit (`Common/texture_cache.h:3186-3216`). The result is returned with `image_handle == 0` and `cache_range` set.
- A mipmapped shader-read texture whose mip 0 is not a direct image. The cache builds a `mipmap_gather` deferred descriptor (`Common/texture_cache.h:3219-3294`).

VK and GL materialize the deferred resource at bind time:

- VK: `VKDraw.cpp:682-689` for fragment units, `768-773` for vertex units, and `935-942` in the descriptor update.
- GL: `GLDraw.cpp:602-605` for fragment units, `671-674` for vertex units.

The Metal backend has the complete materialization machinery:

- `create_temporary_subresource` (`Common/texture_cache.h:2381-2481`).
- `create_temporary_subresource_view` (`MTLTextureCache.h:347-358`).
- `generate_cubemap_from_images` (`MTLTextureCache.h:360-373`).
- `generate_3d_from_2d_images` (`MTLTextureCache.h:375-388`).
- `generate_atlas_from_images` (`MTLTextureCache.h:390-403`).
- `generate_2d_mipmaps_from_images` (`MTLTextureCache.h:405-418`).

The bind path never calls it. This is **F4**.

The result is deterministic: the unit is bound to nothing. With `initializeBindings = true` (`MTLCommandRecorder.cpp:98`), the slot is null on its first use. The shader reads zero. The panel is black. If the slot was bound earlier in the same table, the old image stays (F5).

Severity: **H**. Status: **CONFIRMED code gap. Link to R2/R3/R4 is HYPOTHESIS until T-5 measures the unit.**

### 4.4 F5: the argument table keeps stale slots

`argument_binder::bind` (`MTLArgumentBinds.cpp:26-130`) writes a slot only when the value is non-zero and changed:

- Address: `if (value != 0 && ...)` (`line 43-50`).
- Texture or stream: `if (value._impl != 0 && ...)` (`line 52-59`).
- Unit: the same test for the texture and the sampler (`line 68-80`).

The table is created once per frame slot with `initializeBindings = true` (`MTLCommandRecorder.cpp:93-103`). The same table object is reused for the life of that slot. `binder.reset()` is called at frame open (`MTLGSRender.cpp:792`), but it only clears the CPU-side shadow (`MTLArgumentBinds.cpp:18-24`). It does not clear the Metal table.

A zero value is never written. Therefore:

- A unit that was bound in draw N and is not bound in draw N+1 keeps the draw-N texture.
- A slot that was bound in the previous frame keeps the previous frame's texture, because the table is per slot and the binder reset does not clear it.
- A unit that should be null after a transition stays on the old image.

The probe confirms that `setTexture(null)` and `setSamplerState(null)` are legal, and that a real ID can be overwritten with null. The fix is available.

Severity: **H for the code defect, M for the observed effect**. Status: **CONFIRMED code. Effect is HYPOTHESIS.**

### 4.5 F6: the disabled unit is not guarded

VK and GL clear the sampler state when the guest disables a unit:

- VK: `VKDraw.cpp:330-334`.
- GL: `GLDraw.cpp:365-369`.

The Metal loop has no such check (`MTLGSRender.cpp:1400-1420`). It calls `upload_texture` with the current registers. If the unit is disabled but the width, height, and address are stale, the upload can produce a valid image from stale guest data, or a deferred descriptor. Either result is wrong for a disabled unit.

The effect combines with F5: the wrong image, or nothing, stays in the table.

Severity: **M**. Status: **CONFIRMED divergence. Effect is HYPOTHESIS.**

### 4.6 Assessment for the Xevious panel

| Candidate cause | Evidence for | Evidence against | Verdict |
|---|---|---|---|
| F4: deferred subresource dropped | The panel is flat black. The bind path drops deferred descriptors. | The panel may be a direct 2D upload, which would not be deferred. | Primary hypothesis. |
| F5: stale slot | The table is persistent and never nulled. | A stale image would usually show wrong content, not uniform black. | Secondary hypothesis. |
| F6: disabled unit | The path has no guard. | The panel has working input, so the unit may be enabled. | Contributing hypothesis. |
| Black source data | The panel is a static image. A decode fault does not apply. | No evidence of a black source. | Low. |
| RTT cleared to black | The panel could be an RTT. | No evidence of an RTT path in the log. | Low. |

The recommended test is T-5. It must print, for the panel unit, the guest address, the `validate()` result, the `image_handle`, and the bound `MTLResourceID`.

---

## 5. Black video and FMV

### 5.1 The decoder is healthy

Section 2.4 gives the evidence. The decoder produced 1280x720 YUV420 planar frames. The picture calls returned no logged error, and the game continued to use the frames. `cellVdecGetPicItem` returned `CELL_VDEC_ERROR_EMPTY` only at clip end and cleanup. The frames went to three rotating guest buffers.

The black image is not a decoder failure.

### 5.2 The sample path is the same path

The game samples the video frame as a texture. That path is the same `load_texture_env` path as the panel. F4, F5, and F6 apply without change.

The video adds one requirement: the texture content changes at 30 frames per second. The cache must know that the guest wrote a new frame. The mechanisms are:

- The guest write faults on the protected range. `create_new_texture` protects the range read-only (`MTLTextureCache.h:485-490`).
- `MTLGSRender::on_access_violation` handles the fault (`MTLGSRender.cpp:3487-3643`).
- `MTLGSRender::write_barrier` handles an explicit barrier (`MTLGSRender.cpp:3453-3484`).
- `m_textures_dirty[i]` forces a re-upload at `MTLGSRender.cpp:1415-1421`.
- `test_if_descriptor_expired` handles a surface that changed (`MTLGSRender.cpp:1419`).

### 5.3 F7: the refresh path is not proven

The original log has 14 access violations. All are at 0:00:48 to 0:00:57, before the first video open. There is no logged fault during video playback.

This is not proof that the refresh path is broken. Guest-thread faults are logged only at probe level 3 (`MTLGSRender.cpp:3514`). The level was 0.

There are two possibilities:

1. The faults happened and were handled, but they were not printed. The video refresh works. The black image is F4/F5 only.
2. The faults did not happen. The video range was not protected, or the guest write did not hit the protected page. The cache kept frame 0. If frame 0 is black, every frame is black.

The log cannot separate them. T-9 must measure the `fault/on guest thread` counter and the per-event trace at level 3.

Severity: **M**. Status: **HYPOTHESIS**.

### 5.4 F8: the FIFO desync

The first run has two desync groups:

- 8 events at 0:01:24.48 with `last cmd = 0x10030303`, about 1.6 s after video 2 opens and immediately before `bind_framebuffer refuses the draw`.
- 28 events at 0:01:53.84 to 0:01:59.16 with `last cmd = 0x10000`, immediately after video 2 closes. The run stalls.

The second run has no desync. It reaches car selection and aborts at the cube upload. Therefore the desync is not the black-video cause and not the abort cause.

It is still a stability defect. It correlates with the video open and close. It is out of scope for the four failures, but it is a risk for long video playback.

Severity: **M**. Status: **HYPOTHESIS**.

### 5.5 F9: the deferred copy barrier

If B2 materializes a deferred resource in `load_texture_env`, the copy is encoded in a compute pass before the render pass. `record_pass_hazards` must know that the copy wrote the temporary image. The current `note_write` call requires `resampled == true` (`MTLGSRender.cpp:1482-1487`). A deferred descriptor that is **retained** (not dirty) would materialize without a write record. The pass scheduler would not emit the barrier.

This is a design risk for the B2 fix, not an observed failure yet. B2 must set a `deferred` flag and call `note_write` for every deferred bind.

Severity: **M**. Status: **HYPOTHESIS**.

### 5.6 Assessment for the video and FMV

The decoder works. The black image is a sample-path defect. F4 is the primary candidate because the bind path drops a legal deferred descriptor for every deferred operation. F5 is the secondary candidate because the table keeps old images. F7 is a measurement gap, not a proven cause.

The screenshots support this order:

- `22_03_24` is a full black frame.
- `22_03_26` is the main menu. The UI text and the logo render. The background video is black. A stale glyph atlas would show glyphs, not black. A null bind shows black.
- `21_14_46` shows the Xevious panel as a flat black rectangle. This also favors a null bind over a stale image.

Therefore F4 is the leading cause. F5 is still a real defect and must be fixed, because it can turn a null bind into a wrong image on other screens.

---

## 6. Metal 4 paradigm gaps

Metal 4 changed the binding, copy, and synchronization model. The backend still uses assumptions from Metal 3 and from the Vulkan backend. The table lists the gaps that matter for these failures.

| ID | Metal 4 rule | Backend assumption or state | Consequence | Reference | Severity / status |
|---|---|---|---|---|---|
| G1 | Encoders do not bind resources one by one. An argument table holds the bindings. The table is a reusable object. The encoder looks up the resources from the table. | The binder writes only changed, non-zero slots. It assumes the table is snapshotted at encoder bind and that an unused slot is safe. | A null transition is never written. Old slots persist across draws and frames. | `MTLArgumentBinds.h:174-197`, `MTLArgumentBinds.cpp:43-80`, `MTLGSRender.cpp:2664-2678` | H / CONFIRMED code, effect HYPOTHESIS |
| G2 | There is no blit encoder. A compute encoder combines copy, compute, and acceleration-structure work. A copy is a pass with explicit barriers. | The backend has all deferred-copy helpers but never calls them from the bind path. VK and GL call them per draw. | A deferred texture is never materialized. The unit binds nothing. | `MTLTextureCache.cpp:454-470`, `Common/texture_cache.h:2381-2481`, `VKDraw.cpp:682-689` | H / CONFIRMED code gap |
| G3 | All resources are untracked. The app must synchronize every producer and consumer. | The pass scheduler records only the writes the caller reports. A retained deferred copy reports no write. | A copy can race the fragment read. | `MTLPassScheduler.cpp:49-107`, `MTLGSRender.cpp:1479-1487` | M / HYPOTHESIS |
| G4 | Texture type and array length are independent. A cube has six implicit faces and `arrayLength == 1`. | The backend maps the RSX face count to `arrayLength`. | The descriptor is illegal. Metal aborts. | `MTLTextureCache.h:247-275` | H / CONFIRMED |
| G5 | Store-action options are not available. Resolve and copy are explicit commands. | The surface path already uses explicit resolve and copy. This is not a cause of the four failures. | Latent risk for surface paths. | `MTLSurface.cpp:414-472` | L / CONFIRMED design |
| G6 | A command buffer does not retain resources. The app must keep resources alive until the GPU finishes. | The resource manager defers release per slot. This is implemented. Not a cause here. | Latent use-after-free risk if a slot is abandoned. | `MTLCommandRecorder.cpp:179-205`, `MTLResourceManager.cpp:320-347` | M / HYPOTHESIS |
| G7 | A texture view pool creates lightweight views with their own resource IDs. | The backend uses `newTextureView` and forces a 2D view for cubes. | With the legal array length, a cube view exposes one face. | `MTLHandles.cpp:48-66` | H (latent) / CONFIRMED code |

### 6.1 G1 in detail: the argument table is not a per-bind snapshot

Apple documents the argument table as an object that the app creates, configures, and reuses: "you can create a single argument table that stores bindings to resources that apply to multiple encoders, and then reuse that argument table indefinitely." Apple also says: "the encoder looks up the resources that the method needs from the encoder's argument table."

The backend's comment says: "Metal 4 snapshots the argument table when the encoder binds it" (`MTLArgumentBinds.h:191-196`, `MTLGSRender.cpp:2664-2667`). Apple does not state this. The Hello Triangle sample reuses one table and modifies it between commands. The safe contract is:

- The table is live data.
- A slot that the current program reads must hold the current value or null.
- A slot that the current program does not read may hold anything, but the binder cannot know which slots the program reads if it only tracks values.

The backend's own contract already says "A zero value means 'not bound this draw'" (`MTLArgumentBinds.h:133-135`). The writer must honor that contract. It does not.

This is the core of F5.

### 6.2 G2 in detail: the copy is a pass, not a helper call

In Metal 3, a blit encoder was a separate object and a copy was cheap. In Metal 4, a copy is encoded in a compute pass. The backend correctly added `open_compute()` in `upload_texture_data` (`MTLTextureCache.cpp:50`). It did not add the same call in the bind path.

The VK and GL backends show the expected shape:

```cpp
// VK, fragment unit
if (tex.enabled() && sampler_state->validate())
{
    if (view = sampler_state->image_handle; !view)
        view = m_texture_cache.create_temporary_subresource(cmd, sampler_state->external_subresource_desc);
}
// if (!view) bind a null view
```

The Metal equivalent is:

```cpp
texture* image = descriptor->image_handle;
if (!image && descriptor->external_subresource_desc.op != deferred_request_command::nop)
    image = frame.textures.create_temporary_subresource(frame.recorder, descriptor->external_subresource_desc);
if (!image)
    frame.fragment_textures[i] = {};
else
    frame.fragment_textures[i] = { image->resource_id(), sampler->gpuResourceID() };
```

The call must run **every draw** for dynamic copies. The base cache updates the contents of a cached temporary image for `copy_image_dynamic` (`Common/texture_cache.h:2392-2393`). Do not cache the image handle in the descriptor for dynamic copies. Let the cache decide. This is exactly what VK does.

### 6.3 G4 in detail: the RSX cube is not a Metal array

The RSX cube has six faces. The guest stores the faces in a contiguous mip chain. The Metal cube type also has six faces. The two models match directly. The face count is not an array length.

The backend has two independent mappings to the same wrong field:

- `make_descriptor` sets `arrayLength = 6` (`MTLTextureCache.h:270`).
- `normalize_texture_extent` treats `depth` as 6 and logs it as a mismatch (`MTLTextureCache.cpp:21`; comparison at `MTLTextureCache.h:251`).

Both must change together. If only one changes, the descriptor becomes legal but the diagnostic still lies, or the diagnostic becomes correct but the descriptor still aborts.

### 6.4 Comparison with KosmicKrisp

KosmicKrisp (KK) is a Vulkan driver on Metal. It is a secondary reference, not a specification. Three practices differ from the RPCS3 Metal backend.

1. **Texture storage and cube views.** KK sets `descriptor.arrayLength = layout->layers` (`.kk-mesa/src/kosmickrisp/bridge/mtl_device.m:250-257`). A Vulkan cube is a 2D array with six layers. KK creates the cube as a **view**: `newTextureViewWithPixelFormat:textureType:levels:slices:` with `slices = NSMakeRange(base_array_layer, array_len)` (`.kk-mesa/src/kosmickrisp/bridge/mtl_texture.m:76-88`). RPCS3 stores a cube with `arrayLength = 6` and changes it to a 2D view. The KK model is option B in section 3.6. It avoids the array-length rule by construction.
2. **Argument table scope.** KK uses one argument table per command buffer. The root descriptor is a buffer address at index 0. Samplers are at index 1. Per-draw data is at index 2 (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_buffer.c:133-139`). It rewrites the root address for each bind (`.kk-mesa/src/kosmickrisp/vulkan/kk_cmd_buffer.c:387-391`). It does not keep a per-slot shadow of individual textures. The RPCS3 per-draw texture table is a different design. The null contract in B3 is still required.
3. **No per-bind snapshot.** KK writes the table before the dispatch and reuses it. It does not rely on a snapshot at bind. This supports the G1 conclusion.

These differences do not prove the RPCS3 failures. They show that the two defects (cube storage and table lifetime) have known solutions.

---

## 7. Prioritized fix batches

The batches are in execution order. B0 and B1 must land first, because they stop a crash. B2 and B3 can land together. B4 is small and independent. B5 is a measurement batch. B6 is the cube-view follow-up. B7 is the regression work.

### B0: Add a descriptor legality gate (P0, size S)

**Goal.** Make an illegal descriptor impossible to send to Metal.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLTextureCache.h:247-275`
- `rpcs3/Emu/RSX/MTL/MTLTextureCache.cpp:16-34`

**Design.**

1. Add a single function `legalize_extent(type, width, height, depth, mipmaps)` that returns the extent and the face count.
2. Assert these rules in debug builds:
   - `arrayLength == 1` for 1D, 2D, Cube, and 3D.
   - A 2D array uses its layer count as the array length.
   - `depth == 1` for 1D, 2D, Cube, and 2DArray.
   - `depth >= 1` for 3D.
   - `mipmaps <= floor_log2(max(width, height, depth)) + 1`.
3. Log one bounded error when a guest value is clamped. Report the guest value and the clamped value in the same line.
4. Add a host-side table test (T-1) that calls the function for every type and a set of sizes.

**Acceptance.** The test passes. A cube with 7 mips and 256x256 produces `arrayLength == 1`. No `validateWithDevice` abort.

### B1: Correct the cube descriptor (P0, size S)

**Goal.** Stop the car-selection abort.

**Files.**

- `MTLTextureCache.h:270` (array length)
- `MTLTextureCache.cpp:16-34` (face count)
- `MTLTextureCache.h:251` (comparison)

**Design.**

```cpp
// make_descriptor
descriptor->setDepth(type == MTL::TextureType3D ? extent.depth : 1);
descriptor->setArrayLength(1);

// normalize_texture_extent
extent.faces = (type == MTL::TextureTypeCube) ? 6 : 1;

// make_descriptor diagnostic
const bool faces_match = (depth == extent.faces);
if (extent.width != width || extent.height != height ||
    !faces_match || extent.mipmaps != mipmaps)
{
    // bounded log
}
```

**Acceptance.**

- The 256x256x6 BC3 cube uploads without an abort.
- The log no longer prints "not legal" for a legal cube.
- The car-selection screen renders the car preview.
- T-1 and T-2 pass.

**Dependency.** B0.

### B2: Materialize deferred subresources at bind time (P0/P1, size M)

**Goal.** Bind the image that the guest sampler describes, including deferred copies and gathers.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLTextureCache.cpp:454-470`
- `rpcs3/Emu/RSX/MTL/MTLTextureCache.h:631` (declaration)
- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:1421, 1455-1457, 1523, 1539-1541`

**Design.**

1. Change the signature to accept the command recorder and a non-const descriptor:

```cpp
texture_bind resolve_texture_bind(command_recorder& cmd,
                                 sampled_image_descriptor* descriptor,
                                 MTL::SamplerState* sampler);
```

2. Implement the VK shape:

```cpp
if (!descriptor->image_handle)
{
    if (descriptor->external_subresource_desc.op == deferred_request_command::nop)
        return {};
    texture* image = create_temporary_subresource(cmd, descriptor->external_subresource_desc);
    if (!image)
        return {};
    return { image->resource_id(), sampler->gpuResourceID() };
}
return { descriptor->image_handle->resource_id(), sampler->gpuResourceID() };
```

3. Do **not** store the temporary image in `desc->image_handle` for `copy_image_dynamic`. The cache must update the contents on every draw.
4. Add a `bool deferred` out-parameter or a small result struct. In `load_texture_env`, call `frame.scheduler.note_write` with the bound resource ID and `access_stages(rsx::surface_access::transfer_write, false)` when `deferred` is true, even if the unit was not resampled. This matches the existing call at `MTLGSRender.cpp:1485-1486`.
5. Pass `frame.recorder` at both call sites (fragment and vertex).
6. Keep the `!descriptor || !sampler` guard.
7. If `create_temporary_subresource` returns null because the source is missing, log one bounded warning and bind null.

**Why this is safe.** `upload_texture_data` already opens a compute encoder (`MTLTextureCache.cpp:50`). The bind path runs before `open_pass` (`MTLGSRender.cpp:2008`). The compute copy is therefore encoded before the render pass. The scheduler already carries a queue-form barrier for a cross-pass producer (`MTLPassScheduler.cpp:49-107`).

**Acceptance.**

- T-3 passes for `copy_image_static`, `copy_image_dynamic`, `mipmap_gather`, `atlas_gather`, and `cubemap_gather`.
- The panel and the menu background render in T-5 and T-6.
- No new Metal validation error.

**Dependency.** B1, because `cubemap_gather` creates a cube through `make_descriptor`.

### B3: Write null transitions in the argument table (P0/P1, size S)

**Goal.** A slot that is not bound in this draw must not keep an old value.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLArgumentBinds.cpp:40-80`
- `rpcs3/Emu/RSX/MTL/MTLArgumentBinds.h:174-197`

**Design.**

1. Remove the `value != 0` guards. Write when `first_bind || value != old`.
2. Keep the sampler write inside `slot_has_sampler`.
3. `first_bind` writes **all** slots, including zero and null. This clears cross-frame state after `binder.reset()`.
4. Update the header comment. A zero value is a binding. It is not "nothing to write".
5. Optional debug mode: write every slot on every bind. Use it in the tests to prove that the shadow state equals the table state.
6. Verify that `setAddress(0, index)` is legal with T-12. The probe already proves that `setTexture(null)` and `setSamplerState(null)` are legal.

**Acceptance.**

- T-4 passes: bind A, then null, then B. Every slot is written in order.
- A frame slot that is reused after `binder.reset()` starts with null slots, not the previous frame's images.

**Dependency.** None.

### B4: Add the disabled-unit guard (P1, size S)

**Goal.** Match VK and GL behavior for a disabled texture unit.

**Files.**

- `MTLGSRender.cpp:1400-1420` (fragment)
- `MTLGSRender.cpp:1503-1524` (vertex)

**Design.**

```cpp
if (!tex.enabled())
{
    frame.fragment_textures[i] = {};
    frame.fragment_stencil[i] = {};
    continue;
}
```

Do the same for the vertex loop. The table slot is cleared by B3.

**Acceptance.**

- T-7 passes: disable a unit between two draws. The second draw samples null, not the previous texture.
- No stale image appears on a menu transition.

**Dependency.** B3, for the slot clear.

### B5: Measure and fix the video refresh (P1, size M, conditional)

**Goal.** Prove that the cache sees each new video frame.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:3487-3643` (fault handling)
- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp:3453-3484` (`write_barrier`)
- `rpcs3/Emu/Cell/Modules/cellVdec.cpp:1954` (only if a fix is needed)

**Design.**

1. Do not change the write path first. Measure it.
2. Run with `MTL_PROBE=3`. Count `fault/on guest thread`, `fault/on RSX thread`, and `texture/resolved` during video playback.
3. If the guest-fault count is zero and the video address is protected, add one probe latch at `cellVdecGetPictureExt` that confirms the output range. Then send the existing invalidation notification for that range, through the same path that a guest write uses.
4. If the count is non-zero, the refresh is healthy. Close F7 and keep B2 as the only video fix.
5. Do not add a blind `mm_flush` in the HLE module. It would hide the real cache behavior and add a full flush every frame.

**Acceptance.**

- The probe report shows a non-zero guest-fault count, or the explicit invalidation path is justified by the measurement.
- Consecutive video frames differ in the sampled image dump.

**Dependency.** B2.

### B6: Correct cube views (P2, size S/M)

**Goal.** Keep a cube a cube when a component remap needs a view.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLHandles.cpp:48-66`

**Design (option A).**

```cpp
MTL::TextureType type = source_type;
if (sample_count() > 1)
    type = MTL::TextureType2DMultisample;

const NS::UInteger slices =
    (source_type == MTL::TextureTypeCube) ? 6u : value()->arrayLength();

view->assign(value()->newTextureView(pixel_format(), type,
    NS::Range::Make(0, value()->mipmapLevelCount()),
    NS::Range::Make(0, slices),
    channels));
```

Do not force a cube to 2D.

**Design (option B).** Store six faces in a 2D array with `arrayLength = 6`. Create a cube view for sampling. Use this option if T-6 shows that option A is not legal.

**Acceptance.**

- T-6 passes for a cube with a non-identity remap.
- A cube sample reads all six faces.

**Dependency.** B1.

### B7: Add the regression probes and tests (P2, size M)

**Goal.** Make the four failures observable and repeatable.

**Files.**

- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp` (probe latches)
- `rpcs3/Emu/RSX/MTL/MTLTextureCache.cpp` (bind counters)
- A new host test target for the Swift probes and the unit tests.

**Design.**

1. Add these latches:
   - `texture/bind deferred` (a deferred descriptor was resolved).
   - `texture/bind dropped` (a descriptor was valid but produced no image).
   - `texture/bind null` (the table slot was written as null).
   - `texture/disabled` (a referenced unit was disabled).
2. Raise the per-unit trace cap from 16 to a probe-controlled count. Keep the default at 16 to avoid log floods.
3. Print the guest address, the `validate()` result, the `image_handle`, and the bound `MTLResourceID` in the trace.
4. Add the tests in section 8 to the host test suite. The RR7 tests stay manual.

**Acceptance.** The probe report shows the four counters for the four scenarios. The counters are zero in a healthy run.

**Dependency.** B2, B3, B4.

---

## 8. Test plan (not run)

All tests are **not run**. They are listed for the implementation phase.

| ID | Test | Preconditions | Steps | Pass criteria |
|---|---|---|---|---|
| T-1 | Descriptor legality table | Host Mac, Metal 4 device | Call `make_descriptor` for 1D, 2D, Cube, 2DArray, 3D. Sizes: 1, 2, 256, 4096. Mips: 1, 7, 9, 13. | Every descriptor validates. Cube has `arrayLength == 1`. 3D keeps `depth > 1`. |
| T-2 | Cube heap sizing | Host Mac, Metal 4 device | `heapTextureSizeAndAlign` for a 256x256 BC3 cube with 7 mips and with 9 mips. | Both succeed. Sizes are non-zero. |
| T-3 | Deferred bind resolver | Host test harness, mock recorder | Build a descriptor with `image_handle == 0` and each deferred op. Call the new `resolve_texture_bind`. | Every op returns a non-null bind. Dynamic copy updates the contents on the second call. |
| T-4 | Argument table null contract | Host Mac, Metal 4 device | Create a table. Bind A, then null, then B. Read back with a compute shader. | The shader sees A, then 0, then B. |
| T-5 | RR7 panel and menu | Metal 4 build, BLUS30001, `MTL_PROBE=3` | Boot, open the Xevious panel, return to the menu. Take one screenshot per state. | The panel and the menu background are not black. The probe shows a non-zero `texture/bind` for the panel unit. |
| T-6 | Cube view | Host Mac, Metal 4 device | Create a cube. Create a view with a non-identity remap. Sample all six faces. | All six faces return the expected colors. |
| T-7 | Disabled unit | Host test harness | Draw with unit 0 bound. Disable unit 0. Draw again. | The second draw samples null. No old image appears. |
| T-8 | RR7 FMV | Metal 4 build, BLUS30001, `MTL_PROBE=3` | Play the intro FMV and the race-selection background. Capture 10 frames. | The frames are not black. The video unit is bound. `texture/bind deferred` is zero or handled. |
| T-9 | Video refresh | Same as T-8 | Count `fault/on guest thread` and `texture/resolved` during 10 s of video. Dump the sampled image on frames 1 and 30. | The counter is non-zero, or the explicit invalidation path is active. The two dumps differ. |
| T-10 | Car selection soak | Metal 4 build, BLUS30001 | Enter car selection. Change cars for 20 minutes. | No `validateWithDevice` abort. No "not legal" log for a cube. No `heap growth failed`. |
| T-11 | Null texture read | Host Mac, Metal 4 device | Bind no texture to a sampled slot. Read it in a fragment shader. | The result is zero. This confirms the black-panel mechanism. |
| T-12 | Null buffer address | Host Mac, Metal 4 device | Call `setAddress(0, index)`. Draw with a shader that reads the buffer. | No validation error. The shader reads zero. |

### 8.1 Test environment

- Build: the Metal 4 build from B0 to B7.
- Environment: `MTL_PROBE=3`, `MTL_DEBUG_LOGS=1`, `MTL_PERF_ENABLED=1`.
- Save the log after each run. The log is replaced at each start.
- Keep the crash reports. macOS moves them to `Retired/` after a short time.
- Use one emulator instance. The repository rule is one instance.

### 8.2 Test order

1. T-1, T-2, T-4, T-6, T-11, T-12 on the host. These do not use the emulator.
2. T-3 and T-7 in the host test harness.
3. T-5, T-8, T-9, T-10 on the title. Use the same build and the same custom config.

### 8.3 Regression gate

The fix is complete when:

- T-1 to T-4, T-6 to T-7, T-11, and T-12 pass.
- T-5, T-8, and T-10 pass on three consecutive runs.
- T-9 gives a measured answer, not an assumption.
- The probe counters show zero `texture/bind dropped` for the four scenarios.

---

## 9. Open questions

1. **Is the video unit descriptor deferred or direct?** T-5 and T-8 must print the guest address, the `validate()` result, the `image_handle`, and the deferred op. This question separates F4 from a source-content problem.
2. **Is the Xevious panel a static texture, an RTT, or an FMV?** The screenshot alone cannot decide. A capture with the texture unit trace will decide.
3. **Why were there no guest write faults during video?** The log level hid them. T-9 must measure. If the count is zero, the protection or the invalidation path is defective.
4. **Is the "snapshot at bind" assumption true on this device?** Apple does not document it. T-4 can test it with two draws that share one encoder and one table, with a table change between them.
5. **Is `setAddress(0, index)` legal and does it read as zero?** T-12.
6. **Is a cube view with a six-slice range legal?** T-6. If not, use option B from B6.
7. **Is the FIFO desync at 0:01:53 related to the video close?** It is a separate defect. It needs its own trace. It does not block the four fixes.
8. **Was the 0:03:35 cube the first cube upload of the run?** The log shows the first "not legal" line at the abort. A probe counter will confirm. This is likely, because the car preview is the first cube user.
9. **Does the same black image occur on Vulkan/KosmicKrisp?** This is a cross-backend check. It is not run. If Vulkan is correct, the defect is in the Metal backend, not in the shared cache.
10. **Is the probe overhead acceptable for a 20-minute soak?** Level 3 prints per draw. Use the bounded prefix and the counters, not full traces, for T-10.

---

## Appendix A: Probe results

Probe file: `/var/folders/0x/tq2_tw6d7_lckb2kqwt23klc0000gn/T/opencode/argtable_probe.swift`

Recorded output:

```
device=Apple A18 Pro
table ok
setTexture(null) survived
setSamplerState(null) survived
realID impl=1
overwrite with null survived
```

The probe also reproduced the abort:

```
-[MTLTextureDescriptorInternal validateWithDevice:]:1421: failed assertion `Texture Descriptor Validation
MTLTextureDescriptor has arrayLength (6) greater than the maximum allowed size of 1.
MTLTextureDescriptor requests an arrayLength of 6, but MTLTextureTypeCube requires that arrayLength is 1
```

Earlier probe results from the same device:

| Test | Result |
|---|---|
| Cube, 256x256, `arrayLength = 1`, 7 mips | Legal |
| Cube, 256x256, `arrayLength = 1`, 9 mips | Legal |
| Cube, 256x256, `arrayLength = 1`, 10 mips | Abort. Separate mip-count message. |
| Cube, 256x256, `arrayLength = 6` | Abort. Array-length message. |
| 2D BC3, 256x256, 7 mips | Legal. Size 87808, align 128. |
| `supportsBCTextureCompression` | `true` |

The probe proves three facts:

1. F1 is the exact crash.
2. A null texture and a null sampler write are legal.
3. BC3 is not the cause.

The probe does not prove that a null texture **reads** as zero. T-11 must prove that.

---

## Appendix B: Line reference index

| Reference | Subject |
|---|---|
| `MTLTextureCache.h:237-245` | `create_image` |
| `MTLTextureCache.h:247-275` | `make_descriptor`; `arrayLength` at 270 |
| `MTLTextureCache.h:438-499` | `create_new_texture`; protection at 485-490 |
| `MTLTextureCache.h:347-418` | Temporary-subresource overrides |
| `MTLTextureCache.cpp:16-34` | `normalize_texture_extent` |
| `MTLTextureCache.cpp:50` | Compute encoder for upload |
| `MTLTextureCache.cpp:127-129` | Cube slice selection |
| `MTLTextureCache.cpp:295-355` | `create_temporary_subresource_impl` |
| `MTLTextureCache.cpp:454-470` | `resolve_texture_bind` |
| `MTLArgumentBinds.cpp:26-130` | `argument_binder::bind` |
| `MTLArgumentBinds.h:93-102` | Table counts |
| `MTLArgumentBinds.h:174-197` | Binder contract |
| `MTLCommandRecorder.cpp:93-103` | Table creation, `initializeBindings` |
| `MTLGSRender.cpp:792` | `binder.reset()` per frame |
| `MTLGSRender.cpp:1378-1560` | `load_texture_env` |
| `MTLGSRender.cpp:1441-1445` | Validate-fail clear |
| `MTLGSRender.cpp:1479-1487` | Scheduler write for a resampled unit |
| `MTLGSRender.cpp:2008` | `open_pass` after texture load |
| `MTLGSRender.cpp:2664-2678` | Per-draw table bind |
| `MTLGSRender.cpp:3487-3643` | Access-violation handling |
| `MTLGSRender.cpp:3453-3484` | `write_barrier` |
| `MTLHandles.cpp:12-71` | Texture views; cube to 2D at 56-66 |
| `MTLPassScheduler.cpp:49-107` | Read/write barrier records |
| `MTLResourceManager.cpp:320-347` | `new_texture` |
| `Common/texture_cache.h:570-573` | `validate()` |
| `Common/texture_cache.h:2381-2481` | `create_temporary_subresource` |
| `Common/texture_cache.h:3164-3171` | Zero-dimension rejection |
| `VKDraw.cpp:330-340` | Disabled-unit guard |
| `VKDraw.cpp:682-689` | Deferred materialization, fragment |
| `VKDraw.cpp:768-773` | Deferred materialization, vertex |
| `GLDraw.cpp:365-369` | Disabled-unit guard |
| `GLDraw.cpp:602-605` | Deferred materialization, fragment |
| `GLDraw.cpp:671-674` | Deferred materialization, vertex |
| `cellVdec.cpp:1945-1954` | YUV420 plane layout and `sws_scale` |

---

## Appendix C: Terms

| Term | Meaning |
|---|---|
| Argument table | The Metal 4 object that holds buffer, texture, and sampler bindings for an encoder. |
| Bind | The act of making a texture and sampler available to a shader unit. |
| Deferred subresource | A texture that the cache cannot return as a direct image. It needs a copy, gather, or unwrap before use. |
| Draw | One guest drawing command. |
| Face | One side of a cube texture. A cube has six faces. |
| Guest | The emulated PlayStation 3. |
| Residency | The property that a resource is in GPU-accessible memory. |
| Slot | One entry in an argument table. |
| RTT | Render-to-texture. The guest renders into a surface and samples it later. |
| Unit | One guest texture unit. RR7 uses several units per draw. |

<!-- doc-end -->
