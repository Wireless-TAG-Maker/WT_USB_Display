#!/usr/bin/env python3
"""Read selected process CPU counters and correlate existing FPS logs.

No screen capture, process control, or private window data. 100% = one core.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import time


def cpu_seconds(value):
    days = 0
    if "-" in value:
        day, value = value.split("-", 1)
        days = int(day)
    result = 0.0
    for part in value.split(":"):
        result = result * 60 + float(part)
    return result + days * 86400


def snapshot(pids):
    output = subprocess.check_output(
        ["ps", "-p", ",".join(map(str, pids)), "-o", "pid=,time=,rss="], text=True
    )
    return {int(pid): (cpu_seconds(cpu), int(rss))
            for pid, cpu, rss in (line.split() for line in output.splitlines())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", action="append", required=True, help="label=PID")
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--fps-log", action="append", default=[], help="label=existing-log-path")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    assert 1 <= args.seconds <= 300
    labels = {name: int(pid) for name, pid in (arg.split("=", 1) for arg in args.pid)}
    logs = {name: Path(path) for name, path in (arg.split("=", 1) for arg in args.fps_log)}
    offsets = {name: path.stat().st_size for name, path in logs.items()}
    previous = snapshot(labels.values())
    start = previous_time = time.monotonic()
    started_at = datetime.now(timezone.utc).isoformat()
    rows = []
    for _ in range(args.seconds):
        time.sleep(1)
        current = snapshot(labels.values())
        now = time.monotonic()
        row = {"elapsed": now - start, "interval": now - previous_time}
        for name, pid in labels.items():
            if pid in current and pid in previous:
                delta = current[pid][0] - previous[pid][0]
                if delta < 0:
                    raise RuntimeError("Process CPU counter reset; PID may have been reused")
                row[name] = {"cpu_percent": 100 * delta / row["interval"],
                             "resident_mib": current[pid][1] / 1024}
            else:
                row[name] = None
        rows.append(row)
        previous, previous_time = current, now
    means = {}
    for name in labels:
        valid = [r for r in rows if r[name] is not None]
        means[name] = (sum(r[name]["cpu_percent"] * r["interval"] for r in valid) /
                       sum(r["interval"] for r in valid)) if valid else None
    correlated = {}
    for name, path in logs.items():
        with path.open("rb") as log:
            log.seek(offsets[name])
            text = log.read().decode("utf-8", errors="replace")
        stats = []
        for line in text.splitlines():
            if line.startswith("STATS "):
                values = dict(re.findall(r"([a-zA-Z_]+)=([^ ]+)", line))
                stats.append(values)
        correlated[name] = {"path": str(path), "start_byte": offsets[name], "stats": stats}
    result = {"method": "delta cumulative ps TIME / monotonic wall; TIME resolution0.01s; 100%=one core",
              "started_at_utc": started_at, "duration": previous_time - start,
              "pids": labels, "mean_cpu_percent": means, "rows": rows, "fps_logs": correlated}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({"mean_cpu_percent": means, "duration": result["duration"]}))


if __name__ == "__main__":
    main()
