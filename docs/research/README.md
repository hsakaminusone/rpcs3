# RPCS3 macOS GPU backend research

Running notes for two related efforts:

1. **Near-term:** add a selectable **KosmicKrisp** Vulkan ICD alongside **MoltenVK**.
2. **Northstar:** write a **native Apple Metal backend** (Metal 4 / macOS 27).

Status: docs 01–11 are research/planning; the MVK/KosmicKrisp selection is now **implemented** on
this branch, and **doc 12** is the macOS build guide. See `INSTALL-LEDGER.md` for local-machine impact.

## Docs

| Doc | Contents |
|---|---|
| [01-drivers-kosmickrisp-moltenvk.md](01-drivers-kosmickrisp-moltenvk.md) | The two Apple Vulkan ICDs, versions, requirements, packaging, ICD selection, known-issue surface |
| [02-fork-and-branch.md](02-fork-and-branch.md) | Git/fork setup done so far |
| [03-rpcs3-backend-architecture.md](03-rpcs3-backend-architecture.md) | Codebase map: backend contract, VK subsystem inventory, reusable code, shader pipeline, memory model |
| [04-backend-selection-design.md](04-backend-selection-design.md) | UI selection + keep-both-drivers + fallback design (solves "problem 3") |
| [05-metal-backend-northstar.md](05-metal-backend-northstar.md) | Feasibility/difficulty analysis for a native Metal backend |
| [06-open-questions.md](06-open-questions.md) | Unresolved questions and TODOs |
| [07-metal4-and-api-constraints.md](07-metal4-and-api-constraints.md) | Metal 4 API map + hard Metal/MSL constraints emulators hit |
| [08-prior-art-metal-backends.md](08-prior-art-metal-backends.md) | Dolphin / PCSX2 / Cemu / FNA lessons on native Metal backends |
| [09-shadps4-kosmickrisp-reference.md](09-shadps4-kosmickrisp-reference.md) | shadPS4's working KK integration (commits, diffs, reusable patterns) |
| [10-kk-integration-plan.md](10-kk-integration-plan.md) | File-by-file plan + size estimate to ship MVK+KK selection |
| [11-mvk-kk-coexistence.md](11-mvk-kk-coexistence.md) | Deep dive: MVK+KK coexistence, problem catalogue (C1–C10), latest version pins |
| [12-macos-build-guide.md](12-macos-build-guide.md) | How to build this branch on macOS with Homebrew: deps, configure flags, the ffmpeg/framework link fix, packaging, tests, gotchas |
| [13-kosmickrisp-bundle-ledger.md](13-kosmickrisp-bundle-ledger.md) | Pin/provenance/sha256 for the bundled KosmicKrisp ICD, the bundle layout, and the `VK_LOADER_SEARCH_ONLY_IN_BUNDLE` isolation mechanism |
| [INSTALL-LEDGER.md](INSTALL-LEDGER.md) | Running record of everything this project installs, + a safe wipe runbook |

## TL;DR

- Repo currently ships **MoltenVK only** (submodule commit `4588705`, deploy download **v1.4.2**).
- The **ICD-loading groundwork is already merged** (`b76ca8807`): macOS links the Vulkan Loader and can load any ICD.
- **KosmicKrisp** is now **Vulkan 1.4 conformant** (Sept 2026), shipped in **Vulkan SDK 1.4.363.0** and **Mesa 26.x / Homebrew `mesa`**; needs **macOS 26+ / Metal 4 / Apple Silicon**. Perf is ~**3× since 1.3** and "not too far behind native Metal".
- RPCS3 **hard-codes** `driver_vendor::MVK` on `__APPLE__` (`device.cpp:288`), so a KK ICD would be misdetected. Draft PR **#17902** fixes this; its own thread confirms **backend selection is still unsolved**.
- **macOS 26 Tahoe is the last Intel release; macOS 27 "Golden Gate" is Apple-Silicon-only.** So KK is viable everywhere on macOS 27, and MVK is only needed for **Intel / macOS ≤ 25** and as an escape hatch.
- **Backend selection from the UI is feasible.** Bundle both ICDs; distinguish devices by `driverID` (MVK=`14`, KK=`28`); "Auto" prefers KK when present.
- A **native Metal backend** is a large but well-isolated ~**30k LOC** subsystem; ~**55k LOC** of shared RSX code is reusable. Prior art (Dolphin/FNA) says the win over a *healthy* MVK/KK is **modest** and maintenance-heavy ⇒ treat as a research northstar; ship MVK+KK selection first.
- **shadPS4 already runs KosmicKrisp** (full swap; macOS 26 + Apple Silicon only) — a working reference for the mechanics (doc 09).
- **MVK+KK selection plan** (doc 10): ~**650–1200 LOC** across ~20–25 files; ~**2–4 weeks** for a working selectable build, plus open-ended KK bring-up.
- **Coexistence is a loader problem, not a driver-conflict problem** (doc 11): the loader filters instance extensions **per driver** (LDP_LOADER_9), so enabling the portability flag for MVK is harmless to KK. Real work = disambiguate by `driverID`, key quirks on `driver_vendor` not `__APPLE__`, and **bundle a real loader** (KK, unlike MVK, does not export the public Vulkan API). Latest pins: **MoltenVK v1.4.2**, **loader/headers 1.4.363.0**, **KK `af7dd0a3`** (or Mesa 26.2.5).

_Last updated: 2026-10-15_
