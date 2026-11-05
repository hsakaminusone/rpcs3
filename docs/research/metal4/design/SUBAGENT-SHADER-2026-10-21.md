# Asynchronous Metal 4 shader, library, and pipeline compilation

**Status:** implemented in the Metal 4 backend. Integration build is green and the GPU-free suite passes. The on-device RR7 gate has not run yet.
**Scope:** the Metal 4 backend only. The CPU shader chain (decompile, GLSL, SPIR-V, MSL) stays synchronous. The MSL library compile and the render pipeline link are asynchronous.
**Author:** shader subagent, 2026-10-21. Implementation followed in the main session.
**Required output:** originally this file only. Steps 0-6 are now in the tree; the acceptance runs remain.

## 0. Document control

### 0.0 Implementation status

Landed in the tree: validate-before-key; the per-program library state machine; the
`pipeline_jobs` registry with asynchronous library and link stages; the draw-path miss contract; the
`begin()`/`end()` drains; the asynchronous warm-archive preload; the bounded teardown with archive
gating; device-loss abandonment; and the autorelease pools. The synchronous preload and build paths
remain as the fallback. The build is green and the GPU-free suite passes. Not yet done: the T3 key
equivalence test, the forced-failure and teardown tests, and the T1/T2/T10/T11 RR7 runs. No RR7 run
has used this binary.

### 0.1 Evidence snapshot

The line numbers in this document refer to this working tree:

| file | SHA-256 |
|---|---|
| `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp` | `19cde28f8f0c5cae6c99f1eac26ef46602bee63a7b8ed4920a70b4b8a04496ee` |
| `rpcs3/Emu/RSX/MTL/MTLProgramPipeline.cpp` | `42fbc7b638baf6a26ea6d6acc256871639ef248d6f0b75a6c8b9ce7d0f6fdb4b` |
| `rpcs3/Emu/RSX/MTL/MTLDevice.cpp` | `dd353978446a31cf5aaa6608b1ec010fcf8a4ee17125baa83b9a8aeee4ca5d5c` |

The tree moved by 20 lines in `MTLGSRender.cpp` during the investigation. Resolve a site by function name if the line is stale. The function names in this document are the stable key.

### 0.2 Terms

- **Cache entry.** The `std::unique_ptr<pipeline>` in `program_state_cache::m_storage`. A *null cache entry* is the empty `unique_ptr` that `ProgramStateCache.h:396` inserts to stop a second link of the same key. This is the existing shared-cache term. It is not a stand-in for a design.
- **Job.** One Metal-side record that owns the work of one pipeline key: the two libraries, the link, the callback, and the completion state.
- **Library stage.** `MTL4::Compiler::newLibrary` on one `shader_program`.
- **Link stage.** `MTL4::Compiler::newRenderPipelineState` for one pipeline key.
- **Drain.** Move a completed job to its cache entry on a known thread. A drain is not a wait.
- **Miss.** A pipeline lookup that has no usable cache entry. The draw is skipped, not replayed.
- **Severity.** H: freeze, black frame with no recovery, or data race. M: visible stall or wrong result with recovery. L: performance only.

### 0.3 The specification rule

RSX behaviour is the specification. Vulkan and OpenGL are evidence only. Where this document says "VK does X", it means X is a known-working implementation of the shared RSX contract, not that Metal must copy the host API shape.

## 1. Decision summary

| # | Decision | Reason |
|---|---|---|
| D1 | The guest-to-MSL chain stays synchronous on the calling thread. | `ProgramStateCache` must have complete program data before it forms the key. VK and GL decompile on the calling thread. Moving this chain adds a second asynchronous state machine with no measured win. |
| D2 | The MSL library compile and the render pipeline link are asynchronous. | These are the Metal driver calls that block for 12.8 ms and 336.5 ms in the measured log. |
| D3 | A draw that misses a pipeline is skipped with `execute_nop_draw()`, not queued and replayed. | This is the existing VK/GL miss contract (`VKDraw.cpp:1316-1324`, `GLDraw.cpp:949-957`). Replay needs a full draw-state snapshot and risks a different result. |
| D4 | A flip never waits for a shader compile. It presents the frame it has. | A present that waits on a compiler is the freeze this document removes. |
| D5 | All job submission and all cache publication happen on one thread at a time, under one registry mutex. Completion handlers only retain the delivered object and publish a state. | `MTL4::Compiler` is one object per device (W11). Arbitrary callback work on a Metal thread would touch `program_state_cache` and file I/O from a thread with no established lifetime. |
| D6 | The completion drain runs on the renderer thread at `begin()` and `end()`, and on the preload thread in `wait_for_idle()`. | These are the existing frame and load boundaries. `flip()` stays short. |
| D7 | The preload stays synchronous in the first patch. It becomes asynchronous in a later patch, with one bounded wait and progress updates. | The first patch must be small and must not change the loading dialog contract. |
| D8 | The L2 disk entry is written by one thread: the drain thread, through the existing `notify_pipeline_compiled` callback. | Competing preload workers must not write one cache set. The existing callback already receives exactly the pipeline, vertex program, and fragment program. |
| D9 | The L3 archive is flushed only after the job registry reports idle. | `serializeAsArchiveAndFlushToURL` is destructive and must see a stable serializer. |
| D10 | Teardown performs one bounded wait (10 s) for compiler tasks. On timeout the device and the frame resources are leaked, not freed. | Matches the recorder's existing 10 s policy (`MTLCommandRecorder.cpp:16,123-161`). Freeing a device whose compiler still runs is undefined. A library handler also holds a program address, so the program cache must stay alive after a timeout. |

## 2. Evidence

### 2.1 Confirmed measurements

The first session in `RPCS3.log` reports these rows at `:17482-17495`:

| Row | Calls | Total | Average | Max |
|---|---:|---:|---:|---:|
| `program/pipeline` | 50 | 2095.358 ms | 41.9 ms | **336.454 ms** |
| `program/library` | 100 | 140.132 ms | 1.4 ms | 12.770 ms |
| `program/compile vertex` | 35 | 1259.992 ms | 36.0 ms | **184.959 ms** |
| `program/compile fragment` | 40 | 190.403 ms | 4.8 ms | 22.014 ms |
| `present/flip` | 299 | 2050.259 ms | 6.9 ms | **1128.423 ms** |
| `recorder/begin frame/wait feedback` | 300 | 1024.110 ms | 3.4 ms | **1024.073 ms** |

The coverage table at `:64186-64219` reports, over 306 frames:

- `draw/vertices` 3.
- `present/compose refused` 302.
- `present/compose ran` 4.
- `cache/pipeline miss` 75.
- `cache/pipeline hit` 25.
- `shader/fragment build` 40.
- `shader/vertex build` 35.

The 75 misses are library misses: 35 vertex builds plus 40 fragment builds. The 50 `program/pipeline` calls are 50 distinct pipeline keys.

The warm session at `:64112-64113` reports `recorder/readback wait` 5 calls, 134.310 ms total, **124.718 ms max**, all of it in `gpu wait`. That is the active label-release path (§7.2).

### 2.2 Confirmed synchronous call sites

| # | Site | What blocks | Severity | Confidence |
|---|---|---|---|---|
| S1 | `MTLGSRender.cpp:1986-1993` (`MTLGSRender::end`) | Calls `get_graphics_pipeline` with `compile_async=false` (the `false` at `:1991`). Every first-use pipeline links on the RSX thread. | H | confirmed |
| S2 | `MTLProgramPipeline.cpp:288` | `(void)compile_async;` discards the flag. `build_pipeline` cannot be asynchronous at all. | H | confirmed |
| S3 | `MTLProgramPipeline.cpp:297` | Calls `vertex.ensure_library(extra.compiler)` and `fragment.ensure_library(...)` before the link. Both are synchronous. | H | confirmed |
| S4 | `MTLProgramPipeline.cpp:319` | Calls `extra.compiler->newRenderPipelineState(descriptor, extra.options, &error)`, the synchronous overload. | H | confirmed |
| S5 | `MTLProgramPipeline.cpp:91-129` (`ensure_library`) | Takes `m_library_mutex` at `:96` and holds it across the whole `compile_msl` at `:120`. All pipelines that share the program serialize behind it. | M | confirmed |
| S6 | `MTLShader.cpp:207-227` (`compile_msl`) | Calls `compiler->newLibrary(descriptor, error)` at `:223`, the synchronous overload. | H | confirmed |
| S7 | `MTLShader.cpp:214-221` | Creates `MTL4::LibraryDescriptor`, `NS::String`, and `MTL::CompileOptions` on the calling thread. The RSX thread has no autorelease pool. | M | confirmed |
| S8 | `MTLProgramPipeline.cpp:303-311` | Creates two `LibraryFunctionDescriptor` objects, two `NS::String` entry names, and one `RenderPipelineDescriptor` on the calling thread, with no autorelease pool. | M | confirmed |
| S9 | `MTLProgramPipeline.cpp:321-323` | Releases the descriptors immediately after the synchronous call. Correct for the synchronous form; becomes a lifetime question for the asynchronous form (§3.4). | L | confirmed |
| S10 | `MTLGSRender.cpp:703-726` | The preload calls `shaders->load(...)`, which reaches `add_pipeline_entry` with `compile_async=false` (`MTLProgramPipeline.h:166-169`). All 50 pipelines at init build synchronously. | H | confirmed |
| S11 | `rsx_cache.h:133-151` | `compile_shaders` gives Metal several workers (`:281-283`), but each worker calls the synchronous `build_pipeline`. The worker count raises parallelism only across independent keys; each key still blocks its worker. | M | confirmed |
| S12 | `MTLGSRender.cpp:731-769` (`on_exit`) | Flushes the archive at `:764` with no wait for compiler tasks. There are none today because S4 is synchronous. The asynchronous design makes this a defect unless it is fixed. | H after the change | confirmed |

### 2.3 Additional confirmed defects found in this pass

**A. `validate_pipeline_properties` runs after the key is copied, and only for pre-existing programs.**
`ProgramStateCache.h:368` copies `pipeline_key key = {..., pipeline_properties}`. The validate call is at `:375` and runs only when both programs already existed (`:372`). Therefore the first build of a key skips validation: the descriptor uses the guest write mask, not the fragment program's reflected colour-output mask. The key records the unvalidated mask as well, so two draws that differ only in bits the fragment does not use build two identical pipelines. Severity M (an attachment with no fragment output can be written). Confidence: confirmed by reading the order of the statements.

**B. A failed link leaves a permanent null cache entry.**
`ProgramStateCache.h:396` inserts the null entry before `build_pipeline` at `:438`. If `build_pipeline` returns null, every later lookup returns null at `:380` or `:391` and never calls `build_pipeline` again. There is no retry and no error state. Severity H for a transient failure; M for a deterministic shader error (the draw can never succeed, but the failure is logged). Confidence: confirmed.

**C. Autorelease pool gap.**
`MTLPresenter.cpp:139` and `:284` create pools explicitly because the RSX thread has none. `MTLShader.cpp:214-221` and `MTLProgramPipeline.cpp:303-311` create autoreleased `NS::String` objects on the same thread without a pool. The objects accumulate until some other pool drains. Severity M (unbounded growth per compile on a cold preload). Confidence: confirmed. TODO(excise): level 4; count the objects that survive to the next pool drain if a leak is suspected.

**D. The label-release path is live and drains the GPU on every non-flip label.**
`MTLDevice.cpp:317` sets `cfg.supports_host_gpu_labels = true`. `nv47_sync.hpp:23` therefore calls `MTLGSRender::release_GCM_label` for every label write that is not a flip semaphore. `MTLGSRender.cpp:3365-3368` calls `flush_for_readback()`, which commits and waits up to 10 s (`MTLCommandRecorder.cpp:304-358`). The warm log measures 5 calls, 134 ms total, 124.7 ms max (`RPCS3.log:64112-64113`). Severity H (stall). Confidence: confirmed for the measurement; the frequency at race start is a hypothesis (§7.2). This is not a shader defect and is out of the shader patch.

**E. `release_GCM_label` always returns false.**
`MTLGSRender.cpp:3378` returns false after the drain, so `write_gcm_label` performs the CPU write at `nv47_sync.hpp:62`. This is correct today because the backend does not write the label itself. It is a trap: if the return value is ever changed to true without adding a GPU/CPU write in the backend, the label is never written. Severity M. Confidence: confirmed.

**F. No device-removal observer.**
No `MTL::DeviceWasRemovedNotification` observer exists in `rpcs3/Emu/RSX/MTL`. A device loss during an asynchronous compile leaves jobs in `Compiling` forever unless the registry has a loss path. Severity H after the change. Confidence: confirmed by search.

### 2.4 Hypotheses, clearly marked

**H1. The first-run black frames are caused by the synchronous compile.**
The log proves 50 synchronous pipeline builds take 2.095 s and that 302 of 306 flips refuse compose (`present/compose refused`; the condition at `MTLPresent.cpp:70-74` includes `!source.image` among other causes). The log does **not** prove that the compile caused the missing source. The first session submits only 3 vertices, so the title may simply not have drawn a presentable surface yet. The asynchronous design removes the CPU stall. It does not guarantee content on a frame that has no draw. A separate present/source investigation is required if black frames remain after the stall is gone. Confidence: hypothesis.

**H2. Race starts freeze because many new pipelines arrive in one frame.**
The measured per-key worst case is 336 ms. A start grid can add several keys in one frame, so the RSX thread can block for more than a second. The log has no race-start segment with per-key counts, so the multiplication is inferred, not measured. Confidence: hypothesis, high.

**H3. The first-run archive was absent, so all 50 keys linked from MSL.**
`RPCS3.log:64368` records the archive write after the second session, and `:66687` records the load in the next session. The first session has no `session/archive loaded` latch. Confidence: confirmed by the log sequence, not by a probe inside the first session.

## 3. Design

### 3.1 Object graph

```
MTLGSRender
  m_device : mtl::device
    MTL::Device
    MTL4::CommandQueue
    MTL4::Compiler                 one per device (W11)
    MTL4::PipelineDataSetSerializer
    MTL4::Archive                  optional, warm start
    MTL4::CompilerTaskOptions      lookupArchives, immutable after init
  m_frame_resources : frame_resources
    pipeline_jobs jobs             new: owns all asynchronous work
    program_cache programs         owns shader_program and pipeline objects
    shader_cache shaders           L2 disk set; calls programs
```

`pipeline_jobs` is declared before `programs` and `shaders` in `frame_resources`, so it is destroyed after them. `on_exit` calls `jobs.shutdown()` before it resets `m_frame_resources`, so no callback can reach a destroyed cache even on the normal path.

`shader_context` gains one field:

```cpp
struct shader_context
{
    MTL4::Compiler* compiler = nullptr;
    MTL4::CompilerTaskOptions* options = nullptr;
    pipeline_jobs* jobs = nullptr;   // new
    std::string vertex_entry;
    std::string fragment_entry;
    bool drain_on_submit = true;     // false during preload: only the loader drains
    const std::atomic<bool>* device_lost = nullptr;
};
```

The draw site and the preload site fill it. The existing `MTLGSRender.cpp:1993` and `:704` are the only two constructors.

### 3.2 Job states

The shared cache has two observable states: a real entry and a null entry. The Metal side needs more, because a null entry can mean "in flight", "failed once", or "failed permanently". The registry holds the difference.

```cpp
enum class job_phase : u8
{
    libraries,      // one or both libraries are still compiling
    link,           // both libraries are ready; the link task is in flight
    link_ready,     // the handler delivered a state; the drain owns it
    link_failed,    // the handler delivered an error; the drain owns it
    abandoned       // shutdown or device loss; no callback will run
};
```

Library state lives in each `shader_program`:

```cpp
enum class library_state : u8
{
    absent,
    compiling,
    ready,
    failed
};
```

Mapping to the draw path:

| Cache entry | Registry | Draw |
|---|---|---|
| real | no job | draw |
| null | job in `libraries` or `link` | skip, do not resubmit |
| null | job in `link_ready` | skip this draw; the next drain publishes |
| null | job failed, transient retry pending | skip; the registry resubmits the link after 500 ms |
| null | job failed, permanent | skip forever, log once |
| null | no job (synchronous path failed) | skip forever |

The last row is the existing behaviour and stays for the synchronous path (preload and fallback).

### 3.3 The registry interface

All of this lives in `MTLProgramPipeline.h` and `MTLProgramPipeline.cpp`, so patch 1 adds no translation unit and no CMake edit (`rpcs3/Emu/CMakeLists.txt:710` lists the file).

```cpp
class pipeline_jobs
{
public:
    pipeline_jobs() = default;
    ~pipeline_jobs();

    // Called by MTLTraits::build_pipeline when compile_async is true.
    // Returns false if the key is already known to be permanently failed.
    bool submit(const vertex_program& vertex,
                const fragment_program& fragment,
                const pipeline_properties& properties,
                const shader_context& context,
                std::function<pipeline* (std::unique_ptr<pipeline>&)> callback);

    // Publish ready jobs, submit the next stage, and release finished tasks.
    // Safe to call from the renderer thread and from the preload thread.
    // Performs no unbounded wait.
    void advance();

    // Preload only. Calls advance() until there is no live job or the deadline
    // expires. `progress(done, total)` runs on the caller thread.
    bool wait_for_idle(std::chrono::milliseconds timeout,
                       const std::function<void(u32, u32)>& progress);

    // Teardown. Stops new submissions, waits once for the tasks, drains what is
    // ready, and abandons the rest. Returns true only when every job finished
    // and none was abandoned.
    bool shutdown();

    bool permanently_failed(u32 vertex_id, u32 fragment_id,
                            const pipeline_properties& properties) const;

private:
    struct key;
    struct job;

    bool submit_library(const shader_program& program, MTL4::Compiler* compiler);
    void submit_link(job& work);

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::unordered_map<key, std::shared_ptr<job>, key_hash, key_equal> m_jobs;
    std::unordered_map<key, u8, key_hash, key_equal> m_failures;
    bool m_accepting = true;
    bool m_shutting_down = false;
    bool m_abandoned = false;
    const std::atomic<bool>* m_lost_flag = nullptr;
};
```

`key` is `{u32 vertex_id; u32 fragment_id; pipeline_properties properties;}`. It must hash and compare exactly like `program_state_cache::pipeline_key` (`ProgramStateCache.h:174-199`): `rpcs3::hash_struct<pipeline_properties>` for the properties, XOR of the two ids, and field equality. The ids come from `vertex.id` and `fragment.id` (`MTLProgramPipeline.cpp:143`, `:188`). `pipeline_properties` is trivially copyable (`MTLPipeline.h:26-59`), so a copy in the job is safe.

`key` cannot reuse the private `program_state_cache::pipeline_key`. Reimplementing the three-field hash is smaller than changing the shared cache interface. The two definitions must stay identical; a unit check compares them for a fixed set of properties (test T3).

### 3.4 Submission

`MTLTraits::build_pipeline` becomes a dispatcher:

```cpp
if (!compile_async || !extra.jobs)
{
    return build_pipeline_sync(vertex, fragment, properties, callback, extra);
}

if (extra.jobs->permanently_failed(vertex.id, fragment.id, properties))
{
    return nullptr;
}

if (!extra.jobs->submit(vertex, fragment, properties, extra, std::move(callback)))
{
    return nullptr;
}

if (extra.drain_on_submit)
{
    extra.jobs->advance();
}

return nullptr;
```

`build_pipeline_sync` is the current body of `MTLProgramPipeline.cpp:276-335`, with one change: it uses `ensure_library_sync`, which waits on a library that an asynchronous job already started (§3.6). That keeps the preload path correct even after a draw has submitted the same program.

`submit` inserts the job under `m_mutex` and does nothing else. It returns false if `m_accepting` is false or the device-loss flag is set, so a late draw cannot revive a shutting-down registry. It does not call the compiler, because the caller may be a preload worker and the compiler is a shared object. `advance` submits.

`advance`:

1. Lock `m_mutex`.
2. For each job in `libraries`: call `submit_library(program, work.context.compiler)` for each program whose state is `absent`. If either program is `failed`, move the job to `link_failed`. If both are `ready`, call `submit_link` and move to `link`.
3. For each job in `link_failed`: if the error text is empty (transient), `attempts < 2`, and `now >= retry_at`, set the phase back to `link`, increment `attempts`, and call `submit_link`. Keep the job in `m_jobs`. This is the only retry path. A permanent failure goes to step 4.
4. For each job in `link_ready`, move its record into a local `std::vector<std::shared_ptr<job>>` and erase it from `m_jobs`. A job is moved once, so two concurrent `advance` calls cannot both publish it.
5. Unlock.
6. For each moved job: if ready, take the retained `MTL::RenderPipelineState*` into a `std::unique_ptr<pipeline>`, call the stored callback, and let the callback move it into `m_storage`. If failed, record the key in `m_failures` and log once.
7. Release any finished `MTL4::CompilerTask` on this thread.

Step 4 is the ownership handoff. Step 6 runs without `m_mutex` because the callback takes `program_state_cache::m_pipeline_mutex` and may write the L2 file. Holding the registry mutex across file I/O would block every completion handler's state publication; it would not deadlock, but it would turn a disk write into a registry stall.

Library submission:

```cpp
bool pipeline_jobs::submit_library(const shader_program& program, MTL4::Compiler* compiler)
{
    if (program.library_state() != library_state::absent)
    {
        return false;
    }

    const u64 generation = program.begin_library_compile();   // sets compiling once
    if (!generation)
    {
        return false;
    }

    auto handler = [&program, generation](MTL::Library* library, NS::Error* error)
    {
        NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
        if (library)
        {
            library->retain();
        }
        program.finish_library_compile(library, error, generation);  // generation-checked store, no NS escape
        pool->release();
    };

    MTL4::CompilerTask* task = compile_msl_async(compiler, program.msl, handler);
    program.attach_library_task(task);
    return task != nullptr;
}
```

`compile_msl_async` lives in `MTLShader.cpp` next to `compile_msl` and uses the completion-handler overload:

```cpp
MTL4::CompilerTask* compile_msl_async(MTL4::Compiler* compiler, const std::string& msl,
                                      MTL::NewLibraryCompletionHandlerFunction handler);
```

It builds the same `LibraryDescriptor` and `CompileOptions` as `compile_msl` (`MTLShader.cpp:214-221`), calls

```cpp
compiler->newLibrary(descriptor, std::move(handler))
```

(the overload at `3rdparty/metal-cpp/Metal/MTL4Compiler.hpp:120-121`), releases its own descriptor and options, and returns the task. It wraps the descriptor construction in an autorelease pool.

Link submission builds the same objects as the synchronous body (`MTLProgramPipeline.cpp:303-316`) and calls

```cpp
compiler->newRenderPipelineState(descriptor, options, std::move(handler))
```

(the overload at `3rdparty/metal-cpp/Metal/MTL4Compiler.hpp:129-131`). `submit_link` sets the job phase to `link` **before** it calls the compiler. The handler stores `link_ready` or `link_failed` after that. If the handler wins the race, the later task store must not overwrite the phase; it releases the task immediately instead. The handler is:

```cpp
[work = work.get()](MTL::RenderPipelineState* state, NS::Error* error)
{
    NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
    if (state)
    {
        state->retain();
    }
    work->finish_link(state, error);   // fixed-size error copy + atomic phase store
    pool->release();
};
```

The job record outlives the handler because it is owned by the registry until `advance` moves it, and by the local vector until the callback returns. `job` is held by `std::shared_ptr` so the completion handler can keep its record alive even if `shutdown` has removed it from the registry. That is the one place a shared pointer is justified: the completion handler has no cancellation API, so its record must survive the registry. `job` contains no pointer into `program_state_cache`; it holds only the key, the two `shader_program*`, the context copy, the callback, the atomic phase, and the delivered state. The `shader_program*` pointers remain valid because a timed-out shutdown leaks the frame resources instead of destroying them (§3.13).

### 3.5 Descriptor lifetime

The completion-handler form is documented to copy the descriptor and the options during the call. The synchronous code already releases them immediately after `newRenderPipelineState` returns (`MTLProgramPipeline.cpp:321-323`); the asynchronous form does the same.

The first patch test T4 checks it: submit 200 pipelines with the descriptors released immediately, with `MallocScribble=1` and `MallocGuardEdges=1`. A violation is a crash or a corrupted pipeline, not a silent leak. TODO(excise): level 4; latch `program/descriptor lifetime` once per run when `MTL_PROBE>=4`.

If the test fails, the fallback is a retained descriptor stored in the job and released on the drain. The job record already exists, so the change is two lines. Do not ship the fallback speculatively.

### 3.6 Library state machine

`shader_program` gains:

```cpp
mutable std::mutex m_library_mutex;
mutable std::condition_variable m_library_cv;
mutable std::atomic<library_state> m_library_state{library_state::absent};
mutable std::atomic<u64> m_library_generation{0};
mutable MTL::Library* m_library = nullptr;
mutable MTL4::CompilerTask* m_library_task = nullptr;

library_state library_state() const noexcept;
MTL::Library* library() const noexcept;          // acquire load; null unless ready
u64 begin_library_compile() const;               // absent -> compiling; returns the new generation, 0 if not started
void finish_library_compile(MTL::Library*, NS::Error*, u64 generation) const;
void attach_library_task(MTL4::CompilerTask*) const;
void release_finished_library_task() const;
bool ensure_library_sync(MTL4::Compiler*);       // existing semantics + wait
```

Rules:

- `begin_library_compile` takes the mutex, checks `absent`, sets `compiling`, increments `m_library_generation`, and returns the new value. It returns 0 if the state is not `absent` (compiling, ready, or failed). The caller then calls the compiler outside the mutex. A second caller sees `compiling` and does not submit.
- `finish_library_compile` takes the mutex, and stores the result only if the generation equals `m_library_generation`. A mismatch means the program was replaced while the compile ran; the delivered library is leaked and the state is left alone. On a match it stores `m_library`, stores `ready` or `failed` with `memory_order_release`, and notifies `m_library_cv`. It copies the `NS::Error` text into a fixed `char[256]` before the handler returns; the error object must not escape.
- `library()` reads the state with `memory_order_acquire` and returns `m_library`. The release store publishes the pointer.
- `ensure_library_sync` takes the mutex. If `ready`, return true. If `failed`, return false. If `compiling`, wait on `m_library_cv` for at most 10 s, then re-check. If still compiling, return false and log. Otherwise set `compiling`, release the mutex, call the synchronous `compile_msl`, store the result, notify. This is the current `ensure_library` with the wait added and the compile moved outside the mutex.
- `attach_library_task` stores the task. If the state is already `ready` or `failed` (the handler ran before the store), it releases the task immediately. Otherwise the next `advance` or `shutdown` releases it.
- The destructor releases `m_library` with `release()` on the destroying thread. It asserts `m_library_state != compiling` and leaks `m_library_task` if one is still attached. That happens on the idle teardown path only; on a timeout the cache is leaked so a handler cannot touch freed storage.

### 3.6.1 Moves and recompilation

`shader_program` is stored by value in an `unordered_map` and has a move constructor and a move assignment (`MTLProgramPipeline.cpp:60-89`). Both must transfer the new state fields, and the move assignment must not let a stale completion mark a new MSL as ready.

Rules:

- The move constructor transfers `m_library`, `m_library_state`, `m_library_generation`, and `m_library_task`, then clears the source. A compile cannot be in flight before the object is in the map, so a transferred `compiling` state is a programming error; assert it is `absent`.
- The move assignment increments `m_library_generation` **before** it replaces `msl`, releases the old `m_library` if the state is not `compiling`, leaks the old `m_library_task`, then takes `other`'s fields. A handler that is already in flight captured the old generation; `finish_library_compile` sees the mismatch and discards the result.
- Never move-assign into a program whose state is `compiling` while a link job holds a pointer to it. The generation check makes the late library harmless, but the job would still link against a library pointer that no longer matches its MSL. The program cache does not recompile an existing program at runtime; this is a defensive rule for future invalidation. If invalidation is added, it must first cancel or await the jobs for that program id.
- The destructor asserts the state is not `compiling` on the idle path.

The mutex is no longer held across the compile. That removes defect S5 and lets two pipelines that share a program proceed in parallel: one submits, the other observes `compiling` and continues to its own link stage as soon as the library is ready.

### 3.7 Threads and queues

| Work | Thread | Why |
|---|---|---|
| Decompile, GLSL, SPIR-V, MSL | RSX thread (draw) or preload worker | D1. Keep the shared cache contract. |
| `newLibrary` submission | `advance` caller, under the registry mutex | One submitter at a time; the call returns immediately. |
| Library completion handler | Metal compiler thread | API contract. Only retain + state store + notify. |
| `newRenderPipelineState` submission | `advance` caller, under the registry mutex | Same. |
| Link completion handler | Metal compiler thread | Same. |
| Callback into `program_state_cache` | `advance` caller, after unlocking | Moves the object into `m_storage` under the cache's own mutex. |
| L2 `shaders->store` | The thread that ran the callback (RSX at runtime, preload thread during load) | D8. One writer at a time. |
| L3 serializer flush | `on_exit`, after `jobs.shutdown()` | D9. |
| Release of delivered library/state | `frame_resources` destruction, after `jobs.shutdown()` (not a handler thread) | Last release never runs on a Metal handler thread. |

There is no dedicated compiler thread in this design. Metal owns the compile threads. The backend owns one submission point, which is the registry mutex. This avoids a new thread and avoids a second queue that would need its own shutdown order.

### 3.8 Drain points and frame boundaries

Add one call at each of these sites:

| Site | Call | Purpose |
|---|---|---|
| `MTLGSRender::begin` (`MTLGSRender.cpp:797`) | `if (m_frame_resources) m_frame_resources->jobs.advance();` | Publish pipelines that finished while the previous frame presented. |
| `MTLGSRender::end` (`MTLGSRender.cpp:1641`) | same, at function entry | Publish a pipeline that finished since the last draw clause, so the next clause can use it. |
| `shader_cache::load` / `compile_shaders` (later patch) | `jobs.wait_for_idle(...)` | Preload completion with progress. |
| `MTLGSRender::on_exit` (`:731`) | `jobs.shutdown()` before `m_frame_resources.reset()` | One bounded wait, final drain, stop callbacks. |
| `MTLGSRender::flip` (`:3012`) | none | Present must not depend on the compiler. |

`advance` begins by checking whether there is any work:

```cpp
std::lock_guard lock(m_mutex);

if (m_jobs.empty())
{
    return;
}
```

An empty map is the common case for a warm title. One uncontended mutex acquisition per draw clause is cheaper than maintaining a lock-free hint that the completion handlers would have to update; the handlers must stay free of registry bookkeeping (TS2). Do not add an atomic fast path unless a profile shows the lock. TODO(excise): level 4; gauge `program/async active jobs` once per frame.

`end()` is called once per draw clause, so `advance` can run many times per frame. It must be cheap when idle. The empty-map early return is the requirement, not an optimization.

### 3.9 RSX-thread miss behaviour

The existing block at `MTLGSRender.cpp:1995-2006` already handles a null result:

```cpp
execute_nop_draw();
rsx::thread::end();
return;
```

Keep it. Add three things:

1. `std::this_thread::yield();` before `execute_nop_draw()`, matching `VKDraw.cpp:1319` and `GLDraw.cpp:952`. This is evidence, not RSX specification; it costs nothing and stops the RSX thread from spinning hot while the compiler works.
2. A latch for the reason. The current `diagnostic_pipeline_reported` bool logs once and cannot distinguish "in flight" from "failed". TODO(excise): level 2; `MTL_PROBE_LATCH(2, "draw/skipped pipeline pending")` and `"draw/skipped pipeline failed"`. Keep the one-time error line for the failed case.
3. Do not call `advance()` between the miss and the skip. The next `end()` or `begin()` drains. Calling `advance` here would make the skip path wait on the callback and the L2 disk write.

`frame.compiled_fragment` and `frame.compiled_vertex` are assigned only after a real pipeline exists (`MTLGSRender.cpp:2009-2010`). A skipped draw never reaches them, so no dangling program pointer enters the frame. This is an invariant (TS8).

The draw is skipped, not replayed. The frame continues. `execute_nop_draw()` preserves the RSX draw bookkeeping (the same reason VK and GL call it), and `rsx::thread::end()` closes the clause. There is no deferred draw queue and no draw-state snapshot. The only deferred object is the pipeline itself, and the shared cache owns it, not the draw.

### 3.10 Present and flip

`flip` (`MTLGSRender.cpp:3012-3312`) is unchanged except for the promise that it never waits for a job:

- It opens the frame, delivers the output, resolves the present source, acquires the drawable, composes, and presents.
- A missing pipeline means the surface may not hold new content. The output pass still composes whatever the surface store holds, and the drawable is still acquired and presented. The present is never dropped.
- If the present source is missing, the existing path presents the black clear (`MTLGSRender.cpp:3228-3277`). That is the current behaviour and is outside this design (H1).
- No `advance()` and no `wait_for_idle()` in `flip`. A pipeline that finishes during the flip is published at the next `begin()`.

This is how the design satisfies "skip draws without dropping frames": a frame is a flip. The design may drop draw clauses, never frames.

### 3.11 Preload (patch 4)

Today `add_pipeline_entry` passes `compile_async=false` (`MTLProgramPipeline.h:166-169`) and `compile_shaders` waits for every worker (`rsx_cache.h:133-151`). After patch 1, the preload still uses the synchronous path, which is correct and unchanged.

Patch 4 converts it:

1. `add_pipeline_entry` passes `compile_async=true` and the context with `jobs`.
2. Each worker submits jobs with `drain_on_submit=false`. A worker therefore never runs a callback and never writes the L2 set. The registry mutex serializes the submissions.
3. After `await_workers`, the loader thread calls `jobs.wait_for_idle(deadline, progress)`. That call is the only drain owner during preload: it submits the library and link stages, runs the callbacks, and updates the dialog at most every 16 ms, like the existing loop (`rsx_cache.h:167-174`).
4. On `Emu.IsStopped()`, `wait_for_idle` returns false immediately, `shutdown()` abandons the rest, and the dialog closes. The game is stopping; no cache entry is promised.

`wait_for_idle` uses a per-stage deadline, not a global one:

- A job that has been in `libraries` or `link` for more than 30 s is marked failed. This bounds one hung driver call.
- The loader's overall deadline is `max(60 s, 30 s + 2 s * job_count)` capped at 10 minutes. It exists so the dialog cannot hang forever. It is not a frame-path wait.
- The wait is a `condition_variable::wait_for` with a 10 ms ceiling and an overall deadline; it re-checks `m_jobs` after each wake. This is a sleep, not a spin. No handler is required to notify it.

The L2 store is not called during preload because `allow_notification=false`, so the loader thread is not a disk writer in this phase. That is the existing contract (`MTLGSRender.cpp:520-529`).

### 3.12 Disk L2: the pipeline set

The L2 write is the existing `notify_pipeline_compiled` callback (`MTLGSRender.cpp:520-529`), which calls `shaders->store(properties, vp, fp)` (`rsx_cache.h:296-348`).

With the asynchronous drain, the callback runs on the thread that calls `advance`:

- Runtime: the RSX thread, at `begin()` or `end()`. One write per newly completed pipeline. No competing writer.
- Preload (patch 4): the loader thread, and only if notifications are enabled; they are not.

Rules:

- Never call `store` from a completion handler.
- Never call `store` from two threads at once. The single drain owner guarantees this. If a future change adds a second drain owner, it must also add a serialization point; the design does not rely on `fs::write_file` being atomic (it is not, `rsx_cache.h:315`).
- The callback calls `notify_pipeline_compiled` before it inserts the entry (`ProgramStateCache.h:405-419` when notifications are enabled). The L2 write therefore finishes before the in-memory entry becomes usable. A crash between the two leaves a disk entry and a null memory entry; the next run loads the disk entry. That is safe. During preload `allow_notification` is false, so the callback only inserts and no disk write happens.
- The L2 write runs on the RSX thread inside the drain. It is the only disk work on that thread and is bounded by the files of one pipeline (`rsx_cache.h:296-348`). TODO(excise): level 2; gauge `program/l2 store us`. If the measurement shows more than 5 ms, move the write to the preload worker pool in a later patch, not in the first one.
- If `store` throws or fails, the log records it and the in-memory pipeline stays usable. `shader_cache::store` uses `fs::write_file`, which does not throw on a normal I/O error.

### 3.13 Disk L3: the driver archive

The L3 archive is captured by `MTL4::PipelineDataSetSerializer` on the compiler (`MTLDevice.cpp:160-177`). No per-job work is needed; the compiler records what it builds.

The ordering rule changes with async:

1. `on_exit` calls `jobs.shutdown()`. It clears `m_accepting` (a late draw cannot add a job), stops accepting jobs, and enters a loop that calls `advance()` and then sleeps 10 ms on `m_condition`, for up to 10 s, until `m_jobs.empty()`. It drains ready jobs and marks the rest `abandoned`. It does not wait for a handler to notify it.
2. If `shutdown()` returns false, skip the flush, do not reset `m_frame_resources`, and do not reset the device. "False" includes the timeout case and any abandoned job (device loss, aborted preload). A library handler that is still in flight holds a `shader_program` address. Destroying the cache would leave a dangling pointer. TODO(excise): level 1; latch `session/archive skipped after abort`. Leak the frame resources and the device.
3. If it returns true, reset `m_frame_resources`, flush the archive, log the result, then reset `m_device`.

The current `on_exit` order already flushes after `m_frame_resources.reset()` (`MTLGSRender.cpp:740-765`). Add the shutdown before the reset, carry the boolean, and make both the reset and the flush conditional.

Replay on the next boot is unchanged: `open_pipeline_archive` creates `CompilerTaskOptions` and calls `setLookupArchives` (`MTLDevice.cpp:233-234`). The same options object is passed to every asynchronous link, so a warm archive is consulted by the driver for both stages.

Archive failure rules:

| Case | Detection | Action |
|---|---|---|
| Archive file absent | `std::filesystem::exists` false (`MTLDevice.cpp:215`) | Cold start. TODO(excise): level 2; latch `session/archive absent`. Compile. |
| Archive rejected | `newArchive` returns null (`:224`) | Cold start. TODO(excise): level 2; latch `session/archive rejected`; log with `archive_error_text`. Compile. |
| Archive opens but a lookup misses | Compiler builds the pipeline | Normal. The serializer re-captures it. No error. |
| Archive version mismatch | Driver rejects or lookup misses | Same as above. The path carries the GPU registry id and the macOS major (`:152-153`); a minor OS or toolchain change is handled by the miss, not by a version field. |
| Flush fails | `serializeAsArchiveAndFlushToURL` returns false (`:255`) | Log with domain and code. Leave the old file untouched. The next boot uses the old archive or compiles. |
| Flush after a timed-out shutdown | `shutdown()` false | Skip the flush. Log at level 1. |

### 3.14 Device loss

Add an observer in `device::init`. Add `std::atomic<bool> m_lost{false};` and `const std::atomic<bool>& lost_flag() const` to `device`.

```cpp
NS::ObserverFunction handler = [this](NS::Notification*)
{
    m_lost.store(true, std::memory_order_release);
};

m_removal_observer = NS::NotificationCenter::defaultCenter()->addObserver(
    MTL::DeviceWasRemovedNotification,
    nullptr, nullptr,
    handler);
```

The `ObserverFunction` overload takes a non-const lvalue reference, so the handler is a named local. The wrapper copies it into a block (`Foundation/NSNotification.hpp:97-105`), so the local may die after the call. Store the returned observer token and remove it in the destructor.

On loss:

- `pipeline_jobs::advance` reads the device loss flag through the pointer recorded at the first `submit` (`shader_context::device_lost`, from `m_device->lost_flag()`). When set, it moves every live job to `abandoned`, sets `m_abandoned`, and submits nothing.
- Completion handlers may still run. They retain and publish into a job that is abandoned; `advance`/`shutdown` sees the phase and leaks the retained object rather than releasing it on a handler thread. This is a bounded leak at process end.
- `flip` still presents the last drawable if the queue accepts it. If the queue is gone, the present fails through the existing error paths; no shader-specific action.
- `on_exit` skips the archive flush, does not wait the full 10 s if the device is gone, and leaks the device and the frame resources instead of calling `reset()`. A released device with a live compiler is undefined, and a released cache with a live library handler is a dangling pointer.
- The emulator cannot recover a lost device. TODO(excise): level 1; latch `device/removed` and log one error line, then let the existing RSX fatal path stop the emulation. Do not attempt to recreate the device inside the shader layer.

### 3.15 Autorelease pools

Every function that creates an `NS::Object` on a thread without a pool wraps the creation:

- `compile_msl` (`MTLShader.cpp:207-227`) and `compile_msl_async`.
- `build_pipeline_sync` and `submit_link` (`MTLProgramPipeline.cpp:276-335` and the new function).
- Every completion handler.

Use the explicit form already used by the presenter (`MTLPresenter.cpp:139`):

```cpp
NS::AutoreleasePool* pool = NS::AutoreleasePool::alloc()->init();
// create and consume NS objects
pool->release();
```

The descriptors and options are still released explicitly. The pool covers the autoreleased `NS::String` objects and any framework temporaries. Do not rely on the pool to release objects the code owns.

## 4. Thread safety and lifetime rules

These are invariants. A patch that breaks one is rejected.

**TS1 — One registry mutex, one submission point.** All compiler submissions happen in `pipeline_jobs::advance` (or `wait_for_idle`/`shutdown`, which call it) while `m_mutex` is held. `submit` only inserts a record. No other thread calls `newLibrary` or `newRenderPipelineState` for an RSX pipeline.

**TS2 — Handlers do three things.** Enter a pool, `retain()` the delivered object, and store a phase/pointer with release semantics. They do not call the compiler, the cache, the callback, the filesystem, or `release()` on a delivered object.

**TS3 — Publication order.** The handler writes the object pointer, then the phase (`memory_order_release`). The drain reads the phase (`memory_order_acquire`), then the pointer. The library uses the same pattern through `library_state`.

**TS4 — Callback ownership.** A job is removed from the registry before its callback runs, so exactly one thread calls the callback. The callback receives a `std::unique_ptr<pipeline>&` and moves it into `m_storage`. After the callback, the local pointer is empty.

**TS5 — Lock order.** `pipeline_jobs::m_mutex` may be held while taking `shader_program::m_library_mutex`. The reverse is forbidden. `program_state_cache::m_pipeline_mutex` is taken only after `m_mutex` is released. No path takes `m_pipeline_mutex` and then `m_mutex`.

**TS6 — Programs are stable.** `m_vertex_shader_cache` and `m_fragment_shader_cache` are `std::unordered_map` (`ProgramStateCache.h:169-170`). References to their elements are stable across insert and rehash. Jobs store `const vertex_program*`/`const fragment_program*`, never a copy of the MSL. The maps are destroyed only after `jobs.shutdown()`.

**TS7 — The cache outlives the jobs, or the cache is leaked.** On the normal path, `on_exit` calls `jobs.shutdown()` before `m_frame_resources.reset()`. If `shutdown()` returns false, `on_exit` does not reset `m_frame_resources` and does not reset the device: an in-flight library handler still holds a `shader_program` address. `pipeline_jobs::~pipeline_jobs` asserts `m_jobs.empty()`; if not, it abandons and does not call any callback.

**TS8 — A skipped draw touches no program data.** The miss branch returns before `frame.compiled_vertex`/`compiled_fragment` are set. No argument table, descriptor heap, constant buffer, or encoder is created for a skipped draw.

**TS9 — Compile tasks touch no frame state.** The link reads only the `pipeline_properties` copy, the two libraries, the entry names (copied strings), and the `CompilerTaskOptions` owned by the device. It never reads argument tables, descriptor heaps, encoder state, command buffers, the drawable, or the current frame.

**TS10 — No command buffer references an unpublished pipeline.** `setRenderPipelineState` (`MTLGSRender.cpp:2054`) runs only after a real cache entry exists. Metal's own synchronization makes the state usable once it is handed to the encoder; the backend does not need a barrier or a commit.

**TS11 — The options object is immutable after init.** `open_pipeline_archive` creates `m_task_options` once (`MTLDevice.cpp:233-234`). No code mutates it later. Reads from several submission points are safe because submissions are serialized (TS1).

**TS12 — Task release is on the owning thread.** `MTL4::CompilerTask` pointers are stored in the program/job and released by `advance`/`shutdown`, not by the handler. If the handler runs before `attach_library_task`, the attach path releases immediately. On abandon, the task is leaked, not released: the compiler may still hold it.

**TS13 — Frame/flip/stop boundaries.**
- `begin`: drain only.
- `end`: drain only.
- `flip`: no job interaction.
- `Emu.IsStopped()`: `wait_for_idle` stops waiting; `shutdown` abandons.
- `on_exit`: shutdown, then ring shutdown, then cache destruction, then archive flush, then device reset.
- Device loss: stop submitting, abandon, skip flush, leak the frame resources and the device.

**TS14 — The L2 writer is single.** Only the thread that runs the callback calls `shaders->store`. The runtime owner is the RSX thread; the preload owner is the loader thread. They do not overlap because the load finishes before `GSRender::on_init_thread` returns (`MTLGSRender.cpp:715-728`).

## 5. Failure handling

### 5.1 Compile error

| Stage | Detection | Action | Cache result |
|---|---|---|---|
| Library | Handler receives a null library | Copy the error text (max 255 chars), set library `failed`, log one line with the program id and text. Copy the same text into every dependent job and move it to `link_failed` (non-empty text means no retry). | Null entry stays. Draw skips. |
| Link | Handler receives a null state | Copy the error text, set the job `link_failed`, log one line with vp/fp ids and the text. | Null entry stays. Draw skips. |

A deterministic error (a non-empty error text) is not retried. A stale MSL or an unsupported construct will fail the same way. A null result with an empty error text is treated as transient and its link is resubmitted once after 500 ms; if it fails again, it is permanent. The retry is in the registry (step 3 above), not in the draw path, so the draw still skips immediately.

Use a fixed `char[256]` for the error text. `NS::Error::localizedDescription()->utf8String()` is valid only during the handler; the text is copied before the handler returns. Do not store the `NS::Error*`.

TODO(excise): level 1; one error line per permanently failed key, with the vp/fp ids and the first 255 bytes of the error text. The table's log actions use that marker. Do not print a line per skipped draw.

### 5.2 Cancelled compile

`MTL4::CompilerTask` exposes `status()` and `waitUntilCompleted()` and no cancel method (`3rdparty/metal-cpp/Metal/MTL4CompilerTask.hpp:38-46`). The backend therefore does not cancel a compile. Cancellation means one of:

- The renderer is stopping: `shutdown` abandons.
- The device is lost: `advance` abandons.
- The preload is aborted: `wait_for_idle` returns false and `shutdown` abandons.

An abandoned job never calls its callback. The handler, if it still arrives, leaks the retained object. The compiler task is also leaked, not released, because the compiler may still hold it. `shutdown()` returns false if any job was abandoned, so `on_exit` takes the leak path. The process is ending or the device is gone; a bounded leak is the safe choice.

### 5.3 Duplicate requests

The shared cache already prevents a second `build_pipeline` for the same key while the null entry exists (`ProgramStateCache.h:389-396`). The registry adds a second guard for the case where two paths reach it with the same key (for example, a future preload change): `submit` looks up the key in `m_jobs` and in `m_failures`. If present, it returns false and does not submit. This is the only deduplication point; there is no per-draw allocation.

### 5.4 Bounded waits

| Wait | Bound | On timeout |
|---|---|---|
| `ensure_library_sync` waiting for an async library | 10 s | Log, return false, draw skips. |
| `jobs.shutdown` | 10 s | Abandon, skip archive flush, leak the frame resources and the device. |
| `jobs.wait_for_idle` per job | 30 s in one stage | Mark that job failed. |
| `jobs.wait_for_idle` overall | `max(60 s, 30 s + 2 s * jobs)`, cap 10 min | Abandon the rest, close the dialog, continue. |
| Draw path | none | Never waits. |
| Flip | none | Never waits. |

No loop polls `CompilerTask::status()`. No loop spins on a counter. Every wait is a condition variable with a deadline or a single `waitUntilCompleted()` at teardown, and the latter is only used if the condition-variable path is unavailable.

### 5.5 Cache hit and miss

- **Hit:** the library is `ready` and `m_storage` holds a real pipeline. No job, no allocation, no lock beyond the existing reader lock.
- **Miss:** `build_pipeline` submits one job and returns null. The null entry stays until the callback replaces it.
- **Library hit, pipeline miss:** the link is submitted immediately on the next `advance`; no library compile.
- **Both miss:** both library stages are submitted in the same `advance`, then the link when both are ready. The link is not submitted twice.
- **Failed key:** `permanently_failed` returns true before `submit`, so a stale draw does not create a new job. The null entry remains.

### 5.6 Archive mismatch

See §3.13. A mismatch is a cache miss, not a renderer failure. The only hard failure is a flush error, which leaves the previous archive in place.

### 5.7 Device loss

See §3.14. The shader layer's contract is: stop submitting, abandon jobs, do not flush, do not free the device.

## 6. Implementation sequence

Each step is independently testable. Do not start a step before the previous gate passes.

### Step 0 — Fix the key/properties order (prerequisite, one file)

File: `rpcs3/Emu/RSX/Program/ProgramStateCache.h`.

Change: move the validate call above the key construction, and delete the conditional call at `:375`:

```cpp
const auto& vertex_program = ...;
const auto& fragment_program = ...;

// Validate before the key is formed, so the key and the descriptor agree.
backend_traits::validate_pipeline_properties(vertex_program, fragment_program, pipeline_properties);

const pipeline_key key = { vertex_program.id, fragment_program.id, pipeline_properties };

if (already_existing_vertex_program && already_existing_fragment_program)
{
    // existing lookup, unchanged
}
```

The fragment program may be newly created; `validate_pipeline_properties` already returns early when `fragment.msl` is empty (`MTLProgramPipeline.cpp:239-242`), so the new call is safe for a failed program.

Why now: without this, the first build of every key skips validation and uses the guest write mask. The key then records that mask. Validating before the key means the key and the descriptor both use the fragment's reflected output mask, and two draws that differ only in unused bits share one pipeline.

Gate: a cold run builds pipelines whose write masks match `validate`'s log; a warm run hits them. Severity if skipped: M; the first pipeline of each key uses the wrong write mask, and the asynchronous callback publishes under that key.

### Step 1 — The smallest safe first patch (async draw path)

This is the smallest safe first patch. It depends on Step 0; land them as one series, with Step 0 first. If only one change can land now, land Step 0: it is independent, six lines, and fixes a confirmed correctness defect.

Files:

- `rpcs3/Emu/RSX/MTL/MTLProgramPipeline.h`
- `rpcs3/Emu/RSX/MTL/MTLProgramPipeline.cpp`
- `rpcs3/Emu/RSX/MTL/MTLShader.h`
- `rpcs3/Emu/RSX/MTL/MTLShader.cpp`
- `rpcs3/Emu/RSX/MTL/MTLGSRender.cpp`

Changes:

1. Add `library_state`, the state fields/methods (including the generation), the move-constructor/assignment updates, and `pipeline_jobs` to `MTLProgramPipeline.h`.
2. Add `compile_msl_async` to `MTLShader.h/.cpp`; add the pool to `compile_msl`.
3. Rework `shader_program::ensure_library` into `ensure_library_sync` (mutex not held across the compile; wait if compiling). Keep the name `ensure_library` as the synchronous entry point if that reduces churn, but the body must not hold the mutex across `compile_msl`.
4. Add `build_pipeline_sync` (current body) and the async dispatcher in `build_pipeline`.
5. Implement `pipeline_jobs` in `MTLProgramPipeline.cpp`.
6. Add `pipeline_jobs jobs;` to `frame_resources` before `programs` (`MTLGSRender.cpp:546`).
7. Add `jobs` to the two `shader_context` constructions (`&m_frame_resources->jobs` at `:704`, `&frame.jobs` at `:1993`), and `m_device->lost_flag()` as the last field.
8. Change the fifth argument at `:1991` from `false` to `true`.
9. Add `jobs.advance()` at `begin()` and `end()`.
10. In `on_exit`, call `jobs.shutdown()` before the ring shutdown; reset `m_frame_resources` and flush the archive only when it returns true.
11. Add the miss-path latch and the `yield`.

What this patch deliberately does not do:

- It does not change the preload. The preload stays synchronous and still uses `compile_async=false`.
- It does not change the archive path beyond the shutdown gate.
- It does not add the device-loss observer.
- It does not change `release_GCM_label`.

Gate (test T1/T2): a cold first run no longer blocks the RSX thread for 336 ms per pipeline. `program/pipeline` measures submission only. Draws are skipped while a job is in flight and resume when it completes. Present continues. The archive written at exit loads on the next run.

### Step 2 — Miss and failure instrumentation (small)

Files: `MTLGSRender.cpp`, `MTLProgramPipeline.cpp`.

- TODO(excise): level 2; latches `draw/skipped pipeline pending`, `draw/skipped pipeline failed`, `program/async submitted`, `program/async library ready`, `program/async link ready`, `program/async failed`.
- TODO(excise): level 2; gauge `program/async compile us` from submission to handler, one sample per job.
- TODO(excise): level 1; one error line per permanently failed key, with vp/fp ids and the error text.
- TODO(excise): level 4; gauge `program/async active jobs` once per frame.

Do not print per-draw lines. The miss path fires every draw clause until the pipeline lands; a trace line there would flood the log (MTL_PROBE anti-flood rules, `MTLInstrumentation.h:189-197`).

Gate: the coverage table shows submissions, completions, and zero permanent failures on a normal title.

### Step 3 — L2 single-writer verification

Files: none expected; verify `MTLGSRender.cpp:520-529` and `rsx_cache.h:296-348`.

- Confirm the callback runs only on the drain owner.
- Confirm a compile failure never calls `notify_pipeline_compiled` (the callback is called only with a non-null pipeline, `ProgramStateCache.h:408-411`).
- Add a debug-only assertion that `shaders->store` is called from the RSX thread at runtime. TODO(excise): level 4; one latch `program/l2 store thread` keyed by thread id.

Gate: a cold run with `shaders_cache` removed writes the same number of pipeline files as completed pipelines.

### Step 4 — Asynchronous preload (optional for the first landing)

Files: `MTLProgramPipeline.h` (`add_pipeline_entry`), `rsx_cache.h` (`compile_shaders`), `MTLGSRender.cpp` (load call).

- Pass `compile_async=true` and the context with `jobs`.
- After `await_workers`, call `jobs.wait_for_idle(...)` with a progress callback that updates the dialog at 16 ms intervals.
- Check `Emu.IsStopped()` in the wait loop.
- On timeout, abandon and close the dialog. The runtime draw path fills the gap.

Gate: a cold boot with 50 stored pipelines keeps the loading dialog responsive, and the first drawn frame arrives no later than the synchronous path's first drawn frame.

### Step 5 — Device loss and archive ordering

Files: `MTLDevice.h/.cpp` (observer and `lost()`), `MTLGSRender.cpp` (`on_exit`), `MTLProgramPipeline.cpp` (abandon on loss).

- Add the observer and remove it in the destructor.
- `advance` checks `device_lost()`.
- `on_exit` skips the flush and leaks the frame resources and the device on loss or timeout.
- The archive flush is gated on `jobs.shutdown()`.

Gate: code review plus a forced `m_lost` test hook. TODO(excise): level 5; a debug-only env switch that sets the lost flag after N frames, to exercise the abandon path without a real device removal. Do not ship the switch.

### Step 6 — Keep the synchronous fallback

Once steps 1–5 pass on two titles and a cold/warm pair, the draw path no longer needs the synchronous branch. Keep it anyway for the preload and for `extra.jobs == nullptr` tests. Removing it saves no measurable time and loses the fallback.

### 6.1 Minimal test plan

| ID | Test | Pass condition |
|---|---|---|
| T1 | Cold start: delete `shaders_cache/metal` and `shaders_cache/pipelines/metal`, boot to the first race with `MTL_PROBE=2`. | `program/pipeline` max < 10 ms (submission only). No single frame gap > 250 ms that is attributed to `program/*`. `draw/vertices` becomes non-zero. `present/presented` does not stop. |
| T2 | Warm start: exit cleanly, keep the archive, boot again. | `session/archive loaded` fires. First drawn frame arrives at or before the cold run's. `cache/pipeline hit` > `cache/pipeline miss` after the first 300 frames. |
| T3 | Key equivalence: compare `pipeline_jobs::key` with `program_state_cache::pipeline_key` for 100 pipeline property values. | Same hash and same equality for every value. |
| T4 | Descriptor lifetime: 200 asynchronous links with descriptors released immediately, `MallocScribble=1`. | No crash, no corrupt pipeline, 200 completions. |
| T5 | Duplicate requests: force two draws with the same key in one frame. | One job, one callback, one L2 file. |
| T6 | Forced library error (local test only): corrupt one MSL string. | One error line, one failed key, other draws continue, exit clean, no retry storm. |
| T7 | Forced link error: build a descriptor with an invalid attachment format. | Same as T6. |
| T8 | Stop during preload: request stop while the dialog is up. | Dialog closes within 2 s, exit completes, no callback touches freed memory. |
| T9 | Teardown during compile: stop immediately after a race starts. | `on_exit` completes within 10 s, archive is written or deliberately skipped, no crash. |
| T10 | 60 s race-start capture with `MTL_PROBE=2`. | No `Dead FIFO` line. Max `present/flip` is not caused by `program/pipeline`. |
| T11 | Warm run with `MTL_PROBE=4`. | `program/async active jobs` returns to zero; ready jobs are drained; no handler-thread release is observed. |
| T12 | Memory: cold preload, 5 repeats. | RSS growth per repeat is bounded; no autorelease growth from `MTLShader.cpp:214-221`. |

### 6.2 First patch acceptance

The first patch is accepted when all of these are true:

1. The draw site passes `true`.
2. `build_pipeline` returns without calling `newLibrary` or `newRenderPipelineState` on the RSX thread when `compile_async` is true.
3. A pipeline completion is published at `begin()` or `end()`, and the next draw uses it.
4. A pipeline miss skips the draw and still presents the frame.
5. `on_exit` waits once, bounded, and the archive flush is gated on that wait.
6. No completion handler calls the cache, the callback, the filesystem, or `release()` on a delivered object.
7. The preload path is unchanged and still passes its existing smoke test.

## 7. Non-shader issues (HLE, video, decode, FIFO)

This section is separate from the shader design. These issues can freeze or black the screen independently of pipeline compilation. They are recorded here because the shader design was asked to account for the whole first-run failure, and because two of them interact with the frame loop.

### 7.1 N1 — Dead FIFO queue (confirmed, severity H)

Evidence:

- `RSXThread.cpp:3078` throws `Dead FIFO commands queue state has been detected!` from `recover_fifo`.
- `RSXFIFO.cpp:765` is the `run_FIFO` call site named in the fatal line.
- `RPCS3.log:144223-144243` shows 21 `FIFO error: possible desync event (last cmd = 0x10000)` lines in 0.16 s.
- `RPCS3.log:145415` is the fatal line.

Cause (hypothesis, supported by the log sequence): the guest recycles the FIFO before the GPU has finished the work that a backend semaphore release publishes. The CPU writes the label, the guest reads it as a command, and the FIFO desyncs. The current mitigation is `supports_host_gpu_labels = true` (`MTLDevice.cpp:317`), which routes every non-flip label through `release_GCM_label` and drains before the CPU write.

Status: the desync did not recur in the later session, but the mitigation is expensive (§7.2). This is a FIFO/synchronization issue, not a shader issue.

### 7.2 N2 — The label drain stalls the RSX thread (confirmed measurement, severity H)

Sites:

- `nv47_sync.hpp:23` calls `release_GCM_label` for every label that is not a flip semaphore. The call is before the same-value early return at `:25-30`, so a repeated identical label also drains.
- `MTLGSRender.cpp:3365-3368` calls `flush_for_readback()` when the frame is open.
- `MTLCommandRecorder.cpp:304-358` commits the current slot, waits up to 10 s for its feedback, resets the allocator, and restarts the command buffer.
- The warm log measures 5 calls, 134.310 ms total, 124.718 ms max (`RPCS3.log:64112-64113`).

Why it is H: a race start can write many labels. Each write commits and waits for the GPU. The cost is per label, not per frame. A title that releases several labels in one frame can stall for hundreds of milliseconds even with asynchronous shaders.

Correct direction (separate design):

1. Classify the label. Only a label whose value the guest will read before the next flip needs a CPU-visible write after the GPU work. A label that the guest polls can be published by the slot's commit feedback without a `wait_idle`.
2. Do not use `flush_for_readback` for a label that does not need a readback. `flush_for_readback` exists for a texture read; a label write needs completion ordering, not a readback.
3. If the backend ever writes the label itself, `release_GCM_label` may return true. Today it must return false (`MTLGSRender.cpp:3378`), or `write_gcm_label` skips the only write.
4. Bound the label path with the same 10 s feedback timeout, but do not make the common case wait.

Do not change this in the shader patch. It needs its own log and its own race-start measurement.

### 7.3 N3 — Video decode returns `CELL_VDEC_ERROR_EMPTY` (hypothesis, severity H)

Sites:

- `cellVdec.h:9` defines `CELL_VDEC_ERROR_EMPTY = 0x80610104`.
- `cellVdec.cpp:1981` is `cellVdecGetPicItem`.
- The log records `cellVdecGetPicItem` returning `0x80610104` at 1:26.503 and 1:41.303, together with `cellDmuxResetEs` / `cellDmuxDisableEs` errors (`HANDOFF.md:776`, `LOG-FINDINGS.md:28`).
- `cellDmux.cpp:931` and `:970` are `cellDmuxDisableEs` and `cellDmuxResetEs`.

What is confirmed: the guest polls for a picture and receives "empty"; the game then resets or disables the elementary stream. The race-selection video and the FMVs are black (`METAL4-TODO.md:353`, L12).

What is not confirmed: whether the empty result is an HLE timing bug, a decoder output-queue bug, or the guest polling before the first frame is ready. The Metal texture path is involved only after a picture exists. Do not attribute this to shaders.

Next step (separate): instrument `cellVdecGetPicItem` with the handle state, the AU queue depth, and the time since the last `cellVdecDecodeAu`; compare with a Vulkan run of the same title. TODO(excise): level 3; a per-call trace line capped at 256 lines, plus a level-2 histogram of the queue depth at the empty return.

### 7.4 N4 — Black mini-game panel (hypothesis, severity H, Metal-adjacent)

`METAL4-TODO.md:352` (L11): the Xevious panel is black while input and audio work. The panel is either not drawn, cleared after it is drawn, or sampled stale. The candidate sites are the draw skip path (`MTLGSRender.cpp:1651-1669`), the clear path (`MTLFramebuffer.cpp`), and the texture copy (`MTLTextureCache.cpp`). This is not an HLE or decode issue, but it shares the "black output with live logic" symptom. It must be investigated with its own capture. The shader async change neither causes nor fixes it.

### 7.5 What the shader patch does not fix

- The 302 missing present sources in the first session (H1). Async removes the compile stall; it does not create a surface.
- The video decode empty result (N3).
- The panel (N4).
- The label drain cost (N2).

State this in the landing note. Do not claim a first-run fix from the shader patch alone.

## 8. Acceptance criteria

The design is complete when:

1. No RSX-thread call reaches `newLibrary` or `newRenderPipelineState` for a draw-path miss.
2. The worst measured `program/pipeline` sample on the draw path is submission cost, not link cost (target < 10 ms; the current max is 336 ms).
3. A miss never blocks the present. `present/presented` continues while jobs are in flight.
4. A completed job is visible to the next draw at the next `begin()`/`end()`.
5. No callback, handler, or completion path can touch a destroyed `program_state_cache`.
6. The archive flush is ordered after the compiler is idle, or is skipped with a log.
7. Every wait is bounded and every bound has a defined timeout action.
8. The non-shader defects in §7 remain separately tracked; the shader landing note does not claim them fixed.

## 9. Open questions

**Q1 — Does `MTL4::Compiler` accept concurrent submissions from several threads?**
The design serializes submissions, so it does not depend on the answer. If the answer is yes, `advance` could release the mutex earlier, but there is no measured need. Keep the serialization.

**Q2 — Does the completion-handler form copy the descriptor?**
T4 answers it. The synchronous code already releases immediately; the fallback is a retained descriptor in the job.

**Q3 — Can the library and the link overlap for the same program?**
No. The link needs both libraries. The design submits the link only when both are `ready`. A link cannot start from MSL source without a library.

**Q4 — Should a failed pipeline be retried after a title reloads the shader?**
A new guest program produces a new id and a new key, so it compiles. A transient failure of the same key is retried once. A permanent failure of the same key stays failed for the session. This is the bounded policy; do not add an unbounded retry.

**Q5 — Is the `program/compile vertex` 185 ms max a Metal cost or a decompiler cost?**
The row covers decompile + SPIR-V + MSL + probes. The Metal library row is separate (12.8 ms max). The design keeps the first three on the calling thread (D1). If a later measurement shows the decompiler dominates and the RSX thread still stalls, that is a second, separate design with its own key/lifetime problem. Do not fold it into this patch.

<!-- doc-end -->
