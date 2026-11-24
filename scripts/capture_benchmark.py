#!/usr/bin/env python3
"""Passively capture a freshly started LVGL 8 benchmark (no reset ioctls)."""

import argparse
import os
from pathlib import Path
import re
import select
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--post-report", type=float, default=15)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    deadline = time.monotonic() + args.timeout
    report_deadline = None
    captured = bytearray()
    try:
        with args.output.open("wb", buffering=0) as log:
            while time.monotonic() < deadline:
                if report_deadline is not None and time.monotonic() >= report_deadline:
                    break
                ready, _, _ = select.select([fd], [], [], 0.2)
                if not ready:
                    continue
                data = os.read(fd, 8192)
                if not data:
                    continue
                log.write(data)
                captured.extend(data)
                if (report_deadline is None
                        and b"Benchmark (in csv format)" in captured
                        and re.search(rb"(?m)^Substr\. text \+ opa,\d+\r?\r?$", captured)):
                    report_deadline = time.monotonic() + args.post_report
                    print("Final CSV received; collecting post-test health.", flush=True)
    finally:
        os.close(fd)
    if report_deadline is None:
        raise SystemExit("Timed out before the complete final CSV; inspect the saved log.")
    print(f"Capture saved: {args.output}", flush=True)


if __name__ == "__main__":
    main()
