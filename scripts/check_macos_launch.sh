#!/bin/bash
# Exercise LaunchServices display creation/cleanup without USB or screen capture.
set -euo pipefail
WT_LAUNCH_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
WT_LAUNCH_APP="${WT_LAUNCH_ROOT}/host/macos/build/WTUSBDisplay.app"
WT_LAUNCH_OUTPUT="${WT_LAUNCH_ROOT}/outputs/usb_display"
mkdir -p "${WT_LAUNCH_OUTPUT}"
WT_LAUNCH_LOGS="$(mktemp -d "${WT_LAUNCH_OUTPUT}/launch-check.XXXXXX")"
for WT_LAUNCH_ITERATION in 1 2 3; do
    WT_LAUNCH_LOG="${WT_LAUNCH_LOGS}/${WT_LAUNCH_ITERATION}.log"
    WT_LAUNCH_ERROR="${WT_LAUNCH_LOGS}/${WT_LAUNCH_ITERATION}.stderr.log"
    /usr/bin/open -n -W --stdout "${WT_LAUNCH_LOG}" --stderr "${WT_LAUNCH_ERROR}" \
        "${WT_LAUNCH_APP}" --args --virtual-only --seconds 2
    python3 - "${WT_LAUNCH_LOG}" "${WT_LAUNCH_ERROR}" <<'PY'
from pathlib import Path
import sys
log, error = (Path(p).read_text() for p in sys.argv[1:])
actual = [line for line in log.splitlines() if line.startswith("VIRTUAL_ACTUAL ")]
required = ("online=1", "logical=1280x800", "pixels=1280x800",
            "refresh_hz=60.000", "mirrored=0", "mirrors_display=0")
assert len(actual) == 1 and all(item in actual[0] for item in required), (log, error)
assert "VIRTUAL_ONLY no_USB=yes no_capture=yes" in log, (log, error)
assert "STOPPED capture=off USB=released virtual_display=removed" in log, (log, error)
assert "ERROR:" not in error and "ERROR:" not in log, (log, error)
print("PASS LaunchServices:", actual[0])
PY
done
printf 'Launch/cleanup checks passed 3/3. Logs: %s\n' "${WT_LAUNCH_LOGS}"
