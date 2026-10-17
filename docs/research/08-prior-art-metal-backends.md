# 08 — Prior art: native Metal backends in other emulators

Evidence for how much a native Metal backend is worth, and what it costs, is thin but consistent.

## Dolphin (GameCube/Wii) — the closest case study

Timeline (Dolphin Progress Report Jul–Aug 2022, and earlier):

1. **Stenzek first tried a native Metal backend** early in Metal's life. It **stalled** ("Dolphin's quirks and unfamiliarity with the Metal API").
2. Dolphin instead shipped **MoltenVK** as its macOS path — *"a very successful compromise… it may very well have saved our macOS support."*
3. **TellowKrinkle** (author of PCSX2's Metal backend) then wrote a **native Metal backend** for Dolphin.
4. Initial results looked *too* good — because **MoltenVK had regressed** (a `VkDescriptorPool` change, later bisected and fixed with help from TellowKrinkle). Notably, **having a second backend is what exposed the regression.**
5. After the fair-fight re-test, native Metal was **only a little faster** overall; standout gains **+23%** and **+30%** in GPU-heavy titles (Skyward Sword, Rogue Leader).

> Takeaway: a native Metal backend over a *healthy* MoltenVK is a **modest, title-dependent win**, not a transformation. Its second value is **diagnostic independence** (two backends catch each other's regressions).

## FNA (XNA reimplementation) — maintenance reality

From the MoltenVK use survey (#1085):
- FNA ships **both** Metal and Vulkan (via MoltenVK). Its Metal backend *"took a long time to get there"* and *"maintenance has been a lot tougher… since we only have one person that understands it well."*
- On overhead: *"while MoltenVK is obviously going to add overhead vs directly going to Metal, we're not entirely convinced that the delta is significant in practice"*, and they *"often see places where MoltenVK is faster than our direct Metal work."*

> Takeaway: the **bus factor** of a bespoke backend is the dominant risk, more than the theoretical API overhead.

## PCSX2 — the reference implementation style

- TellowKrinkle's Metal backend lives under `pcsx2/GS/Renderers/Metal/` with a C-accessible shim (`GSMetalCPPAccessible.h`) so the Obj-C++ Metal layer can be called from the C++ core — a pattern directly applicable to RPCS3 (`ComputePass`/renderer split, C++ core + `.mm` glue).
- PCSX2 exposes a **Renderer = Metal** choice in its UI alongside Vulkan/OpenGL/Software (`GSRendererType::Metal`), i.e. exactly the "selectable backend" UX RPCS3 would add.
- PCSX2 supports macOS "officially" with Metal as a first-class renderer.

## RPCS3's own stance (historical)

- Issue **#11922** (2022): maintainers said *"No one wanted a Native metal version. MoltenVK is doing perfectly fine."* — i.e. **no upstream appetite** for a native Metal backend at that time.
- The current macOS effort has instead been about **making the Vulkan path work better on Apple GPUs** (`#19311` "Disable Apple-specific hacks for features & formats supported on Apple GPUs", a whole `OS: macOS` issue label), plus the KK ICD work.

> Takeaway: an RPCS3 native Metal backend is a **solo/personal northstar**, not an upstream-endorsed direction. Treat it as a research project with an explicit maintenance owner.

## Cemu — the constraint warning

- Cemu #444 (2022): Metal pipeline creation fails for game-driven vertex strides that aren't multiples of 4; *"The buffer stride and data comes from the game so this isn't something we really have control over… There is no easy solution."*
- Demonstrable, first-hand evidence that **RSX-like vertex handling will need a dedicated Metal strategy** (vertex pulling / clamping).

## Bottom line for RPCS3

| Question | Evidence-based answer |
|---|---|
| Will native Metal beat MoltenVK? | Yes, but **modestly** (Dolphin: a little, up to ~20–30% in GPU-heavy cases). |
| Will it beat KosmicKrisp? | Probably similarly modest, and KK is a moving target (KS perf tripled in a year, "not too far behind native Metal"). |
| Biggest cost | The **per-title GPU quirk surface** and the **Metal/MSL bug surface**, plus long-term maintenance by a small number of people. |
| Biggest hidden benefit | **Independent validation** of the Vulkan path (caught a 45% MVK regression in Dolphin). |

## Sources
- Dolphin Progress Report Jul–Aug 2022: <https://dolphin-emu.org/blog/6I/>
- MoltenVK use survey #1085 (FNA): <https://github.com/KhronosGroup/MoltenVK/issues/1085>
- PCSX2 GS backend selection: <https://github.com/PCSX2/pcsx2/blob/master/pcsx2/GS/GS.cpp>
- RPCS3 #11922: <https://github.com/RPCS3/rpcs3/issues/11922>
- Cemu #444: <https://github.com/cemu-project/Cemu/issues/444>
