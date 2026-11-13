#!/bin/sh -ex

# Build and run the JIT smoke test exactly as a user would get it: ad-hoc
# signed, with no Developer ID and no notarization. The test itself checks
# MAP_JIT, the write-protect toggle, and an anonymous RX promotion.

cd "$(dirname "$0")"

BIN="$(mktemp -d)/jit-smoke"

clang -O2 -Wall -Wextra -o "$BIN" jit-smoke.c
codesign --force --sign - "$BIN"
"$BIN"