# 1. Core / instance / device / adapter / feature+limit negotiation / queues / memory heaps & allocation / residency

> **Scope 01 of the `docs/research/metal4/` set.** Peers: `02-submission-sync.md`,
> `03-shaders-pipelines-descriptors.md`, `04-images-formats-samplers-rtt.md`,
> `05-presentation-and-impossible-register.md`.
>
> **Method.** RPCS3 is treated as **read-only**; no source, CMake, test or `.ci` file was
> modified and no build was run. `rpcs3/Emu/RSX/VK/**` may be concurrently edited by another
> agent, so **line numbers are advisory and symbol names are the stable reference**. Facts
> verified by reading the tree are marked **[V]**; inferences/opinions are marked **[I]**.
> Pristine references (Mesa KosmicKrisp, Godot, MoltenVK, metal-cpp, Metal 4 sample, Apple
> docs/WWDC25) were read from `/tmp/metal4-refs/**` and the web; see §8.

---

## 1. Scope & RPCS3 inventory

### 1.1 What this scope owns

Instance/device/adapter selection and negotiation; the *feature* and *limit* surface RPCS3
actually consumes; queues; the memory-heap / allocation / sub-allocation stack; the VMM
pool accounting and OOM-recovery loop; external/host-pointer memory; the `data_heap` ring
buffers; and **residency** (who makes a resource GPU-accessible, and when).

Out of scope (owned elsewhere, referenced here only where memory interacts):
command submission / fences / semaphores → **02**; pipelines, argument tables, descriptor
sets → **03**; image formats, samplers, RTT store/load → **04**; swapchain/present → **05**.

### 1.2 Exact RPCS3 inventory (verified from tree)

| # | Construct | Symbol / file:line | Category |
|---|-----------|--------------------|----------|
| A | Instance creation + extension list | `vk::instance::create` `vkutils/instance.cpp:90`; portability `:129,:202`; enumerate bit `:221` | instance |
| B | Metal-surface / MVK layer settings | `VK_EXT_METAL_SURFACE_EXTENSION_NAME` `instance.cpp:164`; `VK_EXT_LAYER_SETTINGS_EXTENSION_NAME` + `MVK_CONFIG_RESUME_LOST_DEVICE`/`MVK_CONFIG_FAST_MATH_ENABLED` `:134-148` | instance |
| C | Device enumeration | `vk::instance::enumerate_devices` `instance.cpp:251` | adapter |
| D | Physical-device probe | `physical_device::create` `vkutils/device.cpp:241`; features `:15`; props0/1 `:163/:184` | adapter |
| E | Vendor / chip classification | `driver_vendor` (`chip_class.h:45-64`: `MVK`,`HONEYKRISP`,`KOSMICKRISP`); `get_driver_vendor` `device.cpp:286`; `get_chip_class` `:427` | adapter |
| F | Feature negotiation | `physical_device::get_physical_device_features` `device.cpp:15`; `TEST_VK_FEATURE(...)` incl. `TEST_VK_FEATURE(sparseBinding)` `:1018` | features |
| G | Extension-feature flags | `optional_features_support.external_memory_host` (`device.h:191` `get_external_memory_host_support()`); custom border color / multidraw / descriptor-indexing structs `device.h:45-70` | features |
| H | Queue query | `get_queue_count` `device.cpp:432`; `get_queue_properties` `:443`; queues enabled in `render_device::create` `:480` | queues |
| I | Limits object | `physical_device::get_limits` `device.cpp:464` | limits |
| J | Memory-type discovery | `get_memory_mapping` `device.cpp:1034-1164`; result `memory_type_mapping` (`device.h:32-42`) | memory |
| K | Memory-type rebind policy | `render_device::get_compatible_memory_type` `device.cpp:916`; `rebalance_memory_type_usage` `:941`; stored `memory_map` `:864` | memory |
| L | Allocator interface + backends | `mem_allocator_base` (`memory.h:79`); `mem_allocator_vma` (`memory.h:109`, `memory.cpp:163-358`); `mem_allocator_vk` (`memory.h:139`, `memory.cpp:360-450`); `VKMemAlloc.cpp` = `#define VMA_IMPLEMENTATION` | memory |
| M | Device-memory block | `memory_block` (`memory.cpp:457`); `memory_block_host` (`memory.cpp:472-513`) | memory |
| N | Host-pointer (external) memory | `buffer::buffer(dev, usage, void* host_pointer, u64)` (`buffer_object.cpp:97-143`); `_vkGetMemoryHostPointerPropertiesEXT` `:119`; `VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT` `memory.cpp:484` | memory |
| O | RPCS3-private create flags | `buffer_object.h:12-15` (`ALLOW_NULL`, `IGNORE_VMEM_PRESSURE`); `image.h:25-28` (`ALLOW_NULL`, `SHAREABLE`) | memory |
| P | Allocation requests | `memory_allocation_request` (`memory.h:69-77`); `memory_type_info` (`memory.h:36-60`); `memory_heap_info` (`memory.h:62-67`) | memory |
| Q | VMM pools + accounting | `vmm_allocation_pool` (`memory.h:19-33`: `SYSTEM`,`SURFACE_CACHE`,`TEXTURE_CACHE`,`SWAPCHAIN`,`SCRATCH`,`SAMPLER`); `vmm_notify_memory_allocated` `memory.h:205` | memory |
| R | Budget query / OOM recovery | `mem_allocator_vma::get_memory_usage` (`memory.cpp:325-348`, `vmaGetHeapBudgets`/`VmaBudget`); `vmm_handle_memory_pressure` (`VKResourceManager.cpp:269`) | memory |
| S | Host-visible ring heaps | `vk::data_heap` (`data_heap.h:23-110`); `heap_pool_*` flags `:15-21`; `dirty_ranges` `:36` | memory |
| T | Ring snapshot/restore | `data_heap_manager::restore_snapshot`/`get_heap_snapshot` (`VKDataHeapManager.h:14-26`); `heap_snapshot` (`VKGSRenderTypes.hpp:185`); use `VKPresent.cpp:285` | memory |
| U | Null/scratch resources | `null_sampler`/`null_image_view`/`get_scratch_buffer` (`scratch.h:6-15`) | memory |
| V | Deferred destruction | `vk::garbage_collector`/`disposable_t` (`garbage_collector.h:7-57`) | lifetime |
| W | Sampler allocation cap | `VKResourceManager.cpp:55-64` (`maxSamplerAllocationCount`) | limits |
| X | Limits actually read | `maxTexelBufferElements`/`maxUniformBufferRange` (`VKGSRender.cpp:663-677`); `minStorageBufferOffsetAlignment` (`:2151,:2491`); `maxComputeWorkGroupCount` (`VKCompute.cpp:97`); `maxImageDimension1D/2D/3D/Cube` (`image.cpp:22-30`) | limits |

**Verified absences (greps returning nothing across `Emu/RSX/VK`):** `vkFlushMappedMemoryRanges`,
`vkInvalidateMappedMemoryRanges`, `VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT`,
`VK_BUFFER_DEVICE_ADDRESS`/`bufferDeviceAddress`, and any explicit `VK_EXT_memory_budget`
request. RPCS3 therefore depends entirely on **host-coherent** memory and on bind-time
implicit residency. **[V]**

### 1.3 The three Metal 4 shifts that define this scope

Apple's own framing (WWDC25 205 "Discover Metal 4"; `understanding-the-metal-4-core-api`)
states Metal 4 offers **"explicit memory management"**, makes **all resources untracked**, and
requires an explicit **residency** mechanism. Concretely:

1. `MTL4CommandAllocator` (not the app's heap) owns command-buffer memory; the app reasons
   about it as a separate budget (`MTL4CommandAllocator::allocatedSize()`). **[V]**
2. `MTLHazardTrackingMode` is irrelevant to `MTL4CommandQueue`: *"Metal doesn't apply hazard
   tracking to commands you submit to an `MTL4CommandQueue`, even when those commands use
   tracked resources"* and *"the value of an `MTLResource`'s `hazardTrackingMode` has no
   effect on the work you submit to an `MTL4CommandQueue`"*. **[V]**
3. Residency is explicit: `MTLResidencySet` is how buffers/textures/heaps are made
   GPU-accessible, attached to a command buffer (`useResidencySet`) or a queue
   (`addResidencySet`). **[V]**

### 1.4 Residency is **not** hazard tracking (division of responsibility)

This is the single most important conceptual split for this scope, and it is stated
verbatim by Apple:

> "Residency sets don't track hazards, so you need to account for hazards with fences and
> events." — `MTLResidencySet` overview **[V]**

So the two responsibilities are orthogonal and each now needs its own machinery:

| Responsibility | Vulkan (RPCS3 today) | Metal 4 | Owner |
|---|---|---|---|
| **Residency** (is the backing memory reachable by the GPU?) | implicit on `vkBindBufferMemory`/`vkBindImageMemory`; driver keeps it resident | explicit: `MTLResidencySet` + `addAllocation` + `commit` + `requestResidency`/`endResidency`; attach to buffer or queue | **this scope (01)** |
| **Hazards** (is it safe to read/write now?) | implicit via driver tracking + render-pass/queue ordering | none for MTL4 work; explicit barriers / events / fences | **scope 02** |

A correct native backend must do **both**. "Add to residency set" does **not** remove the
need for a barrier, and a barrier does **not** make memory resident.

### 1.5 What scope 5 §9 "rank 4" invalidates in scopes 01 and 02

`05-presentation-and-impossible-register.md` §9 ranks item 4 *"Explicit memory model /
barriers vs Metal 4 all-untracked resources"* as owning **"01 / 02"**. Precisely, it breaks
these assumptions currently baked into scope 01/02 code:

1. **"Bound ⇒ visible."** RPCS3's `memory_block`/`vkBindBufferMemory`/`vkBindImageMemory`
   path (`buffer_object.cpp:46-95`, `image.cpp:58-160`) assumes residency follows binding.
   Under Metal 4 this is false: binding/heap-allocation is **necessary but not sufficient**;
   every allocation must additionally be in a committed residency set attached to the queue
   or command buffer. New failure mode: valid-looking resources read as garbage/zero if not
   resident.
2. **"Coherent ⇒ ordered."** RPCS3 never calls flush/invalidate and relies on
   `HOST_COHERENT` (see §1.2 absences). Metal 4 "untracked" means *hazards are not ordered*,
   not that coherency changed. So even on Apple silicon (where `MTLStorageModeShared` **is**
   coherent — confirmed by `MVKDeviceMemory::isMemoryHostCoherent()` returning true for
   Shared, and by KK's no-op flush/invalidate) the **ordering** argument is void. Any code
   in scope 02 that assumed "same coherent buffer ⇒ safe" must add barriers.
3. **"Render pass orders writes."** RPCS3's render-pass/`vkCmdPipelineBarrier` reasoning
   (owned by scope 02) is replaced for MTL4 by explicit
   `MTL4::CommandEncoder::barrier(afterStages:beforeStages:)`; the memory-type side of a
   barrier (`srcAccessMask`/`dstAccessMask`) has **no** ML4 object — it is purely stage
   ordering. **The access-mask half of every RPCS3 barrier is dropped; only the stage half
   survives.**
4. **"Fences keep memory alive."** Scope 02's `vk::fence`/semaphore teardown assumes the
   driver retains residency while the fence is pending. With Metal 4, resource *retention*
   is the app's job (`MTL4CommandBuffer` holds **no strong references** —
   `understanding-the-metal-4-core-api`), and *residency* is the residency set's job; the
   existing `garbage_collector` (`garbage_collector.h`) must not release before the
   corresponding `MTLResidencySet::removeAllocation`/`endResidency`.

Scope 01's own contribution to closing rank 4 is therefore: **(a)** define the single
device-level residency set(s) and their commit/request ordering, and **(b)** hand scope 02 a
resource-retention contract that outlives in-flight command buffers.

---

## 2. Vulkan/RSX → Metal 4 mapping table

Difficulty: **1** trivial rename · **2** mechanical · **3** real work, well-trodden ·
**4** hard / needs design · **5** unknown or impossible. All RPCS3 symbols verified in-tree.

| # | Vulkan/RSX construct | Metal 4 equivalent | Metal 4 objects/methods | Diff | Notes |
|---|---|---|---|---|---|
| 1 | `VkInstance` / `vkCreateInstance` (`instance.cpp:90,224`) | none (process-wide device) | `MTL::CreateSystemDefaultDevice()`, `MTL::CopyAllDevices()` | 1 | No instance object, no validation layers, no loader. |
| 2 | `VK_KHR_portability_enumeration` + `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR` (`instance.cpp:129,221`) | none | — | 1 | Exists only to unmask MVK; meaningless natively. |
| 3 | `VK_EXT_metal_surface` → layer (`instance.cpp:164`; `metal_layer.mm:9`) | `CA::MetalLayer` directly | `CA::MetalLayer`; layer device/pixel format | 1 | Already a 1-line shim. See scope 05. |
| 4 | `VK_EXT_layer_settings` + `MVK_CONFIG_*` (`instance.cpp:134-148`) | none; fast-math → compiler option | `MTLCompileOptions::fastMathEnabled` / `MTL4Compiler` options | 2 | `disable_msl_fast_math` config must move to the shader compiler (scope 03). |
| 5 | `vkEnumeratePhysicalDevices` (`instance.cpp:251`) | device list | `MTL::CopyAllDevices()` / `CreateSystemDefaultDevice()` | 1 | RPCS3 picks one GPU; Metal likewise. |
| 6 | `vk::physical_device` + `get_physical_device_properties_0/1` (`device.cpp:241,163,184`) | `MTL::Device` + derived props | `hasUnifiedMemory()`, `supportsFamily:`, `registryID` | 2 | Godot `MetalDeviceProperties` is the canonical adapter-probe. |
| 7 | `driver_vendor` / `chip_class` (`chip_class.h:45-64`; `device.cpp:286,427`) | GPU family probe | `supportsFamily(GPUFamilyApple3…Apple10, Metal3)` | 2 | Apple vendorID 0x106B currently → `APPLE_MVK`/`APPLE_HK_generic`; a native backend wants its own enum. |
| 8 | `VkPhysicalDeviceMemoryProperties` heaps (`device.cpp:459`; consumed `:1034`) | one unified heap budget | `recommendedMaxWorkingSetSize()`, `currentAllocatedSize()`, `hasUnifiedMemory()` | 3 | KK reports **exactly one** heap = `recommendedMaxWorkingSetSize` (§3.1). Budget, not an array of heaps. |
| 9 | Memory classes `host_visible_coherent`/`device_local`/`device_bar` (`device.h:32-42`; `device.cpp:1098-1162`) | two storage modes + one mode collapse | `MTL::StorageModeShared`, `MTL::StorageModePrivate` (managed on Intel Macs) | 4 | `device_bar` (host-visible+device-local / ReBAR) has **no** Metal distinction; on unified memory all three collapse to Shared-vs-Private. |
| 10 | `get_compatible_memory_type` / type-index search (`device.cpp:916`; loops `memory.cpp:229-245`) | choose storage mode at resource creation | `MTLResourceOptions` (buffer) / `MTLTextureDescriptor.storageMode` | 3 | Metal has no "memory type index"; the choice is fixed per resource at creation and immutable (`storageMode` "cannot be changed"). |
| 11 | `mem_allocator_vma` (VMA) (`memory.h:109`; `memory.cpp:163-358`; `VKMemAlloc.cpp`) | heap suballocator | `MTLHeapDescriptor` (`HeapTypePlacement`) + `Heap::newBuffer/newTexture(offset:)` | 4 | Godot `MetalHeapAllocator` is the reference: `OffsetAllocator` over placement heaps, dedicated heap for large sizes. |
| 12 | `mem_allocator_vk` raw `vkAllocateMemory` (`memory.h:139`; `memory.cpp:360-450`) | direct resource alloc | `MTLDevice::newBuffer/newTexture` (or `newHeap`) | 4 | Two allocator backends collapse into one Metal allocator; both must reproduce `mem_allocator_base`'s map/unmap/usage contract. |
| 13 | `memory_block` + `vkAllocateMemory` (`memory.cpp:457-470`) | allocation wrapper | `MTL::Allocation` (buffer/texture/heap) | 3 | Base `memory_block` becomes a thin handle over `MTLBuffer`/`MTLTexture`/`MTLHeap`. |
| 14 | `memory_block_host` host-pointer import (`memory.cpp:472-513`; `buffer_object.cpp:97-143`) | page-aligned no-copy buffer import | `newBufferWithBytesNoCopy:length:options:deallocator:` | 4 | **Buffers only, page-aligned, whole-allocation**; no texture analog. See §4. |
| 15 | `VK_MEMORY_PROPERTY_HOST_COHERENT` reliance (no flush/invalidate anywhere) | coherent storage or explicit `didModifyRange:` | `StorageModeShared`; on Intel Macs `StorageModeManaged` + `didModifyRange:` | 4 | On Apple silicon Shared is coherent (KK flush/invalidate are no-ops). On Intel/AMD Macs the managed path resurfaces. |
| 16 | `VK_EXT_memory_budget` via `vmaGetHeapBudgets` (`memory.cpp:325-348`) | device counters | `currentAllocatedSize()` (used), `recommendedMaxWorkingSetSize()` (budget) | 3 | KK: `heapUsage = currentAllocatedSize`; `budget = 0.9 × available` (`kk_get_sysmem_heap_budget`). |
| 17 | VMM pools + `vmm_notify_memory_allocated` + `vmm_handle_memory_pressure` (`memory.h:19-33,205`; `VKResourceManager.cpp:269`) | app-side accounting retained | wrap `MTLHeap`/resource `allocatedSize()`; `setPurgeableState:` for pressure | 3 | Pool taxonomy is backend-agnostic; only the byte source changes. OOM "recovery" becomes purge + `[I]`. |
| 18 | `VK_EXT_memory_budget` absent (not enabled) | n/a | — | — | RPCS3 never enables it; VMA opportunistically queries it. **[V]** |
| 19 | `vk::buffer` alloc + `ALLOW_NULL`/`IGNORE_VMEM_PRESSURE` (`buffer_object.cpp:46-95`; `buffer_object.h:12-15`) | `MTLBuffer` alloc / nullable wrapper | `newBufferWithLength:options:`; app-level null handle | 3 | "Nullable buffer" is an app concept Metal doesn't model; keep the wrapper. |
| 20 | `vk::image` alloc + `ALLOW_NULL`/`SHAREABLE` (`image.h:25-28`; `image.cpp:122-160`) | `MTLTexture` alloc | `newTextureWithDescriptor:` (from heap: `offset:`) | 3 | Format/usage detail → scope 04. |
| 21 | `vk::data_heap` host-coherent ring + VRAM shadow (`data_heap.h:23-110`; `data_heap.cpp:56,80,149,173`) | persistently-mapped ring buffer(+private shadow) | `StorageModeShared` `MTLBuffer` (persistent `contents()`); optional `StorageModePrivate` shadow + blit | 4 | `sync(cmd)` blit logic is scope 02; the heap itself is scope 01. |
| 22 | `data_heap_manager::restore_snapshot` ring positions (`VKDataHeapManager.h:14-26`; `VKGSRenderTypes.hpp:185`) | ring cursor snapshot | app-side offset map over persistent buffers | 3 | Scope 5 §9 rank 24: no Metal heap snapshot API; reimplement bookkeeping. |
| 23 | `null_sampler`/`null_image_view`/scratch pool (`scratch.h:6-15`; `scratch.cpp:100`) | null-binding sentinels | null `MTLTexture`/`MTLSamplerState`; small scratch `MTLBuffer` | 2 | Detail in scopes 03/04. |
| 24 | `vk::garbage_collector` / `disposable_t` (`garbage_collector.h:7-57`) | deferred ObjC `release` | `NS::TransferPtr`/`RetainPtr`; defer `->release()` | 2 | Must respect MTL4 no-strong-ref rule + residency removal ordering (§1.5). |
| 25 | `vk::sampler` alloc cap (`VKResourceManager.cpp:55-64`; `sampler.cpp:101`) | `MTLSamplerState` objects | `newSamplerStateWithDescriptor:` | 3 | Cap 1024 from `maxArgumentBufferSamplerCount`; see scope 04. |
| 26 | **Implicit residency** on bind (no RPCS3 code) | **explicit residency set** | `MTLResidencySet` + `addAllocation`/`commit`/`requestResidency` + queue `addResidencySet` | 3 | New mandatory subsystem; KK uses one device-level set (§3.1). Residency ≠ hazards (§1.4). |
| 27 | Sparse binding (feature-dumped only, `device.cpp:1018`; **never used**) | Metal 4 placement sparse (heap-relative) | `HeapTypeSparse`/`HeapTypePlacement` + `MTL4CommandQueue::updateBufferMappings`/`copyMappingsFromBuffer` | 5 | Positional `VkSparseImageMemoryBind` impossible; §4. Scope 5 rank 10. |
| 28 | Feature negotiation `TEST_VK_FEATURE(...)` (`device.cpp:15-160`) | OS + family gates | `supportsFamily:`, `os_version` gates (Godot `MetalDeviceProperties`) | 3 | Replace `VkPhysicalDeviceFeatures` with a `MetalFeatures` struct. |
| 29 | Queue family/count/properties (`device.cpp:432-458`) | one `MTL4CommandQueue` (+allocators) | `newCommandQueue`/`makeMTL4CommandQueue`; `MTL4CommandAllocator` | 3 | RPCS3 is effectively single-queue; Metal has no queue families. Detail → scope 02. |
| 30 | `get_limits` consumers (`VKGSRender.cpp:663-2491`; `VKCompute.cpp:97`; `image.cpp:22-30`) | `MetalLimits` table | `maxBufferLength`, `maxThreadsPerThreadgroup`, family-table constants | 2 | Godot `MetalLimits` is a ready-made mapping. |
| 31 | `vkMapMemory`/`vkUnmapMemory` (`memory.cpp:425-435,289-303`) | persistent mapping (shared) / none (private) | `MTLBuffer::contents()` (shared only) | 3 | Shared MTLBuffers need no map/unmap; private buffers are unreachable from CPU. |
| 32 | `supports_passthrough_dma` = external-host feature (`VKGSRender.cpp:798`; `device.h:191`) | **no host-pointer import** for the DMA path | (fallback: CPU copy) | 5 | Must force `false`; see §4 and scope 5 rank 10. |

---

## 3. Pristine reference code

### 3.1 Mesa KosmicKrisp — the canonical Vulkan→Metal 4 memory mapping

This is the single most important reference because it is *a Vulkan driver whose backend is
Metal 4*, i.e. exactly the mapping problem scope 01 faces.

- **One heap.** `kk_enumerate_physical_devices` registers **a single** heap:
  `.flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT`, `.size = kk_get_sysmem_heap_size()`, with
  `budget`/`used` callbacks (`kk_physical_device.c` ~`:1180-1186`). `[V]`
- **One memory type.**
  `.propertyFlags = HOST_VISIBLE | HOST_COHERENT | HOST_CACHED | DEVICE_LOCAL`
  (`kk_physical_device.c` ~`:1192-1195`). The entire Vulkan memory-type model collapses to a
  single unified type. `[V]`
- **Budget = Metal counters.** `kk_get_sysmem_heap_used` returns
  `mtl_device_current_allocated_size`; `kk_get_sysmem_heap_budget` =
  `vk_physical_device_heap_budget_from_system(&pdev->vk, 0.9f, heap_size, used)`; heap size
  is the system-memory query (`kk_physical_device.c` `:1010-1038`). `[V]`
- **Limits from the device.** `maxMemoryAllocationCount = 4096`, `maxMemoryAllocationSize =
  pdev->info.max_buffer_size` (`= mtl_device_max_buffer_length`), `maxSamplerAllocationCount
  = max_samplers` (`= mtl_device_max_argument_buffer_sampler_count`),
  `minMemoryMapAlignment = os_page_size`, `nonCoherentAtomSize = 64`
  (`kk_physical_device.c:565,566,624,663,689,1049-1057`). `[V]`
- **Allocation = placement heap + offset buffer.** `kk_alloc_bo` calls
  `heapBufferSizeAndAlignWithLength` → align → `mtl_new_heap` with `MTLHeapTypePlacement`,
  `resourceOptions = KK_MTL_RESOURCE_OPTIONS`, `sparsePageSize = MTLSparsePageSize16`, then
  `newBufferWithLength:options:offset:` (`kk_bo.c`; bridge `mtl_heap.m:23-81`). `[V]`
- **Residency = one device-level set.** `kk_CreateDevice` creates a single
  `mtl_residency_set` with `initialCapacity = 100`; every BO adds its `MTLHeap` (or the raw
  `MTLBuffer` for host imports) to it; `kk_device_make_resources_resident` calls `commit`
  then `requestResidency`; BO destroy removes the allocation; the set is released last
  (`bridge/mtl_residency_set.m:18-76`, `mtl_device.m`, `kk_device.c`). `[V]`
- **Coherency is assumed.** `kk_FlushMappedMemoryRanges` / `kk_InvalidateMappedMemoryRanges`
  are **no-ops returning `VK_SUCCESS`** (KK enables `VK_EXT_map_memory_placed` +
  `VK_EXT_memory_budget`); `kk_bo_map_placed` uses `mach_vm_remap`. `[V]`
- **Device queries.** `mtl_device.m` exposes `maxBufferLength`, `recommendedMaxWorkingSetSize`,
  `currentAllocatedSize`, `heapBufferSizeAndAlignWithLength` (`:151,160,169,238`). `[V]`
- **External memory.** KK supports `VK_EXT_external_memory_metal` (MTLHEAP handle) and
  `VK_EXT_external_memory_host` (host pointer → `newBufferWithBytesNoCopy`, buffers only). `[V]`
- **Workarounds register** (`docs/drivers/kosmickrisp/workarounds.rst`): memory-relevant are
  W6 (coherency), W8 (Rosetta heap size), W11 (`MTL4Compiler` concurrency), W15
  (volatile/coherent miscompile), W18 (stale reads post-barrier). `[V]`

### 3.2 Godot — heap suballocator + adapter probe (ready-made)

- `drivers/metal/metal_allocator.cpp`: `MetalHeapAllocator` creates `MTLHeapTypePlacement`
  heaps with `HazardTrackingModeUntracked`, 3 pools (`PRIVATE` 128 MB, `SHARED` 32 MB,
  `SHARED_WC` 16 MB), `OffsetAllocator` suballocation, and a **dedicated heap** when
  `size > block/2` (`_create_heap` `:135-142`, `_pool_allocate` `:145-216`,
  `_dedicated_allocate` `:218`, `new_buffer` `:295-320`). `[V]`
- `drivers/metal/metal_device_properties.{h,cpp}`: `MetalFeatures` (incl.
  `supports_residency_sets = supportsFamily(Apple6)`, `supports_gpu_address`, argument-buffer
  tier) and `MetalLimits` (`maxBufferLength`, `maxThreadsPerThreadGroup`,
  `minUniformBufferOffsetAlignment`, …). `[V]`
- `rendering_device_driver_metal.cpp`: buffer create maps usage/allocation type →
  `StorageModeShared` / `StorageModePrivate` / `StorageModeMemoryless` +
  `HazardTrackingModeUntracked` (`:112-131`); heap suballocation is enabled only in
  `Barriers` sync mode (`:3015-3026`). `[V]`

### 3.3 MoltenVK — the vocabulary of the memory-type model

- `MVKPhysicalDevice::initMemoryProperties` (`MVKDevice.mm:3376-3441`): one main heap
  (`VK_MEMORY_HEAP_DEVICE_LOCAL_BIT`, size `getVRAMSize()`); on unified memory shared types
  are folded into the main heap and marked `DEVICE_LOCAL`; types = Private, Shared,
  (Managed on non-Apple), Memoryless. `_hostCoherentMemoryTypes = sharedBit`;
  `_privateMemoryTypes = privateBit | memlessBit`. `[V]`
- `getMTLStorageModeFromVkMemoryPropertyFlags` (`:3448-3468`): not host-visible → Private
  (or Memoryless if lazily-allocated on Apple); host-visible+coherent → Shared; host-visible
  non-coherent → Shared (Apple) / Managed (other). `[V]`
- `MVKDeviceMemory::isMemoryHostCoherent()` returns `_mtlStorageMode == Shared`
  (`MVKDeviceMemory.h:66`); managed memory needs `flushToDevice`/`synchronizeResource:`
  (`MVKDeviceMemory.mm:83-110`). `[V]`

### 3.4 metal-cpp headers (the actual API surface)

`MTLDevice.hpp`: `heapBufferSizeAndAlignWithLength:options:` `:363`; `maxBufferLength` `:383`;
`newHeapWithDescriptor:` `:453`; `newResidencySetWithDescriptor:error:` `:496`;
`recommendedMaxWorkingSetSize` `:529`; `currentAllocatedSize` `:345`; `hasUnifiedMemory` `:355`.
`MTLHeap.hpp`/`HeapDescriptor`: `HeapType{Automatic,Placement,Sparse}`, `setResourceOptions`,
`setSparsePageSize`, `maxAvailableSize`, `setPurgeableState`. `MTLResource.hpp`:
`allocatedSize`, `hazardTrackingMode`, `heapOffset`, `makeAliasable`, `setPurgeableState`.
`MTLResidencySet.hpp`: `addAllocation(s)`, `remove…`, `commit`, `requestResidency`,
`endResidency`, `allocatedSize`, `allocationCount`. `MTL4CommandQueue.hpp`: `addResidencySet`,
`removeResidencySet`, `commit:count:options:`, `waitForEvent:value:`, `signalEvent:value:`,
`updateBufferMappings:heap:operations:count:`, `copyMappingsFromBuffer:…`. `MTL4CommandBuffer`:
`useResidencySet`. `[V]`

### 3.5 Metal 4 sample app — authoritative residency usage

`metal-4-basics/…/Metal4Renderer.swift`: creates `MTLResidencySetDescriptor`
(`initialCapacity = 64`), `device.makeResidencySet(descriptor:)` (`:88-90`), populates once
(`residencySet.addAllocations(scene.resources)` `:195-206`), then
`commandQueue.addResidencySet(residencySet)` + `residencySet.commit()` (`:206-207`); the
layer's dynamic residency set is added to the queue (`:292`). This is the minimal correct
pattern for "attach for the app lifetime". `[V]`

---

## 4. Impossible / not-recommended

1. **Positional sparse binding (`VkSparseImageMemoryBind` at an exact offset) — impossible.**
   Metal placement-sparse is **heap-relative**; Apple's sparse tiles are assigned from a heap
   via `updateBufferMappings`, not placed at a caller-chosen address (Metal 4 core API;
   WWDC25 254; scope 5 §9 rank 10). RPCS3 never uses sparse today (`device.cpp:1018` only
   dumps the feature), so this is an accept-and-document item, not a migration blocker. **[V]**
2. **External-host-pointer import for textures — impossible.** Metal exposes
   `newBufferWithBytesNoCopy:` (buffer, page-aligned, whole-allocation) but no external host
   pointer for textures. RPCS3's `memory_block_host` (`memory.cpp:472`) is only ever used from
   `buffer::buffer(..., host_pointer, ...)` (`buffer_object.cpp:97`), i.e. buffers, so the
   mapping is feasible for the actual use — but the imported region must be page-aligned and
   the `deallocator` lifetime must be managed. **[V]/[I]**
3. **`device_bar` / "resizable BAR" as a distinct memory class — not meaningful.** RPCS3
   splits `device_local_total_bytes` vs `device_bar_total_bytes` and even parks a ReBAR heap
   out of stats (`memory.cpp:197-212,329-333`; comment `device.cpp:1119` "parked for future
   use"). Metal has no host-visible+device-local class and no multi-heap query: it has one
   budget. `device_bar` must collapse to `device_local` and the ReBAR bookkeeping deleted. **[V]/[I]**
4. **`VK_EXT_memory_budget` semantics (per-heap budget/usage arrays) — not reproducible
   exactly.** Metal gives one global pair (`currentAllocatedSize`, `recommendedMaxWorkingSetSize`);
   KK approximates a heap budget as `0.9 × available`. RPCS3's per-pool VMM percentages
   (`get_memory_usage`) must be re-cast against a single budget. **[V]/[I]**
5. **`VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT` / memoryless as a Vulkan memory type — not a
   memory-type flag in Metal.** Memoryless is a `StorageMode` on *textures only* (Apple GPU
   guidance), decided at `MTLTextureDescriptor` creation, never a buffer. RPCS3 doesn't use it
   today; a Metal backend may adopt it for transient RTs in scope 04. **[V]**
6. **Buffer device address — not used by RPCS3 and not needed.** Verified absent. Metal has
   `MTLGPUAddress`/`gpuResourceID` but RPCS3's descriptor model doesn't rely on raw device
   addresses. **[V]**
7. **`VK_EXT_external_memory_host` feature gate / `supports_passthrough_dma`
   (`VKGSRender.cpp:798`) — must be forced `false`.** The DMA path (`VKDMA.cpp:306`) that
   relies on it has no Metal equivalent; the copy fallback must be used. This is the concrete
   cost of scope 5 §9 rank 10 in scope 01. **[V]**

---

## 5. Tricky (ranked)

Difficulty is design-effort for a *correct* MVP, not API surface.

1. **(4) Memory-type model collapse.** Three RPCS3 classes (`host_visible_coherent`,
   `device_local`, `device_bar`) must become two storage modes, and every call site that asks
   `memory_type_info::get(dev, access_flags, type_mask)` (e.g. `buffer_object.cpp:68`,
   `data_heap.cpp:80,173`, `image.cpp`) must be re-pointed. The `type_ids` array
   (`memory.h:38`) becomes a single `MTLStorageMode`. Verified against three independent
   Vulkan-on-Metal drivers (KK, MVK, Godot) that all collapse to one type on Apple silicon, so
   the *direction* is certain; the risk is the ~dozens of call sites and the Intel/AMD-Mac
   `managed` corner. **[V]**
2. **(4) Allocator re-tooling (VMA → placement heap).** `mem_allocator_vma`/`mem_allocator_vk`
   (`memory.cpp`) must become one allocator over `MTLHeap` (Godot's `MetalHeapAllocator` is
   the template). RPCS3 relies on VMA features with no direct Metal analog: `BEST_FIT` vs
   `MIN_TIME` strategy flags (`memory.cpp:350-358`), `vmaMapMemory`/`vmaUnmapMemory`, and
   `get_vk_device_memory_offset` (`memory.cpp:317-322`) used to sub-allocate one
   `VkDeviceMemory` across objects. Metal placements are heap offsets; aliasing via
   `makeAliasable` is the analog. **[V]/[I]**
3. **(4) Coherency + residency + hazards triple.** RPCS3 currently gets residency *and*
   ordering *and* coherency "for free". Metal 4 keeps coherency (Shared) but removes ordering
   for MTL4 and makes residency explicit. Splitting the old single assumption into three
   independent contracts (§1.4) is the highest-risk correctness item. KK workarounds W6/W15/W18
   show the compiler/driver side also bites. **[V]**
4. **(3) Budget + VMM accounting + OOM recovery.** `get_memory_usage` (`memory.cpp:325-348`),
   `vmm_handle_memory_pressure` (`VKResourceManager.cpp:269`), and the recovery-retry loops
   (`memory.cpp:393-408`, `:257-272`) assume a resizable budget with per-heap granularity and
   that freeing resources actually returns memory. On Metal, `currentAllocatedSize` is
   monotonic-ish and purge (`setPurgeableState`) is the pressure valve. Re-deriving the pool
   percentages is real work but well-trodden. **[V]/[I]**
5. **(3) `data_heap` rings over persistent shared buffers.** `data_heap` (`data_heap.h:23`)
   already avoids map/unmap for perf (comment `:49-51`) and uses a VRAM shadow. On Metal the
   host-visible ring is a persistent `StorageModeShared` buffer; the shadow is
   `StorageModePrivate` + a blit (`sync`, scope 02). The VMM pool flag machinery
   (`heap_pool_force_vram_shadow` `:20`) survives conceptually. **[V]/[I]**
6. **(3) Residency-set partitioning policy.** KK uses **one** set for the whole device
   (`initialCapacity = 100`). RPCS3 allocates at a far higher rate (per-draw rings, texture
   cache). Apple recommends *"fewer residency sets with more resources each"* (WWDC25 254),
   and each queue/buffer supports up to **32** sets. Choosing static-vs-streaming sets (and
   whether to attach to queue vs command buffer) is a design decision with performance
   consequences. **[V]/[I]**
7. **(3) Ring snapshot/restore (`data_heap_manager`).** Scope 5 §9 rank 24: no Metal heap
   snapshot. The `managed_heap_snapshot_t` (`VKDataHeapManager.h:14`) is a cursor map, so it
   ports to Metal easily; the risk is the interaction with `requestResidency`/`commit`
   ordering and with the async scheduler (scope 02). **[V]/[I]**
8. **(3) External-host buffer import lifetime.** `buffer::buffer(..., host_pointer, ...)`
   (`buffer_object.cpp:97-143`) wraps OS memory that RPCS3 owns. `newBufferWithBytesNoCopy:`
   requires page alignment and a `deallocator` that must not free early. `memory_block_host`
   (`memory.cpp:472`) currently frees the `VkDeviceMemory` but not the host pointer; the
   ownership contract must be re-established. **[V]**
9. **(2) Feature/limits surface.** Straight translation to a `MetalFeatures`/`MetalLimits`
   struct (Godot has it). Low risk. **[V]**
10. **(2) Deferred destruction ordering.** `garbage_collector` (`garbage_collector.h`) must
    order `MTLResidencySet::removeAllocation` *after* the last in-flight command buffer and
    `release()` *after* that; MTL4 command buffers hold no strong refs. **[V]** (contract
    detailed by scope 02).

---

## 6. Minimal Metal 4 code sketch (metal-cpp)

Illustrative only — shows device probe → placement heap → suballocate → residency set →
queue attach, with the untracked/barrier contract called out. Not RPCS3-shaped.

```cpp
#import <Metal/Metal.hpp>              // MTL::Device, MTL::Heap, MTL::ResidencySet
#import <Metal/MTL4CommandQueue.hpp>   // MTL4::CommandQueue, MTL4::CommandAllocator

struct GpuMemory {
    MTL::Device*       device       = nullptr;
    MTL::Heap*         heap         = nullptr;   // HeapTypePlacement, shared or private
    MTL::ResidencySet* residency     = nullptr;   // ONE device-level set (KK pattern)

    bool init(MTL::Device* dev) {
        device = dev;

        const uint64_t budget = device->recommendedMaxWorkingSetSize();   // "heap size"
        const uint64_t used   = device->currentAllocatedSize();           // "heap usage"
        (void)budget; (void)used;

        // --- One residency set for the device (resident for the app lifetime) ---
        auto* rsd = MTL::ResidencySetDescriptor::alloc()->init();
        rsd->setInitialCapacity(128);
        NS::Error* err = nullptr;
        residency = device->newResidencySet(rsd, &err);   // error path omitted
        rsd->release();

        // --- A placement heap we sub-allocate from (VMA replacement) ---
        auto* hd = MTL::HeapDescriptor::alloc()->init();
        hd->setType(MTL::HeapTypePlacement);              // suballocation + aliasing
        hd->setStorageMode(MTL::StorageModeShared);       // coherent, host-visible
        hd->setHazardTrackingMode(MTL::HazardTrackingModeUntracked); // Metal 4: ignored anyway
        hd->setSize(64ull * 1024 * 1024);
        heap = device->newHeap(hd);
        hd->release();

        residency->addAllocation(heap);   // whole heap becomes resident
        residency->commit();              // stage -> apply
        residency->requestResidency();    // do the CPU work ahead of first commit
        return heap != nullptr;
    }

    // Sub-allocate one buffer from the heap (offset form replaces VMA suballocation).
    MTL::Buffer* allocBuffer(size_t bytes, size_t offset) {
        return heap->newBuffer(bytes, MTL::ResourceStorageModeShared, offset);
    }
};

void attach(MTL4::CommandQueue* queue, MTL::ResidencySet* residency) {
    // Attach once for resources used every frame (WWDC25 205/254); <= 32 sets/queue.
    queue->addResidencySet(residency);
    // NOTE: residency does NOT order accesses. Metal 4 treats ALL resources as untracked;
    // stage-to-stage barriers/events are a separate, mandatory obligation (scope 02).
}
```

---

## 7. Open questions for the human

1. **Residency-set partitioning.** Do we mirror KK's single device-level set, or split into
   (a) a "static/persistent" set attached to the queue and (b) per-frame "streaming" sets
   attached to command buffers? Apple advises fewer/larger sets, but RPCS3's allocation churn
   is much higher than a game's. This is the main design fork in this scope.
2. **Intel/AMD Macs.** Do we support the `Managed` storage-mode path (and `didModifyRange:`),
   or target Apple silicon only (KK/MVK unified-memory assumption)? RPCS3's `device_bar`
   logic suggests the team historically cared about discrete GPUs.
3. **`supports_passthrough_dma`.** Confirm we accept forcing it `false` (CPU-copy DMA
   fallback) — this removes a perf feature on `VKDMA.cpp:306`. Any title regression budget?
4. **Buffer device address / `MTLGPUAddress`.** RPCS3 doesn't use it today; is there a future
   bindless plan (scope 03/04) that would want it, changing this scope's allocation layout?
5. **Purgeable memory for OOM pressure.** `vmm_handle_memory_pressure` currently frees caches;
   should the Metal backend instead mark texture-cache resources `volatile`
   (`setPurgeableState`) as Apple recommends? That changes ownership semantics.
6. **`data_heap` shadow.** Keep the VRAM-shadow + blit design (`data_heap.h:35`,
   `heap_pool_force_vram_shadow`), or use persistent shared rings everywhere given unified
   memory? TBDR economics may differ.

---

## 8. Sources

**RPCS3 tree (read-only, verified).** `rpcs3/Emu/RSX/VK/vkutils/{memory.h,memory.cpp,device.h,
device.cpp,instance.h,instance.cpp,chip_class.h,chip_class.cpp,buffer_object.h,buffer_object.cpp,
image.h,data_heap.h,data_heap.cpp,scratch.h,scratch.cpp,garbage_collector.h,metal_layer.h,
metal_layer.mm}`, `rpcs3/Emu/RSX/VK/{VKMemAlloc.cpp,VKResourceManager.cpp,VKResourceManager.h,
VKDataHeapManager.h,VKDataHeapManager.cpp,VKGSRender.cpp,VKGSRenderTypes.hpp,VKPresent.cpp,
VKCompute.cpp,VKDMA.cpp}`. Verified: `VKMemAlloc.h` does **not** exist (only `VKMemAlloc.cpp`,
`#define VMA_IMPLEMENTATION`).

**Peer docs.** `docs/research/metal4/02-submission-sync.md`, `03-shaders-pipelines-descriptors.md`,
`04-images-formats-samplers-rtt.md`, `05-presentation-and-impossible-register.md` (§9 ranks 4,
10, 24), `docs/research/01-drivers-kosmickrisp-moltenvk.md`, `05-metal-backend-northstar.md`.

**Apple documentation (fetched from `developer.apple.com/tutorials/data/...`).**
- Understanding the Metal 4 core API — <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- MTLResidencySet — <https://developer.apple.com/documentation/metal/mtlresidencyset>
- MTLResidencySetDescriptor — <https://developer.apple.com/documentation/metal/mtlresidencysetdescriptor>
- Simplifying GPU resource management with residency sets — <https://developer.apple.com/documentation/metal/simplifying-gpu-resource-management-with-residency-sets>
- MTL4CommandQueue — <https://developer.apple.com/documentation/metal/mtl4commandqueue>
- MTL4CommandQueue.updateMappings(buffer:heap:operations:) — <https://developer.apple.com/documentation/metal/mtl4commandqueue/updatemappings(buffer:heap:operations:)>
- MTL4CommandAllocator — <https://developer.apple.com/documentation/metal/mtl4commandallocator>
- MTL4CommandBuffer.useResidencySet(_:) — <https://developer.apple.com/documentation/metal/mtl4commandbuffer/useresidencyset(_:)>
- MTLHeap — <https://developer.apple.com/documentation/metal/mtlheap>
- MTLHeapDescriptor — <https://developer.apple.com/documentation/metal/mtlheapdescriptor>
- MTLHeapType — <https://developer.apple.com/documentation/metal/mtlheaptype>
- Memory heaps — <https://developer.apple.com/documentation/metal/memory-heaps>
- MTLAllocation — <https://developer.apple.com/documentation/metal/mtlallocation>
- MTLStorageMode — <https://developer.apple.com/documentation/metal/mtlstoragemode>
- Setting resource storage modes — <https://developer.apple.com/documentation/metal/setting-resource-storage-modes>
- Choosing a resource storage mode for Apple GPUs — <https://developer.apple.com/documentation/metal/choosing-a-resource-storage-mode-for-apple-gpus>
- MTLHazardTrackingMode — <https://developer.apple.com/documentation/metal/mtlhazardtrackingmode>
- MTLResource.hazardTrackingMode — <https://developer.apple.com/documentation/metal/mtlresource/hazardtrackingmode>
- MTLResource.storageMode — <https://developer.apple.com/documentation/metal/mtlresource/storagemode>
- MTLPurgeableState — <https://developer.apple.com/documentation/metal/mtlpurgeablestate>
- Resource fundamentals — <https://developer.apple.com/documentation/metal/resource-fundamentals>
- Reducing the memory footprint of Metal apps — <https://developer.apple.com/documentation/metal/reducing-the-memory-footprint-of-metal-apps>
- MTLDevice.makeHeap(descriptor:) — <https://developer.apple.com/documentation/metal/mtldevice/makeheap(descriptor:)>
- MTLDevice.makeResidencySet(descriptor:) — <https://developer.apple.com/documentation/metal/mtldevice/makeresidencyset(descriptor:)>
- MTLDevice.recommendedMaxWorkingSetSize — <https://developer.apple.com/documentation/metal/mtldevice/recommendedmaxworkingsetsize>
- MTLDevice.currentAllocatedSize — <https://developer.apple.com/documentation/metal/mtldevice/currentallocatedsize>
- MTLDevice.hasUnifiedMemory — <https://developer.apple.com/documentation/metal/mtldevice/hasunifiedmemory>
- Metal Feature Set Tables (PDF, "Metal 3 & 4") — <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>

**WWDC25 sessions.** 205 "Discover Metal 4" — <https://developer.apple.com/videos/play/wwdc2025/205/>;
254 "Explore Metal 4 games" — <https://developer.apple.com/videos/play/wwdc2025/254/>;
211 "Go further with Metal 4 games" — <https://developer.apple.com/videos/play/wwdc2025/211/>;
262 "Combine Metal 4 machine learning and graphics" — <https://developer.apple.com/videos/play/wwdc2025/262/>.

**Pristine source trees (`/tmp/metal4-refs/`).**
- Mesa KosmicKrisp: `src/kosmickrisp/vulkan/kk_physical_device.c` (heap/type/budget/limits
  `:565-1300`), `kk_device_memory.c`, `kk_bo.c`, `kk_device.c`, `kk_instance.c`, `kk_queue.c`;
  `src/kosmickrisp/bridge/{mtl_device.m,mtl_heap.m,mtl_residency_set.m,mtl_buffer.m}`;
  `docs/drivers/kosmickrisp/workarounds.rst` (W1–W18).
- Godot: `drivers/metal/{metal_allocator.cpp,metal_allocator.h,metal_device_properties.h,
  metal_device_properties.cpp,rendering_device_driver_metal.cpp,rendering_device_driver_metal.h,
  rendering_context_driver_metal.cpp}`.
- MoltenVK: `MoltenVK/MoltenVK/GPUObjects/{MVKDevice.mm,MVKDevice.h,MVKDeviceMemory.h,
  MVKDeviceMemory.mm,MVKBuffer.mm,MVKImage.mm}`.
- metal-cpp: `Metal/{MTLDevice.hpp,MTLHeap.hpp,MTLResource.hpp,MTLResidencySet.hpp,
  MTL4CommandQueue.hpp,MTL4CommandBuffer.hpp,MTL4CommandAllocator.hpp}`.
- Metal 4 sample: `metal-4-basics/Metal4Basics/Sources/Engine/Metal4Renderer.swift`.
