# RSX primary sources

Reference material for the Metal 4 port.  Cross-check RSX method and value
semantics here.  The repo design docs still govern how a dial is re-expressed.

| Source | Status |
|--------|--------|
| `nucleus-gpu.md` — Nucleus GPU doc (AlexAltea/nucleus `docs/technical/ps3/gpu.md`) | captured |
| `nucleus-cpu.md` — Nucleus CPU doc | captured |
| `nucleus-system.md` — Nucleus system doc | captured |
| https://www.psdevwiki.com/ps3/RSX | NOT captured: Cloudflare returns 403/429 to every fetch path. |
| https://www.psdevwiki.com/ps3/RSXFIFOCommands | NOT captured: Cloudflare returns 403/429 to every fetch path. |

The Nucleus documents are the machine-readable substitute for the psdevwiki
pages.  `gpu.md` covers the RSX FIFO, the method registers and the command
semantics that the dial inventory encodes.
