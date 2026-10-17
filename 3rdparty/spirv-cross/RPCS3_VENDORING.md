# SPIRV-Cross — RPCS3 vendoring record

Upstream: https://github.com/KhronosGroup/SPIRV-Cross
Vendored commit: `300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1`
Commit date: 2026-10-14
Commit subject: `MSL: Preserve negative zero in precise multiplication`

Vendored as **plain files**, on the model `06` used for `metal-cpp`: no git
submodule, no `.git`, no tests, no samples, no shaders, no non-MSL/GLSL backends.

## Copied

- Core: `GLSL.std.450.h`, `NonSemanticShaderDebugInfo100.h`, `spirv.hpp`,
  `spirv_common.hpp`, `spirv_cfg.{hpp,cpp}`, `spirv_cross.{hpp,cpp}`,
  `spirv_cross_containers.hpp`, `spirv_cross_error_handling.hpp`,
  `spirv_cross_parsed_ir.{hpp,cpp}`, `spirv_parser.{hpp,cpp}`.
- GLSL backend (`CompilerMSL` derives from it): `spirv_glsl.{hpp,cpp}`.
- MSL backend: `spirv_msl.{hpp,cpp}`.
- Public support headers: `include/spirv_cross/**`.
- License: `LICENSE` (Apache-2.0; the per-file SPDX headers offer
  Apache-2.0 OR MIT, and RPCS3 takes the MIT option — see `06` §2.1).

`NonSemanticShaderDebugInfo100.h` is not in upstream's `spirv-cross-core-sources`
list, but `spirv_parser.cpp` includes it, so it is required to compile.

## Dropped

The C API (`spirv_cross_c.*`, `spirv.h`), the C++/HLSL/reflection/util backends,
`main.cpp`, all `shaders*`, `reference`, `samples`, `tests-other`, `gn`,
`cmake`, `pkg-config`, `Package.swift`, the make/CMake build files and the CI
files.

## Build integration

See `rpcs3.cmake` in this directory. It is the block the parent inserts into
`3rdparty/CMakeLists.txt` (this vendoring pass added new files only). Two points
matter:

- SPIRV-Cross is third-party. RPCS3 builds with `-Werror=*`
  (`buildfiles/cmake/ConfigureCompiler.cmake:51-79`) and SPIRV-Cross has
  unannotated switch fall-throughs, so the target appends `-w -Wno-error`.
- RPCS3 builds with `-fno-exceptions` (`ConfigureCompiler.cmake:33`), so the
  target defines `SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS`
  (`spirv_cross_error_handling.hpp:30-77`). A SPIRV-Cross compile error then
  aborts instead of throwing. This is a known risk; it matches RPCS3's own
  fatal-error model.
