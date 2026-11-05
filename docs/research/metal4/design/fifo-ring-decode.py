#!/usr/bin/env python3
"""Decode the ring window that a FIFO desync capture prints.

TODO(excise): this tool only serves the level 2 desync diagnostic in
`MTLGSRender::on_fifo_desync` / `rsx::thread::recover_fifo`.  It goes away with
the `words_before` / `cache_words` capture fields.

The probe prints eight lines of 32 words, oldest first, with the base address
in the first line.  A method word is ``count << 18 | method << 2`` with bit 30
for non-increment; bits 0..1 and 16..17 must be clear.  The decoder walks the
stream from every 4-byte boundary and reports the starts that land exactly on
the failing word (the last word of the window), because a command whose
argument count ran long leaves its header just before the point where the host
stopped parsing commands.

Usage: fifo-ring-decode.py PROBE_LOG
"""

import re
import sys

NON_METHOD = 0xA0030003

FLOW = (
    ("old jump", 0xE0000003, 0x20000000, 0x1FFFFFFC),
    ("new jump", 0xE0000003, 0x00000001, 0xFFFFFFFC),
    ("call", 0x00000003, 0x00000002, 0x1FFFFFFC),
    ("return", 0xFFFF0003, 0x00020000, 0),
)


def classify(cmd):
    if (cmd & NON_METHOD) == 0:
        return ("method", (cmd >> 18) & 0x7FF, cmd)

    for name, mask, value, offset_mask in FLOW:
        if (cmd & mask) == value:
            return (name, cmd & offset_mask)

    return ("invalid", cmd)


def decode(words, start):
    index = start
    path = []

    while index < len(words):
        entry = classify(words[index])
        path.append((index, entry))

        if entry[0] != "method":
            break

        index += 1 + entry[1]

    return index, path


def read_window(path):
    base = None
    words = []

    with open(path, "r", encoding="utf-8", errors="replace") as source:
        for line in source:
            match = re.search(r"FIFO desync ring 0x([0-9a-f]+):(.*)$", line)

            if not match:
                continue

            address = int(match.group(1), 16)
            chunk = [int(word, 16) for word in match.group(2).split()]

            if base is None:
                base = address

            words.extend(chunk)

            if len(words) >= 256:
                break

    return base, words


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return

    base, words = read_window(sys.argv[1])

    if base is None or not words:
        print("no desync ring lines in", sys.argv[1])
        return

    get_index = len(words) - 1
    print(f"base 0x{base:x}, {len(words)} words, failing word at index {get_index} (0x{base + get_index * 4:x})")

    hits = []

    for start in range(len(words)):
        end, path = decode(words, start)

        if end == get_index:
            hits.append((start, path))

    print(f"{len(hits)} start offset(s) parse exactly onto the failing word")

    for start, path in hits[-4:]:
        print(f"  start 0x{base + start * 4:x}, {len(path)} command(s)")
        for index, entry in path[-6:]:
            print(f"    0x{base + index * 4:x} {entry}")

    end, path = decode(words, 0)
    print(f"from the window base the stream ends at 0x{base + end * 4:x} after {len(path)} command(s)")
    for index, entry in path[-8:]:
        print(f"    0x{base + index * 4:x} {entry}")


if __name__ == "__main__":
    main()
