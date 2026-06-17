#!/usr/bin/env python3
"""Compare a trusted text list against dumped R2007 sections."""

from __future__ import annotations

import argparse
import collections
import re
from pathlib import Path


ENCODINGS = ("utf-16le", "utf-16be", "gbk", "utf-8", "ascii")


def read_truth(path: Path) -> list[str]:
    data = path.read_bytes()
    for encoding in ("utf-16", "utf-8-sig", "utf-8", "gbk"):
        try:
            text = data.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        text = data.decode("utf-8", errors="replace")
    return [line.strip() for line in text.replace("\r", "\n").split("\n")
            if line.strip()]


def encoded_needles(text: str) -> list[tuple[str, bytes]]:
    out: list[tuple[str, bytes]] = []
    for encoding in ENCODINGS:
        try:
            needle = text.encode(encoding)
        except UnicodeEncodeError:
            continue
        if needle:
            out.append((encoding, needle))
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("truth", type=Path)
    parser.add_argument("sections_dir", type=Path)
    parser.add_argument("--show", type=int, default=80)
    args = parser.parse_args()

    truth = read_truth(args.truth)
    unique = list(collections.Counter(truth))
    blobs = [(path.name, path.read_bytes())
             for path in sorted(args.sections_dir.glob("section_*.bin"))]

    present: dict[str, list[tuple[str, str, int]]] = {}
    for encoding in ENCODINGS:
        encoded: dict[bytes, list[str]] = collections.defaultdict(list)
        for text in unique:
            try:
                needle = text.encode(encoding)
            except UnicodeEncodeError:
                continue
            if needle:
                encoded[needle].append(text)
        if not encoded:
            continue

        pattern = re.compile(
            b"|".join(re.escape(needle)
                      for needle in sorted(encoded, key=len, reverse=True)))
        for section, data in blobs:
            counts: collections.Counter[bytes] = collections.Counter(
                match.group(0) for match in pattern.finditer(data))
            for needle, count in counts.items():
                for text in encoded[needle]:
                    present.setdefault(text, []).append((section, encoding,
                                                        count))

    missing = [text for text in unique if text not in present]

    s_unique = [text for text in unique if "S=" in text]
    s_missing = [text for text in missing if "S=" in text]
    print(f"truth_total={len(truth)} truth_unique={len(unique)}")
    print(f"section_present_unique={len(present)} "
          f"section_missing_unique={len(missing)}")
    print(f"truth_S_unique={len(s_unique)} "
          f"section_missing_S_unique={len(s_missing)}")
    print("missing_S:")
    for text in s_missing[:args.show]:
        print(f"  {text}")
    print("missing_other:")
    for text in [item for item in missing if "S=" not in item][:args.show]:
        print(f"  {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
