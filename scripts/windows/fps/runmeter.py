"""Record the native Windows UVC stream around the complete motion window."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("session", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("--mode", type=int, default=13)
    parser.add_argument("--camera-id", default=os.environ.get("WT_UVC_ID", "1bcf:2d4f"))
    args = parser.parse_args()
    output = args.output.resolve()
    state = json.loads((args.session / "session.json").read_text())
    stream = json.loads((args.session / "stream.json").read_text())
    if (state["status"] != "running" or state["display"]["primary"]
            or state["worker_pid"] != stream["pid"] or time.time() - stream["unix_time"] > 5):
        raise RuntimeError("Expected a running extended P4 desktop")
    if any((output / name).exists() for name in ("record.csv", "record.bin", "events.csv", "paint.csv")):
        raise FileExistsError("Capture files already exist in the output directory")
    meter = output / "MotionMeter.exe"
    shutil.copy2(args.build / meter.name, meter)
    cmd = [str(args.build / "Camera.exe"), "record", str(args.mode), "91", str(output / "record")]
    with (output / "camera.txt").open("w") as log, subprocess.Popen(
        cmd, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, "WT_UVC_ID": args.camera_id}
    ) as camera:
        try:
            deadline = time.monotonic() + 10
            index = output / "record.csv"
            while not index.exists() or index.stat().st_size < 1000:
                if time.monotonic() >= deadline or camera.poll() is not None:
                    raise RuntimeError(f"UVC capture did not start. See {output / 'camera.txt'}")
                time.sleep(0.07)
            time.sleep(0.5)
            subprocess.run([str(meter), state["display"]["name"]], cwd=output, timeout=95, check=True)
            if camera.wait(timeout=12):
                raise RuntimeError("UVC recording failed")
        finally:
            if camera.poll() is None:
                camera.terminate()
    print((output / "camera.txt").read_text())


if __name__ == "__main__":
    main()
