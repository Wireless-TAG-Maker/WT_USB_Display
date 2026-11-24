#!/usr/bin/env python3
"""Validate the complete, unmodified LVGL 8.4.0 benchmark CSV report.

Usage: python3 scripts/analyze_benchmark.py outputs/benchmark/monitor.log
Only the last official report is considered. BENCHMARK_FINISHED is metadata,
never evidence that the report was printed. Exit codes: 0 pass, 1 failed
validation, 2 input/output or command-line error.
Any ESP-IDF W/E or LVGL [Warn]/[Error] line requires review and fails the run.

Scene names, weights and integer arithmetic follow the official tagged source:
https://github.com/lvgl/lvgl/blob/v8.4.0/demos/benchmark/lv_demo_benchmark.c
The serial CSV does not contain weights. The embedded canonical table checks
every case, and --benchmark-source can additionally check the actual C source.
No device access or third-party dependencies are used.
"""

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import re
import sys
from datetime import datetime, timezone


VERSION = "8.4.0"
SOURCE_URL = (
    "https://github.com/lvgl/lvgl/blob/v8.4.0/"
    "demos/benchmark/lv_demo_benchmark.c"
)
SCENES = (
    ("Rectangle", 30),
    ("Rectangle rounded", 20),
    ("Circle", 10),
    ("Border", 20),
    ("Border rounded", 30),
    ("Circle border", 10),
    ("Border top", 3),
    ("Border left", 3),
    ("Border top + left", 3),
    ("Border left + right", 3),
    ("Border top + bottom", 3),
    ("Shadow small", 3),
    ("Shadow small offset", 5),
    ("Shadow large", 5),
    ("Shadow large offset", 3),
    ("Image RGB", 20),
    ("Image ARGB", 20),
    ("Image chorma keyed", 5),
    ("Image indexed", 5),
    ("Image alpha only", 5),
    ("Image RGB recolor", 5),
    ("Image ARGB recolor", 20),
    ("Image chorma keyed recolor", 3),
    ("Image indexed recolor", 3),
    ("Image RGB rotate", 3),
    ("Image RGB rotate anti aliased", 3),
    ("Image ARGB rotate", 5),
    ("Image ARGB rotate anti aliased", 5),
    ("Image RGB zoom", 3),
    ("Image RGB zoom anti aliased", 3),
    ("Image ARGB zoom", 5),
    ("Image ARGB zoom anti aliased", 5),
    ("Text small", 20),
    ("Text medium", 30),
    ("Text large", 20),
    ("Text small compressed", 3),
    ("Text medium compressed", 5),
    ("Text large compressed", 10),
    ("Line", 10),
    ("Arc think", 10),
    ("Arc thick", 10),
    ("Substr. rectangle", 10),
    ("Substr. border", 10),
    ("Substr. shadow", 10),
    ("Substr. image", 10),
    ("Substr. line", 10),
    ("Substr. arc", 10),
    ("Substr. text", 10),
)
CASES = tuple(
    (name + (" + opa" if opaque else ""), name,
     "opa" if opaque else "normal", max(weight // 2, 1) if opaque else weight)
    for name, weight in SCENES for opaque in (False, True)
)
CASE_LOOKUP = {case[0]: (index, case) for index, case in enumerate(CASES)}
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
IDF_PREFIX = re.compile(r"^(?:\[[^\]\r\n]+\]\s*)?[IWEVD]\s+\(\s*\d+\)\s+[^:]+:\s*")
HEADER = re.compile(r"LVGL v(\d+\.\d+\.\d+)\b[^\r\n]*Benchmark \(in csv format\)")
FAULT_PATTERNS = (
    ("watchdog", re.compile(r"\b(?:task_wdt|int_wdt|watchdog|wdt)\b", re.I)),
    ("panic", re.compile(r"Guru Meditation|\bpanic(?:ked)?\b|\babort\(\)|assert(?:ion)? failed|\bbacktrace:", re.I)),
    ("underrun", re.compile(r"\b(?:underrun|underflow)\b", re.I)),
    ("warning", re.compile(r"(?:^|\s)W\s+\(\s*\d+\)\s|\[Warn(?:ing)?\]", re.I)),
    ("error", re.compile(r"(?:^|\s)E\s+\(\s*\d+\)\s|\[Error\]|ESP_ERROR_CHECK failed|\bESP_ERR_[A-Z_]+\b|\b(?:fatal|failed|failure)\b|\berrors?\s*[:=]\s*[1-9]\d*", re.I)),
)


def payload(line):
    """Accept direct LV_LOG printf or a single ESP-IDF logger prefix."""
    return IDF_PREFIX.sub("", line.strip(), count=1).strip()


def source_record(path):
    if path is None:
        return {"url": SOURCE_URL, "table": "embedded official v8.4.0", "actual_source_checked": False}, []
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    table = re.search(r"static\s+scene_dsc_t\s+scenes\[\]\s*=\s*\{(.*?)\n\};", text, re.S)
    parsed = () if table is None else tuple(
        (name, int(weight)) for name, weight in re.findall(
            r'\.name\s*=\s*"([^"\n]+)"\s*,\s*\.weight\s*=\s*(\d+)', table.group(1)
        )
    )
    errors = [] if parsed == SCENES else ["Actual benchmark source scene names/order/weights differ from official LVGL 8.4.0"]
    return {"url": SOURCE_URL, "path": str(path.resolve()), "sha256": hashlib.sha256(raw).hexdigest(),
            "actual_source_checked": True, "scene_table_matches": parsed == SCENES}, errors


def analyze(raw, log_path, source=None):
    normalized = ANSI.sub("", raw.decode("utf-8", errors="replace")).replace("\r\n", "\n").replace("\r", "\n")
    lines = normalized.splitlines()
    errors = []
    faults = []
    for line_number, line in enumerate(lines, 1):
        categories = [name for name, pattern in FAULT_PATTERNS if pattern.search(line)]
        if categories:
            faults.append({"line": line_number, "categories": categories, "text": line})
    if faults:
        errors.append(f"Runtime fault/error indicators found on {len(faults)} log lines")

    source_info, source_errors = source_record(source)
    errors.extend(source_errors)
    headers = [(i, HEADER.search(line)) for i, line in enumerate(lines) if HEADER.search(line)]
    result = {
        "schema_version": 1,
        "analyzed_at": datetime.now(timezone.utc).isoformat(),
        "status": "fail",
        "log": {"path": str(log_path.resolve()), "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
        "benchmark": {"expected_version": VERSION, "source": source_info,
                      "report_count": len(headers), "expected_scenes": len(SCENES),
                      "expected_cases": len(CASES), "nominal_scene_time_ms": 1000,
                      "nominal_suite_time_ms": 96000,
                      "weights": {"normal": 457, "opa": 216, "total": 673},
                      "fps_meaning": "Official weighted LVGL refresh-work FPS; not physical panel refresh rate"},
        "complete_report": False,
        "reported": {"weighted_fps": None, "opa_speed_pct": None},
        "recomputed": None,
        "case_count": 0,
        "faults": faults,
        "errors": errors,
    }
    rows = []
    if not headers:
        errors.append("No official final CSV report header found")
        return result, rows
    start, match = headers[-1]
    result["benchmark"].update({"version": match.group(1), "report_start_line": start + 1})
    if match.group(1) != VERSION:
        errors.append(f"Expected LVGL {VERSION}, got {match.group(1)}")

    summaries = {"weighted_fps": [], "opa_speed_pct": []}
    seen = set()
    previous_index = -1
    summary_before_rows = True
    for index in range(start + 1, len(lines)):
        text = payload(lines[index])
        if re.search(r"LVGL \d+\.\d+\.\d+ official benchmark|\bBENCHMARK_START\b|ESP-ROM:|\brst:0x", text):
            errors.append(f"A new run/reset begins after the selected final report at line {index + 1}")
        weighted = re.fullmatch(r"Weighted FPS:\s*(\d+)", text)
        opacity = re.fullmatch(r"Opa\. speed:\s*(\d+)%", text)
        if weighted or opacity:
            key = "weighted_fps" if weighted else "opa_speed_pct"
            summaries[key].append(int((weighted or opacity).group(1)))
            if rows:
                summary_before_rows = False
            continue
        csv_row = re.fullmatch(r"([^,\r\n]+),\s*(\d+)\s*", text)
        if csv_row is None:
            # A damaged expected row must not silently become a valid case.
            if any(text.startswith(case[0] + ",") for case in CASES):
                errors.append(f"Malformed scene CSV row at line {index + 1}: {text}")
            continue
        case_name, fps_text = csv_row.groups()
        entry = CASE_LOOKUP.get(case_name)
        if entry is None:
            errors.append(f"Unexpected scene CSV row at line {index + 1}: {case_name}")
            continue
        case_index, (_, scene, mode, weight) = entry
        if case_name in seen:
            errors.append(f"Duplicate scene CSV row at line {index + 1}: {case_name}")
        if case_index != previous_index + 1:
            errors.append(f"Scene CSV order/gap mismatch at line {index + 1}: {case_name}")
        previous_index = case_index
        seen.add(case_name)
        fps = int(fps_text)
        if index == len(lines) - 1 and not normalized.endswith("\n"):
            errors.append("Final scene CSV row has no terminating newline; capture may be truncated")
        if fps > 0xFFFFFFFF:
            errors.append(f"Scene FPS exceeds uint32_t at line {index + 1}")
        rows.append({"case_index": case_index, "scene": scene, "mode": mode,
                     "weight": weight, "fps": fps, "weighted_fps": fps * weight,
                     "log_line": index + 1})

    result["case_count"] = len(rows)
    for key, values in summaries.items():
        if len(values) != 1:
            errors.append(f"Expected exactly one {key} summary, found {len(values)}")
        else:
            result["reported"][key] = values[0]
    if not summary_before_rows:
        errors.append("Official summaries appeared after scene CSV rows")
    missing = [case[0] for case in CASES if case[0] not in seen]
    if missing:
        errors.append(f"Missing {len(missing)} scene cases: " + "; ".join(missing))
    if len(rows) != len(CASES):
        errors.append(f"Expected {len(CASES)} scene CSV rows, found {len(rows)}")

    if len(rows) == len(CASES) and len(seen) == len(CASES):
        normal_rows = [row for row in rows if row["mode"] == "normal"]
        opa_rows = [row for row in rows if row["mode"] == "opa"]
        normal_weight = sum(row["weight"] for row in normal_rows)
        opa_weight = sum(row["weight"] for row in opa_rows)
        if (normal_weight, opa_weight) != (457, 216):
            errors.append("Normal/opa weight sums differ from official 457/216")
        normal_sum = sum(row["weighted_fps"] for row in normal_rows)
        opa_sum = sum(row["weighted_fps"] for row in opa_rows)
        if max(normal_sum, opa_sum, normal_sum + opa_sum) > 0xFFFFFFFF:
            errors.append("Weighted sums overflow official uint32_t arithmetic")
        normal_average = normal_sum // normal_weight
        opa_average = opa_sum // opa_weight
        weighted_fps = (normal_sum + opa_sum) // (normal_weight + opa_weight)
        opa_speed = opa_average * 100 // normal_average if normal_average else None
        result["recomputed"] = {"weighted_fps": weighted_fps, "opa_speed_pct": opa_speed,
                                "normal_weighted_average_fps": normal_average,
                                "opa_weighted_average_fps": opa_average,
                                "normal_weighted_sum": normal_sum, "opa_weighted_sum": opa_sum}
        if not normal_average:
            errors.append("Normal weighted average is zero: official opacity-speed division is undefined")
        if opa_average * 100 > 0xFFFFFFFF:
            errors.append("Opacity-speed numerator overflows official uint32_t arithmetic")
        for key in ("weighted_fps", "opa_speed_pct"):
            if result["reported"][key] != result["recomputed"][key]:
                errors.append(f"Official {key} does not match recomputation")
        result["complete_report"] = not any(
            message for message in errors if not message.startswith("Runtime fault/error indicators")
        )
    result["status"] = "pass" if result["complete_report"] and not errors else "fail"
    return result, rows


def write_outputs(result, rows, json_path, csv_path):
    if json_path.resolve() == csv_path.resolve():
        raise ValueError("JSON and CSV output paths must differ")
    csv_buffer = io.StringIO(newline="")
    writer = csv.DictWriter(csv_buffer, fieldnames=("case_index", "scene", "mode", "weight", "fps", "weighted_fps", "log_line"))
    writer.writeheader()
    writer.writerows(rows)
    for path, text in ((csv_path, csv_buffer.getvalue()),
                       (json_path, json.dumps(result, ensure_ascii=False, indent=2) + "\n")):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path, help="Captured serial log containing the full official final report")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parents[1] / "outputs/benchmark")
    parser.add_argument("--json", type=Path, help="Override result.json path")
    parser.add_argument("--csv", type=Path, help="Override scene-results.csv path")
    parser.add_argument("--benchmark-source", type=Path, help="Also validate the actual lv_demo_benchmark.c scene table")
    args = parser.parse_args(argv)
    json_path = args.json or args.output_dir / "result.json"
    csv_path = args.csv or args.output_dir / "scene-results.csv"
    try:
        protected = {args.log.resolve()}
        if args.benchmark_source:
            protected.add(args.benchmark_source.resolve())
        if json_path.resolve() in protected or csv_path.resolve() in protected:
            raise ValueError("Output paths must not overwrite the log or benchmark source")
        result, rows = analyze(args.log.read_bytes(), args.log, args.benchmark_source)
        write_outputs(result, rows, json_path, csv_path)
    except (OSError, UnicodeError, ValueError) as error:
        print(f"Analysis error: {error}", file=sys.stderr)
        return 2
    print(f"{result['status'].upper()}: {result['case_count']}/{len(CASES)} cases; "
          f"Weighted FPS={result['reported']['weighted_fps']}; "
          f"Opa. speed={result['reported']['opa_speed_pct']}%")
    for message in result["errors"]:
        print(f"  {message}", file=sys.stderr)
    print(f"JSON: {json_path}\nCSV: {csv_path}")
    return 0 if result["status"] == "pass" else 1


if __name__ == "__main__":
    sys.exit(main())
