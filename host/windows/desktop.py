"""Start or stop the Windows P4 extended-desktop validation session."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from usb_stream import USB, library_path

SOURCE = Path(__file__).resolve().parent
RUNTIME = ("serve.py", "usb_stream.py", "live_sender.py", "topology.py", "virtual_monitor.py")


def start(build, library):
    usb = USB(library)
    with usb.devices() as matches:
        if len(matches) != 1:
            raise RuntimeError(f"Expected one P4 on HUSB, found {len(matches)}")
    files = [build / name for name in ("P4Desktop.exe", "GpuCapture.dll")]
    if not all(path.is_file() for path in files):
        raise FileNotFoundError(f"Run build.cmd first. Capture binaries expected in {build}")
    session = Path(tempfile.mkdtemp(prefix="wt-p4-desktop-"))
    for path in [*files, *(SOURCE / name for name in RUNTIME)]:
        shutil.copy2(path, session / path.name)
    env = {**os.environ, "WT_LIBUSB_DLL": library}
    with (session / "supervisor.log").open("w") as log:
        process = subprocess.Popen(
            [sys.executable, "-u", str(session / "serve.py")], cwd=session, env=env,
            stdin=subprocess.DEVNULL, stdout=log, stderr=log,
            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
        )
    print(f"Session: {session}", flush=True)
    print(f'Graceful stop: py -3 "{SOURCE / "desktop.py"}" stop "{session}"', flush=True)
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError((session / "supervisor.log").read_text(errors="replace").strip())
        if (session / "stream.json").is_file():
            state = json.loads((session / "stream.json").read_text())
            print(f"First P4 presentation ACK received on {state['display']['name']}")
            return
        time.sleep(0.2)
    (session / "stop.request").touch()
    raise TimeoutError(f"No P4 presentation ACK within 20 s. See {session / 'worker.log'}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    begin = commands.add_parser("start")
    begin.add_argument("--build", type=Path, default=Path(os.environ["LOCALAPPDATA"]) / "WT_USB_Display/build")
    begin.add_argument("--libusb", help="absolute path to 64-bit libusb-1.0.dll")
    end = commands.add_parser("stop")
    end.add_argument("session", type=Path, help="session directory printed by start")
    args = parser.parse_args()
    if args.command == "start":
        start(args.build.resolve(), library_path(args.libusb))
    elif (args.session / "session.json").is_file():
        (args.session / "stop.request").touch()
    else:
        parser.error("Expected an existing P4 session directory")


if __name__ == "__main__":
    main()
