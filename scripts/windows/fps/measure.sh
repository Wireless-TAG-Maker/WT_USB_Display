#!/usr/bin/env bash
# Measure the existing Windows session from WSL with FUSB attached to WSL.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 4 ]]; then
    echo "Usage: bash measure.sh SESSION_DIR FUSB_PORT [WINDOWS_BUILD_DIR] [UVC_MODE]" >&2
    exit 2
fi
src=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
wsl_directory() {
    if [[ -d $1 ]]; then
        (cd "$1" && pwd -P)
    else
        wslpath -u "$1"
    fi
}
session=$(wsl_directory "$1")
port=$2
py=${WT_FPS_PYTHON:-python3}
build=${3:-$(py.exe -3 -c 'import os; from pathlib import Path; print(Path(os.environ["LOCALAPPDATA"]) / "WT_USB_Display/build")' | tr -d '\r')}
build=$(wsl_directory "$build")
mode=${4:-13}
[[ -r "$session/stream.json" && -r "$port" && -f "$build/MotionMeter.exe" && -f "$build/Camera.exe" && -f "$build/RateChart.exe" ]] || {
    echo "A running P4 session, readable FUSB port and built Windows tools are required." >&2
    exit 1
}
temp=$(py.exe -3 -c 'import tempfile; print(tempfile.gettempdir())' | tr -d '\r')
output=$(mktemp -d "$(wslpath -u "$temp")/wt-p4-fps-XXXXXXXX")
echo "Measurement directory: $output"
"$py" - "$output" <<'PY'
import sys
from pathlib import Path
import cv2 as cv
directory = Path(sys.argv[1])
dictionary = cv.aruco.getPredefinedDictionary(cv.aruco.DICT_4X4_50)
for marker in (21, 33, 9, 46):
    if not cv.imwrite(str(directory / f"mark-{marker}.png"), cv.aruco.generateImageMarker(dictionary, marker, 120)):
        raise OSError("Cannot write calibration markers")
PY
topology=$(wslpath -w "$src/../topology.py")
py.exe -3 "$topology" > "$output/before.json"
"$py" "$src/sample.py" "$session" "$output" current 35 --port "$port"
"$py" "$src/sample.py" "$session" "$output" controlled 96 --port "$port" &
sampler=$!
trap 'if [[ -n ${sampler:-} ]]; then kill "$sampler" 2>/dev/null || true; fi' EXIT
py.exe -3 "$(wslpath -w "$src/runmeter.py")" "$(wslpath -w "$output")" "$(wslpath -w "$session")" \
    "$(wslpath -w "$build")" --mode "$mode" --camera-id "${WT_UVC_ID:-1bcf:2d4f}"
wait "$sampler"
sampler=
py.exe -3 "$topology" > "$output/after.json"
"$py" "$src/analyze_camera.py" "$output"
"$py" "$src/summarize.py" "$output"
py.exe -3 -c 'import subprocess, sys; subprocess.run(sys.argv[1:], check=True)' \
    "$(wslpath -w "$build/RateChart.exe")" "$(wslpath -w "$output/timeline.csv")" "$(wslpath -w "$output/fps-timeline.png")"
if [[ ${WT_KEEP_UVC_RAW:-0} != 1 ]]; then
    rm -- "$output/record.bin"
fi
