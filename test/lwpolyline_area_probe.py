#!/usr/bin/env python3
"""Probe whether missing S= values match closed LWPOLYLINE areas."""

from __future__ import annotations

import argparse
import collections
import json
import math
import re
from pathlib import Path


NUMBER = re.compile(r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")


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


def read_geojson_texts(path: Path) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    out: list[str] = []
    for layer in data:
        out.extend(str(value) for value in layer.get("Text", []))
    return out


def expand_positive_delta(left: collections.Counter[str],
                          right: collections.Counter[str]) -> list[str]:
    out: list[str] = []
    for key, count in left.items():
        delta = count - right.get(key, 0)
        if delta > 0:
            out.extend([key] * delta)
    return out


def matching_bracket(text: str, start: int) -> int:
    depth = 0
    in_string = False
    escape = False
    for i in range(start, len(text)):
        c = text[i]
        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
        elif c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                return i
    return -1


def matching_object(text: str, start: int) -> int:
    depth = 0
    in_string = False
    escape = False
    for i in range(start, len(text)):
        c = text[i]
        if in_string:
            if escape:
                escape = False
            elif c == "\\":
                escape = True
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def extract_array(obj: str, key: str) -> str | None:
    marker = f'"{key}":['
    pos = obj.find(marker)
    if pos < 0:
        return None
    start = pos + len(marker) - 1
    end = matching_bracket(obj, start)
    if end < 0:
        return None
    return obj[start + 1:end]


def parse_points(points_text: str) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for match in re.finditer(r"\[([^\[\]]+)\]", points_text):
        values = [float(item.group(0)) for item in NUMBER.finditer(match.group(1))]
        if len(values) >= 2:
            points.append((values[0], values[1]))
    return points


def parse_numbers(text: str | None) -> list[float]:
    if not text:
        return []
    return [float(item.group(0)) for item in NUMBER.finditer(text)]


def parse_scalar(obj: str, key: str) -> str:
    match = re.search(rf'"{re.escape(key)}":("[^"]*"|-?\d+(?:\.\d+)?)', obj)
    return match.group(1).strip('"') if match else ""


def feature_key(obj: str) -> tuple[str, str, str, str, str, str, str]:
    color_match = re.search(r'"color":\{([^}]*)\}', obj)
    color = ""
    if color_match:
        index_match = re.search(r'"index":(\d+)', color_match.group(1))
        flag_match = re.search(r'"flag":(\d+)', color_match.group(1))
        color = (index_match.group(1) if index_match else "?")
        if flag_match:
            color += f"/f{flag_match.group(1)}"
    layer_match = re.search(r'"layer":\[[^\]]+\]', obj)
    ltype_match = re.search(r'"ltype":\[[^\]]+\]', obj)
    return (
        parse_scalar(obj, "flag"),
        parse_scalar(obj, "const_width"),
        parse_scalar(obj, "thickness"),
        parse_scalar(obj, "ltype_flags"),
        color,
        layer_match.group(0) if layer_match else "",
        ltype_match.group(0) if ltype_match else "",
    )


def polyline_area(points: list[tuple[float, float]],
                  bulges: list[float]) -> float:
    area = 0.0
    count = len(points)
    for i in range(count):
        x1, y1 = points[i]
        x2, y2 = points[(i + 1) % count]
        area += 0.5 * (x1 * y2 - x2 * y1)
        if i < len(bulges) and abs(bulges[i]) > 1e-12:
            chord = math.hypot(x2 - x1, y2 - y1)
            theta = 4.0 * math.atan(bulges[i])
            sin_half = math.sin(theta / 2.0)
            if chord > 0.0 and abs(sin_half) > 1e-12:
                radius = chord / (2.0 * abs(sin_half))
                area += math.copysign(0.5 * radius * radius
                                      * (abs(theta) - math.sin(abs(theta))),
                                      theta)
    return abs(area)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("truth", type=Path)
    parser.add_argument("geojson", type=Path)
    parser.add_argument("minjson", type=Path)
    parser.add_argument("--show", type=int, default=80)
    parser.add_argument("--details", action="store_true")
    parser.add_argument("--stats", action="store_true")
    args = parser.parse_args()

    truth = collections.Counter(read_truth(args.truth))
    output = collections.Counter(read_geojson_texts(args.geojson))
    missing = expand_positive_delta(truth, output)
    missing_s = [item for item in missing if "S=" in item]
    missing_values = collections.defaultdict(list)
    for item in missing_s:
        match = re.search(r"S=(-?\d+(?:\.\d+)?)", item)
        if match:
            missing_values[f"{float(match.group(1)):.2f}"].append(item)

    text = args.minjson.read_text(encoding="utf-8", errors="ignore")
    closed_areas: collections.defaultdict[str, int] = collections.defaultdict(int)
    all_areas: collections.defaultdict[str, int] = collections.defaultdict(int)
    area_objects: collections.defaultdict[str, list[str]] = collections.defaultdict(list)
    feature_stats: collections.Counter[tuple[str, str, str, str, str, str, str]] = collections.Counter()
    matched_feature_stats: collections.Counter[tuple[str, str, str, str, str, str, str]] = collections.Counter()
    output_gap_candidates: collections.Counter[str] = collections.Counter()
    pos = 0
    closed_seen = 0
    all_seen = 0
    while True:
        pos = text.find('{"entity":"LWPOLYLINE"', pos)
        if pos < 0:
            break
        end = matching_object(text, pos)
        if end < 0:
            break
        obj = text[pos:end + 1]
        pos = end + 1
        flag_match = re.search(r'"flag":(\d+)', obj)
        flag = int(flag_match.group(1)) if flag_match else 0
        points_text = extract_array(obj, "points")
        points = parse_points(points_text or "")
        if len(points) < 3:
            continue
        bulges = parse_numbers(extract_array(obj, "bulges"))
        area = polyline_area(points, bulges)
        if area > 0.0:
            key = f"{area:.2f}"
            all_areas[key] += 1
            all_seen += 1
            if flag & 512 or flag & 1:
                closed_areas[key] += 1
                closed_seen += 1
                area_text = f"S={key}"
                if output[area_text] < output_gap_candidates[area_text] + 1:
                    output_gap_candidates[area_text] += 1
                fkey = feature_key(obj)
                feature_stats[fkey] += 1
                if key in missing_values:
                    matched_feature_stats[fkey] += 1
                if args.details and key in missing_values:
                    area_objects[key].append(obj[:1200])

    matched_closed = [(value, missing_values[value], closed_areas[value])
                      for value in missing_values if value in closed_areas]
    matched_all = [(value, missing_values[value], all_areas[value])
                   for value in missing_values if value in all_areas]
    print(f"missing_S_total={len(missing_s)} missing_S_unique={len(missing_values)}")
    print(f"closed_lwpolyline_areas={closed_seen} "
          f"unique_closed_area_values={len(closed_areas)}")
    print(f"all_lwpolyline_areas={all_seen} "
          f"unique_all_area_values={len(all_areas)}")
    print(f"matched_closed_missing_S_unique={len(matched_closed)}")
    for value, items, count in matched_closed[:args.show]:
        print(f"  closed {value}: area_count={count} missing={items[:3]}")
    print(f"matched_all_missing_S_unique={len(matched_all)}")
    for value, items, count in matched_all[:args.show]:
        print(f"  all {value}: area_count={count} missing={items[:3]}")
    candidate_matches = sum(
        min(count, missing_values.get(value[2:], []).__len__())
        for value, count in output_gap_candidates.items()
    )
    candidate_extra = sum(
        max(0, count - len(missing_values.get(value[2:], [])))
        for value, count in output_gap_candidates.items()
    )
    print(f"output_gap_area_candidates_total={sum(output_gap_candidates.values())} "
          f"unique={len(output_gap_candidates)} "
          f"would_match_missing={candidate_matches} "
          f"would_extra={candidate_extra}")
    if args.details:
        print("details:")
        for value, items, count in matched_closed[:args.show]:
            for obj in area_objects.get(value, [])[:3]:
                snippet = obj.replace("\n", " ")
                print(f"  value={value} missing={items[:3]} area_count={count} obj={snippet}")
    if args.stats:
        print("feature_stats:")
        for key, matched_count in matched_feature_stats.most_common(args.show):
            print(f"  matched={matched_count} total={feature_stats[key]} key={key}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
