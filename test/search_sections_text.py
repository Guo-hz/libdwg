#!/usr/bin/env python3
"""Search dumped R2007 sections for text probes in common encodings."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("sections_dir", type=Path)
    parser.add_argument("probe", nargs="+")
    parser.add_argument("--encoding", action="append",
                        default=["utf-16le", "utf-16be", "gbk", "utf-8", "ascii"])
    args = parser.parse_args()

    files = sorted(args.sections_dir.glob("section_*.bin"))
    blobs = [(path.name, path.read_bytes()) for path in files]
    for probe in args.probe:
        hits: list[tuple[str, str, int]] = []
        for encoding in args.encoding:
            try:
                needle = probe.encode(encoding)
            except UnicodeEncodeError:
                continue
            if not needle:
                continue
            for name, data in blobs:
                count = data.count(needle)
                if count:
                    hits.append((name, encoding, count))
        print(f"{probe}: {hits}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
