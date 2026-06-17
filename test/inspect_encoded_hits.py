#!/usr/bin/env python3
"""Inspect byte-level hits for a text encoded with a chosen codepage."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=Path)
    parser.add_argument("text")
    parser.add_argument("--encoding", default="big5")
    parser.add_argument("--show", type=int, default=5)
    parser.add_argument("--context", type=int, default=64)
    args = parser.parse_args()

    data = args.file.read_bytes()
    needle = args.text.encode(args.encoding)
    positions: list[int] = []
    pos = 0
    while True:
        pos = data.find(needle, pos)
        if pos < 0:
            break
        positions.append(pos)
        pos += 1

    print(f"count={len(positions)} first={positions[:args.show]}")
    for pos in positions[:args.show]:
        start = max(0, pos - args.context)
        end = min(len(data), pos + len(needle) + args.context)
        chunk = data[start:end]
        print(f"--- pos={pos}")
        print(chunk.hex(" ", 1))
        print(chunk.decode(args.encoding, errors="replace"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
