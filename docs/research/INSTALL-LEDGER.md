# INSTALL LEDGER — what this project adds to the machine (for later cleanup)

Purpose: a running record of everything **this project** installs/creates, so it can be fully removed
later **without** touching pre-existing system state. Restore point: repo branch
`kosmickrisp-integration` @ `17543d273`.

> ⚠️ **Baseline matters.** Homebrew already contained a set of unrelated packages before we started
> (see §1). The wipe must be **delta-based** (`after − baseline`). Never `rm -rf /opt/homebrew` or
> `brew uninstall --ignore-dependencies` everything.

---

## 1. Baseline snapshot (captured 2026-10-15T15:25:06Z, BEFORE installs)

Machine: macOS 27.0.1, arm64. Disk free at baseline: **~39 GiB** (tight — see §5).

**Homebrew prefix:** `/opt/homebrew` (total size at baseline ≈ **2.9 GB**)

**Formulae present at baseline (`/opt/homebrew/Cellar`):**
```
abseil ada-url apktool brotli c-ares ca-certificates cairo fmt fontconfig freetype
gettext gh giflib git glib go graphite2 harfbuzz hdrhistogram_c highway icu4c@78
jpeg-turbo json-c libffi libnghttp2 libpng libtiff libunistring libuv libx11 libxau
libxcb libxdmcp libxext libxrender little-cms2 llhttp lz4 lzo merve nbytes node
openjdk openssl@4 pcre2 pixman playwright-cli readline simdjson simdutf sqlite
uvwasi webp xorgproto xz zsh-autosuggestions zsh-syntax-highlighting zstd
```
**Casks at baseline (`/opt/homebrew/Caskroom`):** `android-commandlinetools`, `android-platform-tools`

**Not present at baseline (so anything below is OURS):** `cmake`, `ninja`, `llvm`, `qt`, `sdl3`,
`opencv`, `vulkan-headers`, `vulkan-loader`, `molten-vk`, `glew`, `googletest`, `ffmpeg`, `pkg-config`.

**Git submodules:** ALL uninitialized at baseline (`git submodule status` → every line prefixed `-`).

**Repo build dirs:** none (`build/`, `build-tests/` absent). Note `buildfiles/` is a tracked dir, not ours.

**/tmp leftovers from earlier research:** `/tmp/shadPS4` ≈ 101 MB (KosmicKrisp reference clone).

---

## 2. What this project adds (expected / to be reconciled after the agents finish)

### 2a. Homebrew formulae (installed by the build agent)
`cmake ninja llvm qt sdl3 opencv vulkan-headers vulkan-loader molten-vk glew googletest ffmpeg
pkg-config` **+ all their transitive dependencies**. Homebrew pulls many deps (e.g. `qt` alone drags
in dozens). Compute the exact delta with §3 rather than this list.

### 2b. Git submodule contents
`git submodule update --init` on `3rdparty/**` downloads GB into the repo (LLVM/SDL/OpenCV/ffmpeg may
be excluded if system versions are used — reconcile via §3).

### 2c. Build trees
- `build/` (main app) — can be several GB (Release).
- `build-tests/` (test suite).
Caches: `~/.cache/ccache` (or `$CCACHE_DIR`), `~/Library/Caches/Homebrew`.

### 2d. Downloads / clones
- `/tmp/shadPS4` (101 MB, research).
- Any Qt downloaded by the CI-style `qt-downloader` into `/tmp/Qt` or `qt-downloader/` in the repo.
- MoltenVK `MoltenVK-macos-privateapi.tar` + extracted `MoltenVK/` inside `build/bin` during deploy.
- Homebrew download cache (`~/Library/Caches/Homebrew/downloads`).

### 2e. Repo artifacts (ours)
- `docs/research/*.md` — research docs 01–11 + `README.md`, plus `BUILD-STATUS.md` (build agent) and this ledger. Whole `docs/` dir is untracked.
- **Tests agent (done, verified):**
  - `rpcs3/Emu/RSX/VK/vkutils/vk_driver_selection.h` (new, header-only seam, 993 lines)
  - `rpcs3/tests/test_vk_driver_selection.cpp` (new, 1268 lines, 78 `TEST`s / 320 assertion macros)
  - `rpcs3/CMakeLists.txt` (**modified**, +1 line registering the test file at ~line 201)
- **Build agent (in progress):** `.ci/build-mac-homebrew.sh` (new); may edit `.ci/*`, `3rdparty/*`, root `CMakeLists.txt`.
- Build trees `build/`, `build-tests/` (once created).
- The packaged **`RPCS3.app`** (+ any `rpcs3-v*.7z`).

> To drop just our code changes: `git checkout -- rpcs3/CMakeLists.txt` then delete the two new
> `vk_driver_selection.h` / `test_vk_driver_selection.cpp` files and `.ci/build-mac-homebrew.sh`.

---

## 3. Capture the exact delta (run when the agents finish)

```sh
# Homebrew formulae added vs baseline
ls /opt/homebrew/Cellar > /tmp/ledger_cellar_after.txt
cat > /tmp/ledger_cellar_before.txt <<'EOF'
abseil ada-url apktool brotli c-ares ca-certificates cairo fmt fontconfig freetype gettext gh giflib git glib go graphite2 harfbuzz hdrhistogram_c highway icu4c@78 jpeg-turbo json-c libffi libnghttp2 libpng libtiff libunistring libuv libx11 libxau libxcb libxdmcp libxext libxrender little-cms2 llhttp lz4 lzo merve nbytes node openjdk openssl@4 pcre2 pixman playwright-cli readline simdjson simdutf sqlite uvwasi webp xorgproto xz zsh-autosuggestions zsh-syntax-highlighting zstd
EOF
tr ' ' '\n' < /tmp/ledger_cellar_before.txt | sort -u > /tmp/b.txt
sort -u /tmp/ledger_cellar_after.txt > /tmp/a.txt
comm -13 /tmp/b.txt /tmp/a.txt        # <-- formulae WE installed (the wipe list)

# Casks added
ls /opt/homebrew/Caskroom 2>/dev/null     # compare against §1

# Submodules now initialized
git -C /Users/abhakash/PersonalWS/rpcs3 submodule status | grep -v '^-'   # <- initialized by us

# Sizes
du -sh /opt/homebrew /Users/abhakash/PersonalWS/rpcs3/{build,build-tests} 2>/dev/null
du -sh ~/Library/Caches/Homebrew /tmp/Qt /tmp/shadPS4 2>/dev/null

# Disk
df -h /
```

Append the results to §4.

**Reconciled delta (fill in when agents finish):**
- Formulae installed by us: _(pending)_
- Casks installed by us: _(pending)_
- Submodules initialized: _(pending)_
- Sizes / disk delta: _(pending)_

---

## 4. Wipe runbook (ordered, safe)

```sh
# 0) Sanity: only remove what §3 lists as OURS.

# 1) Remove build trees and generated artifacts
rm -rf /Users/abhakash/PersonalWS/rpcs3/build /Users/abhakash/PersonalWS/rpcs3/build-tests
rm -rf /Users/abhakash/PersonalWS/rpcs3/qt-downloader
rm -rf /Users/abhakash/PersonalWS/rpcs3/build/bin/MoltenVK /Users/abhakash/PersonalWS/rpcs3/build/bin/*.tar

# 2) Deinit submodule checkouts (keeps .gitmodules; frees the downloaded sources)
cd /Users/abhakash/PersonalWS/rpcs3
git submodule deinit -f --all
rm -rf .git/modules/*            # optional: drop cached submodule git data (re-downloadable)
git checkout -- 3rdparty 2>/dev/null || true

# 3) Homebrew: uninstall ONLY the delta from §3, then clean caches
#    (replace <formula...> with the comm -13 output)
brew uninstall --ignore-dependencies <formula...>
brew autoremove                 # removes deps that became orphaned BY OUR uninstalls
brew cleanup -s                 # purge downloads + old versions
rm -rf ~/Library/Caches/Homebrew/downloads
# If a cask was added by us:  brew uninstall --cask <cask>

# 4) Temp + caches
rm -rf /tmp/shadPS4 /tmp/Qt
rm -rf ~/.cache/ccache           # only if this project created/used it

# 5) (Optional) our repo artifacts
#   Docs are research output — delete only if you don't want them:
# rm -rf /Users/abhakash/PersonalWS/rpcs3/docs/research
```

> `brew autoremove` is generally safe (only removes formulae with no dependents), but review its
> plan with `brew autoremove -n` first.

---

## 5. Space budget / caution

- **Free disk at baseline was only ~39 GiB.** A from-scratch toolchain + submodules + Release build
  can approach that. Watch `df -h /`; if it gets critically low the build agent may fail. If so, the
  biggest wins are: use **system LLVM** (not the submodule), avoid `STATIC_LINK_LLVM`, and drop
  unneeded submodules (llvm/opencv/sdl/ffmpeg/curl/zlib).
- Qt (brew) and LLVM (brew) are each ~1–2 GB; build trees are the real variable.

### Time budget (from GitHub Actions logs, as a reference)
`RPCS3 Mac Apple Silicon` (`macos-15`) job: total **~13–35 min**, `Build` step **~9 min warm / ~25–30 min cold**
(ccache + cached Qt + prebuilt MacPorts). `Mac Intel` job is **2–3× slower** (build 22–61 min).
CI is faster than a local cold build because it has warm `ccache`, cached Qt, and prebuilt MacPorts deps.
Expect a local cold build to be **~1–2 h** end-to-end (brew install ~20–40 min + submodules ~5–15 min +
configure/build ~30–60 min + deploy ~2–5 min).

---

## 6. Notes

- Nothing here installed with `sudo` / system-wide paths were observed **yet**; if the build agent runs
  `xcode-select --install` or similar, record it here. Re-check with §3.
- Any change to `/opt/local` (MacPorts) would be unexpected on this box — MacPorts is **not** installed.
