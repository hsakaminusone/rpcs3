# Conflict register

A conflict is a place where a design doc and the RSX tree disagree about a dial.
The rule: record the dial, both references, and move on.  Never rewrite either
side silently.  The RSX tree wins on what the RSX does.  A design doc wins on how
we re-express the RSX in Metal 4.

---

## C1 — the clear region

**Dials.** `M_NV4097_SET_CLEAR_RECT_HORIZONTAL` (0x1d98),
`M_NV4097_SET_CLEAR_RECT_VERTICAL` (0x1d9c).

**Design doc says.** `LLD-surface.md:137-138`: "`SET_CLEAR_RECT_HORIZONTAL`/
`_VERTICAL` are unbound but read by `clear_surface`; they scope the clear to a
sub-rectangle."  `LLD-surface.md:257-259` and `:288-289` repeat that the clear
rect is the sub-rectangle source.

**The tree says.** The two registers have no reader anywhere:

- They are bound to `nullptr` in the method table
  (`rsx_methods.cpp:1476-1477`).
- No accessor exists in `rsx_methods.h` (no `clear_rect` symbol).
- Both backends scope the clear with the **scissor**, not the clear-rect
  registers: `VKGSRender.cpp:1441-1454` (`m_scissor`, then `rsx::clip_region`),
  `GLGSRender.cpp:668-671` (`scissor_origin_x`/`scissor_width`/...).
- The framebuffer layout reads neither register (`RSXThread.cpp:1472-1660`).

**Resolution.** Implement the clear region from the scissor state
(`scissor_origin_x`/`scissor_width`/`scissor_origin_y`/`scissor_height`,
`rsx_methods.h:204-222`), exactly as VK and GL do.  `MTLSurfaceState` reads the
scissor and records the conflict here.  The LLD text is left unchanged for its
author to correct; the disposition of the two dials stays as the shard records
it, because the registers are still unbound guest state.

**Open for the LLD author.** If a title is later found that writes the clear-rect
registers and expects them to take effect, the RSX would need a reader that does
not exist today.  No such reader is added by this port.
