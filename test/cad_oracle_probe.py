#!/usr/bin/env python3
"""Compare CAD text-search/property exports with GeoJSON/MINJSON/raw dumps.

Expected oracle columns are optional, but useful names are:
text, layer, type, handle, style, x, y
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


def sniff_dialect(path: Path) -> csv.Dialect:
    sample = path.read_text(encoding="utf-8-sig", errors="replace")[:4096]
    try:
        return csv.Sniffer().sniff(sample, delimiters="\t,;")
    except csv.Error:
        class Tab(csv.excel_tab):
            pass
        return Tab


def read_oracle(path: Path) -> list[dict[str, str]]:
    dialect = sniff_dialect(path)
    rows: list[dict[str, str]] = []
    with path.open("r", encoding="utf-8-sig", errors="replace", newline="") as fp:
        reader = csv.DictReader(fp, dialect=dialect)
        if not reader.fieldnames:
            return rows
        for row in reader:
            clean = {str(k).strip().lower(): str(v).strip()
                     for k, v in row.items() if k is not None and v is not None}
            if clean.get("text"):
                rows.append(clean)
    return rows


def read_geojson(path: Path) -> tuple[Counter[str], dict[str, Counter[str]]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    counts: Counter[str] = Counter()
    layers: dict[str, Counter[str]] = defaultdict(Counter)
    for layer in data:
        layer_name = str(layer.get("Layer", ""))
        for value in layer.get("Text", []):
            text = str(value)
            counts[text] += 1
            layers[text][layer_name] += 1
    return counts, layers


def normalize_handle(value: str | None) -> str:
    if not value:
        return ""
    value = value.strip()
    if value.lower().startswith("0x"):
        value = value[2:]
    return value.upper().lstrip("0") or "0"


def read_raw_dump(path: Path | None) -> tuple[dict[str, list[str]],
                                             dict[str, list[str]]]:
    by_text: dict[str, list[str]] = defaultdict(list)
    by_handle: dict[str, list[str]] = defaultdict(list)
    if not path or not path.exists():
        return by_text, by_handle
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
        parts = line.split("\t", 5)
        if len(parts) != 6:
            continue
        address, obj_handle, layer_handle, obj_type, source, text = parts
        source_text = (f"{source}@{address}/obj={obj_handle}/layer="
                       f"{layer_handle}/type={obj_type}")
        by_text[text].append(source_text)
        by_handle[normalize_handle(obj_handle)].append(f"{text} {source_text}")
    return by_text, by_handle


def iter_json_objects(node: Any):
    if isinstance(node, dict):
        yield node
        for value in node.values():
            yield from iter_json_objects(value)
    elif isinstance(node, list):
        for value in node:
            yield from iter_json_objects(value)


def read_minjson_index(path: Path | None) -> tuple[dict[str, list[dict[str, Any]]],
                                                  dict[str, list[dict[str, Any]]]]:
    by_handle: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_text: dict[str, list[dict[str, Any]]] = defaultdict(list)
    if not path or not path.exists():
        return by_handle, by_text
    data = json.loads(path.read_text(encoding="utf-8", errors="replace"))
    for obj in iter_json_objects(data):
        handle = normalize_handle(str(obj.get("handle", "")))
        if handle:
            by_handle[handle].append(obj)
        for key in ("text_value", "text", "default_value", "user_text",
                    "label_text", "name"):
            value = obj.get(key)
            if isinstance(value, str) and value:
                by_text[value].append(obj)
    return by_handle, by_text


def short_obj(obj: dict[str, Any]) -> str:
    entity = obj.get("entity") or obj.get("object") or obj.get("dxfname") or "?"
    handle = normalize_handle(str(obj.get("handle", "")))
    layer = obj.get("layer") or obj.get("Layer") or ""
    return f"{entity}/handle={handle}/layer={layer}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("oracle", type=Path,
                        help="CAD text/property export CSV/TSV")
    parser.add_argument("geojson", type=Path)
    parser.add_argument("--minjson", type=Path)
    parser.add_argument("--raw-dump", type=Path)
    parser.add_argument("--show", type=int, default=40)
    args = parser.parse_args()

    oracle = read_oracle(args.oracle)
    geo_counts, geo_layers = read_geojson(args.geojson)
    raw_by_text, raw_by_handle = read_raw_dump(args.raw_dump)
    min_by_handle, min_by_text = read_minjson_index(args.minjson)

    oracle_counts = Counter(row["text"] for row in oracle)
    missing = []
    for text, count in oracle_counts.items():
        deficit = count - geo_counts.get(text, 0)
        if deficit > 0:
            missing.extend([text] * deficit)

    print(f"oracle_total={len(oracle)} oracle_unique={len(oracle_counts)}")
    print(f"geojson_total={sum(geo_counts.values())} "
          f"geojson_unique={len(geo_counts)}")
    print(f"missing_total={len(missing)} missing_unique={len(set(missing))} "
          f"missing_S_total={sum('S=' in x for x in missing)}")

    print("rows:")
    for row in oracle[:args.show]:
        text = row.get("text", "")
        handle = normalize_handle(row.get("handle"))
        layers = ", ".join(f"{k}:{v}" for k, v in geo_layers[text].most_common(4))
        status = "hit" if geo_counts.get(text, 0) else "missing"
        print(f"  {status} text={text!r} geo_count={geo_counts.get(text, 0)} "
              f"geo_layers=[{layers}] handle={handle or '-'} "
              f"layer={row.get('layer', '-')}")
        if handle and raw_by_handle.get(handle):
            print("    raw_handle: " + " | ".join(raw_by_handle[handle][:3]))
        elif raw_by_text.get(text):
            print("    raw_text: " + " | ".join(raw_by_text[text][:3]))
        if handle and min_by_handle.get(handle):
            print("    minjson_handle: "
                  + " | ".join(short_obj(o) for o in min_by_handle[handle][:3]))
        elif min_by_text.get(text):
            print("    minjson_text: "
                  + " | ".join(short_obj(o) for o in min_by_text[text][:3]))

    print("missing_samples:")
    for text in missing[:args.show]:
        raw = "; ".join(raw_by_text.get(text, [])[:2])
        minj = "; ".join(short_obj(o) for o in min_by_text.get(text, [])[:2])
        suffix = ""
        if raw:
            suffix += f" raw=[{raw}]"
        if minj:
            suffix += f" minjson=[{minj}]"
        print(f"  {text}{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
