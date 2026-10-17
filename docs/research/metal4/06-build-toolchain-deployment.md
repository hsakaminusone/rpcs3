# 6. Build system, shader toolchain, deployment, fallback, packaging & CI

Owner/scope: Scope 6 (cross-cutting) | Status: draft | Date: 2026-10-15

**Verification legend.** `[V]` = verified this session by reading a file in-repo, a pristine
reference checkout under `/tmp/metal4-refs/`, or fetched source/doc text (URL in §11). `[I]` =
inference from `[V]` facts. `[U]` = unverified / no primary source found.

Read-only task: no source, CMake, test, `.ci` file, submodule or build was modified; nothing was
committed. `file:line` references are point-in-time and advisory; symbol names are the stable
reference. Difficulty scale (same as the rest of this set): **1** trivial · **2** mechanical ·
**3** well-trodden real work · **4** hard / needs design · **5** unknown or impossible.

---

## 1. Scope & what is settled elsewhere

This doc owns the *engineering edges* of a native Metal 4 backend: how it is **built**, what
**shader toolchain** it needs and how that is vendored, what **OS/device** it ships to and how that
is gated, whether a **Metal 3 fallback** is real, and what **packaging/CI** can prove. It does not
re-map Vulkan constructs.

**Settled elsewhere — carried as given, not re-derived:**

- **Shader path (scope 3, Option A):** `GLSL (unchanged) → glslang → SPIR-V → SPIRV-Cross
  CompilerMSL → MSL → MTL4Compiler`. No NIR→MSL backend (KK needed one only because KK starts from
  NIR). `[V]`
- **Floor (scopes 2/3):** Metal 4 = **macOS 26+ and Apple silicon M1+ only**. MTL4 types are
  `API_AVAILABLE(macos(26.0))`. `[V]`
- **Hazard model (scope 2):** Metal 4 has **no driver hazard tracking**
  (`MTLResource.hazardTrackingMode` has no effect on `MTL4CommandQueue` work); Metal 3 does. `[V]`
- **Workarounds (R10):** KK documents 18 `KK_WORKAROUND_*`. Relevant here: **W11** concurrent
  `MTL4Compiler` instances corrupt the heap → one compiler per device (fixed macOS 27 beta 5);
  **W1** uninitialized scratch crashes the MSL compiler (fixed macOS 26); **W2** `while(true)` +
  invariant break crashes the compiler (re-enabled on M5/macOS 27). `[V]`
- **Existing KK seam:** `kosmickrisp_min_macos_major = 27`
  (`rpcs3/Emu/RSX/VK/vkutils/vk_driver_selection.h:801`) and
  `host_supports_kosmickrisp(...) = arch == arm64 && macos_major >= 27` (`:803-805`). `[V]`

**Repo facts this scope is written against (`[V]`):**

| Fact | Location |
|---|---|
| `project(rpcs3 LANGUAGES C CXX)` — no `OBJCXX` language declared | `CMakeLists.txt:10` |
| One Obj-C++ file today, built via the CXX toolchain (object exists under `build/`) | `rpcs3/Emu/CMakeLists.txt:681`, `rpcs3/Emu/RSX/VK/vkutils/metal_layer.mm:9` |
| Vulkan hard-required on Apple (`FATAL_ERROR` if not found) | `3rdparty/CMakeLists.txt:185,203` |
| glslang vendored in-tree, static, `EXCLUDE_FROM_ALL` + alias | `3rdparty/glslang/…`, `3rdparty/CMakeLists.txt:92,378` |
| `add_executable(rpcs3 WIN32 MACOSX_BUNDLE)`, plist | `rpcs3/CMakeLists.txt:101,110`; `rpcs3/rpcs3.plist.in:31-32` (`LSMinimumSystemVersion 15.0`) |
| Build sysroot = `xcrun --sdk macosx --show-sdk-path`, target `15.0`, arch `arm64` | `.ci/build-mac-homebrew.sh:51-52`; `docs/research/12-macos-build-guide.md:77-78` |
| CI builds Intel + Apple Silicon **separately** | `.github/workflows/rpcs3.yml:192` (`macos-15-intel`), `:196` (`macos-15`) |
| Shader cache namespaced by backend string; validity = `sizeof(pipeline_data)` | `rpcs3/Emu/RSX/VK/VKGSRender.cpp:721`, `rpcs3/Emu/RSX/rsx_cache.h:104,212-240` |

---

## 2. Shader toolchain: SPIRV-Cross vs native MSL, runtime vs offline

### 2.1 Vendoring SPIRV-Cross — cheap, and Option A stands

**SPIRV-Cross is not in the tree today.** No `spirv_cross*` / `spirv_msl*` files exist; the only
copy anywhere near RPCS3 is *inside* the MoltenVK submodule
(`/tmp/metal4-refs/MoltenVK/MoltenVKShaderConverter/SPIRV-Cross`, pinned via
`MoltenVK/ExternalRevisions/SPIRV-Cross_repo_revision`), which RPCS3 builds as its own subproject
and, in the Homebrew flow, does not build at all (`USE_SYSTEM_MVK=ON`, `.ci/build-mac-homebrew.sh:62`).
`[V]`

**Cost to vendor it like glslang (cheap; difficulty 2).** `[V]`/`[I]`

- Add a `3rdparty/SPIRV-Cross` submodule (`.gitmodules`; the existing CI `awk` exclusion filter
  only names specific paths and needs no change) and `add_subdirectory(SPIRV-Cross
  EXCLUDE_FROM_ALL)` mirroring `3rdparty/CMakeLists.txt:92`, plus a `3rdparty::SPIRV-Cross` alias
  like `:378`.
- Link **statically** (the C++ API is not ABI-stable; Godot and MoltenVK both compile it in). The
  stable C API (`spvc_*`) exists but adds marshalling for no benefit here.
- CPU-only C++11, no build-time codegen, no extra system dependency — the same shape as the
  existing glslang dependency.

**License is clean via MIT.** SPIRV-Cross is **Apache-2.0 OR MIT** (per-file SPDX headers use the
dual form; the AOSP/Chromium tree carries `LICENSES/Apache-2.0.txt` and the Fedora review notes the
`MIT` option on `spirv_msl.{cpp,hpp}` and friends). RPCS3 is GPLv2, so taking the **MIT** option is
unambiguously compatible. `[V]`

**Alternatives, and why Option A is not revisited.**

| Alternative | Verdict |
|---|---|
| **Reuse MoltenVK's vendored SPIRV-Cross** | Rejected. It is an internal subproject detail, not a stable interface; RPCS3 uses *system* MVK in the Homebrew build, and its pinned revision is controlled by MVK, not RPCS3. `[I]` |
| **Emit MSL directly from a new decompiler target** | Rejected. RPCS3's decompilers emit GLSL; an MSL target duplicates the RSX opcode→GLSL logic (`GLSLCommon.cpp`, `RSXROPEpilogue.glsl`) and re-solves everything SPIR-V already solves. Scope 3 already chose SPIRV-Cross; the build side adds no reason to reopen it. `[V]`/`[I]` |

### 2.2 Runtime MSL is mandatory

RPCS3 generates shaders **at runtime** from emulated RSX microcode; the set is per-title, unbounded
and unknown at build time. MSL text is therefore produced in-process by SPIRV-Cross and handed to
Metal at runtime. metal-cpp confirms the exact shape: `MTL4::LibraryDescriptor::setSource(...)`
(`MTL4LibraryDescriptor.hpp:55`) → `compiler->newLibrary(descriptor, &err)`
(`MTL4Compiler.hpp:119`). There is **no build-time step that can precompile game shaders.** `[V]`

### 2.3 Offline toolchain: real, but only for the fixed shader set

Xcode/`xcrun` ship a Metal CLI: `metal` (source→Metal IR), `metallib`, `metal-ar`, and `metal-tt`
(the offline GPU-binary generator). Xcode's *default* build path compiles `.metal` files to a
Metal-IR `default.metallib` loaded via `makeDefaultLibrary()`; a library supplied as a **source
string** compiles to Metal IR on device and then goes through the secondary GPU compile — i.e. the
runtime `setSource` path is normal and supported. `[V]`

RPCS3 *does* have a small fixed shader set (UI overlay, attachment clear, video-out calibration,
resolve helpers, FSR1, the compute meta-passes in `Program/GLSLSnippets/**`). Those could in
principle be offline-precompiled into a shipped `.metallib`. **Recommendation: do not — compile
only from MSL at runtime for the first cut.** All shaders already flow through the same
`MTL4Compiler` path; a shipped `.metallib` would be a second codepath for a marginal startup win,
and it drags the ~700 MB Metal toolchain (§5.2) into the build. `[V]`/`[I]`

### 2.4 Binary archives: build on-device, never ship

`MTL4PipelineDataSetSerializer` (created via `MTL4CompilerDescriptor.pipelineDataSetSerializer`)
captures what a compiler produces. Two serializations
(`MTL4PipelineDataSetSerializer.hpp:32-34`): `CaptureDescriptors` → `serializeAsPipelinesScript`
(input for `metal-tt`); `CaptureBinaries` → `serializeAsArchiveAndFlushToURL` (a real archive).
Facts that constrain a build/packaging decision `[V]`:

- Archives are **GPU-specific slices** and are **opaque** — they only accelerate compilers they were
  attached to (Apple: *"objects … can't accelerate compilation for compilers they are not attached
  to"*). `[V]`
- **Both Metal 3 and Metal 4 can load binary archives** (so an archive is not a Metal-4-only
  artifact). `[V]`
- Apple warns a device OS upgrade can invalidate binaries and force recompilation from the
  archive's Metal IR. `[V]`

**Recommendation:** generate and cache archives **locally**, keyed by device+OS, or skip them. Do
**not** ship them in the bundle — the game-shader set is per-title and unbounded. `[I]`

### 2.5 How archives + `MTL4Compiler` fit RPCS3's existing shader cache

RPCS3 already has the right shape. `rsx::shaders_cache<pipeline_storage_type, backend_storage>` is
aliased per backend (`rpcs3/Emu/RSX/VK/VKGSRenderTypes.hpp:40`) and constructed with a backend
**class string** and **version prefix**: `vk::shader_cache(*m_prog_buffer, "vulkan", "v1.95")`
(`VKGSRender.cpp:721`; OpenGL uses `"opengl"`). On-disk layout is
`<ppu_cache>/shaders_cache/pipelines/<class>/<version_prefix>/` plus `shaders_cache/raw/`
(`rsx_cache.h:212-240`); `<ppu_cache>` comes from `rpcs3::cache::get_ppu_cache()`
(`cache_utils.cpp:14`), rooted under `fs::get_cache_dir()` (`system_utils.cpp:219-221`). The
on-disk toggle is `disable_on_disk_shader_cache` (`system_config.h:159`). `[V]`

Concrete integration for Metal 4 `[I]`:

- Add a **`"metal"` class** (own `pipeline_data` payload) and a version prefix, so the Metal cache
  never collides with `"vulkan"`/`"opengl"`.
- The **`f.size() != sizeof(pipeline_data)`** check (`rsx_cache.h:104-106`) already invalidates on a
  binary-incompatible layout change — reuse it; keep the Metal `pipeline_data` self-describing.
- `MTL4BinaryArchive` (or a serializer-produced archive) is the natural analogue of
  `VkPipelineCache`. Load it on warm start, append newly compiled entries, flush on exit. Because
  archives are device/OS-specific (§2.4) the archive file should itself be keyed by GPU identity +
  macOS major.
- **`MTL4Compiler` concurrency (W11).** Keep **one compiler per `MTLDevice`** and let it do
  concurrent compilation internally (Apple: the protocol "applies multithreading … by default").
  W11 is fixed only in macOS 27 beta 5, and the backend floor is 26, so RPCS3 must not fan out one
  compiler per worker thread. RPCS3's current async pool (`g_num_pipe_compilers`,
  `shader_compiler_threads_count`, `system_config.h:177`) becomes *one compiler + N compiler tasks
  with completion handlers*. `[V]`

---

## 3. Deployment target, availability gating, the x86_64 question

### 3.1 Hard constraints

- Metal 4 hardware: **Mac = Apple silicon M1 or later**; **macOS Tahoe 26 or later**. `[V]`
- `MTL4Compiler`/`MTL4Archive`/`MTL4PipelineDataSetSerializer` are `macOS 26.0+`. `[V]`
- **There is no Metal 4 on Intel Macs** — the API was built Apple-silicon-first and the enumerated
  supported hardware excludes Intel entirely. This is a hardware+OS wall, not a switch. `[V]`
- Conversely, an artifact compiled for the macOS 26 toolchain fails at load on macOS 15 (reported
  widely) — the version wall is enforced at the Metal-IR/AIR level, not only at the API level. `[V]`

### 3.2 RPCS3 today

Target `15.0` (`.ci/build-mac-homebrew.sh:51`; `rpcs3/rpcs3.plist.in:31-32`), built **per-arch**
(`-DCMAKE_OSX_ARCHITECTURES=arm64`, `.ci/build-mac-homebrew.sh:52`) by two separate CI jobs, Intel
and arm64 (`.github/workflows/rpcs3.yml:192,196`). `[V]`

### 3.3 Options

| Option | Effect | Verdict |
|---|---|---|
| **(a)** keep target `15.0`, build with the macOS 26 SDK, **runtime-gate** every `MTL4*` call | one bundle; backend self-enables on M1+/26+, absent elsewhere | **Recommended** |
| (b) raise `CMAKE_OSX_DEPLOYMENT_TARGET`/`LSMinimumSystemVersion` to `26.0` | drops all macOS 15–25 Apple-silicon users | not recommended |
| (c) second macOS-26-only binary/slice | duplicate packaging + release machinery | not recommended |

(a) is well-trodden: the **build SDK** (macOS 26) may be newer than the **deployment target** (15.0),
and Clang weak-links the later symbols so a runtime check suffices (this is the classic
availability/weak-linking mechanism). `[V]`

### 3.4 The gate is the union of three checks

1. **Build**: compile with the **macOS 26 SDK**. `[V]`
2. **OS**: `@available(macOS 26.0, *)` (ObjC++) or `__builtin_available(macOS 26.0, *)` (C++). `[V]`
3. **Device**: `device->supportsFamily(MTL::GPUFamily::GPUFamilyMetal4)` (`MTLDevice.hpp:194,558`)
   and/or a successful `device->newCompiler(...)` returning non-nil (`MTLDevice.hpp:425`). `[V]`

**Caveat that matters for the language choice:** metal-cpp headers carry **no `API_AVAILABLE`
annotations** (verified: none in `MTL4Compiler.hpp`/`MTLDevice.hpp`), so the compiler **cannot**
enforce the `@available`/`__builtin_available` check — the developer must guard manually.
metal-cpp's mitigations are runtime-only: `supports…()` helpers call
`Object::sendMessageSafe` and auto-return `false` when a selector is absent, and error-domain
strings are weak-linked to `nullptr`. The **ObjC SDK headers do** carry `API_AVAILABLE(macos(26.0))`,
so a direct ObjC++ implementation gets compile-time `-Wunguarded-availability` protection. This is
a real argument for putting the *gating* at an ObjC++ seam. `[V]`

A predicate mirroring the existing KK style: `arm64 && macOS_major >= 26 &&
device.supportsFamily(Metal4)`. `[I]`

### 3.5 x86_64

- **Build/ship:** yes, unchanged, but **exclude the MTL sources from the x86_64 build**. Precedent
  for arch-conditional code already exists at `CMakeLists.txt:98` and, on the KK side,
  `bundle_should_include(kosmickrisp, arch) = (arch == arm64)`
  (`vk_driver_selection.h:809-822`). The Intel bundle stays MoltenVK-only. `[V]`
- **Run:** the Intel slice can **never** run Metal 4. `[V]`

### 3.6 One inconsistency to settle

The native API floor is macOS **26**, but the existing KosmicKrisp gate is **27**
(`kosmickrisp_min_macos_major`). The native backend is *not* KK, so its floor should be **26**
unless there is a deliberate product reason to require 27. Flagged in §10 Q1. `[V]`

---

## 4. Metal 4 vs a Metal 3 fallback

**A Metal 3 fallback is a second backend, not a build flag. Do not build it; MoltenVK already is the
pre-26 fallback.**

Why it forks rather than toggles `[V]`:

- **Synchronization is inverted.** Metal 3 `MTLCommandQueue` automatically hazard-tracks resources
  created `tracked` (device resources default `tracked`; heap resources default `untracked`); Metal 4
  `MTL4CommandQueue` ignores `hazardTrackingMode` entirely. Scope 2's barrier/residency redesign is
  Metal-4-only and does not carry over. `[V]`
- **Encoders differ:** Metal 3 has `MTLBlitCommandEncoder`; Metal 4 has none (copies/fills/mipmaps
  live on `MTL4ComputeCommandEncoder`). `[V]`
- **Binding differs:** Metal 3 binds resources directly on the encoder; Metal 4 routes through
  `MTL4ArgumentTable`. `[V]`
- **Compilation differs:** `MTLLibrary`/`newRenderPipelineState` vs `MTL4Compiler` + descriptors. `[V]`

Only SPIRV-Cross→MSL, the RSX→GLSL decompilers, the format table, the CMake wiring and the cache
namespace survive a fork — everything above the translator is re-derived. `[I]`

**Counter-argument to name, then reject:** Xcode 26's Metal-4 tooling gaps (Metal Debugger GPU
profiling / shader debugging not available for Metal 4) tempt a Metal 3 path *for debugging*. That
is a dev convenience, not a product fallback, and it doubles R10 maintenance. `[V]`

**Recommendation:** no Metal 3 renderer. Pre-26 macOS and Intel keep the shipping MoltenVK path; the
native backend is an **additive** macOS-26-arm64 fast path. If a Metal 3 backend is ever a real
product goal, scope it as a scope-2-sized project (difficulty **5**), not a CMake option. `[I]`

---

## 5. Language, SDK and CMake integration

### 5.1 Language

- **RPCS3 already compiles Obj-C++ today**: `metal_layer.mm` (`rpcs3/Emu/CMakeLists.txt:681`),
  with an object present under `build/`, even though `project()` declares only `C CXX`
  (`CMakeLists.txt:10`). So the toolchain can compile `.mm`; a broader Obj-C++ move may still want
  `enable_language(OBJCXX)` for clarity, but it is not a hard blocker. `[V]`
- **Recommended:** **metal-cpp (C++)** for the backend bulk (header-only, C++17, maps every Metal
  class into `MTL::`; keeps the code in C++ like the rest of RSX), plus a **small Obj-C++ shim** for
  the `CAMetalLayer` (an Obj-C object; `GetCAMetalLayerFromMetalView` already exists,
  `metal_layer.mm:9`). `[V]`
  metal-cpp build facts: one translation unit defines the implementation
  (`NS_/MTL_/CA_PRIVATE_IMPLEMENTATION`); **no ARC** (manual lifetimes, optional `NS::SharedPtr`);
  `supports…()` auto-false and weak-linked string constants. `[V]`
- **Alternative:** direct Obj-C++ (`MTL*.h`) throughout. Closer to the SDK docs and gets
  `API_AVAILABLE` enforcement (§3.4), at the cost of more Obj-C++ in a C++ codebase. A reasonable
  hybrid: metal-cpp for the object model, Obj-C++ at the availability seam. `[I]`

### 5.2 SDK & toolchain

- Build with **Xcode 26+ / macOS 26 SDK** (today `.ci/build-mac-homebrew.sh` already uses
  `xcrun --sdk macosx --show-sdk-path` and Apple clang first, Homebrew LLVM last, `:39`). `[V]`
- The **Metal CLI toolchain is a separate ~700 MB download** in Xcode 26
  (`xcodebuild -downloadComponent MetalToolchain`), needed only if offline compilation / `xcrun
  metal` validation is adopted. `[V]`
- RPCS3 pins LLVM 22 (Homebrew `llvm@22`); Obj-C++/Metal must go through **Apple clang**, which the
  Homebrew script already prefers. `[V]`

### 5.3 CMake wiring (patterns to copy)

| Change | Pattern already in-tree | Diff |
|---|---|---|
| `option(USE_METAL4 ...)` (default ON on arm64) | `option(USE_VULKAN ...)` block | 1 |
| `3rdparty/SPIRV-Cross` submodule + `add_subdirectory(EXCLUDE_FROM_ALL)` + alias | `3rdparty/CMakeLists.txt:92,378` | 2 |
| Vendor metal-cpp + one impl TU | WebKit/bkaradzic precedent | 3 |
| New `rpcs3/Emu/RSX/MTL/` source list under `if(USE_METAL4)` | the VK source list in `rpcs3/Emu/CMakeLists.txt` | 2 |
| Compile `.mm` (Obj-C++) | `rpcs3/Emu/CMakeLists.txt:681` already does | 2 |
| Exclude MTL sources on x86_64 | `CMakeLists.txt:98`; `bundle_should_include(...)` | 2 |

**Opportunity:** a native backend is the first thing that could let an Apple build set
`USE_VULKAN=OFF`; today that is a `FATAL_ERROR` (`3rdparty/CMakeLists.txt:203`). Out of scope to
force; flagged §10 Q7. `[V]`

---

## 6. Packaging, signing and entitlements

`.ci/deploy-mac-homebrew.sh` (the shipping script for this branch) bundles the Vulkan loader
(`:24`) and `libMoltenVK.dylib` + `MoltenVK_icd.json` (`:37-42`), prunes unused Qt frameworks
(`:45-52`), thins to the host arch (`:54-57`), then **ad-hoc** signs
(`codesign --force --deep -s -`, `:60-61`). `.ci/deploy-mac.sh` is the MacPorts analogue. No
Developer ID, notarization or entitlements are surfaced. `[V]`

What a native backend changes `[I]`:

- **On the macO26-arm64 fast path the loader + MVK ICD are unnecessary** (Metal is linked
  directly) — a packaging *win*, but it must stay **conditional** (pre-26 users still need MVK).
  **Recommendation: keep bundling MVK/loader for now.**
- **Entitlements:** Metal access under RPCS3's non-sandboxed, non-App-Store model needs **no**
  `com.apple.security.*` entitlement (the current MVK-based app needs none either). `[U]/[I]` — no
  Apple page states this negatively; it is reasoned from the absence of any Metal entitlement
  requirement.
- **Private API:** the current MVK bundle is the `-privateapi` variant (for `wideLines`/`sampleMask`);
  a native backend cannot use Apple private API, which matches the features scope 3 already
  clamped (e.g. `LINE_WIDTH` → 1.0). No packaging change implied. `[V]`
- **Caches/archives** live outside the bundle (`shaders_cache/` under the data dir), so signing is
  unaffected. If a fixed-shader `.metallib` were ever shipped it would live in `Contents/Resources`
  and be sealed by the signature — prefer explicit nested signing over the deprecated `--deep`.
  `[I]`

---

## 7. What CI can and cannot verify

### 7.1 Hosted-runner reality

- Standard macOS runners are VMs; there is **no GPU-passthrough option** for standard runners
  (long-standing request, `actions/runner-images` discussion #6138). Historically
  `MTLCreateSystemDefaultDevice()` returned `nil` inside a hosted runner (`#1779`). `[V]`
- As of Oct 2026 the **Intel** image exposes a *paravirtualized* GPU — *"Apple Paravirtualized
  Graphics Device … Metal 2, `supportsFamily(.metal3) false`"* (`#14836`) — i.e. even where a Metal
  device exists, it is a weak paravirtual device, not Metal 4. `[V]`
- A real GPU is available only on **larger paid runners** (e.g. `macos-15-xlarge`, M2 Pro with GPU
  acceleration); RPCS3 does not use them. `[V]`
- `macos-26` (arm64) images exist, but the same paravirtual GPU caveat applies; the OS being 26
  does not make the device `metal4`-capable. `[V]`

### 7.2 What CI *can* verify (GPU-free) — and should

- **Compile & link** the arm64 backend against the macOS 26 SDK: type availability,
  `__builtin_available` guards, metal-cpp impl TU, SPIRV-Cross build, CMake wiring. `[V]`
- **SPIR-V→MSL correctness gate:** SPIRV-Cross is CPU-only, so CI can run the *whole translation*
  for a corpus of RPCS3-generated shaders and pipe the MSL through `xcrun -sdk macosx metal …`
  (after `downloadComponent MetalToolchain`) — catching translator regressions and MSL syntax
  errors **with no GPU**. This is the highest-value CI addition. `[V]`/`[I]`
- **CPU unit tests** (`-DBUILD_RPCS3_TESTS`/`-DRUN_RPCS3_TESTS`) for cache keys, the gating
  predicate, and binding-flattening. `[V]`

### 7.3 What CI *cannot* verify

Actually executing Metal 4: device creation, residency, `MTL4CommandQueue` submission, present,
GPU-produced binary archives, `MTL_SHADER_VALIDATION`, and any frame-correctness regression — all
need M1+ hardware on macOS 26, which hosted standard runners do not dependably provide. `[V]`/`[I]`

### 7.4 Recommendation

1. Add a **`macos-26` arm64 compile-only** job; fold MSL validation into it.
2. Keep the Intel job building the emulator with MTL sources excluded.
3. For real rendering, use a **self-hosted Apple-silicon runner on macOS 26** (or a paid larger
   runner) as an opt-in/nightly job — **never** a required PR gate. `[I]`

---

## 8. Effort / risk table

| # | Item | Diff | Verdict / notes | Src |
|---|---|---|---|---|
| 1 | Vendor SPIRV-Cross + CMake + submodule | 2 | Mirror `3rdparty/glslang`; static; MIT option | §2.1 |
| 2 | Vendor metal-cpp + one impl TU | 3 | Header-only C++17, no ARC; well-trodden | §5.1 |
| 3 | `USE_METAL4` option + arm64/x86_64 gating | 2 | Copy `CMakeLists.txt:98` shape | §3.5, §5.3 |
| 4 | Availability gating (SDK + `__builtin_available` + family/device probe) | 3 | metal-cpp gives no compile-time help → must be manual | §3.4 |
| 5 | Build against macOS 26 SDK; keep target 15.0 | 2 | Weak-linking is standard | §3.3 |
| 6 | Runtime MSL path (`setSource`→`newLibrary`) | 2 | Apple-demonstrated | §2.2 |
| 7 | `"metal"` shader-cache namespace + archive keying | 3 | Reuse `rsx_cache`; key by device+OS | §2.5 |
| 8 | One `MTL4Compiler` per device + task pool rework (W11) | 3 | Replaces N-thread compiler pool | §2.5 |
| 9 | Offline `.metallib` for fixed helpers (**optional**) | 3–4 | Second codepath; skip first cut | §2.3 |
| 10 | Ship binary archives | 4 | Do **not**; build on device only | §2.4 |
| 11 | CI: `macos-26` compile-only job | 2 | Not a GPU test | §7.4 |
| 12 | CI: GPU-free SPIR-V→MSL→`metal` validation | 3 | Highest-value addition | §7.2 |
| 13 | CI: real GPU render test | 5 | No dependable hosted GPU | §7.3 |
| 14 | Packaging: conditional loader/MVK bundling | 3 | Low urgency | §6 |
| 15 | Signing: drop `--deep`, explicit nested signing | 2 | Independent cleanup | §6 |
| 16 | **Metal 3 fallback backend** | **5** | Second backend; do not build | §4 |
| 17 | Raise deployment target to 26.0 | 1 / high cost | Do **not**; keep 15.0 + gate | §3.3 |
| 18 | macOS build without Vulkan | 3 | Opportunity, not required | §5.3 |

---

## 9. Impossible / not-recommended

**Impossible (no path):**
1. **Metal 4 on Intel/x86_64** or on macOS ≤ 25 — hardware+OS wall. `[V]`
2. **Build-time precompilation of game shaders** — the set is generated at runtime from RSX ucode,
   per-title and unbounded. `[V]`/`[I]`
3. **A "Metal 3 fallback" as a build flag** — it is a distinct submission/sync/encoder/binding/
   compiler model. `[V]`/`[I]`
4. **Dependable Metal execution on standard GitHub-hosted macOS runners.** `[V]`

**Not recommended:**
1. Raising the deployment target to 26.0 — keep 15.0 + runtime gate. §3.3.
2. Shipping binary archives — GPU slices, OS-invalidation, opacity. §2.4.
3. Building a Metal 3 renderer — MoltenVK is the fallback. §4.
4. Reusing MoltenVK's vendored SPIRV-Cross as RPCS3's copy. §2.1.
5. `codesign --deep` — deprecated; prefer explicit nested signing. §6.
6. Fanning out one `MTL4Compiler` per worker thread (W11). §2.5.

---

## 10. Open questions for the human

1. **Native backend floor: 26 or 27?** The API floor is 26; the existing KK gate is 27. §3.6. `[I]`
2. **Keep shipping the Intel build?** With MTL sources excluded (MoltenVK-only), or drop Intel
   given macOS 27 reportedly ends Intel support? §3.5. `[I]`
3. **Confirm "build with the macOS 26 SDK, deploy to 15.0, runtime-gate".** §3.3. `[I]`
4. **metal-cpp vs direct Obj-C++** — and the license posture. metal-cpp is Apache-2.0 (questionable
   compiled into a GPLv2 binary, vs MoltenVK's *separately-loaded* dylib); SPIRV-Cross is fine via
   MIT. §5.1. `[U]`
5. **Runtime-only MSL, or offline-precompile fixed helper shaders?** The latter pulls the ~700 MB
   Metal toolchain into the build. §2.3, §5.2. `[I]`
6. **Shader-cache namespace + version** for `"metal"`, and whether archive keying is device+OS. §2.5. `[I]`
7. **Should macOS build without Vulkan?** A native backend would finally allow `USE_VULKAN=OFF`. §5.3. `[V]`
8. **CI budget** — compile+MSL validation only, vs self-hosted arm64/26 nightly, vs paid GPU runner. §7.4. `[I]`
9. **Is a Metal 3 backend ever an explicit product goal**, or is MoltenVK the accepted pre-26
   fallback? §4. `[I]`

---

## 11. Sources

**Apple — Metal 4, compilation & deployment**
- Metal overview ("Mac: Apple silicon (M1 or later)"; Metal 4 hardware list) — <https://developer.apple.com/metal/>
- Support for Metal on Apple devices ("macOS Tahoe 26 or later … Apple silicon") — <https://support.apple.com/en-us/102894>
- `MTL4Compiler` (macOS 26.0+; `makeLibrary`, `pipelineDataSetSerializer`) — <https://developer.apple.com/documentation/metal/mtl4compiler>
- `MTL4LibraryDescriptor` (`setSource`) — <https://developer.apple.com/documentation/metal/mtl4librarydescriptor>
- `MTL4PipelineDataSetSerializer` (`serializeAsArchiveAndFlushToURL`, `serializeAsPipelinesScript`; opaque objects) — <https://developer.apple.com/documentation/metal/mtl4pipelinedatasetserializer>
- Metal binary archives (GPU-specific slices, `metal-tt`) — <https://developer.apple.com/documentation/metal/metal-binary-archives>
- Metal libraries (Metal IR, `default.metallib`, source-string→IR→GPU compile) — <https://developer.apple.com/documentation/metal/metal-libraries>
- Using the Metal 4 compilation API (dedicated compiler, thread pool, archives; "both Metal 3 and 4 can load binary archives") — <https://developer.apple.com/documentation/metal/using-the-metal-4-compilation-api>
- Porting your Metal code to Apple silicon (Apple-family vs Mac-family) — <https://developer.apple.com/documentation/apple-silicon/porting-your-metal-code-to-apple-silicon>

**Apple — hazard tracking / synchronization**
- `MTLHazardTrackingMode` ("Metal doesn't apply hazard tracking to … MTL4CommandQueue") — <https://developer.apple.com/documentation/metal/mtlhazardtrackingmode>
- Resource synchronization (device resources default `tracked`, heaps default `untracked`) — <https://developer.apple.com/documentation/metal/resource-synchronization>
- Managing Metal 4 synchronization (game-porting skill; "Metal 3 handled this implicitly") — <https://github.com/apple/game-porting-toolkit/blob/main/game-porting-skills/skills/managing-metal4-synchronization/SKILL.md>

**Apple — availability / SDK**
- `API_AVAILABLE` macro — <https://developer.apple.com/documentation/os/api_available>
- Frameworks and Weak Linking — <https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPFrameworks/Concepts/WeakLinking.html>
- Detecting GPU features and Metal software versions (`#available` / `supportsFamily`) — <https://developer.apple.com/documentation/metal/detecting-gpu-features-and-metal-software-versions>
- Clang AttributeReference (`availability`, weak linking) — <https://clang.llvm.org/docs/AttributeReference.html>

**metal-cpp**
- Get started with metal-cpp (C++17, `*_PRIVATE_IMPLEMENTATION`, `supports…()` auto-false, weak-linked string constants) — <https://developer.apple.com/metal/cpp>
- apple/metal-cpp — <https://github.com/apple/metal-cpp>
- Local `/tmp/metal4-refs/metal-cpp/Metal/{MTL4Compiler,MTL4LibraryDescriptor,MTL4PipelineDataSetSerializer,MTLDevice}.hpp` (`GPUFamilyMetal3=5001`, `GPUFamilyMetal4=5002`, `newCompiler`)

**SPIRV-Cross**
- README / LICENSE (Apache-2.0; CMake is the supported build; C++11) — <https://github.com/KhronosGroup/SPIRV-Cross>
- Fedora packaging review (per-file licenses; `spirv_msl` = Apache-2.0 OR MIT) — <https://bugzilla.redhat.com/show_bug.cgi?id=2394940>

**CI / hosting**
- GitHub-hosted runners reference (macOS images; larger GPU runners) — <https://docs.github.com/en/actions/reference/runners/github-hosted-runners>
- `actions/runner-images` #1779 — `MTLCreateSystemDefaultDevice` returns nil — <https://github.com/actions/runner-images/issues/1779>
- `actions/runner-images` #14836 — macos-15-intel paravirtual GPU (Metal 2, `.metal3` false) — <https://github.com/actions/runner-images/issues/14836>
- `actions/runner-images` discussion #6138 — "Add support for Metal in macOS images" — <https://github.com/actions/runner-images/discussions/6138>

**Version wall corroboration**
- Metal kernels built for macOS 26+ fail on macOS 15 at load — <https://github.com/huggingface/kernels/issues/705>

**Pristine reference checkouts under `/tmp/metal4-refs/`**
- `mesa/docs/drivers/kosmickrisp/workarounds.rst` — W1 (~465), W2 (~427,443), W11 (~162)
- `MoltenVK/ExternalRevisions/SPIRV-Cross_repo_revision`, `MoltenVK/MoltenVKShaderConverter/SPIRV-Cross`

**RPCS3 in-repo (read-only)**
- `CMakeLists.txt:10,98`; `3rdparty/CMakeLists.txt:92,160-181,185,203,378`
- `rpcs3/CMakeLists.txt:101,110`; `rpcs3/rpcs3.plist.in:31-32`
- `rpcs3/Emu/CMakeLists.txt:681`; `rpcs3/Emu/RSX/VK/vkutils/metal_layer.mm:9`
- `rpcs3/Emu/RSX/VK/VKGSRender.cpp:721`; `VKGSRenderTypes.hpp:40`; `rpcs3/Emu/RSX/rsx_cache.h:104,212-240`
- `rpcs3/Emu/RSX/VK/vkutils/vk_driver_selection.h:801-822`
- `rpcs3/Emu/system_config.h:159,177`; `rpcs3/Emu/system_utils.cpp:219-221`; `rpcs3/Emu/cache_utils.cpp:14`
- `.ci/build-mac-homebrew.sh:39,51-52,62`; `.ci/deploy-mac-homebrew.sh:24,37-42,45-61`; `docs/research/12-macos-build-guide.md:77-78`
- `.github/workflows/rpcs3.yml:192,196`

---

*End of Scope 6. The single most consequential verified constraints: Metal 4 is
Apple-silicon/macOS-26-only (so x86_64 can never run it, and it is not a Metal-3 variant), and
GitHub-hosted standard macOS runners cannot execute it. The two `[U]` items are the "no Metal
entitlement" conclusion (§6) and the metal-cpp-vs-GPLv2 license posture (§10 Q4).*
