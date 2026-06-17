#!/usr/bin/env python3
"""Compare simplified GeoJSON layer text output with a trusted text list."""

from __future__ import annotations

import argparse
import collections
import json
import re
from pathlib import Path


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


def read_geojson_texts(path: Path) -> tuple[list[str], list[str],
                                           dict[str, list[str]]]:
    source = path.read_text(encoding="utf-8", errors="replace")
    try:
        data = json.loads(source)
    except json.JSONDecodeError:
        return read_tolerant_geojson_texts(source)
    layers: list[str] = []
    texts: list[str] = []
    text_layers: dict[str, list[str]] = collections.defaultdict(list)
    for layer in data:
        name = layer.get("Layer", "")
        layers.append(name)
        for value in layer.get("Text", []):
            text = str(value)
            texts.append(text)
            text_layers[text].append(name)
    return layers, texts, text_layers


def read_tolerant_geojson_texts(source: str) -> tuple[list[str], list[str],
                                                     dict[str, list[str]]]:
    """Read historical/broken GeoJSON-like dumps used as oracle files.

    Some reference files contain a leading quoted text list followed by
    incomplete `{"Layer": "...", "Text": [` fragments. Treat standalone
    quoted values as text and use the latest layer header when available.
    """
    layers: list[str] = []
    texts: list[str] = []
    text_layers: dict[str, list[str]] = collections.defaultdict(list)
    current_layer = ""

    for line in source.splitlines():
        layer_match = re.search(r'"Layer"\s*:\s*"((?:[^"\\]|\\.)*)"', line)
        if layer_match:
            try:
                current_layer = json.loads(f'"{layer_match.group(1)}"')
            except json.JSONDecodeError:
                current_layer = layer_match.group(1)
            layers.append(current_layer)
            continue

        if '"Text"' in line and len(re.findall(r'"((?:[^"\\]|\\.)*)"', line)) <= 1:
            continue

        for raw in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            if raw in {"Layer", "Text"}:
                continue
            try:
                value = json.loads(f'"{raw}"')
            except json.JSONDecodeError:
                value = raw
            if not value:
                continue
            texts.append(value)
            text_layers[value].append(current_layer)

    return layers, texts, text_layers


def read_raw_dump(path: Path | None) -> dict[str, list[str]]:
    if not path:
        return {}
    raw_sources: dict[str, list[str]] = collections.defaultdict(list)
    if not path.exists():
        return {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = line.split("\t", 5)
        if len(parts) != 6:
            continue
        address, obj_handle, layer_handle, obj_type, source, text = parts
        raw_sources[text].append(
            f"{source}@{address}/obj={obj_handle}/layer={layer_handle}/type={obj_type}")
    return raw_sources


def raw_unassigned_text_count(path: Path) -> int:
    data = json.loads(path.read_text(encoding="utf-8"))
    return sum(len(layer.get("Text", [])) for layer in data
               if layer.get("Layer", "") == "__RAW_UNASSIGNED__")


def expand_positive_delta(left: collections.Counter[str],
                          right: collections.Counter[str]) -> list[str]:
    out: list[str] = []
    for key, count in left.items():
        delta = count - right.get(key, 0)
        if delta > 0:
            out.extend([key] * delta)
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("truth", type=Path)
    parser.add_argument("geojson", type=Path)
    parser.add_argument("--show", type=int, default=40)
    parser.add_argument("--probe", action="append", default=[])
    parser.add_argument("--raw-dump", type=Path)
    args = parser.parse_args()

    if args.truth.suffix.lower() in {".json", ".geojson"}:
        _, truth, _ = read_geojson_texts(args.truth)
    else:
        truth = read_truth(args.truth)
    layers, output, text_layers = read_geojson_texts(args.geojson)
    raw_sources = read_raw_dump(args.raw_dump)
    truth_counter = collections.Counter(truth)
    output_counter = collections.Counter(output)
    missing = expand_positive_delta(truth_counter, output_counter)
    extra = expand_positive_delta(output_counter, truth_counter)
    absent_unique = [key for key in truth_counter if output_counter.get(key, 0) == 0]
    deficit_unique = [
        key for key in truth_counter
        if output_counter.get(key, 0) and output_counter[key] < truth_counter[key]
    ]

    print(f"truth_total={len(truth)} truth_unique={len(truth_counter)}")
    print(f"output_total={len(output)} output_unique={len(output_counter)}")
    print(f"layers_total={len(layers)} raw_unassigned_texts="
          f"{raw_unassigned_text_count(args.geojson)}")
    print(f"truth_S_total={sum('S=' in item for item in truth)} "
          f"truth_S_unique={sum('S=' in item for item in truth_counter)}")
    print(f"output_S_total={sum('S=' in item for item in output)} "
          f"output_S_unique={sum('S=' in item for item in output_counter)}")
    print(f"missing_total={len(missing)} missing_unique={len(set(missing))} "
          f"missing_S_total={sum('S=' in item for item in missing)} "
          f"missing_S_unique={sum('S=' in item for item in set(missing))}")
    print(f"absent_unique={len(absent_unique)} "
          f"absent_S_unique={sum('S=' in item for item in absent_unique)} "
          f"deficit_unique={len(deficit_unique)} "
          f"deficit_S_unique={sum('S=' in item for item in deficit_unique)}")
    print(f"extra_total={len(extra)} extra_unique={len(set(extra))} "
          f"extra_S_total={sum('S=' in item for item in extra)} "
          f"extra_S_unique={sum('S=' in item for item in set(extra))}")

    if args.probe:
        print("probes:")
        for probe in args.probe:
            print(f"  {probe}: truth={truth_counter.get(probe, 0)} "
                  f"output={output_counter.get(probe, 0)}")

    print("missing_S:")
    for item in [value for value in missing if "S=" in value][:args.show]:
        print(f"  {item}")
    print("absent_S:")
    for item in [value for value in absent_unique if "S=" in value][:args.show]:
        print(f"  {item}")
    print("deficit_S:")
    for item in [value for value in deficit_unique if "S=" in value][:args.show]:
        print(f"  {item}: truth={truth_counter[item]} "
              f"output={output_counter[item]}")
    print("missing_other:")
    for item in [value for value in missing if "S=" not in value][:args.show]:
        print(f"  {item}")
    print("extra_S:")
    for item in [value for value in extra if "S=" in value][:args.show]:
        layer_counts = collections.Counter(text_layers.get(item, []))
        layer_text = ", ".join(f"{name}:{count}"
                               for name, count in layer_counts.most_common(5))
        source_text = ""
        if item in raw_sources:
            source_text = " raw=" + "; ".join(raw_sources[item][:3])
        print(f"  {item} layers=[{layer_text}]{source_text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
