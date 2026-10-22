# HLD-07 — Shader and pipeline cache: warm start, parallelism, driver pipeline cache

Scope: the *shader and pipeline layer* of the Metal 4 backend. This document
closes three gaps against Vulkan and OpenGL (`HANDOFF.md` §5.3, gaps 11 and 12):
the on-disk shader cache, the multi-threaded pipeline build, and the driver
pipeline cache. It does not change the translation itself (`HLD-05`) or the
`MTL4::RenderPipelineState` contents; it changes *when*, *where*, and *how often*
the translation and the compilation run.

---

## 1. The problem

Today every Metal pipeline is built on the RSX thread, at the draw that first
needs it, and the result is lost at process exit:

- `MTLGSRender` calls `get_graphics_pipeline` from the draw path with
  `compile_async = false` and `allow_notification = false`
  (`MTLGSRender.cpp:1671-1678`).
- `MTLTraits::build_pipeline` ignores `compile_async` and links synchronously
  (`MTLProgramPipeline.cpp:232-256`).
- No object writes to `<ppu_cache>/shaders_cache/`, so no warm start exists.
- The `MTL4::Compiler` is created with a bare descriptor
  (`MTLDevice.cpp:86`), so no compiled pipeline is ever captured or reused.

The costs, in order of size:

| step | artifact | where it runs | reproducible across boots |
|---|---|---|---|
| 1 decompile RSX ucode → GLSL | GLSL text | RSX thread | yes |
| 2 glslang: GLSL → SPIR-V | SPIR-V | RSX thread | yes |
| 3 SPIRV-Cross: SPIR-V → MSL | MSL text | RSX thread | yes |
| 4 `MTL4::Compiler::newLibrary`: MSL → AIR | `MTL::Library` | Metal compiler | yes, per device+OS |
| 5 `MTL4::Compiler::newRenderPipelineState`: link | `MTL::RenderPipelineState` | Metal compiler | yes, per device+OS |
| 6 shared cache lookup | `mtl::pipeline` | RSX thread | n/a |

Steps 4 and 5 run on the Metal compiler. They are the only steps that can
stall the frame for a long time. Steps 1–3 are pure CPU.

## 2. What the other backends do

| aspect | Vulkan | OpenGL | Metal 4 today |
|---|---|---|---|
| On-disk set | `vk::shader_cache<vk::pipeline_props, vk::program_cache>` (`VKGSRender.cpp:721`) | `gl::shader_cache` | **none** |
| Class string | `"vulkan"`, version `v1.95` | `"opengl"` | **none** |
| Warm start | `m_shaders_cache->load(...)` in `on_init_thread` (`VKGSRender.cpp:1382-1390`) | same | **none** |
| Populated by | `notify_pipeline_compiled` → `store` (`VKGSRender.cpp:707-714`) | same | callback is empty (`MTLGSRender.cpp:451`) |
| Draw-path notification | `allow_notification = true` (`VKGSRender.cpp:2038`) | `false` | `false` |
| Preload workers | `utils::get_thread_count()` (`rsx_cache.h:277`) | 1 | 1 |
| Driver pipeline cache | `VkPipelineCache` (`g_pipeline_cache`) | none | **none** |
| Runtime build | worker pool, `COMPILE_DEFERRED` | inline | inline |

The Metal backend therefore differs from Vulkan in three ways, and those are the
three items of the work order.

## 3. Cache layers and their invalidation regimes

The six steps split into three layers. Each layer has a different invalidation
rule, so each layer is a different object with a different key.

| layer | content | key | invalidated by | owner |
|---|---|---|---|---|
| L1 in-memory | `mtl::pipeline` | vp id + fp id + `pipeline_properties` | process exit | `program_state_cache` |
| L2 on-disk set | `pipeline_properties` + vp/fp ucode | file name = vp hash + fp hash + props hash + state hash | `sizeof(pipeline_data)` change, version prefix change | `rsx::shaders_cache` |
| L3 driver cache | compiled pipeline binaries | Apple's descriptor hash | GPU, macOS, Metal toolchain | `MTL4::PipelineDataSetSerializer` |

L1 exists. L2 and L3 are the work.

L2 is device-independent: it stores the RSX ucode and the pipeline key only. The
MSL, the library, and the pipeline object are rebuilt from them. L2 therefore
survives a Metal toolchain change, and it needs no device identity in its key.
This is the Vulkan contract, and the backend keeps it.

L3 is device-specific and opaque. Apple states an archive only accelerates
compilers it is attached to, and that an OS upgrade can force recompilation from
the archive's Metal IR (`06` §2.4). L3 is therefore a pure accelerator: it must
always be safe to delete. The archive file name carries the GPU registry id and
the macOS major version, so a GPU or OS change selects a different file.

## 4. Design

### 4.1 L2: the on-disk pipeline set

Reuse `rsx::shaders_cache` without a structural change. The Metal backend adds
the same surface that `vk::program_cache` adds (`VKProgramBuffer.h:79-109`):

| method | purpose |
|---|---|
| `get_hash(const pipeline_properties&)` | the props half of the file name |
| `get_hash(const RSXVertexProgram&)` | the vertex ucode hash |
| `get_hash(const RSXFragmentProgram&)` | the fragment ucode hash |
| `preload_programs(hint, vp, fp)` | decompile and translate, so the link step only links |
| `add_pipeline_entry(vp, fp, props, args...)` | build one pipeline from a stored key |

The disk payload type is `mtl::pipeline_properties`. It is member-wise stable,
it is already the L1 key, and `pack`/`unpack` copy it by value
(`rsx_cache.h:381,419`). A `pipeline_properties` struct is trivially copyable, so
`fs::write_file(..., &data, sizeof(data))` and `f.read(&pdata, f.size())` stay
valid (`rsx_cache.h:112,341`). `sizeof(pipeline_data)` is the ABI guard
(`rsx_cache.h:104-109`): it removes entries written by an older layout.

The class string is `"metal"`. The version prefix is `v1`. A change to the
*meaning* of a `pipeline_properties` field without a size change must bump the
prefix; the size check cannot see it.

The alias is `mtl::shader_cache`. It lives in `frame_resources`, next to the
`program_cache` it indexes, so the two share a lifetime and no backend header
grows a `shaders_cache` dependency.

`allow_notification` becomes `true` at the draw site. Without it the
`notify_pipeline_compiled` callback never runs and the cache stays empty. This is
the single line that turns the whole layer on.

### 4.2 L3: the driver pipeline cache

`MTL4::PipelineDataSetSerializer` is the analogue of `VkPipelineCache`. It is an
attribute of the compiler, not of a draw:

```
MTL::Device::newPipelineDataSetSerializer(descriptor)   -> serializer
MTL4::CompilerDescriptor::setPipelineDataSetSerializer(serializer)
MTL::Device::newCompiler(descriptor)                    -> compiler
  ... every pipeline the compiler builds is captured ...
serializer->serializeAsArchiveAndFlushToURL(url, error)  -> the archive file
MTL::Device::newArchive(url, error)                      -> MTL4::Archive
MTL4::CompilerTaskOptions::setLookupArchives(array)      -> reuse on the next build
```

The device owns all four objects, because the compiler is per device (workaround
W11) and the lifetime of the serializer must equal the lifetime of the compiler
(`MTLDevice.cpp:116-137`). The archive file name is

```
<ppu_cache>/shaders_cache/metal/archive-<registry-id>-<macos-major>.bin
```

`setConfiguration` takes a mask, and on this OS build only one value makes the
flush write:

| configuration | `serializeAsArchiveAndFlushToURL` |
| --- | --- |
| `CaptureBinaries` | writes the archive |
| `CaptureDescriptors` | returns NO |
| both | returns NO |

The nil case returns NO with a null error object, which is why the failure first
read as "unknown": nothing in the message said what went wrong. The header
documents `CaptureBinaries` as an addition to `CaptureDescriptors`, so the mask
that the field would suggest is the one that fails. The archive holds binaries
and the lookup side reads them, so the mask is `CaptureBinaries` alone
(`MTLDevice.cpp:126`). Measured with a standalone metal-cpp program that creates
a serializer, builds two pipelines on one compiler and flushes to a URL: the
combination failed in every run, `CaptureBinaries` alone wrote a 39,568-byte
archive in every run. The error text of both archive paths carries the domain and
the code (`archive_error_text`, `MTLDevice.cpp:19-31`), because a failure can
also carry an error whose description is empty.

Two further properties of the API shape the layer:

- The flush is destructive. A second call on the same serializer returns NO with
  `MTLBinaryArchiveDomain` code 3, "Nothing to serialize". `on_exit` is therefore
  the only call site, and it stays the only one.
- A warm boot re-captures what it looks up. A boot that builds nothing new and
  flushes at the end writes the same set again, so the layer cannot shrink a good
  archive to an empty one.

A failed flush leaves no file behind, so the next boot reads either a complete
archive or none.

Three rules hold the layer safe:

- The layer is a pure accelerator. A missing, unreadable, or rejected archive
  must leave the renderer working. `newArchive` failure is a log line.
- The serializer is created whenever the on-disk cache is enabled, even on a
  cold start, or nothing would ever be recorded.
- The write happens in `on_exit`, before the device is dropped
  (`MTLGSRender.cpp:662-667`). RPCS3 reaches `on_exit` on a clean stop only: it
  installs no signal handler, so a process killed with `SIGTERM` writes no
  archive. That is the intended trade. The archive is a step-5 accelerator that
  a later boot rebuilds, and the L2 set is already on disk per pipeline
  (`rsx_cache.h:294-346`), so nothing is lost that the next boot cannot redo.

What L3 does *not* cover: the library. To build a `LibraryFunctionDescriptor`
the renderer must hold an `MTL::Library`, and to hold the library it must
compile the MSL. L3 can therefore skip step 5 and cannot skip step 4. Step 4 is
the next lever, not this one (§8).

### 4.3 Parallelism

The preload has two phases, and both are safe to run on several threads:

- `load_shaders` reads the files and runs steps 1–3 (`rsx_cache.h:82-130`).
- `compile_shaders` runs steps 4–6 (`rsx_cache.h:132-151`).

`rsx_cache.h:277` gives Vulkan all host threads for both phases and gives every
other backend one thread. Metal joins Vulkan. `MTL4::Compiler` is documented to
multithread internally and the backend keeps one compiler per device, so the
worker count is a queue depth, not a compiler count (`06` §2.5).

Two shared objects become reachable from several threads and must be fixed:

- `shader_program::ensure_library` writes `m_library` with no lock
  (`MTLProgramPipeline.cpp:88-111`). Two pipelines that share a program can call
  it at the same time. A mutex in `shader_program` makes the creation once.
- `validate_pipeline_properties` increments a function-local `static` counter
  (`MTLProgramPipeline.cpp:206-213`). Make it atomic.

The runtime build stays synchronous. `program_state_cache` requires
`build_pipeline` to return the pipeline pointer before it returns
(`ProgramStateCache.h:438-446`), so an asynchronous build must return null and
let a later frame find the object. Vulkan hides that gap with the shader
interpreter (`VKGSRender.cpp:2059-2070`). Metal has no interpreter, so an
asynchronous build would draw nothing for the frame. With L2 and L3 in place the
runtime builds are the rare, unseen-shader case. Recorded as §8.

## 5. Interfaces

```
mtl::program_cache : program_state_cache<MTLTraits>
    u64  get_hash(const pipeline_properties&)
    u64  get_hash(const RSXVertexProgram&)
    u64  get_hash(const RSXFragmentProgram&)
    void preload_programs(rsx::program_cache_hint_t*, const RSXVertexProgram&, const RSXFragmentProgram&)
    template <typename... Args>
    void add_pipeline_entry(RSXVertexProgram&, RSXFragmentProgram&, pipeline_properties&, Args&&...)

mtl::shader_cache = rsx::shaders_cache<mtl::pipeline_properties, mtl::program_cache>

mtl::shader_context
    MTL4::Compiler*             compiler
    MTL4::CompilerTaskOptions*  options     // the L3 lookup, null on a cold start
    std::string                 vertex_entry
    std::string                 fragment_entry

mtl::device
    MTL4::CompilerTaskOptions* pipeline_task_options() const
    void                       flush_pipeline_archive()
```

`shader_context` grows one field. It is the argument the shared cache forwards to
`build_pipeline` (`ProgramStateCache.h:444`), so the lookup archives reach every
build that the preload and the draw path issue.

## 6. Integration points

| site | change |
|---|---|
| `rsx_cache.h:277` | the preload worker count includes `video_renderer::metal` |
| `MTLProgramPipeline.h` | the `program_cache` surface; `shader_cache`; one `shader_context` field |
| `MTLProgramPipeline.cpp` | the mutex in `ensure_library`; atomic counter; the `CompilerTaskOptions` argument |
| `MTLDevice.h` / `MTLDevice.cpp` | serializer, archive, task options, path, flush |
| `MTLGSRender.cpp:451,468` | the notify callback and the `shader_cache` member |
| `MTLGSRender.cpp:571-615` | create and load the cache |
| `MTLGSRender.cpp:1669-1678` | `allow_notification = true`; pass the task options |
| `MTLGSRender.cpp:662-667` | flush the archive |

No Vulkan or OpenGL file changes except the one worker-count line.

## 7. Failure and invalidation

| event | behaviour |
|---|---|
| empty cache (first run) | every program compiles; the progress dialog reports it; entries are written |
| `pipeline_data` size mismatch | the entry file is removed and skipped (`rsx_cache.h:104-109`) |
| props meaning change | bump the version prefix |
| MSL emitter change | L2 stays valid (it stores no MSL); L1 and the L3 archive are rebuilt |
| GPU change | a different archive file; L2 stays valid |
| macOS change | a different archive file; L2 stays valid |
| archive rejected | log line, no lookup archives, everything recompiles |
| archive write fails | log line with the domain and the code; the next boot runs cold |
| wrong serializer configuration | the flush fails with no error object at all; §4.2 |
| `disable_on_disk_shader_cache` | both layers off: no root path, no archive path |
| concurrent `ensure_library` | the mutex makes the library once |
| concurrent compiler tasks | one compiler; the count is a queue depth |

## 8. Out of scope, and why

**A stage-artifact cache for steps 2–4.** The MSL is a pure function of the RSX
ucode, so it can be stored per program hash next to the raw ucode, and the
compiled library can in principle be loaded again (`MTL::Device::newLibraryWithData`,
`MTL4::Compiler::newDynamicLibrary`, `MTL4::Archive::newBinaryFunction`). All
three routes are `[U]` and each is a second codepath in the shader layer. Measure
step 4 with the L2+L3 build first: this document adds `MTL_PERF_SCOPE` rows that
separate it (`program/library`, `program/pipeline`, `archive/flush`).

**An asynchronous runtime build.** Needs the interpreter fallback that
`program_state_cache` presumes. §4.3.

**Shipped precompiled libraries.** `06` §2.3 rejects them: the game shader set is
per title and unbounded, and it drags the Metal toolchain into the build.

## 9. Verification

Every probe of the backend reads the one switch, `MTL_PROBE`: unset or `0` runs
nothing, `1` turns on the counters, the periodic report, the acquire-stall probe
and the main-thread heartbeat, and `2` or `trace` adds the verbose `MTL_DBG` log.
`MTL_PROBE_INTERVAL` (frames between reports) and `MTL_PROBE_STALL_MS` (acquire
stall threshold in ms) tune two of them and are read only while the switch is
set. The flag reaches a probe only when the switch, not the build, is the control:
the verbose log costs real time per guest method, so it is the higher level.

1. `./build/bin/rpcs3_test` stays green.
2. Isolated round trip: a standalone metal-cpp program builds two pipelines
   against a serializer and flushes to a URL. §4.2's table is its result, and a
   read process that loads the archive gets a state back from
   `MTL4::Archive::newRenderPipelineState` for both descriptors.
3. Cold boot: delete `<ppu_cache>/shaders_cache/pipelines/metal` and
   `shaders_cache/metal`. Boot once. The directory and the archive must appear,
   and the perf table must show `program/library` and `program/pipeline` rows.
   The log must not report a failed archive creation. The boot must end in a
   clean stop, not a signal: the archive is written from `on_exit` (§4.2), so a
   killed process proves nothing about step 2. The clean stop is the game's own
   exit or the game stopped from RPCS3; an Apple Event quit is not a stop path
   (see HANDOFF §7).
4. Warm boot: boot again. The pipeline set must load, the log must carry
   `Metal: pipeline archive loaded from`, and the report must show the same
   pipeline count with a lower `program/pipeline` total.
5. `MTL_PERF_SCOPE("archive/flush")` at exit: report the time of the flush.
6. Draw path: no `Metal 4: the program cache returned no pipeline`, and the
   `present/displayed` count against `present/flip` must say whether the display
   consumes what the renderer presents.
7. `MTL_PROBE=2` once: the counts of decompile, translate, and link must match.
8. Delete-and-rerun: a deleted archive must not change behaviour, only timing.

## 10. Questions

- Does `lookupArchives` hit at all when the library is rebuilt from source each
  boot? Answered by §9 step 2: the archive matches and returns a state, so the
  lookup is real. The size of the win stays a measurement (§9 step 4), because
  the OS driver cache also warms `program/pipeline` across processes.
- Is a synchronous `MTL4::Compiler` call from several threads safe? `[U]`. The
  W11 note requires one compiler, not one caller. §9 step 6 answers it.
- Which of the three step-4 routes is cheapest? `[U]`. §8.

<!-- doc-end -->
