# Handoff — the RSX to Metal 4 backend

This document hands over the Metal 4 backend project at its current state. It
records what exists, what was measured, what is missing, and what the next worker
has to do. Read it first. Then read `REALISATION.md` for the per-cluster status,
`REVIEW-FINDINGS.md` for the itemised defects and their evidence, and
`CONFLICTS.md` for the places where a design document and the RSX tree disagree.

This revision was rewritten around the pending work. Superseded narrative was
removed: the per-dial prose, the closed-finding stories, the planning estimates
that no longer govern, and the probe history that the measurement table now
carries. Section 7 holds the work and is the reason to read the document.

The document names modules by their role, not by their path, so it stays useful
when the tree moves. The registers and the measurement table carry exact
references.

---

## 1. The backend in one page

### 1.1 What it is

RPCS3 emulates the RSX, the PlayStation 3 GPU, with a host GPU backend. Two
backends ship: Vulkan and OpenGL. On Apple silicon Vulkan reaches the GPU through
a translator: MoltenVK translates Vulkan to Metal, and KosmicKrisp is a Vulkan
driver whose own backend is Metal 4.

The Metal 4 backend removes the translator. It drives the Metal 4 command queue,
command buffer and encoders directly. It keeps the shared RSX emulation unchanged
behind the existing `rsx::thread` contract and the backend capability struct. The
shared part is about 55,000 lines. The new part is a second implementation of the
graphics resource layer.

### 1.2 Scope and the RSX-first rule

Scope: reach parity with the Vulkan backend's RSX feature set. Do not build a
Metal 3 fallback; MoltenVK already is the pre-Metal-4 fallback, and a second
backend is not a build flag.

The backend is RSX-first. The RSX contract is the starting point, not the Vulkan
objects. The design asks "what does the RSX do" and "what is the smallest Metal 4
expression of it". This deletes Vulkan-only scaffolding: the render-pass object and
its packed key, the descriptor-set and layout machinery, the loader and instance
layer, and the push-constant workaround.

The backend is performance-first. The cost centres are the encoder boundary, the
pipeline-object cache, the per-draw CPU work and the hazard barriers. State goes on
the encoder where Metal 4 puts it there, the pipeline key stays small, per-draw
values go into rings, and the barriers come from real resource hazards.

### 1.3 The target and the environment

| item | value |
|---|---|
| SoC class | Apple A18 Pro |
| operating system | macOS 27.0.1 |
| machine class | MacBook Neo, Mac17,5 |
| memory | 8 GB |
| deployment target | macOS 15.0 |
| Metal 4 API floor | macOS 26 |
| backend runtime floor | macOS 27 |
| build directory | `./build`, one build at a time, `-j4` |

The floor is macOS 27. The Metal 4 API exists from 26, but every Metal 4 defect
found on the floor device is absent on 27, and KosmicKrisp gates on 27. The
deployment target stays 15.0 so the binary loads on older systems and the backend
declines at run time. Metal 4 exists on Apple silicon only, never on Intel.

### 1.4 Where the code is

| role | files | note |
|---|---|---|
| the Metal subsystem | 66 files, 13,025 lines | `Emu/RSX/MTL/` |
| the C++ test asset | 3,972 lines | `tests/test_rsx_metal4_*.cpp` plus a generated dial list |
| the shared RSX emulation | about 55,000 lines | unchanged; both backends call it |

The subsystem has no stub body. A search for `TODO`, `FIXME` and placeholder text
in `Emu/RSX/MTL/*.cpp` returns nothing, and the five exception sites are all real
error paths.

---

## 2. The rules of the work

### 2.1 The evidence rule

The RSX is the source of truth. A Metal mapping is correct when it reproduces the
RSX behaviour. The evidence for RSX behaviour, in order of strength:

1. The RSX state inventory and the state headers — the decoded register layouts and
   the value enums.
2. The two shipping backends. They are prior art that already passed the title test.
3. The shared emulation code that both backends call.
4. The RSX documentation.

When a design document and the RSX tree disagree about a dial, the work stops, the
dial and both references go into the conflict register, and the work moves on.
Neither side is edited in silence.

### 2.2 The completeness spine

The project generated an inventory of everything the guest can put into RSX state,
reading the RSX headers and the decode tables. Every row has a stable ID, and the
ID is the C++ symbol name; an ID is never invented. A coverage test reads the
inventory and fails when a row is absent from a disposition list, so a dial cannot
drop out in silence.

| space | content |
|---|---|
| COMMAND | the FIFO method registers |
| DECODE | the bitfield layout per register |
| VALUE | the enum values a register can carry |
| SHADER | the vertex and fragment instruction encoding |

Every register row has one disposition.

| disposition | meaning |
|---|---|
| `direct` | maps near one to one to a Metal 4 construct |
| `reshape` | the same need with a different shape |
| `emulate` | no native path; realised in a shader, a compute pass or host code |
| `impossible` | no Metal API path exists |
| `not-recommended` | expressible but a known trap |
| `moot` | the construct never appears under the RSX-first frame |
| `na` | no state, or not applicable |

The spine holds 282 rows. 203 are live. The work dials are the rows whose
disposition needs code: 178, being 133 `reshape` and 45 `emulate`.

**"Wired" is a test metric, not an implementation metric.** A dial is wired when a
C++ test writes its register, reads the latch and checks the field. That proves the
decode reached a latched field. It proves nothing about whether the field is then
used correctly. Section 7.2 gives the real coverage picture.

### 2.3 The probe rule

When a mechanism is not derivable from the tree, write a standalone device probe: a
small program in a temporary directory, compiled against the vendored Metal headers
as system headers, run on the floor device, printing a table. Keep the probe and its
log, and move the result into the findings or the conflict register as a
measurement.

Run every probe with the validation layer on and off. A result that changes between
the two runs is a tool artefact, not a device fact. This rule has teeth: the
explicit-residency rule was learned from it, and several early "device defects"
turned out to be probe defects.

### 2.4 The subagent rules

Probe work runs in subagents so the main build stays single-threaded. The limits
are strict: a subagent writes new files only, under a temporary directory; it never
runs git, ninja or cmake; it never touches the build directory; it never edits a
shared file.

### 2.5 The build and test loop

One build at a time, four jobs. The machine has 8 GB of memory, so a wider build
swaps. The loop is: change; build the app and the test binary; run the tests; run
the document linter; run the inventory validator. A change is not complete until
the tests pass and the linter reports zero errors.

**One emulator instance at a time, and never detached.** The emulator is heavily
multithreaded, so two concurrent instances drive the load high enough to make the
desktop unresponsive on this machine, and an instance that is started detached
cannot be killed by the handle that started it, so it keeps running and keeps the
load high. Before every boot, kill any existing instance and confirm none is left;
start the instance under a handle that can be killed; bound the run; kill it and
confirm again. This rule was learned the hard way.

---

## 3. What is built

The architecture decomposes into components. `REALISATION.md` gives the per-cluster
status and the reasoning. This section gives the shape.

### 3.1 The substrate

**The resource layer.** A placement-heap pool per storage class, a suballocator with
a free list, explicit residency sets, and a deferred-release list. The guest window
is imported as one no-copy buffer, so a guest address is a buffer offset and a
resource needs no per-resource copy. Deferred release is per frame slot and drains
the completing slot: the frames after the requesting frame may still reference the
object, and an early version released a slot early and freed live objects.

**The command recorder.** A ring of three frame slots, each with one allocator, one
reusable command buffer and one argument table per stage. It is the only way to open
a pass, close an encoder or submit, so the recorded stream has one chokepoint, and
there is one submit path per frame: the present drawable belongs to the frame's own
submit.

**The barrier chokepoint and the pass scheduler.** The barrier carries the producer,
consumer and visibility of one edge, decides whether the two sides are legal inside
one encoder, and emits the intra-pass form. A `false` return is a pass-split
request. The scheduler derives the encoder boundaries from the hazards — it tracks
the writer and the reader of each resource in the open pass and raises a split when
an edge is not legal inside one encoder — and never opens or closes an encoder. A
pass boundary orders every resource, so the touch sets clear with the pass and not
with the frame.

**The surface store.** `mtl::surface` is the Metal projection of one guest range: an
owned, polymorphic texture handle, a resolve target when the surface is
multisampled, and the deferred data movement the generic store asks for before an
access. The surface type derives from the image type, because the shared store casts
an image pointer back to a surface pointer and cross-casts an image to a reference
count; containment cannot work. A surface has two sample counts: the guest sample
count and the host image sample count, which follow different things.

### 3.2 The pipeline and binding layer

The pipeline key is the shared cache's properties struct, so there is no second
cache and the key is exactly the pipeline's inputs. The argument table replaces
descriptor sets, layouts and push constants; its order is mutate, bind, draw,
because the table is captured at bind time. Cull mode, front face, fill mode, depth
clip, depth bias, viewport, scissor, stencil references, blend colour and the
depth-stencil object are encoder state and key nothing, which keeps the key small.
The depth-stencil object is the only carrier of the depth state, because the encoder
has no compare-function and no write-flag setter.

### 3.3 The shader layer

The chain is guest ucode → decompiled GLSL (Vulkan rules) → SPIR-V (vendored
glslang) → MSL (vendored SPIRV-Cross) → a Metal library. Keeping the chain preserves
about 5,000 lines of shared emulation. There are no text post-passes: the
cross-compiler has a native path for programmable blending and push-constant
substitution, and the level-of-detail bias already works in the emulation library.
The subpass-input form is what makes programmable blending work: the shared ROP code
declares `subpassInput` and the cross-compiler lowers it to an MSL `[[color(n)]]`
parameter.

Both SPIRV-Cross and the shader TU build with `-fexceptions`, and the shader build
catches, so a malformed guest shader fails its pipeline instead of aborting the
process.

### 3.4 The state, the clear and the draw path

A full-attachment clear is a load action; a partial clear is a scissored fill,
because Metal has no scissored clear. Cull mode, the scissor and the rest are
encoder state. Each latch field hashes at its own width, because the stencil
operations and the comparison functions are 16-bit values and packing them into
narrower fields made distinct states collide. The attachments are compacted, because
the RSX renumbers the enabled targets. A cull-both draw drops rasterisation only for
triangles; points and lines must still rasterise. A two-sample request folds up to
four, because some devices reject two samples.

### 3.5 The presenter and the flip

The drawable carries the source resolution, so the present is one copy and no scale
pass. A mismatched destination drops the frame, because a copy needs a matching
sample count and format. The presenter is not the frame owner, so a dropped present
still advances the ring. The vsync mode drives the layer: off is no sync, adaptive
is no sync plus a paced present at the display's maximum frame rate, full is display
sync. The display's maximum frame rate has no binding in the vendored Metal headers,
so the Objective-C++ unit reads it from the AppKit layer.

### 3.6 The reuse-only clusters

`fixed-function`, `misc`, `sync-flip`, `nv0039-dma`, `nv2d-surface`, `nv3089-image`
and `nv308a-image` need no new Metal code. Every live row is realised by shared
`Core/`, `Common/` or `Program/` code that the Metal backend calls the same way the
other two backends do. The transfer and sync engines run the shared CPU handlers,
because the guest window is a shared buffer and the backend declines host GPU labels
and passthrough DMA; a Metal-specific decode of the same registers would only drift.

---

## 4. The measurements

These are device facts, taken on the floor device. They are the load-bearing numbers
behind the design. The probe directories are not in the tree.

### 4.1 The shader limits

| limit | value |
|---|---|
| MSL `[[buffer(n)]]` index | 0 to 30 (31 fails) |
| MSL `[[sampler(n)]]` index | 0 to 15 (16 fails) |
| MSL `[[texture(n)]]` index | 0 to 127 (128 fails) |
| argument table, `setMaxBufferBindCount` | accepts 100000 |
| argument-buffer escape hatch | compiled with no ceiling found up to 8000 entries |
| the language version | does not move the three limits (3.1, 3.2, 4.0 all the same) |

A combined image sampler that is only ever fetched still emits an MSL `[[sampler(n)]]`
argument. The cross-compiler pushes a sampler for every sampled-image type and has
no fetch-only check (`spirv_msl.cpp:15557-15661`), even when no binding mapping is
supplied. A separate texture (SPIR-V `Sampled = 2`) fetched with `read()` emits no
sampler. A bind past the declared sampler count aborts the process on the validation
layer, not inertly.

### 4.2 The depth and stencil measurements

| measurement | result |
|---|---|
| depth resolve, producer barrier, all twelve configurations | 576 of 576 |
| depth resolve, producer barrier, 48-case set | 48 of 48 |
| depth resolve, consumer barrier | 46 of 48 |
| depth resolve, no barrier | 4 of 48 and 3 of 48 on the clear-only and draw-written cases |
| combined depth-and-stencil resolve | 32 of 32 |
| stencil resolve, all barrier forms | 48 of 48 |
| the named-plane combined-format copy | writes and reads back bit-exact on private and shared storage |
| the un-named combined-format copy | invalid API; the validation layer rejects `MTLBlitOptionNone` |
| a depth-only view of `Depth32Float_Stencil8` | rejected (`pixelFormat not castable`); only the stencil view is castable |
| the guest z24s8 byte layout | stencil in bits 7:0, depth in bits 31:8 (`rsx_decode.h:3404`) |
| the guest z16 byte layout | depth in bits 15:0 |
| the read-back encode | bit-exact word at every texel; `MTLFormatsDepthPlane.EncodesTheGuestDepthPack` |

### 4.3 The shadow compare

| form | result |
|---|---|
| the directed comparison functions | 32 of 72 mismatches |
| the swapped functions | 0 of 72 mismatches |
| linear compare, four / three / two / one / zero passing texels | 1.0 / 0.75 / 0.5 / 0.25 / 0.0 |

The native comparison sampler is correct with only the sampler's directed functions
swapped. Emulation is not needed.

### 4.4 The presentation measurements

| measurement | result |
|---|---|
| drawable count | 3 |
| the explicit producer-to-consumer barrier between two encoders | changed no pixel on this device for this encoder structure |

The last row is a measurement, not a rule. It does not license dropping the barrier.

### 4.5 The frame probe

A device probe ran the whole shape of a playable 2-D frame in one command buffer: a
sprite upload verified with `getBytes` before rendering, an alpha-blended pass whose
arithmetic is exact at three sample points, a depth surface copied to a mirror, a
second pass that reads the samplerless stencil and depth mirrors and re-samples the
RTT at a half-scale offset so its output is distinguishable from the first, a
read-back of both surfaces, and a present. Every check held. The probe proves the
encoder shapes; it says nothing about the RSX emulation layer, the guest stream or
the backend's translation.

### 4.6 The explicit-residency rule

A resource that is not in a `MTL::ResidencySet` on the queue is not resident. A
compute-encoder copy to it writes nothing, the commit reports success, and the queue
wedges with `MTL4CommandQueueTimeout`. Every earlier read-back failure was a probe
that broke this rule. The residency sets and the barriers are separate, because
residency does not track hazards.

---

## 5. The backend against Vulkan and OpenGL

This section is the gap analysis. It is the input to section 7.4.

### 5.1 The size comparison

| backend | .cpp files | .cpp lines | headers |
|---|---|---|---|
| Metal 4 | 31 | 9,114 | 3,791 |
| Vulkan | 29 | 18,528 | 4,758 |
| OpenGL | 18 | 9,102 | 3,463 |

The Metal backend is the size of the OpenGL backend and about half of Vulkan. The
Vulkan excess is its own descriptor-set layer, its SPIR-V interpreter, an async
scheduler, a compute helper set and its command stream — several of which Metal 4
replaces. A line count is a weak proxy; the subsystem list in 5.3 is the real
comparison.

### 5.2 The capability matrix

Every backend fills `rsx::thread::backend_configuration` (`RSXThread.h:80-102`).
Only the rows that differ matter.

| capability | Vulkan | OpenGL | Metal | consequence on Metal |
|---|---|---|---|---|
| `supports_last_provoking_vertex` | device-dependent | true | **false** | RSX flat shading falls back to smooth interpolation |
| `supports_hw_conditional_render` | true (not MVK) | not set | **false** | the CPU skip runs; correct but slower (A8) |
| `supports_native_logic_op` | true (default) | true (default) | **false** | emulated in the ROP (C1) |
| `supports_host_gpu_labels` | setting | true | **false** | guest writes stay CPU-side |
| `supports_extended_depth_range` | device-dependent | device-dependent | **false** | the shared float-depth emulation runs |
| `supports_passthrough_dma` | device-dependent | not set | **false** | measured; the shared CPU handlers run |
| `supports_multidraw` | true | true | **false** | **inert — no reader in the tree** |
| `supports_hw_renormalization` | NVIDIA only | NVIDIA only | false | correct; Apple silicon is not NVIDIA |
| `supports_asynchronous_compute` | true | not set | true | — |
| `supports_programmable_blending` | true | not set | true | — |

### 5.3 The mechanisms Vulkan and OpenGL have and Metal does not

| mechanism | Vulkan | OpenGL | Metal | consequence |
|---|---|---|---|---|
| shader interpreter (`shadermode`) | `VKShaderInterpreter` | `GLShaderInterpreter` | **none** | a shader the chain rejects is dropped with no CPU fallback |
| occlusion query pool | `VKQueryPool` | yes | none | ZCULL exists; a per-draw query pool does not |
| upscalers (FSR1, RCAS) | `upscalers/fsr1` | yes | **none** | the present path cannot upscale |
| async scheduler, command stream | yes | no | no | deliberate; Metal 4 is one queue |

### 5.4 The settings Metal ignores

Every setting below is a user workaround for specific titles. The list was made by
tracing each setting to its consumers.

| setting | honoured by | Metal |
|---|---|---|
| `read_color_buffers`, `write_color_buffers`, `read_depth_buffer`, `write_depth_buffer` | the shared `Common/texture_cache.h` and `Common/surface_utils.h`, **and** the Vulkan and OpenGL surface caches | the shared half is inherited; the backend half is absent |
| `strict_rendering_mode`, `disable_zcull_queries`, `relaxed_zcull_sync` | shared `texture_cache.h`, `RSXThread.cpp`, `RSXZCULL.cpp`, `RSXOffload.cpp` | inherited |
| `disable_hardware_texel_remapping` | Vulkan and OpenGL only | **missing** |
| `disable_vertex_cache` | Vulkan and OpenGL only | a no-op by construction: Metal has no vertex cache |
| `force_high_precision_z_buffer` | Vulkan and OpenGL only | effectively always on: Metal maps a 16-bit float depth to `Depth32Float` |
| `precise_zpass_count` | shared ZCULL, plus Vulkan and OpenGL | the shared half is inherited; the backend half is absent |
| `host_label_synchronization` | Vulkan and OpenGL | matches the false capability |
| `disable_msl_fast_math` | Vulkan | ignored; Metal sets no fast-math option |
| `log_programs`, `debug_output` | Vulkan and OpenGL | debug-only; absent |
| `renderdoc_compatiblity`, `disable_vulkan_mem_allocator` | Vulkan | not applicable |
| `output_scaling`, `rcas_sharpening_intensity`, `min_scalable_dimension`, `resolution_scale_percent` | Vulkan and OpenGL presenters | **missing**; see the upscaler row in 5.3 |

### 5.5 The accumulated workarounds, by class

Two backends and several years of title testing produced three classes of
workaround. Only the third is a risk for Metal.

**Class A — host-driver workarounds.** About 130 vendor-conditional sites. Ampere
"does not like the direct transfer hack" (`VKTexture.cpp:845`); the MoltenVK
conditional-render speedhack (`VKGSRender.cpp:793-795`); NVIDIA barycentric
interpolation and `fdiv` precision; Intel driver selection; the Vulkan allocator
switch. Not applicable to Metal, which has one vendor — **except** the Apple
entries, because KosmicKrisp is the Vulkan-on-Metal driver. The concrete one:
KosmicKrisp unrolls loops, so Vulkan disables the native primitive-restart path and
emulates it (`VKHelpers.cpp:127-128`). Metal must check the same behaviour on its own
index path.

**Class B — guest-behaviour quirks in the shared layer.** Metal inherits these, and
they are what a real title hits.

| quirk | where |
|---|---|
| `_builtin_normalize` NaN guard (BFBC) | `FragmentProgramDecompiler.cpp:965` |
| zero-NaN vertex constants | `ProgramStateCache.cpp:791` |
| a data race with Cell on texture upload | `texture_cache.h:1812` |
| `force_bg_load` on a partial subresource | `texture_cache.h:2803` |
| pitch conversion is unimplemented, so overlapping surfaces of differing pitch are truncated | `surface_store.h:457, 1177` |
| the depth-to-colour `force_convert` bitcast | `texture_cache_helpers.h:704` |
| ZCULL: some titles leave zpass active and never drain it (Diablo III, Yakuza 5) | `RSXThread.cpp:3524` |
| the subpixel bias for `fdiv` precision | `RSXThread.cpp:2540` |

**Class C — OpenGL-only hacks that document guest behaviour.** The depth-to-BGRA8
resample "do not filter, to avoid bits leaking" (`GLDraw.cpp:440-441`); the
trilinear "approximate with NEAREST" (`:460`); the clip-control z-remap
(`GLVertexProgram.cpp:310-313`, including the note that a title may legitimately use
very large `w` values, putting the post-multiplied z in the hundreds). Metal does
not need the workarounds, but it must reproduce the behaviour they encode. That
behaviour lives in the shared vertex prologue.

---

## 6. The findings register

Six review passes read the implementation against the RSX behaviour. The register
holds 63 findings. Read `REVIEW-FINDINGS.md` for the itemised list and the evidence.

| total | fixed | partial | blocked | open | limitations | duplicates | won't fix |
|---|---|---|---|---|---|---|---|
| 63 | 47 | 5 | 0 | 1 | 7 | 2 | 1 |

A fix that closes a finding marks it fixed. A finding whose fix reduced but did not
close it is partial. A finding that records an accepted deviation is a limitation.

One finding is open: the flat-shading path (G1), because Metal has no provoking-vertex
state. The last two of the earlier pass, the binding-cap set (A9) and the logic op
(C1), closed in the current pass. The five partials are B16, E13, F5, F9 and E8;
section 7.6 lists them with their remaining halves.

**The last pass added two findings**, recorded in the register as G1 and G2:

1. RSX flat shading is disabled on Metal, because `supports_last_provoking_vertex`
   is false. The Metal flat-shading path is dead code.
2. `supports_multidraw` is an inert field: declared, assigned by all three backends,
   read by none.

---

## 7. The pending work

This is the reason to read this document. The items are ordered by dependency and
grouped by kind. Each item names what it is, why it matters, where it lives, and
what settles it.

### 7.1 The gate: first light

Nothing else on this list is verifiable before one frame runs. No dial has executed
end to end. No shader the backend produced has run on the GPU. No pixel has been
compared with anything.

**State of the prerequisites.**

| prerequisite | state |
|---|---|
| the app binary | built at `build/bin/rpcs3.app/Contents/MacOS/rpcs3` |
| the test binary | built at `build/bin/rpcs3_test` |
| the firmware | **not installed** (`dev_flash`, `dev_flash2`, `dev_flash3` hold 0 files) |
| the title package | **not installed** (`filesdump/Mega Man 9 PSN.pkg`) |
| the licence file | **not present**; the likely hard stop |

**The recipe.** The firmware install needs the GUI; the command line refuses it. The
title package installs from the command line.

1. `open build/bin/rpcs3.app --args --installfw "$PWD/filesdump/PS3UPDAT.PUP"`
2. `open build/bin/rpcs3.app --args --installpkg "$PWD/filesdump/Mega Man 9 PSN.pkg"`
   → installs to `dev_hdd0/game/NPUB30036/`
3. If the boot reports a missing licence, place the RAP at
   `~/Library/Application Support/rpcs3/dev_hdd0/home/00000001/exdata/UP0102-NPUB30036_00-INTIRM9E00000000.rap`
   (`unedat.cpp:863`, `system_utils.cpp:413-438`).
4. Set the renderer to Metal. The raw config string is `"Metal"`
   (`~/Library/Application Support/rpcs3/config.yml`); the UI shows it as
   "Metal (experimental)" and hides the entry below macOS 27.
5. Boot with `--no-gui`, so a fatal `verify()` cannot raise a dialog:
   `build/bin/rpcs3.app/Contents/MacOS/rpcs3 --no-gui <dev_hdd0/game/NPUB30036/USRDIR/EBOOT.BIN>`
6. Read `~/Library/Application Support/rpcs3/rpcs3.log`.

**How to read the first boot.** Treat it as a diagnosis, not a pass. Expect one of
these, in order of likelihood:

- a validation abort that names an over-cap binding or a missing residency entry;
- a uniformly black frame, which points at the residency rule (4.6) or at the
  presenter;
- a clean stop at the first state the backend does not carry;
- a logo or a menu, which is the success case.

**The dials that will be exercised first**, and are therefore the first to check:
the sync-flip cluster, the surface cluster, the clear path, the flat-shaded and
textured 2D draw of a PSN title, and the present.

### 7.2 The dials: the coverage work

**What "wired" means.** A dial is wired when a C++ test writes the register, reads
the latch and checks the field. That is a decode proof. It is the floor of coverage,
not a measure of the backend.

| cluster | live dials | wired | has a decoder |
|---|---|---|---|
| depth-rop | 30 | 17 | 30 |
| surface | 25 | 0 | 24 |
| vertex | 25 | 0 | 4 |
| raster | 23 | 17 | 22 |
| sync-flip | 17 | 0 | 7 |
| nv3089-image | 15 | 0 | 14 |
| blend | 13 | 11 | 13 |
| texture | 13 | 0 | 3 |
| draw | 13 | 6 | 9 |
| nv0039-dma | 10 | 0 | 9 |
| nv2d-surface | 7 | 0 | 7 |
| nv308a-image | 6 | 0 | 1 |
| misc | 3 | 0 | 2 |
| fixed-function | 3 | 0 | 3 |
| **total** | **203** | **51** | **138** |

**Why the other 152 are not wired, and what that means.** The latch test can only
exist where there is a CPU-observable latch. The four clusters that have one are
depth-rop, raster, blend and draw. The other ten clusters put their state straight
into a resource — a texture, a surface, a vertex stream, a DMA object — where there
is nothing on the CPU to assert. A test cannot prove a texture upload without a
device.

So the work splits:

1. **The dials whose only honest proof is a device observation.** This is most of
   the 152. They become testable when a device harness runs inside the tree
   (7.9 item 2), or when a frame runs.
2. **The dials that need a mechanism, not a field.** 71 dials are difficulty 3 or 4:
   33 at difficulty 1, 99 at 2, 54 at 3, 17 at 4. A difficulty 3 or 4 dial needs a
   design decision about what the Metal expression is, because the RSX feature has
   no native Metal equivalent.
3. **The dials with no decoder yet.** 65 of the 203 live dials have no decoder in
   the tree (`has_decoder = 0`). The sharpest are `vertex` (4 of 25) and `texture`
   (3 of 13). These are the concrete next rows to decode, because a row with no
   decoder cannot be latched or tested at all.

### 7.3 The in-tree code that has never executed

These are written and correct by construction, with a measurement behind the
mechanism but no execution. Each names what settles it.

| code | why unproven | settled by |
|---|---|---|
| the A9 samplerless declarations (the integer `utexture2D` mirror and the `texture2D` depth input) | no test reaches the GLSL-to-MSL hop; the form is measured on the device but not emitted by the tree | first light, on a title that reads a depth-as-texture or enables the depth-compare emulation |
| the C1 logic-op shader (the `subpassInput` read, `MRTn_LOAD_DO`, `_logic_op_apply`, the push-constant op) | the same | first light, on a title that uses a logic op |
| the pass scheduler inside a frame | it is correct by construction and has never been driven by a real hazard | first light |
| the flush queue on a real guest fault | modelled on the OpenGL mechanism | a title that writes into a sampled surface |
| the presenter and the readback wait | the shapes are measured; the frame ring is not | first light |
| the shadow-compare emulated fallback against the native path | the swap is measured; the two have never been compared in a frame | a title that uses the shadow compare |
| the adaptive vsync approximation | Metal has no true mailbox present on this path | a title sensitive to pacing, or accept it |
| the Metal 3 blit encoder against a Metal 4 command buffer | no ordering rule is known | a probe that interleaves the two families, or avoid the mix |

### 7.4 The gap list against Vulkan and OpenGL

These are the work items the analysis in section 5 produces. Each is a place where a
shipping backend does something Metal does not.

- **Gap 1. Flat shading (`supports_last_provoking_vertex` false).** RSX flat shading
  takes the colour from the last vertex; Metal has no provoking-vertex state, so
  `RSXThread.cpp:2368-2373` clears `RSX_SHADER_CONTROL_FLAT_SHADING` and the vertex
  program never emits its own `flat` path (`MTLVertexProgram.cpp:246`). Effect:
  flat-shaded geometry interpolates, and the Metal flat path is dead code. Settle it
  by finding a Metal expression — a triangle rotation on the index path, or an
  explicit provoking-attribute replicate — or by recording it as a limitation.
- **Gap 2. No shader interpreter.** A shader the chain rejects is dropped, with no CPU
  fallback. Vulkan and OpenGL always have one. Settle it by measuring how often the
  chain rejects a real title's shader before deciding whether a fallback is needed.
- **Gap 3. No occlusion query pool.** ZCULL is implemented; a per-draw query pool is
  not. Settle it against a title that uses `CELL_GCM_ZPASS_PERCENTAGE`.
- **Gap 4. No upscalers.** `output_scaling`, `rcas_sharpening_intensity`,
  `min_scalable_dimension` and `resolution_scale_percent` do nothing on Metal, so
  the present path cannot upscale. Output-side, not RSX emulation.
- **Gap 5. `disable_hardware_texel_remapping` is ignored.** Vulkan and OpenGL honour
  it. It is the workaround for titles with a broken texture remap.
- **Gap 6. The backend half of the surface read/write flags.** The shared texture cache
  honours `read/write_color_buffers` and `read/write_depth_buffer`, so Metal inherits
  the semantics. Vulkan and OpenGL also consult them inside their own surface
  caches. Metal does not. Look here first when a title that needs write-back behaves
  differently.
- **Gap 7. `precise_zpass_count`.** The shared ZCULL half is inherited; the backend half
  is absent.
- **Gap 8. `disable_vertex_cache` and `force_high_precision_z_buffer` are no-ops.**
  Both are inert by construction on Metal, but the settings do nothing silently. A
  note in the docs, or a UI gate, is the honest fix.
- **Gap 9. Primitive restart against the Apple driver.** KosmicKrisp unrolls loops and
  Vulkan emulates primitive restart for it. Check the same behaviour on Metal's
  index path.
- **Gap 10. `disable_msl_fast_math`.** Metal sets no fast-math option. Decide the
  default and expose it, or record it as not applicable.

### 7.5 The guest-behaviour work items

The quirks in 5.5 Class B are inherited through the shared code, so they need no
work by default. Two need a check rather than a change.

- **Q1. The pitch-conversion limitation.** Overlapping surfaces of differing pitch
  are truncated, not reproduced (`surface_store.h:1177`). Metal inherits the
  limitation. Confirm that a title does not depend on a full pitch conversion.
- **Q2. The depth-to-colour `force_convert` bitcast.** The shared code decides the
  bitcast; the Metal side must have the resample path. Confirm it exists and is
  reachable.

### 7.6 The verified defects with a remaining half

| ID | remaining half | where |
|---|---|---|
| B16 | the encode is written and tested, and the predicate is settled (`depth_format_needs_decode`); what is missing is the synchronisation point for the CPU pass after the frame slot commits, and the stencil plane of a combined read-back | `MTLFormats.cpp`, the surface write-back path |
| E13 | the stipple copy and the harness-only marking are in; which harness-only fields the harness releases is a decision, and the per-clause re-issue is deferred with its shape recorded | the latch dataflow |
| F5 | the flip preamble does not consume the display-buffer pitch, the format or the stereo second eye | the flip path |
| F9 | the drawable is sized from the source rather than the view | the drawable layer |
| E8 | the level-of-detail clamp is in; the bias has no descriptor field and stays a limitation | the sampler state |

### 7.7 The findings the last pass added

Both are in the register. G1 is open; G2 is a limitation.

1. **Flat shading is disabled on Metal** (register G1, open). It changes pixels for
   any title that uses a flat-shaded attribute.
2. **`supports_multidraw` is inert.** `RSXThread.h:82` declares it; all three
   backends assign it; a repository-wide search finds no reader. Same class as the
   E12 inert key fields. Metal's `false` costs nothing, but the field should be
   either consumed or recorded as inert.
3. **The `encode_unorm` defect, found and fixed.** `value * range + 0.5` in f32
   overshoots: `0xFFFF + 0.5` rounds to 2^16 and `0xFFFFFF + 0.5` rounds to 2^24, and
   the z24s8 pack then shifts the overflow away, so a depth of 1.0 encoded as zero.
   Fixed with f64 arithmetic and a clamp to the range. Record the lesson: the defect
   was invisible until the round-trip test existed.
4. **The stale ABI assertion, found and fixed.**
   `MTLConstantsLayout.StructSizesMatchTheShaderAbi` still expected `blend_values` to
   be 32 bytes after C1 grew it to 36. The test now pins the size and the `logic_op`
   offset at 32.

### 7.8 Performance work owed

The telemetry counters were dropped and not replaced, so no frame-time budget has
been measured against the Vulkan backend. The cost centres to measure, in order: the
encoder boundary count per frame, the pipeline-object cache hit rate, the per-draw
CPU time, and the number of hazard barriers per frame. Measure after first light,
because a frame is what makes a measurement meaningful.

### 7.9 Process and hygiene owed

1. Keep the register and its totals in step when a pass adds a finding. The totals
   are 63/47/5/0/1/7/2/1; the parked table carries G1.
2. Port the verification ladder into the test asset. The only honest CPU-side check
   that is missing is the GLSL-to-SPIR-V hop: `spirv::compile_glsl_to_spv` and the
   fragment decompiler are reachable from `rpcs3_test`, but no existing test reaches
   the hop, so the A9 and C1 GLSL is unchecked. A test that decompiles a logic-op
   program and a redirected-depth program and compiles the result would close 7.3's
   first two rows without a GPU. Note the limit: with an invalid program the
   constants loop is cleared, so such a test verifies the extension, the
   `frag_depth` form, the push-constant block and the logic-op epilogue, not the
   mirror declarations.
3. Keep the capture-and-replay substrate in mind. It is backend-agnostic and
   per-command annotated, so a future Metal verification loop inherits a
   deterministic, command-localisable path unchanged.
4. Split the first-light commit from the bring-up fixes. The first boot produces
   defects; commit them separately, each with its own test.

---

## 8. Accepted limitations

These are accepted deviations, not defects.

1. The point polygon mode collapses to a fill. Metal has no point mode.
2. The per-primitive polygon-offset enables collapse to the fill enable. Metal
   applies the bias per primitive class and has no separate point or line enable.
3. The guest dither is dropped. Metal has no dither state, and Vulkan drops it too.
4. The hardware conditional-render path is not implemented; the CPU skip is used.
5. The sampler level-of-detail bias needs a fragment-shader injection, which changes
   the texture contract and a shared header.
6. The draw-state latch is a decode proof, not hot-path state.
7. The pipeline key carries inert fields.
8. The border colour is not arbitrary. Metal offers four fixed colours.
9. The drawable stretches a source whose aspect ratio differs from the window.
10. The present stereo second eye is a deviation.
11. The stencil plane of a combined image cannot be uploaded, so the guest stencil
    stays marked unknown on that path.
12. Pitch conversion is unimplemented, so overlapping surfaces of differing pitch are
    truncated.

---

## 9. Roadblocks

### 9.1 The blocking roadblock

First light has not happened, so no dial has run end to end. Nothing in the code
blocks it. The remaining gate is the environment: the firmware and the title package
must be installed, the firmware install needs the GUI, and the title needs its
licence file, which is not present.

### 9.2 The environment roadblocks

1. No new dependency installs are allowed on this machine.
2. The machine has 8 GB of memory, so only a four-job build is safe.
3. Builds must run one at a time, in `./build`.
4. Run one emulator instance at a time, never detached, and always kill it when the
   run ends (see 2.5). Concurrent or orphaned instances freeze the desktop.

### 9.3 The tooling roadblocks

1. `ripgrep` is not available. Use the plain search tool and quote the globs.
2. There is no `timeout` tool.
3. The shell is zsh: a different status array name, and no word splitting of an
   unquoted variable.
4. `sed` is the BSD form, with no word-boundary support.
5. `MoltenVKShaderConverter` is on the PATH but takes SPIR-V input only.
   `glslangValidator` and `glslc` are absent; SPIRV-Tools is present.
6. A GPU test risks a fatal `verify()` that raises a dialog and is then killed, so
   the unit suite stays GPU-free.

### 9.4 The platform roadblocks

1. The vendored Metal headers carry no availability annotations, so the run-time
   gate is the only guard against an unavailable symbol. A gate that misses a path
   crashes on an older OS.
2. A depth-only view of a combined `Depth32Float_Stencil8` texture is rejected, so
   the depth input binds the combined texture directly.
3. `[[position]]` cannot be declared both in the stage-in struct and as a fragment
   parameter.
4. A bind past the declared sampler count aborts on the validation layer.

### 9.5 The measurement roadblocks

1. A probe that ignores the explicit-residency rule produces a false device defect.
   Put every resource in a residency set on the queue before the first use.
2. A result that changes between the validation-on and validation-off runs is a tool
   artefact.

---

## 10. The verification ladder

From the cheapest check to the most complete.

1. GPU-free unit tests. These run anywhere.
2. A shader-translate check. This compiles the emitted shader without a GPU.
3. Guest-observable readback. This is the only objective oracle, because the result
   is compared against what the guest reads, not against another backend.
4. A differential comparison against the shipping backend.
5. Golden images.

Two facts govern the ladder. RPCS3 has no golden-image suite today and the hosted
runners expose no dependable GPU, so without a self-hosted Apple-silicon runner the
GPU half is manual. And a differential comparison runs a tile-based deferred
renderer against a non-tile-based one, so both false positives and false negatives
are expected; it needs a classified tolerance model, not an exact match.

Rung 2 is the cheapest unused rung, and 7.9 item 2 is how to use it.

---

## 11. How to continue

1. Read `REALISATION.md` for the per-cluster status and the reuse-only clusters.
2. Read `REVIEW-FINDINGS.md` for the itemised defects and their evidence.
3. Read `CONFLICTS.md` for the places where a design document disagrees with the
   tree.
4. Do section 7.1. First light unlocks most of section 7, and it is the only item
   that changes the state of the project rather than a detail of it.
5. Then work section 7.4 in order. G1 and G2 are the two that change behaviour
   rather than robustness.
6. Keep the loop: build the app and the test binary, run the tests, run the document
   linter, run the inventory validator.
7. For a mechanism that is not derivable from the tree, write a probe. Run it with
   the validation layer on and off. Keep the probe and its log.
8. Keep the subagent limits: new files only, under a temporary directory, no git, no
   ninja, no cmake, no build directory, no shared files.
9. Keep the evidence rule: the RSX is the source of truth; the two shipping backends
   and the shared code are evidence; a disagreement is a recorded conflict, never a
   silent edit.

---

## 12. Datapoint index

| datapoint | value |
|---|---|
| commits ahead of the remote branch | 45 |
| files in the Metal subsystem | 66 |
| lines in the Metal subsystem | 13,025 |
| Metal .cpp lines | 9,114 |
| test suites in the Metal C++ asset | 3, plus 1 generated include |
| lines in the Metal C++ tests | 3,972 |
| total tests | 467 |
| total suites | 79 |
| disabled tests | 2 |
| Metal-filtered tests | 180 |
| Metal-filtered suites | 40 |
| dials covered | 51 of 203 |
| dials executed end to end | 0 of 203 |
| inventory register rows | 282 |
| live rows | 203 |
| work dials | 178 (133 reshape, 45 emulate) |
| live dials with no decoder | 65 |
| live dials at difficulty 3 or 4 | 71 (54 at 3, 17 at 4) |
| design documents | 25 |
| clusters | 14 |
| linter gaps | 0 |
| linter errors | 0 |
| total findings | 63 |
| fixed | 47 |
| partial | 5 |
| blocked | 0 |
| open | 1 |
| limitations | 7 |
| duplicates | 2 |
| won't fix | 1 |
| MSL buffer index cap | 30 |
| MSL sampler index cap | 15 |
| MSL texture index cap | 127 |
| argument-buffer escape hatch, highest tested | 8000 entries |
| depth resolve, producer barrier, twelve configurations | 576 of 576 |
| depth resolve, consumer barrier | 46 of 48 |
| combined depth-and-stencil resolve | 32 of 32 |
| stencil resolve, all barrier forms | 48 of 48 |
| shadow compare, direct functions | 32 of 72 mismatches |
| shadow compare, swapped functions | 0 of 72 mismatches |
| the guest z24s8 layout | stencil in bits 7:0, depth in bits 31:8 |
| the read-back encode | bit-exact at every texel |
| combined-format copy, named plane | bit-exact |
| combined-format copy, un-named | invalid API |
| reference-driver workaround entries | 13 |
| reference-driver entries still live | 2 |
| drawable count | 3 |
| Metal 4 API floor | macOS 26 |
| backend runtime floor | macOS 27 |
| deployment target | 15.0 |
| shader emulation asset lines | about 5,000 |
| code written, estimate | about 65 percent |
| code verified, estimate | about 35 percent |

<!-- doc-end -->
