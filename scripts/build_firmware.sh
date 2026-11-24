#!/bin/bash
# Build a source snapshot under an ASCII-only path. Never flash a device.
set -eo pipefail
umask 077

WT_BUILD_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
WT_BUILD_SOURCE="${WT_BUILD_REPO_ROOT}/firmware"
WT_BUILD_PROFILE="${WT_SCREEN_PROFILE:-probe}"
case "${WT_BUILD_PROFILE}" in
    probe|display|benchmark|touch|latency|usb_display) ;;
    *) printf 'Unsupported firmware profile: %s\n' "${WT_BUILD_PROFILE}" >&2; exit 2 ;;
esac
WT_BUILD_OUTPUT="${WT_BUILD_REPO_ROOT}/outputs/${WT_BUILD_PROFILE}"
WT_BUILD_ESPVM_DIR="${ESPVM_DIR:-${HOME}/.espvm}"
WT_BUILD_IDF_ROOT="${WT_BUILD_ESPVM_DIR}/esp-idf/versions/v6.0.2"
WT_BUILD_PYTHON_ENV=""

for WT_BUILD_CANDIDATE in "${WT_BUILD_IDF_ROOT}"/tools/python_env/idf6.0_py*_env; do
    if [[ -x "${WT_BUILD_CANDIDATE}/bin/python" ]]; then
        WT_BUILD_PYTHON_ENV="${WT_BUILD_CANDIDATE}"
        break
    fi
done

if [[ ! -f "${WT_BUILD_ESPVM_DIR}/espvm.sh" ||
      ! -f "${WT_BUILD_IDF_ROOT}/esp-idf/export.sh" ||
      -z "${WT_BUILD_PYTHON_ENV}" ]]; then
    printf '%s\n' 'Missing installed espvm / ESP-IDF 6.0.2 / idf6.0 Python environment.' >&2
    exit 2
fi
if [[ ! -f "${WT_BUILD_SOURCE}/CMakeLists.txt" ]]; then
    printf 'Firmware source not found: %s\n' "${WT_BUILD_SOURCE}" >&2
    exit 2
fi

WT_BUILD_PYTHON="${WT_BUILD_PYTHON_ENV}/bin/python"
WT_BUILD_SNAPSHOT="$(mktemp -d /tmp/wt-p4-wlk-build.XXXXXX)"
WT_BUILD_LOG="$(mktemp /tmp/wt-esp-build.XXXXXX)"
trap 'WT_BUILD_EXIT=$?; if [[ ${WT_BUILD_EXIT} -ne 0 ]]; then printf "Build failed (%s). Snapshot: %s\nLog: %s\n" "${WT_BUILD_EXIT}" "${WT_BUILD_SNAPSHOT}" "${WT_BUILD_LOG}" >&2; fi' EXIT

printf 'Source snapshot: %s\nPrivate build log: %s\n' "${WT_BUILD_SNAPSHOT}" "${WT_BUILD_LOG}"

(
    # Activate the already installed SDK without switching global espvm state.
    source "${WT_BUILD_ESPVM_DIR}/espvm.sh" || exit "$?"
    export IDF_PATH="${WT_BUILD_IDF_ROOT}/esp-idf"
    export IDF_TOOLS_PATH="${WT_BUILD_IDF_ROOT}/tools"
    export IDF_PYTHON_ENV_PATH="${WT_BUILD_PYTHON_ENV}"
    export IDF_TARGET="esp32p4"
    export SDKCONFIG_DEFAULTS="sdkconfig.defaults"
    if [[ "${WT_BUILD_PROFILE}" != "probe" ]]; then
        export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.display"
    fi
    if [[ "${WT_BUILD_PROFILE}" == "benchmark" || "${WT_BUILD_PROFILE}" == "touch" || "${WT_BUILD_PROFILE}" == "latency" ]]; then
        export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};sdkconfig.benchmark"
    fi
    if [[ "${WT_BUILD_PROFILE}" == "touch" || "${WT_BUILD_PROFILE}" == "latency" ]]; then
        export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};sdkconfig.touch"
    fi
    if [[ "${WT_BUILD_PROFILE}" == "latency" ]]; then
        export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};sdkconfig.latency"
    fi
    if [[ "${WT_BUILD_PROFILE}" == "usb_display" ]]; then
        export SDKCONFIG_DEFAULTS="${SDKCONFIG_DEFAULTS};sdkconfig.usb_display"
    fi
    export PATH="${WT_BUILD_PYTHON_ENV}/bin:${IDF_PATH}/tools:${PATH}"
    source "${IDF_PATH}/export.sh" || exit "$?"
    set -u

    if [[ "${IDF_PATH}" != "${WT_BUILD_IDF_ROOT}/esp-idf" ||
          "${IDF_PYTHON_ENV_PATH}" != "${WT_BUILD_PYTHON_ENV}" ]]; then
        printf '%s\n' 'SDK activation selected an unexpected IDF or Python environment.' >&2
        exit 2
    fi

    "${WT_BUILD_PYTHON}" - "${WT_BUILD_SOURCE}" "${WT_BUILD_SNAPSHOT}" <<'PY' || exit "$?"
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import shutil
import sys

source, snapshot = map(Path, sys.argv[1:])
excluded = {
    "sdkconfig", "sdkconfig.old", "managed_components",
    ".git", ".DS_Store", "__pycache__", ".cache",
}

def ignore(_directory, names):
    return {name for name in names if name in excluded or name.startswith("build")}

shutil.copytree(source, snapshot, dirs_exist_ok=True, ignore=ignore)
files = []
for path in sorted(snapshot.rglob("*")):
    if path.is_file():
        data = path.read_bytes()
        files.append({
            "path": path.relative_to(snapshot).as_posix(),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
manifest = {
    "created_at": datetime.now(timezone.utc).isoformat(),
    "idf_version": "6.0.2",
    "source_root": str(source),
    "snapshot_root": str(snapshot),
    "files": files,
}
(snapshot / "source_manifest.json").write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print(f"Copied {len(files)} source files into {snapshot}", flush=True)
PY

    cd "${WT_BUILD_SNAPSHOT}" || exit "$?"
    "${WT_BUILD_PYTHON}" "${IDF_PATH}/tools/idf.py" --version || exit "$?"
    "${WT_BUILD_PYTHON}" "${IDF_PATH}/tools/idf.py" build || exit "$?"
) 2>&1 | tee "${WT_BUILD_LOG}"

# The pipeline has completed, so the log is closed before it is copied.
# Validate a successful build before replacing its known output files.
# Leave runtime evidence such as flash.log untouched, including open log files.
"${WT_BUILD_PYTHON}" - "${WT_BUILD_SNAPSHOT}" "${WT_BUILD_OUTPUT}" "${WT_BUILD_LOG}" <<'PY'
from datetime import datetime, timezone
from pathlib import Path
import hashlib
import json
import os
import shutil
import sys
import tempfile

snapshot, output, log = map(Path, sys.argv[1:])
build = snapshot / "build"
flasher = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
relative_files = {Path(name) for name in flasher["flash_files"].values()}
relative_files.update({Path("flasher_args.json"), Path("flash_args"),
                       Path(flasher["app"]["file"]).with_suffix(".elf"),
                       Path(flasher["app"]["file"]).with_suffix(".map")})
bootloader_elf = Path("bootloader/bootloader.elf")
if (build / bootloader_elf).is_file():
    relative_files.add(bootloader_elf)
for name in relative_files:
    if name.is_absolute() or ".." in name.parts or not (build / name).is_file():
        raise RuntimeError(f"Missing or unsafe build artifact: {name}")
for name in ("sdkconfig", "source_manifest.json"):
    if not (snapshot / name).is_file():
        raise RuntimeError(f"Missing build input record: {name}")

output.parent.mkdir(parents=True, exist_ok=True)
stage = Path(tempfile.mkdtemp(prefix=".probe-stage-", dir=output.parent))
for name in sorted(relative_files):
    target = stage / name
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build / name, target)
for name in ("sdkconfig", "source_manifest.json"):
    shutil.copy2(snapshot / name, stage / name)
if (snapshot / "dependencies.lock").is_file():
    shutil.copy2(snapshot / "dependencies.lock", stage / "dependencies.lock")
shutil.copy2(log, stage / "build.log")
records = []
for path in sorted(stage.rglob("*")):
    if path.is_file():
        data = path.read_bytes()
        records.append({
            "path": path.relative_to(stage).as_posix(),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
(stage / "artifact_manifest.json").write_text(
    json.dumps({
        "created_at": datetime.now(timezone.utc).isoformat(),
        "idf_version": "6.0.2", "files": records,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)

output.mkdir(parents=True, exist_ok=True)
publish = sorted(path for path in stage.rglob("*") if path.is_file()
                 and path.name != "artifact_manifest.json")
# Publish the checksummed manifest last so it describes the completed copy.
publish.append(stage / "artifact_manifest.json")
for source_file in publish:
    target = output / source_file.relative_to(stage)
    target.parent.mkdir(parents=True, exist_ok=True)
    os.replace(source_file, target)
for directory in sorted((p for p in stage.rglob("*") if p.is_dir()),
                        key=lambda p: len(p.parts), reverse=True):
    directory.rmdir()
stage.rmdir()
print(f"Build succeeded. Artifacts: {output}")
print("Existing runtime logs and other non-build files were left untouched.")
print(f"Retained source/build snapshot: {snapshot}")
print(f"Private original log: {log}")
PY
