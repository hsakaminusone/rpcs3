# REVIEW — RSX is the specification, Metal 4 is the implementation

This review covers every fix landed in batch 1 and in the following synchronization,
surface and clear batches.  The lens is narrow:

- **RSX is the specification.**  The hardware behaviour is the target.
- **Vulkan and OpenGL are evidence of the RSX behaviour.**  Their code tells us what the
  guest expects.  They are not the target.
- **Metal 4 is the implementation.**  The mechanism must be the native Metal 4 mechanism
  for the RSX behaviour.  Apple documentation and KosmicKrisp are the references for that
  mechanism.

A fix passes this review only if the RSX rule, the evidence for it, and the native Metal
mechanism are each stated.  "Vulkan does X, so Metal does X" fails, even when X is correct.

## 1. Verdict by fix

| Fix | RSX rule | Metal mechanism | Verdict |
|---|---|---|---|
| PF1–PF7, PF10, PF11 | A guest flip must compose and present the frame; the acquire path must not end the frame. | Own frame-open flag, bounded feedback wait, slot generation. | **Pass.**  The rule is emulator-level, the mechanism is native. |
| DR1, DR2 | `primitive_restart_barrier` ends the current strip before the next index. | One Metal draw per subrange. | **Pass.**  This is the RSX strip rule, not a Vulkan pass rule. |
| DR7 | A refused draw must still run the clause bookkeeping. | `execute_nop_draw()` (`RSXThread.cpp:1002`) — CPU-side dependency execution, no GPU draw. | **Pass.**  It is not a Vulkan render-pass workaround. |
| SF1, SF2, SF12 | A bound target must not be released; a stale lock must not be taken. | Cache guard order. | **Pass, wording only.**  The code follows the cache invariant; several comments justify it as "Vulkan order".  Reword to the invariant. |
| SF6 | Surface identity includes the guest sample count. | Cache key. | **Pass.** |
| SF11 | Two surfaces are interchangeable only when the guest bytes and their meaning match. | Host pixel format **and** component layout. | **Fail, then fixed.**  See R4. |
| V1 | A swizzled texture's extent comes from its layout, not from a zero pitch. | Scan buffer. | **Pass.** |
| F1 | `output_scaling=nearest` selects nearest sampling. | Sampler selection. | **Pass.** |
| SY1, SY2, SY3, SY5, SY9 | Resources may not be released while the GPU can still read them. | Bounded wait, quarantine, slot release, residency removal, atomic generation. | **Pass.**  Native Metal lifetime.  The 10 s bound is a Metal recovery policy, not an RSX rule. |
| S3 | The clear mask is a per-channel write enable. | Pipeline write mask. | **Pass.** |
| S4 | The clear is a word copy of the clear register; a format stores a subset. | `resolve_surface_clear` from the RSX helpers, `MTLClearColor` or a scissored fill. | **Pass for the in-Metal path.**  The surface memory boundary is R2. |
| S5 | A stencil clear writes the reference through the stencil write mask. | Full-screen draw, compare always, pass op replace, write mask. | **Pass.** |
| S7 | A sampled surface returns the guest channels. | `MTLTexture.setSwizzle`. | **Fail in part, then fixed for textures.**  See R1 and R2. |

## 2. Findings

### R1 — the packed 16-bit texture formats were mapped with the Vulkan/OpenGL *format* semantics but Metal's `B5G6R5` bit order (confirmed, High)

**Evidence for the guest layout.**

- The RSX clear and texture decoders read the guest word as `R` in the high field:
  `TextureUtils.cpp:47-49` (`r5 = bits >> 11`), `:71-74` (`a1` high, then `r5`),
  `:86-89` (`r5` high, `a1` low), `:101-103` (`r6` high).
- Vulkan on the same target uses the guest layout directly:
  `VKFormats.cpp:200-205` selects `VK_FORMAT_R5G6B5_UNORM_PACK16`,
  `VK_FORMAT_R5G5B5A1_UNORM_PACK16`, `VK_FORMAT_A1R5G5B5_UNORM_PACK16`,
  `VK_FORMAT_R4G4B4A4_UNORM_PACK16`.  Vulkan's packed format names are most-significant
  field first, so this is the bit order.
- OpenGL matches it with `GL_RGB565`, `GL_BGR5_A1`, `GL_RGBA4` (`GLTexture.cpp:89-97`),
  which is why OpenGL can keep an identity swizzle.

**Evidence for Metal's bit order.**

- Metal names a packed format from the most significant field: `B5G6R5Unorm` is
  `B[15:11]`, `G[10:5]`, `R[4:0]`; `BGR5A1Unorm` is `B[15:11]`, `G[10:6]`, `R[5:1]`,
  `A[0]`; `A1BGR5Unorm` is `A[15]`, `B[14:10]`, `G[9:5]`, `R[4:0]`; `ABGR4Unorm` is
  `A[15:12]`, `B[11:8]`, `G[7:4]`, `R[3:0]`.
- KosmicKrisp proves the direction with a format it must emulate:
  `.kk-mesa/src/kosmickrisp/vulkan/kk_format.c:231-234` maps Mesa `A4R4G4B4` (A high) to
  Metal `A4B4G4R4` plus `MTL_SWIZZLE_BGRA`.  That permutation is correct only when Metal's
  `A4B4G4R4` is A high.  The native rows at `:224` (`B5G6R5`) and `:226` (`B5G5R5A1`)
  then fix the rest.

**The defect.**  The backend copied OpenGL's identity swizzle onto a host format with the
opposite field order, and it swapped the two alpha-bearing formats:

| Guest | Old host | Old read | Correct host | Correct read |
|---|---|---|---|---|
| `R5G6B5` | `B5G6R5Unorm` | identity | `B5G6R5Unorm` | `{One, Blue, Green, Red}` |
| `R5G5B5A1` | `A1BGR5Unorm` | identity | `BGR5A1Unorm` | `{Alpha, Blue, Green, Red}` |
| `A1R5G5B5` | `BGR5A1Unorm` | identity | `A1BGR5Unorm` | `{Alpha, Blue, Green, Red}` |
| `D1R5G5B5` | `BGR5A1Unorm` | identity | `A1BGR5Unorm` | `{One, Blue, Green, Red}` |
| `A4R4G4B4` | `ABGR4Unorm` | identity | `ABGR4Unorm` | `{Red, Alpha, Blue, Green}` |

The guest `A4R4G4B4` is stored `R4G4B4A4` (R high, A low), which Vulkan states directly.
Against Metal's `ABGR4` every field moves one place, so the read is a four-way rotation,
not a red/blue swap.  Identity put the guest alpha in red, red in alpha, green in blue and
blue in green.

**Fixed.**  `MTLFormats.cpp:389-406` and `:467-490`; tests in
`test_rsx_metal4_mapping.cpp` (`PackedFormatsUseTheGuestBitOrderWithAPermutedRead`).

### R2 — the packed 16-bit *surfaces* are consistent inside Metal but wrong at guest memory (confirmed, Medium)

The surface path chooses `B5G6R5Unorm`/`BGR5A1Unorm` (`MTLFormats.cpp:47-53`) and the
identity read (`:106-111`).  That combination is **self-consistent**: the fragment output
writes the guest red into the host red field, the clear does the same, and the read returns
it from the same field.  A frame rendered and sampled entirely inside Metal is correct.

The guest memory boundary is not.  `surface::write_to_memory` is a raw texture-to-buffer
copy (`MTLSurface.cpp:482-517`).  The guest expects red in the high field; the host stores
it in the low field.  Every write-back of a packed surface therefore exchanges the red and
blue bit fields (and, for the `x1r5g5b5` family, moves the alpha bit as well).  A surface
that is uploaded from guest memory and not fully redrawn has the same problem in reverse.

This is a Metal-specific problem: Vulkan avoids it by using the guest format natively
(`VKGSRender.cpp:44-53`).  Metal has no such format.

**Plan.**  Choose one convention and apply it at all four sites together (fragment output,
clear, read, memory transfer):

1. **Storage equals guest memory.**  The fragment output writes the guest red into the
   host blue field (a per-surface output permutation), the clear stores the same, the read
   permutes back, and the raw write-back becomes correct.  No per-pixel conversion.  This
   is the performant design.
2. **Thirty-two-bit staging.**  Render into `BGRA8Unorm` and pack on write-back.  Simpler
   shader, twice the bandwidth and a conversion on every transfer.

Until a title that uses a packed surface is measured, this stays planned.  S7's identity
read must not be changed alone: without the output permutation it would make a rendered
surface sample red and blue exchanged.

### R3 — `R6G5B5` has no packed Metal host (confirmed, Medium)

`R6G5B5` stores six bits of red, five of green and five of blue
(`TextureUtils.cpp:101-103`).  Metal's `B5G6R5Unorm` stores six bits of **green**.  A raw
upload therefore places the six-bit red field in the five-bit blue field: no component
permutation can recover it.  Vulkan emulates it by sampling the six-bit field from
`R5G6B5` and discarding the high bit (`VKFormats.cpp:202`), which is a known approximation.
The correct Metal implementation expands `R6G5B5` to `BGRA8Unorm` on upload with the
existing `convert_rgb655_to_bgra8` (`TextureUtils.cpp:99-111`), as the Intel path already
does (`TextureUtils.cpp:966-1015`).  Planned: needs the uploader capability to be a
runtime decision instead of the current compile-time `#if`.

### R4 — `is_compatible_surface` lost the channel meaning (confirmed, Medium)

`MTLSurfaceCache.cpp:36-42` compared only the host pixel format.  `x8r8g8b8_z`,
`x8r8g8b8_o` and `a8r8g8b8` all map to `BGRA8Unorm` but read alpha as zero, one and the
stored byte respectively (`MTLFormats.cpp:96-111`).  A region merge keeps the destination
surface's format, so accepting one as the other changes the meaning of the stored alpha.
SF11 fixed the guest-versus-host mistake but introduced this one.  **Fixed** by comparing
`get_native_component_layout()` as well.

### R5 — the format probe could not have measured bit order (methodology, High)

The previous mapping cited `/tmp/fmtprobe2/main.mm` as proof of the identity read.  A probe
that writes a channel through a shader and reads the same channel back is invariant under
any channel permutation; it proves linearity, not bit order.  A valid probe must write a
known **word** to memory (or a texture with `replaceRegion`) and read that word back.  The
repo evidence above is stronger and does not need a device run.

## 3. Documentation changes

- `METAL4-TODO.md`: the rule at the top said "translate the RSX behaviour to Metal 4 as
  Vulkan and OpenGL do".  That sentence is the source of R1.  It now says RSX is the
  specification and Vulkan/OpenGL are evidence.
- `DEVIATIONS-TEXTURE.md`: the packed-format rows now cite `VKFormats.cpp:200-205` and the
  Metal field order, not the GL swizzle.
- `HANDOFF.md`: the texture table records the permutation.

## 4. Exit criteria for this review

- No fix cites "Vulkan/OpenGL does X" as its reason.  The reason is the RSX rule; the
  reference only shows the rule.
- Every host format whose field order differs from the guest has an explicit permutation,
  at every site that touches the bits: upload, clear, fragment output, read, write-back.
- `is_compatible_surface` compares everything that changes the sample meaning.

<!-- doc-end -->
