# HLD-02 — Resource model: RSX memory, surfaces, textures, tiling, aliasing, residency

Backend: native Apple **Metal 4** (`MTL4*`) on Apple-silicon (M1+), macOS 26+ (`10` §10 `[V]`).
Scope of this HLD: the layer between guest XDR and the GPU — guest memory window, surface
objects (colour A/B/C/D + zeta), textures, address→resource mapping, aliasing, tiling/swizzle,
and residency. Pass/encoder synthesis is `HLD-03`; barriers are `HLD-04`; command submission is
out of scope here.

Reading conventions: `[V]` verified against primary source (`file:line` or Apple doc), `[I]`
inference, `[U]` unverified — every `[U]` carries the experiment that would resolve it.
Register IDs are taken verbatim from `inventory/out/rsx_state_space.csv`; values from
`inventory/out/rsx_value_space.csv`; **no register or constant is invented here.**

---

## 1. Purpose & non-goals

### 1.1 Purpose

Define the **resource model** for a Metal 4 RSX backend: what a "surface", a "texture", and "guest
memory" *are* on the Metal side, how a guest address resolves to a Metal resource, how the
guest's aliasing/reuse of memory is preserved, how RSX tiling and swizzle are handled, and how
resources are made GPU-resident.

This HLD fixes the shape that the lower-level designs (`LLD-surface`, `LLD-texture`,
`LLD-nv3089`, `LLD-nv2d-surface`) must obey, and pins the two deliverables that the scope request
names explicitly:

- a **tiling/swizzle decision** (§2.4) with a compare-and-pick between "keep guest-accurate
  swizzle" and "translate to Metal layouts on import/export";
- a **`CELL_GCM_TEXTURE_*` → `MTLPixelFormat` table** with gaps and chosen fallbacks (§2.7).

### 1.2 Non-goals

1. **Not a shader-ABI or binding design** — argument tables and slot mapping are `10` §4.2.
2. **Not a pass/encoder or barrier design** — this HLD says *what a surface is*, not *when an
   encoder opens*; hazard derivation is `10` §4.3/§4.4 and RN1.
3. **Not the allocator internals** — the placement-heap/upload-ring policy is `01`; this HLD
   consumes it (`01` §3.2, §5 rank 2).
4. **Not a port of Vulkan vocabulary** — `VkImage`/`VkImageView`/`VkFramebuffer` do not reappear
   here; §3 names the Metal objects directly.
5. **Not MSAA resolve scheduling** — exploded resolve texture *existence* is §2.2/§3; the resolve
   *barrier* is RN1/`HLD-03`.

---

## 2. Model — RSX semantics → Metal 4 semantics

### 2.0 The shape change (one paragraph)

RSX resources are **bytes in a unified guest address space**, keyed by a base address, a pitch,
a format, and an optional tile descriptor; any range may later be reinterpreted as a different
resource type (the same XDR region is a colour surface, then a sampled texture, then a DMA
destination). Metal resources are **typed, format-fixed, driver-owned allocations** with no
address identity; a `MTLTexture` cannot wrap guest RAM, and no Metal object can reinterpret
untracked bytes as a different format outside documented pixel-format-view groups.

Therefore the RSX→Metal mapping is **not** an address translation. It is:

> *guest XDR is a canonical byte image; every surface and texture is an owned Metal allocation
> that is a projection of that byte image, kept coherent by explicit copy-in/copy-out at
> guest-visible boundaries, with the existing RPCS3 address map (not the GPU) holding identity.*

The rest of §2 makes that precise.

### 2.1 Guest memory → Metal

**RSX requirement.** A single contiguous pool — main + local memory (~512 MB) — plus host-managed
rings (attribute, index, constant uploads). Guest-visible memory is unified; the RSX thread writes
into it (`10` §4.1 `[V]`).

**Metal 4 shape.**

- The whole guest window becomes **one `MTL::Buffer` created with
  `newBufferWithBytesNoCopy:length:options:deallocator:`** (`StorageModeShared`), page-aligned,
  whole-allocation, per `01` §4 item 2 (`[V]`: this is the *only* host-pointer import path; there
  is no texture analog). The `deallocator` must not free the guest region (`01` §5 rank 8 `[V]`).
  The import source is the **unprotected mirror** `vm::g_sudo_addr`, not the guest view
  `vm::g_base_addr`: `newBufferWithBytesNoCopy:` returns `nil` unless *every page* of the range is
  writable, and the guest view carries `protection::no` pages for unmapped guest memory
  (`vm.cpp:1060`; measured, §6 Q1). The mirror is written only RW (`vm.cpp:846`), is never
  protected or unmapped on Apple platforms (`vm.cpp:1067` is `_WIN32`-only), and aliases the same
  guest RAM — it is the view the other renderers already read guest memory through
  (`vm::get_super_ptr`; `VKDMA.cpp:77`, `GLDMA.cpp:22`). Guest addresses stay buffer offsets
  because `g_sudo_addr == g_base_addr + 4 GiB`.
- Sub-ranges that are only ever read as *buffers* (vertex/index/constant rings) are bound by
  offset into this buffer — no per-resource allocation (`10` §4.1 `[I]`).
- **Textures and surfaces are *not* views into this buffer.** They are separate `MTL::Texture`
  allocations, sub-allocated from a placement `MTL::Heap` (`HeapTypePlacement`) owned by `01`
  (§3.2 `[V]`). Guest bytes and GPU bytes are reconciled by copies (§2.4), where RPCS3 already
  performs them: `texture_cache` uploads and surface `load_memory`
  (`VKRenderTargets.cpp:694-746`).
- Metal's `hasUnifiedMemory()` collapses the Vulkan `host_visible_coherent`/`device_local`/
  `device_bar` trichotomy to **two** storage modes; `device_bar` is deleted (`01` §4 item 3 `[V]`).

**Why a copy and not an alias.** `newTextureWithDescriptor:` has no host-pointer initializer and
Metal exposes no external-texture import; a heap may alias a *buffer* and a *texture* at the same
offset but that is device-heap memory, not guest XDR (`01` §4 items 2/5 `[V]`). So the guest byte
image is canonical and every GPU-visible surface/texture is a copy. This is the single fact that
forces §2.4's translation choice.

**Note on passthrough DMA.** `supports_passthrough_dma` must be forced `false`; the DMA path
(`VKDMA.cpp:306`) has no Metal equivalent and falls back to CPU copy (`10` §9 R4 `[V]`).

### 2.2 Surfaces (colour A/B/C/D, zeta) → MTLTexture

**RSX requirement.** Up to four colour surfaces A/B/C/D plus one depth ("zeta") surface, selected
by target, each an address+pitch, with an optional tile descriptor. MRT targets
(`MRT1`=0x13, `MRT2`=0x17, `MRT3`=0x1f) select contiguous prefixes of A..D
(`rsx_value_space.csv:608-613`). Formats come from `M_NV4097_SET_SURFACE_FORMAT` (0x0208, 6 decoded
fields: colour format, depth format, type, antialias, log2 width, log2 height) and pitches from
`M_NV4097_SET_SURFACE_PITCH_A/B/C/D` (0x020c/0x021c/0x0280/0x0284) and
`M_NV4097_SET_SURFACE_PITCH_Z` (0x022c); addresses from `M_NV4097_SET_SURFACE_COLOR_AOFFSET`
(0x0210), `..._BOFFSET` (0x0218), `..._COFFSET` (0x0288), `..._DOFFSET` (0x028c), and
`M_NV4097_SET_SURFACE_ZETA_OFFSET` (0x0214); target from `M_NV4097_SET_SURFACE_COLOR_TARGET`
(0x0220). `M_NV4097_SET_CONTEXT_DMA_COLOR_A/B/C/D` (0x0194/0x018c/0x01b4/0x01b8) and
`M_NV4097_SET_CONTEXT_DMA_ZETA` (0x0198) carry the DMA context. (`rsx_state_space.csv`.)

**Metal 4 shape.** A surface is a **`MTL::Texture`**, exactly as RPCS3's `image_storage_type`
already is on the GPU side (`surface_utils.h:130` is generic over it). The descriptor field set
maps onto `MTL::TextureDescriptor`/`newTextureWithDescriptor:` (or
`MTL::Heap::newTextureWithDescriptor:offset:` for heap sub-allocation — `01` §3.4 `[V]`):

| RSX surface attribute | RPCS3 field (`surface_utils.h`) | Metal 4 |
|---|---|---|
| base address, memory span | `base_addr` :133, `memory_range` :158 | identity in the app address map (not a Metal field) |
| `rsx_pitch` (guest bytes/row) | `rsx_pitch` :144 | `rowBytes`-equivalent; drives `native_pitch` |
| `surface_width`/`surface_height` | :146-147 | `MTLTextureDescriptor.width`/`height` |
| sample layout | `spp`/`samples_x`/`samples_y` :148-150 | `sampleCount`; exploded targets are *separate textures* |
| `aa_mode` | :153 | `surface_antialiasing` → sample count + resolve target |
| colour/depth format | `format_info` union :169-174 | `MTLPixelFormat` (see §2.7) |
| `raster_type` | :162 | not a Metal field — see §2.4 |
| `old_contents` (aliased/inherited) | :141 | app-side copy list (see §2.5) |
| `resolve_surface` | :160 | `MTL4RenderPassDescriptor` `resolveTexture` (`04` §2.7 `[V]`) |
| lifetime | `ref_counted` base :130 | retain/release + `invalidated_resources` |

**Exploded MSAA is preserved.** RPCS3 keeps the multisample surface and a *separate* resolve
target in memory and performs explicit resolves/unresolves/spills (`04` §2.7 `[V]`; `samples_x`/
`samples_y` at `surface_utils.h:149-150`, e.g. 2×1/2×2). Metal's memoryless/tile storage **cannot**
be a drop-in: RSX can sample the multisample surface directly and swap surfaces mid-frame, so the
resolve target must persist and be samplable (`04` §2.7 `[I]`). Consequence for this model: the
resolved, samplable image is a **first-class `MTL::Texture`**, not an implicit store-action
artifact alone; `storeAction = MultisampleResolve`/`StoreAndMultisampleResolve` targets it
(`04` §2.7 `[V]`). Memoryless is left as a possible optimisation for *transient, never-guest-read*
attachments only (`[U]`, experiment Q6).

**MRT.** Up to four colour attachments; the `MTLLogicalToPhysicalColorAttachmentMap` lets one
encoder configure a superset and remap shader outputs per pipeline (`10` §4.3 `[V]`), which is
where `M_NV4097_SET_SURFACE_COLOR_TARGET`'s target index is realised.

### 2.3 Pitch

**RSX requirement.** `rsx_pitch` is guest bytes-per-row and is *not* required to equal
`bpp × width`; it can exceed it (padding) or be *too small* and be corrected by RPCS3
(`RSXThread.cpp:1628-1660` recomputes `minimum_color_pitch`/`minimum_zeta_pitch` and drops or
fixes misconfigured depth surfaces `[V]`).

**Metal 4 shape.** Two distinct pitches, kept distinct exactly as `render_target_descriptor`
already does (`surface_utils.h:144-145`):

- `rsx_pitch` — guest truth; determines the **memory range** of the surface (used by the address
  map and by guest readback).
- `native_pitch` — the Metal texture row stride. `MTLTextureDescriptor` has no explicit row-bytes
  knob that can express an arbitrary guest pitch below the format's natural stride; the texture is
  allocated at a width whose byte stride is `≥ rsx_pitch`, and the surface's `memory_range` is
  derived from `rsx_pitch × height`, not from the texture. `[I]`

The invariant (§4) is `native_pitch ≥ rsx_pitch` and `native_pitch ≥ bpp × width`. Note RPCS3
already tracks the mismatch and works around cross-pitch overlap
(`surface_store.h:1177-1230`, `truncate_memory_range_by_pitch` `[V]`); Metal's inability to express
an under-sized pitch is not a new problem, it is the existing pitch-fixup path.

### 2.4 Tiling & swizzle — the import/export decision (COMPARE AND PICK)

There are **two distinct mechanisms** in RSX and they must not be conflated:

1. **Surface raster type** — `M_NV4097_SET_SURFACE_FORMAT.type_raw` decoded via
   `rsx_decode.h:3350` → `to_surface_raster_type` (`gcm_enums.h:1339-1345`) →
   `surface_raster_type{linear=CELL_GCM_SURFACE_PITCH(1), swizzle=CELL_GCM_SURFACE_SWIZZLE(2)}`
   (`gcm_enums.h:1332-1337`). A *swizzled* surface compacts rows (RPCS3 "packed render",
   `RSXThread.cpp:1611-1626` `[V]`).
2. **Memory tiles** — `CellGcmTileInfo` (`GCM.h:88-94`) packed into `GcmTileInfo`
   (`GCM.h:129-154`): `offset`/`size`/`pitch`/`bank`/`comp`/`base`. A **linear (§1-style) region
   in tiled memory** is de-tiled before use; `GCM_context::get_tiled_memory_region`
   (`RSXContext.cpp:9-45`) and `GCM_tile_reference::tile_align` (`RSXContext.cpp:47-54`) resolve and
   align the region; RPCS3 de-tiles it on load (`VKRenderTargets.cpp:715-746` `[V]`).

**The two candidate designs.**

| | **Option A — keep guest-accurate swizzle** | **Option B — translate to Metal layouts on import/export** |
|---|---|---|
| GPU storage | RSX byte layout preserved verbatim in an `MTL::Buffer`; every sampler/ROP access computes swizzled addresses | Metal-native linear `MTL::Texture`; guest layout produced only at boundaries |
| Feasible? | Metal has **no swizzled texture storage mode** for either mechanism; it becomes a buffer + manual addressing in every shader and in the ROP path | Supported directly: textures are linear; conversion is the existing `convert_linear_swizzle`/`detile_memory_block` code |
| Texture units | unusable for swizzled data (manual fetch) | usable (hardware sampling/`[[color(N)]]`) |
| Guest readback | trivially byte-exact | exact **iff** a re-swizzle happens on writeback |
| Precedent in RPCS3 | none | `convert_linear_swizzle` at flush (`VKTextureCache.h:299` `imp_flush`), `detile_memory_block` at load (`VKRenderTargets.cpp:738`), `nv3089.cpp:525-531` swizzle on transfer |
| Cost | O(1) import/export, O(access) forever | O(pixels) import/export, O(1) access |

**Pick: Option B — translate on import/export.** `[I]`

Rationale:

1. **Forced by §2.1.** Because a Metal texture cannot wrap guest bytes, the guest image is already
   a copy; keeping "guest-accurate swizzle" would mean storing that copy in an `MTL::Buffer` and
   *never* using a texture, which deletes the texture unit and the whole fragment path. Option A is
   strictly worse once §2.1 holds. `[I]`
2. **It is what RPCS3 already does.** Swizzle/tile conversion lives at exactly two seams — upload
   and flush — and both are already implemented for Vulkan (`VKTextureCache.cpp:1208-1285`,
   `VKTextureCache.h:299`, `nv3089.cpp:525-531,634-670`). Option B reuses that code unchanged
   under a Metal storage type. `[V]`
3. **Guest bytes remain the oracle.** The verification loop's only RSX-objective oracle is
   guest-observable memory (`07`/README §4 `[V]`); Option B keeps guest bytes byte-exact by
   construction (re-swizzle on flush), so the oracle stays meaningful.

**Carve-out (lazy conversion).** Conversion is performed **only when a boundary requires it**:
(a) a surface/texture is guest-read (flush), (b) a guest-swizzled region is guest-written and then
GPU-read (upload), or (c) a swizzled surface is sampled as a texture. A resource that is
GPU-only for its whole life is never converted. This mirrors RPCS3's lazy behaviour and is stated
as an invariant (§4). `[I]`

**Mapping of the two mechanisms under Option B.**

- *Raster swizzle*: `raster_type == swizzle` is **not** stored in the texture; it is metadata on
  the surface (as today, `surface_utils.h:162`) and selects the flush/upload converter. `[I]`
- *Memory tiles*: de-tile on import exactly as `VKRenderTargets.cpp:715-746`; the tile descriptor
  is never a Metal object. `tile_align` (`RSXContext.cpp:47-54`) may still over-align the
  *copy range* so a detile reads whole 64×pitch units. `[V]`

### 2.5 Address→resource mapping and guest-address aliasing

**RSX requirement.** `surface_store` keys surfaces by guest address in
`ranged_map<surface_storage_type, 0x400000>` split into colour and depth maps
(`surface_store.h:49-54`), and resolves overlap by **intersection, split, and inheritance**
(`surface_store.h:1012-1069` `prepare_render_target`; `:1117-1157`
`create_surface_from_rsx_section`; `:1177-1230` `truncate_memory_range_by_pitch`). The same XDR
range is routinely reinterpreted: surface → sampled texture (`texture_cache`), texture → surface,
surface → DMA destination. RPCS3 tracks this with `old_contents`
(`surface_utils.h:141`), `superseded_surfaces` and `orphaned_surfaces`
(`surface_store.h:71-74`), and `last_use_tag`/`cache_tag`/`write_tag` (`surface_store.h:132`,
`:78-79`). `invalidated_resources` is capped at **256** (`surface_store.h:76-77`).

**Metal 4 shape — the address map stays; Metal does not provide it.**

- Identity is the **app address map**, reusing `ranged_map`. There is no Metal object keyed by
  guest address. `[V]` (by absence in `01` §3.4's surface list).
- **One canonical view per identity.** For a guest `(base_addr, format, pitch, sample-count)` the
  model holds **one** `MTL::Texture`. Other formats over the same bytes are exposed as
  `MTLTextureViewPool` views *only within legal pixel-format-view groups* (same bit length;
  sRGB↔non-sRGB same compressed format; combined depth-stencil ↔ `x24_stencil8`/`x32_stencil8`)
  and only with `MTLTextureUsagePixelFormatView` (`04` §2.5 `[V]`). Outside those groups, a
  **distinct allocation + copy** is required. `[I]`
- **RSX aliasing = two cases.**

  | RSX case | Mechanism today | Metal 4 |
  |---|---|---|
  | Same address, **same** format/pitch/samples | inherit in place (`old_contents`) | **same `MTL::Texture`**; no copy |
  | Same address, **different** pitch (partial overlap) | split/truncate (`truncate_memory_range_by_pitch`) | **separate textures + explicit copy** of the overlapping region |
  | Same bytes, different format, legal view group | format reinterpretation | `MTLTextureViewPool` view |
  | Same bytes, different format, illegal view group | format reinterpretation | separate texture + copy (block-texel views per `04` §2.5 `[V]`) |
  | Texture→RT / RT→texture | texture_cache invalidation (`is_expired`, `texture_cache.h:583`) | keep the *surface* as canonical while bound; invalidate/convert the cached texture (RPCS3's existing flow) |

- **Do not use Metal heap aliasing for guest aliasing.** Metal `makeAliasable()` / heap-offset
  aliasing exists, but `MTL4VisibilityOptionResourceAlias` is **not-recommended** for ordinary
  aliasing (`README` §4.1/§6, `01` §1.5 `[V]`). Three reasons to reject it here:
  (a) guest alias granularity (arbitrary byte ranges, per-format views) does not match heap-offset
  granularity; (b) aliasing requires the *previous contents* anyway (`old_contents`), so a copy is
  needed regardless; (c) untracked Metal 4 hazards make aliased resources a barrier trap
  (`README` §4.1 `[V]`). `makeAliasable` is reserved for the **transient scratch / temporary-view
  pool** where lifetimes are provably disjoint (e.g. texture_cache temporary subresource views,
  `texture_cache.h:655,697-710`). `[I]`
- **`surface_store` and `texture_cache` roles — reuse / thin / replace.** Per `10` §2.3/`README`
  §6 RN1, the generic caches are kept and only the *traits* are rewritten (`[V]`). Concretely for
  this model:
  - `rsx::surface_store<Traits>` — **reuse**, unchanged search/split/inherit logic; the traits
    supply `surface_storage_type` = the Metal surface, `create_new_surface`,
    `prepare_for_reuse`, `prepare_surface_for_sampling`, `notify_surface_reused` (the
    `VKRenderTargets.h:159-574` interface is the template). `[V]`/`[I]`
  - `rsx::texture_cache<Self, Traits>` — **reuse** the search/invalidation/protection/format
    reconciliation (`texture_cache.h:21,145,581,655`); the traits supply the upload/flush storage
    (the `VKTextureCache.h:21,299` interface is the template). `[V]`/`[I]`
  - **Nothing is replaced**; the RN1 "surface-hazard encoder scheduler" is the *consumer* of
    these caches, not a replacement for them (`10` §9 RN1 `[I]`).

### 2.6 Residency (`MTLResidencySet`)

**The single most important split** (`01` §1.4 `[V]`): *residency* (is memory reachable?) is a
separate, mandatory subsystem from *hazards* (is access ordered?). Apple verbatim: *"Residency sets
don't track hazards, so you need to account for hazards with fences and events."* A barrier does
not make memory resident; `addAllocation` does not order anything.

**Metal 4 shape.** `MTL::ResidencySet` with `addAllocation(s)` / `removeAllocation(s)` /
`commit` / `requestResidency` / `endResidency`; attach to a queue (`MTL4CommandQueue::
addResidencySet`) or a command buffer (`MTL4CommandBuffer::useResidencySet`); ≤ 32 sets per
queue/buffer (`01` §3.4, §5 rank 6 `[V]`).

**Proposed policy (design fork, see Q2).**

- **Tier 1 — persistent set, attached to the queue.** The guest-window `MTL::Buffer` (§2.1) and
  the long-lived placement heaps are added once for the application lifetime
  (`residencySet.addAllocations(...); queue.addResidencySet(set); set.commit();` — the
  Metal-4-basics pattern, `01` §3.5 `[V]`). Mirrors KK's single device-level set
  (`01` §3.1 `[V]`).
- **Tier 2 — per-frame streaming sets, attached to the command buffer.** Resources created by
  `surface_store`/`texture_cache` during a frame are added to a per-frame set, `commit()`-ed, and
  attached with `useResidencySet`; on the matching `MTL4CommitFeedback` the CB's resources are
  `removeAllocation`-ed (in the same order as §2.5's lifetime rules).
- **Commit timing.** `commit()` stages the changes; `requestResidency()` performs the CPU work
  ahead of first GPU use; every resource referenced by a committed command buffer must be in a
  set already `commit()`-ed and attached (`01` §1.3/§3.5 `[V]`). The recording points for this are
  already identified by `11` §3 (`[V]`).
- **Apple guidance.** "fewer residency sets with more resources each" (WWDC25 254, `01` §5 rank 6
  `[V]`) argues against one set per surface; Tier 2 is *batched per commit*, not per resource.
  The partitioning is a **fork** (`01` §7 Q1) and is flagged `[U]` with an experiment (Q2).

### 2.7 Format table (`CELL_GCM_TEXTURE_*` → `MTLPixelFormat`), gaps and fallbacks

Texture formats are the `CellGcmTexture` values (`rsx_value_space.csv:64-90`), mapped per `04`
§2.3 (cross-checked KK + Godot). `direct` = native enum; `view` = legal pixel-format-view;
`sub` = substitute; `emul` = native enum + swizzle/fallback.

| `CELL_GCM_TEXTURE_*` | value | `MTLPixelFormat` | verdict | note |
|---|---|---|---|---|
| `B8` | 0x81 | `R8Unorm` | direct | |
| `A1R5G5B5` | 0x82 | `BGR5A1Unorm` | emul | KK's `A1B5G5R5Unorm` **disabled** (border-colour HW bug); RGB-swapped (`04` §2.3 `[V]`) |
| `A4R4G4B4` | 0x83 | `ABGR4Unorm` + swizzle | emul | KK `is_native=false` (`04` §2.3 `[V]`) |
| `R5G6B5` | 0x84 | `B5G6R5Unorm` | direct | RPCS3 X64-Apple path avoids it → `BGRA8Unorm` (`04` §2.2 `[V]`) |
| `A8R8G8B8` | 0x85 | `BGRA8Unorm` | direct | |
| `COMPRESSED_DXT1` | 0x86 | `BC1_RGBA` (+ `RGB1` swizzle) | emul | sRGB variant `BC1_RGBA_sRGB`; uncompressed fallback exists |
| `COMPRESSED_DXT23` | 0x87 | `BC2_RGBA` | direct | |
| `COMPRESSED_DXT45` | 0x88 | `BC3_RGBA` | direct | |
| `G8B8` | 0x8b | `RG8Unorm` | direct | |
| `COMPRESSED_B8R8_G8R8` | 0x8d | `BGRA8Unorm` | sub | no native enum (`04` §2.2 `[V]`) |
| `COMPRESSED_R8B8_R8G8` | 0x8e | `BGRA8Unorm` | sub | no native enum |
| `R6G5B5` | 0x8f | `B5G6R5Unorm` | direct | same 16-bit path as `R5G6B5` |
| `DEPTH24_D8` | 0x90 | **`Depth32Float_Stencil8`** | sub | **no `Depth24Unorm_Stencil8` on Apple silicon**; RPCS3 already has the fallback (`gpu_formats_support.d24_unorm_s8=false`) (`04` §2.3 `[V]`) |
| `DEPTH24_D8_FLOAT` | 0x91 | `Depth32Float_Stencil8` | direct | needs `PixelFormatView` for depth-only views (`04` §2.5 `[V]`) |
| `DEPTH16` | 0x92 | `Depth16Unorm` | direct | Godot substitutes `Depth32Float` on iOS/tvOS (`04` §2.3) |
| `DEPTH16_FLOAT` | 0x93 | `Depth32Float` | direct | |
| `X16` | 0x94 | `R16Unorm` | direct | |
| `Y16_X16` | 0x95 | `RG16Unorm` | direct | |
| `R5G5B5A1` | 0x97 | `B5G5R5A1Unorm` | direct | component order differs from `A1R5G5B5` (`04` §2.3) |
| `COMPRESSED_HILO8` | 0x98 | `RG8Unorm` | sub | no native HILO enum; RPCS3 samples via `RG8` |
| `COMPRESSED_HILO_S8` | 0x99 | `RG8Snorm` | sub | signed variant of the above |
| `W16_Z16_Y16_X16_FLOAT` | 0x9a | `RGBA16Float` | direct | |
| `W32_Z32_Y32_X32_FLOAT` | 0x9b | `RGBA32Float` | direct | |
| `X32_FLOAT` | 0x9c | `R32Float` | direct | |
| `D1R5G5B5` | 0x9d | `BGR5A1Unorm` | emul | shares the `A1R5G5B5` fallback |
| `D8R8G8B8` | 0x9e | `BGRA8Unorm` | direct | |
| `Y16_X16_FLOAT` | 0x9f | `RG16Float` | direct | |

**Gaps and chosen fallbacks (summary).**

1. **24-bit depth-stencil** (`DEPTH24_D8`) → `Depth32Float_Stencil8`; on Apple silicon this is the
   *only* correct behaviour (`04` §2.3 `[V]`).
2. **Packed 24-bit RGB** (no `CELL_GCM_TEXTURE_*` value; `R8G8B8` internal) → `Invalid`; must be
   emulated or promoted to `BGRA8` (`04` §2.3 `[V]`).
3. **`A1B5G5R5` border-colour HW bug** → `BGR5A1Unorm` (RGB-swapped) or emulated; a real fork
   (`04` §2.3).
4. **`A4R4G4B4`** → `ABGR4Unorm` + component swizzle.
5. **HILO8/HILO_S8** → `RG8Unorm`/`RG8Snorm` substitution.
6. **`DEPTH16`** → `Depth32Float` on iOS/tvOS-style targets (not relevant to macOS 26 target, but
   recorded).

**Surface colour formats** (`surface_color_format`, `gcm_enums.h:1364-1380`, values from
`rsx_value_space.csv:620-633`) map into the same MTL enum set; the notable ones:

| `surface_color_format` | value | `MTLPixelFormat` | note |
|---|---|---|---|
| `r5g6b5` | 3 | `B5G6R5Unorm` | |
| `x8r8g8b8_z8r8g8b8` / `x8r8g8b8_o8r8g8b8` | 4 / 5 | `BGRA8Unorm` | Z/O variants are colour-surface AA encodings |
| `a8r8g8b8` | 8 | `BGRA8Unorm` | |
| `b8` | 9 | `R8Unorm` | |
| `g8b8` | 10 | `RG8Unorm` | |
| `x8b8g8r8_*` | 14 / 15 | `RGBA8Unorm` | byte order differs from `x8r8g8b8` |
| `a8b8g8r8` | 16 | `RGBA8Unorm` | |
| `w16z16y16x16` | 11 | `RGBA16Float` | |
| `w32z32y32x32` | 12 | `RGBA32Float` | |
| `x32` | 13 | `R32Float` | |

**Depth surface formats** (`surface_depth_format2`, `gcm_enums.h:1303`, values
`rsx_value_space.csv:614-615`): `Z16` → `Depth16Unorm`; `Z24S8` → `Depth32Float_Stencil8`
(the uint/float variants select the resolve/readback encoding, not the MTL enum). `Z24S8` is the
one case where RPCS3's own `get_compatible_surface_depth_format` (`TextureUtils.h:417`) plus the
`d24_unorm_s8=false` fallback (`04` §2.3 `[V]`) picks the enum.

**Sample counts.** `CELL_GCM_SURFACE_CENTER_1` (0) → 1; `DIAGONAL_CENTERED_2` (3) → 2;
`SQUARE_CENTERED_4` (4) / `SQUARE_ROTATED_4` (5) → 4 (`rsx_value_space.csv:616-619`,
`gcm_enums.h:1347-1362`). Exploded layout (§2.2) decides `samples_x`/`samples_y`.

---

## 3. Interfaces / data structures — types, lifetimes, ownership

Proposed types are `[I]` designs constrained by §2; field names reuse RPCS3's where the semantics
match (`surface_utils.h:129-181`) so the existing generic caches need only a traits shim.

### 3.1 `mtl_guest_window` — guest XDR projection

```
struct mtl_guest_window {          // one per RSX host memory window
    MTL::Buffer* buffer;           // newBufferWithBytesNoCopy of vm::g_sudo_addr, Shared, page-aligned
    u32          base;             // guest base
    u32          size;             // ~512 MB (main + local)
    u32          host;             // MTLGPUAddress for buffer-offset bindings
};
```
- **Lifetime:** application lifetime; the `deallocator` never frees guest memory (`01` §5 rank 8
  `[V]`).
- **Ownership:** owned by the device layer (`01`); surfaces/textures never own it.
- **Used for:** buffer-typed bindings (vertex/index/constant rings) and as the copy source/dest
  for surfaces/textures. Not a texture source (§2.1).

### 3.2 `mtl_surface` — one RSX surface

Mirrors `render_target_descriptor` (`surface_utils.h:129-181`):

```
struct mtl_surface : rsx::ref_counted {
    MTL::Texture* texture;         // canonical view: format, native_pitch, sampleCount
    MTL::Texture* resolve;         // exploded resolve target (spp>1), persisting & samplable
    u32  base_addr, rsx_pitch, native_pitch;
    u16  surface_width, surface_height;
    u8   spp, samples_x, samples_y;
    rsx::surface_antialiasing aa_mode;
    rsx::surface_raster_type  raster_type;      // metadata for the converter (§2.4)
    rsx::address_range32      memory_range;     // from rsx_pitch, not native_pitch
    std::vector<deferred_clipped_region<mtl_surface*>> old_contents; // aliasing chain
    flags32_t memory_usage_flags, state_flags, msaa_flags;
    // colour/depth format union as in surface_utils.h:169-174
};
```
- **Lifetime:** `ref_counted`; when unreferenced it is moved to
  `surface_store::invalidated_resources`, capped at **256** (`surface_store.h:76-77` `[V]`).
- **Ownership:** the `MTL::Texture` is retained by `mtl_surface`; the allocation sits in the
  placement heap and in the appropriate residency set (§2.6).
- **`old_contents`** holds references (RPCS3's existing cascade-deref warning on destroy,
  `surface_utils.h:185-192`), so release order matters (§4).

### 3.3 `mtl_texture_cache_entry` — cached sampled texture

Mirrors `cached_texture_section` (`texture_cache.h:398-434`):
```
struct mtl_texture_cache_entry {   // Traits::cached_texture_section
    MTL::Texture* texture;         // GPU-native linear storage (Option B, §2.4)
    bool          swizzled;        // guest raster swizzle flag (set by set_swizzled)
    MTL::Texture* temporary_view;  // MTLTextureViewPool handle for deferred subresources
    // protection / format / mip metadata as today
};
```
- **Lifetime:** managed by `texture_cache` invalidation/protection (`texture_cache.h:581`),
  with deferred subresource copies (`texture_cache.h:145,655`).
- **Ownership:** cache owns the texture; the residency set owns reachability; the
  `garbage_collector` must not release before `removeAllocation` (§4).

### 3.4 `mtl_residency_manager`

```
struct mtl_residency_manager {
    MTL::ResidencySet* persistent;     // Tier 1, queue-attached
    MTL::ResidencySet* per_frame[3];   // Tier 2, command-buffer-attached, ringed
    void on_frame_begin(u32 idx);
    void on_bind(MTL::Allocation*);    // add to current per-frame set
    void on_commit_complete(u32 idx);  // removeAllocation for that frame's set
};
```
- **Lifetime:** device lifetime; per-frame sets are ringed with the frame count.
- **Ownership:** must outlive the command buffers referencing its allocations (Metal 4 CBs hold
  **no** strong refs, `01` §1.5 item 4 `[V]`).
- **Attach order:** `set.commit()` **then** `queue.addResidencySet` / `cb.useResidencySet`
  (`01` §3.5 `[V]`).

### 3.5 `guest_address_map`

Reuses `ranged_map` with two instances (colour / depth) exactly as
`surface_store::surface_ranged_map` (`surface_store.h:49-54`), plus the format-view registry per
§2.5. No new shape beyond the existing caches.

---

## 4. Invariants

I1. **One canonical view per identity.** For a guest `(base_addr, format, pitch, sample-count)`
    there is exactly one `MTL::Texture`; every other view is a legal pixel-format view or a copy
    (§2.5 `[I]`).

I2. **Guest bytes are canonical.** A surface/texture read by the guest (via DMA or `cellGcm`) must
    byte-match the guest layout; therefore any GPU-native storage is converted back on flush
    (Option B, §2.4 `[V]` for the existing code, `[I]` for the rule).

I3. **Lazy conversion.** No swizzle/tile conversion happens unless a boundary requires it (§2.4
    carve-out `[I]`).

I4. **Pitch.** `native_pitch ≥ max(rsx_pitch, bpp × width)`; `memory_range` is derived from
    `rsx_pitch × height`, never from `native_pitch` (§2.3 `[I]`).

I5. **Residency before reference.** Every resource (directly or via its heap) referenced by a
    committed command buffer is in a residency set that was `commit()`-ed and attached to that
    queue/buffer (§2.6 `[V]`).

I6. **Residency ≠ order.** Adding to a residency set orders no accesses; ordering is RN1/`HLD-04`
    (§2.6 `[V]`).

I7. **No release before residency removal.** `removeAllocation` precedes `release`, and both
    follow the last commit-feedback for the referencing command buffer (`01` §1.5 item 4, §5
    rank 10 `[V]`).

I8. **Aliased-by-address ⇒ copy or inherit.** Guest-address overlap with a differing pitch/format
    is resolved by inheritance (same resource) or an explicit copy; never by Metal heap aliasing
    for ordinary aliasing (§2.5 `[I]`).

I9. **Exploded resolve persists.** The resolved image and the multisample image both exist as
    `MTL::Texture` for as long as the RSX surface is live; memoryless is never selected for a
    surface the guest may sample (§2.2/`04` §2.7 `[V]`).

I10. **Generic caches are not forked.** `surface_store`/`texture_cache` search, split, inherit,
    and protection logic is reused; only traits change (§2.5, `10` §2.3 `[V]`/`[I]`).

---

## 5. Failure modes & limits

F1. **Non-resident resource reads garbage/zero.** The `01` §1.5 item 1 failure: a valid-looking
    allocation that was never added to a committed residency set. Detection: after the first frame,
    assert every bound resource's heap is in an attached set. `[V]`

F2. **Aliased-resource hazard (untracked).** If Metal heap aliasing *is* used (transient pool),
    an untracked read/write across the alias produces corrupt data unless a barrier with
    `VisibilityOption`/`ResourceAlias` or `Device` precedes it (`README` §4.1 `[V]`). Mitigation:
    disjoint-lifetime discipline only (§2.5).

F3. **Pitch under-size.** A guest pitch below `bpp × width` cannot be represented by an MTL
    texture stride; RPCS3's correction/drop path (`RSXThread.cpp:1644-1657`) must run *before*
    the MTL texture is created. Failure: misconfigured surfaces (CoD3, BLUS30072) render wrong or
    silently drop. `[V]`

F4. **Resolve-then-swap race.** RSX may sample the multisample surface and swap surfaces
    mid-frame; if the resolve target is treated as ephemeral/tile-only, the sampled image is
    stale (`04` §2.7 `[I]`). Mitigation: I9.

F5. **Format-view abuse.** Reinterpreting bytes outside a legal group (e.g. `BGRA8`↔`R32Float`)
    is illegal; the correct path is a separate allocation + copy (`04` §2.5 `[V]`).

F6. **Residency set arithmetic.** > 32 sets per queue/buffer, or per-frame sets leaking across
    frames, causes silent eviction under memory pressure (`01` §5 rank 6 `[V]`). Mitigation:
    ringed, bounded framing (§3.4).

F7. **Release-before-removal use-after-free.** Metal 4 command buffers hold no strong references
    (`01` §1.5 item 4 `[V]`); releasing a texture before its CB completes and before
    `removeAllocation` is a use-after-free. Mitigation: I7.

F8. **24-bit depth.** Any attempt to use `Depth24Unorm_Stencil8` on Apple silicon fails; the only
    supported path is `Depth32Float_Stencil8` (`04` §2.3 `[V]`). No mitigation needed beyond the
    format table.

F9. **Tiled-region over-read.** De-tiling reads whole `64 × tile->pitch` units
    (`GCM_tile_reference::tile_align`, `RSXContext.cpp:47-54` `[V]`); a copy range that is not
    over-aligned may read past the resource. Mitigation: align the copy, not the resource.

---

## 6. Open questions

Q1. **Guest-window import feasibility.** Resolved: **only through the unprotected mirror.**
    The whole 4 GiB guest address space *can* be wrapped by one `newBufferWithBytesNoCopy:`
    (page-aligned, `StorageModeShared`, no-op `deallocator`) — but **not** from the guest view
    `vm::g_base_addr`: one non-writable page anywhere in the range makes the call return `nil`,
    and RPCS3 leaves `protection::no` pages in the guest view for unmapped memory
    (`vm.cpp:1060`). The import therefore reads `vm::g_sudo_addr`, which aliases the same guest
    RAM and is uniformly writable. Measured on A18 Pro / macOS 27 (page size 16384,
    `maxBufferLength` exactly 4 GiB): plain RW 4 GiB → OK; any `PROT_NONE` hole → `nil`;
    `PROT_NONE` + partly committed → `nil`; whole range RW → OK; `MAP_SHARED` sub-range → OK;
    length need not be a page multiple. Implementation: `MTLDevice.cpp:import_guest_window`.
    Resolves I2's foundation and F1.

Q2. **Residency-set partitioning `[U]`** (`01` §7 Q1). Single device-level set (KK) vs Tier-1
    persistent + Tier-2 per-frame streaming (§2.6)?
    *Experiment:* instrument `allocationCount`/`currentAllocatedSize` and per-frame
    `commit`/`requestResidency` cost on a texture-heavy title; compare a single set vs the tiered
    design for frame time and eviction events.

Q3. **Metal heap aliasing for transient surfaces `[U]`.** Is `makeAliasable()` + a
    disjoint-lifetime scratch pool a net win over plain allocation + `garbage_collector`, given
    untracked hazards?
    *Experiment:* implement the texture_cache temporary-view pool (`texture_cache.h:655,697-710`)
    both ways; measure memory and barrier count on a title with heavy RTT temporary views.

Q4. **Memoryless for transient, never-guest-read attachments `[U]`** (`04` §2.7).
    *Experiment:* mark a scratch resolve target `StorageModeMemoryless` and confirm TBDR tile
    behaviour; verify the guest never samples it (grep the replay for a sample of that address).

Q5. **`A1R5G5B5` path `[U]`.** `BGR5A1Unorm` (RGB-swapped) vs a shader-emulated `A1R5G5B5`
    (KK disables the native enum for a border-colour HW bug, `04` §2.3).
    *Experiment:* render a known texture with border sampling and compare against a reference
    (KK/MVK) for the border-colour case.

Q6. **Depth/stencil resolve synchronization `[U]`** (R16). KK forces depth/stencil resolve to meta
    passes because of a Metal bug combining `MultisampleResolve` with a depth/stencil pass
    (`04` §2.7 `[V]`). Does macOS 26 still exhibit it?
    *Experiment:* use `storeAction = MultisampleResolve` with a DS pass on device; compare resolved
    depth against a meta-pass resolve.

Q7. **Sampler-count bypass `[U]`** (R17, `10` §9). Does binding samplers via
    `ArgumentTable.setSamplerState` avoid the 1024 argument-buffer sampler cap?
    *Experiment:* the SDK spike named in `10` §9 R17.

Q8. **Block-texel-view cost `[U]`** (`04` §2.5). Mirror KK (disable layout optimisation, large
    memory cost) vs separate texture + re-upload for `BLOCK_TEXEL_VIEW_COMPATIBLE` semantics.
    *Experiment:* measure both on a title that uses block-texel views.

---

## 7. Provenance

**Primary — RPCS3 tree (read-only; line refs advisory).**
`rpcs3/Emu/RSX/GCM.h:43-61,88-94,129-154` (`CellGcmSurface`, `CellGcmTileInfo`, `GcmTileInfo`);
`gcm_enums.h:1297-1309,1332-1388,1347-1362` (`surface_depth_format2`, `surface_raster_type`,
`surface_color_format`, `surface_antialiasing`); `Common/surface_utils.h:129-199`
(`render_target_descriptor`); `Common/surface_store.h:30-89,1012-1069,1117-1157,1177-1230`
(`ranged_map`, `prepare_render_target`, `create_surface_from_rsx_section`,
`truncate_memory_range_by_pitch`); `Common/texture_cache.h:21,145,398-434,581,655,697-710`;
`Common/TextureUtils.h:416-417` (`get_compatible_surface_color_format`,
`get_compatible_surface_depth_format`); `VK/VKRenderTargets.h:159-574` (surface traits template);
`VK/VKRenderTargets.cpp:694-746` (load + de-tile); `VK/VKTextureCache.h:21,299`
(texture traits, `imp_flush`); `VK/VKTextureCache.cpp:1208-1285`;
`GL/GLTextureCache.cpp:113-117` (`convert_linear_swizzle`); `NV47/HW/nv3089.cpp:525-531,634-670`;
`Core/RSXContext.cpp:9-54` (`get_tiled_memory_region`, `tile_align`); `RSXThread.cpp:1611-1660`
(packed render, pitch fixups); `rsx_decode.h:3350`.

**Primary — inventory (facts).**
`inventory/out/rsx_state_space.csv` (register IDs: `M_NV4097_SET_SURFACE_*`,
`M_NV4097_SET_CONTEXT_DMA_COLOR_A/B/C/D`, `M_NV4097_SET_CONTEXT_DMA_ZETA`,
`M_NV4097_CLEAR_SURFACE`); `inventory/out/clusters/surface.csv` (40 surface-cluster rows);
`inventory/out/clusters/texture.csv`; `inventory/out/rsx_value_space.csv:64-90` (`CellGcmTexture`
formats), `:600-633` (targets, depths, AA, surface colour formats);
`inventory/SCHEMA.md` §4 (disposition vocab), §6 (HLD template).

**Primary — sibling scopes.**
`10-rsx-native-metal4-architecture.md` §2.2/§2.3, §3.1, §4.1, §4.3, §4.4, §9 (RN1);
`01-core-memory.md` §1.3, §1.4, §3.1, §3.2, §3.4, §3.5, §4, §5, §7;
`04-images-formats-samplers-rtt.md` §2.2, §2.3, §2.5, §2.6, §2.7;
`README.md` §3.2 (#28), §4, §6; `11-verification-loop.md` §3.

**Primary — Apple / external.** WWDC25 254 (residency guidance, `MTLLogicalToPhysicalColorAttachmentMap`);
WWDC25 205 / `understanding-the-metal-4-core-api` (all-untracked, resume buckets,
`MTL4::CommandBuffer` `beginCommandBufferWithAllocator`, `beginResume`, `beginUnretainedResume`);
Mesa KosmicKrisp (`kk_physical_device.c`, `kk_bo.c`, `bridge/mtl_residency_set.m`); Godot
`drivers/metal/{pixel_formats.cpp,metal_allocator.cpp}`; metal-cpp (`MTLDevice.hpp`,
`MTLHeap.hpp`, `MTLResidencySet.hpp`, `MTL4ArgumentTable.hpp`).

<!-- doc-end -->
