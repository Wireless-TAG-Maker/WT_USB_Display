"""Own one Amyuni output and reconnect its capture/USB worker after topology changes."""
import ctypes as C
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback

from live_sender import publish
from topology import devs, set_test_mode
from virtual_monitor import command

ROOT = Path(__file__).resolve().parent
STOP = ROOT / "stop.request"
ADAPTER = "USB Mobile Monitor Virtual Display"


def targets(monitors):
    return [monitor for monitor in monitors if monitor["adapter"] == ADAPTER]


def overlaps(a, b):
    return (a["x"] < b["x"] + b["width"] and b["x"] < a["x"] + a["width"]
            and a["y"] < b["y"] + b["height"] and b["y"] < a["y"] + a["height"])


def wait_for_topology(check):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        monitors = devs()
        if check(monitors):
            return monitors
        time.sleep(0.25)
    raise RuntimeError("Timed out waiting for the virtual display")


def state(status, **extra):
    publish(ROOT / "session.json", dict(pid=os.getpid(), status=status, unix_time=time.time(), **extra))


def run_worker(display):
    env = {**os.environ, "WT_P4_MONITOR": json.dumps(display)}
    with (ROOT / "worker.log").open("w") as log, subprocess.Popen(
        [sys.executable, "-u", str(ROOT / "usb_stream.py")], cwd=ROOT, env=env,
        stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
        creationflags=subprocess.CREATE_NO_WINDOW,
    ) as child:
        state("running", worker_pid=child.pid, display=display)
        while child.poll() is None:
            if STOP.exists():
                try:
                    child.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    child.terminate()
                break
            time.sleep(0.4)
        return child.wait()


def serve():
    before = devs()
    if not before or targets(before):
        raise RuntimeError("An Amyuni display is already in use or no interactive desktop is available")
    publish(ROOT / "before.json", before)
    state("starting")
    name = None
    command("add")
    try:
        added = wait_for_topology(lambda monitors: len(targets(monitors)) == 1)
        name = targets(added)[0]["name"]
        set_test_mode()
        active = wait_for_topology(lambda monitors: any(
            d["name"] == name and d["width"] * 10 == d["height"] * 16 for d in targets(monitors)))
        if [d for d in active if d["adapter"] != ADAPTER] != before:
            raise RuntimeError("Existing monitors changed while enabling extension")
        publish(ROOT / "active.json", active)
        while not STOP.exists():
            monitors = devs()
            display = next((d for d in targets(monitors) if d["name"] == name), None)
            if display is None:
                break
            if display["primary"] or display["orientation"] or any(
                overlaps(display, d) for d in monitors if d != display
            ):
                state("waiting_for_separate_landscape_monitor", display=display)
            else:
                code = run_worker(display)
                if not STOP.exists():
                    state("reconnecting", display=display, last_worker_exit=code)
            if not STOP.exists():
                time.sleep(2)
    finally:
        remaining = targets(devs())
        if len(remaining) == 1 and (name is None or remaining[0]["name"] == name):
            command("remove")
    state("stopped")


if __name__ == "__main__":
    # The adapter's add/remove calls operate on monitors shared across sessions.
    # Serialise launches in this Windows logon session before checking the topology.
    kernel = C.WinDLL("kernel32", use_last_error=True)
    kernel.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel.CreateMutexW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    mutex = kernel.CreateMutexW(None, False, r"Local\WT-P4-ExtendedDesktop")
    if not mutex:
        raise C.WinError(C.get_last_error())
    try:
        if C.get_last_error() == 183:
            raise RuntimeError("Another P4 desktop session is already running")
        serve()
    except Exception:
        state("error", reason=traceback.format_exc())
        raise
    finally:
        kernel.CloseHandle(mutex)
