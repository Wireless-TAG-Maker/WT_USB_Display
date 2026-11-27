"""Own one Amyuni output and reconnect its capture/USB worker after topology changes."""
import ctypes as C
from ctypes import wintypes
import json
import os
from pathlib import Path
from queue import Empty, SimpleQueue
import subprocess
import sys
import threading
import time
import traceback

from display_paths import VirtualDisplayAddition
from live_sender import publish
from topology import ADAPTER, devs, set_test_mode
from usb_stream import USB
from virtual_monitor import command

SOURCE = Path(__file__).resolve().parent
WATCH = __name__ == "__main__" and sys.argv[1:] in (["--watch"], ["--watch", "--connect"])
ROOT = Path.cwd() if WATCH else SOURCE
STOP = ROOT / "stop.request"
last_status = None
display_choices = SimpleQueue()


def targets(monitors):
    return [monitor for monitor in monitors if monitor["adapter"] == ADAPTER]


def overlaps(a, b):
    return (a["x"] < b["x"] + b["width"] and b["x"] < a["x"] + a["width"]
            and a["y"] < b["y"] + b["height"] and b["y"] < a["y"] + a["height"])


def independent(display, monitors):
    return not display["primary"] and not any(overlaps(display, d) for d in monitors if d != display)


def separate_landscape(display, monitors):
    return not display["orientation"] and independent(display, monitors)


def display_choice(display):
    return f"{display['name']}\t{display['width']}x{display['height']}"


def wait_for_topology(check, error="Timed out waiting for the virtual display"):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if STOP.exists():
            raise InterruptedError("Session stopped")
        monitors = devs()
        if check(monitors):
            return monitors
        time.sleep(0.25)
    raise RuntimeError(error)


def wait_for_extension(addition, usb):
    start = time.monotonic()
    deadline, next_check = start + 12, start + 1
    attempted = manual = False
    reason = "Windows did not expose the new virtual display as an extended desktop"
    while not STOP.exists():
        monitors = devs()
        outputs = targets(monitors)
        if len(outputs) > 1:
            raise RuntimeError("Multiple virtual displays appeared during connection")
        if outputs and independent(outputs[0], monitors):
            return outputs[0]["name"]
        now = time.monotonic()
        if now >= next_check:
            if usb and device_status(usb):
                raise InterruptedError("USB display disconnected")
            if manual and not addition.target_available():
                raise InterruptedError("Virtual display disconnected")
            if addition and not attempted and not STOP.exists():
                try:
                    attempted = addition.try_extend()
                except (OSError, RuntimeError) as ex:
                    attempted = True
                    reason = str(ex)
                    if addition.target is not None:
                        manual = True
                        state("extend_display", reason=reason)
            next_check = now + 1
        if now >= deadline and not manual:
            if addition is None or addition.target is None:
                raise RuntimeError(reason)
            # Keep the plugged target available while the user changes "1|2" to Extend.
            manual = True
            state("extend_display", reason=reason)
        time.sleep(0.25)
    raise InterruptedError("Session stopped")


def state(status, **extra):
    global last_status
    report = status
    if status == "existing_available":
        report += "\t" + display_choice(extra["display"])
    if WATCH and last_status == report and (not extra or status == "existing_available"):
        return
    publish(ROOT / "session.json", dict(pid=os.getpid(), status=status, unix_time=time.time(), **extra))
    if WATCH and last_status != report:
        try:
            print(report, flush=True)
        except OSError:
            # A closed application also closes its status pipe. Finish display cleanup.
            STOP.touch()
    last_status = report


def run_worker(display):
    env = {**os.environ, "WT_P4_MONITOR": json.dumps(display)}
    receipt = ROOT / "stream.json"
    receipt.unlink(missing_ok=True)
    with (ROOT / "worker.log").open("w") as log, subprocess.Popen(
        [sys.executable, "-B", "-u", str(SOURCE / "usb_stream.py"), "--session", str(ROOT)], cwd=ROOT, env=env,
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
            if WATCH and last_status != "streaming" and receipt.is_file():
                state("streaming", worker_pid=child.pid, display=display)
            time.sleep(0.4)
        return child.wait()


def serve(usb=None, choice=None, auto_select=False):
    before = devs()
    existing = targets(before)
    if not before or existing:
        if not WATCH:
            raise RuntimeError("An Amyuni display is already in use or no interactive desktop is available")
        if len(existing) == 1:
            display = existing[0]
            if separate_landscape(display, before) and display["width"] * 10 == display["height"] * 16:
                target = display_choice(display)
                if not auto_select and choice != target:
                    state("existing_available", display=display)
                    return
                # Capture the borrowed output at its current mode. Only its creator removes it.
                publish(ROOT / "before.json", before)
                publish(ROOT / "active.json", before)
                code = run_worker(display)
                if not STOP.exists():
                    state("reconnecting", display=display, last_worker_exit=code)
                return target
            state("incompatible_display")
            return
        state("other_display" if before else "no_desktop")
        return
    publish(ROOT / "before.json", before)
    try:
        addition = VirtualDisplayAddition(before)
    except OSError:
        # GDI can still find an already extended new output if CCD is unavailable.
        addition = None
    if addition and addition.already_present:
        if not WATCH:
            raise RuntimeError("An Amyuni target is already connected")
        state("extend_display")
        return
    state("starting")
    name = None
    command("add")
    try:
        name = wait_for_extension(addition, usb)
        set_test_mode(name)
        active = wait_for_topology(lambda monitors: [d for d in monitors if d["adapter"] != ADAPTER] == before
                                   and any(d["name"] == name and d["width"] * 10 == d["height"] * 16
                                           for d in targets(monitors)),
                                   "Windows did not expose a 16:10 virtual desktop with the original monitor layout")
        publish(ROOT / "active.json", active)
        while not STOP.exists():
            if usb and device_status(usb):
                break
            monitors = devs()
            display = next((d for d in targets(monitors) if d["name"] == name), None)
            if display is None:
                break
            if not separate_landscape(display, monitors):
                state("waiting_for_separate_landscape_monitor", display=display)
            else:
                code = run_worker(display)
                if not STOP.exists():
                    state("reconnecting", display=display, last_worker_exit=code)
            if not STOP.exists():
                time.sleep(2)
    finally:
        remaining = targets(devs())
        if not remaining or (len(remaining) == 1 and (name is None or remaining[0]["name"] == name)):
            # A stop can arrive before GDI gets an independent source for our monitor.
            try:
                removable = addition.can_remove() if addition else (name is None or bool(remaining))
            except OSError:
                removable = name is not None and bool(remaining)
            if removable:
                command("remove")
    if not WATCH:
        state("stopped")
    return display_choice(next(d for d in active if d["name"] == name))


def device_status(usb, handshake=False):
    with usb.devices() as devices:
        if not devices:
            return "unplugged"
        if len(devices) != 1:
            return "multiple_devices"
        if usb.dll.libusb_get_device_speed(devices[0]) < 3:
            return "slow_usb"
        if handshake:
            with usb.interface(devices[0]) as handle:
                usb.hello(handle)
    return None


def connect_if_ready(usb, choice=None, auto_select=False):
    try:
        status = device_status(usb, handshake=True)
    except (OSError, RuntimeError):
        state("usb_error", reason=traceback.format_exc())
        return
    if status:
        state(status)
        return
    if STOP.exists():
        return
    try:
        command("probe")
    except (OSError, RuntimeError):
        state("driver_error", reason=traceback.format_exc())
        return
    try:
        return serve(usb, choice, auto_select)
    except InterruptedError:
        pass
    except (OSError, RuntimeError):
        state("display_error", reason=traceback.format_exc())


def read_app_commands():
    # The app closes stdin on pause/exit. Windows also closes it if the UI crashes.
    try:
        with os.fdopen(os.dup(sys.stdin.fileno()), "rb", buffering=0) as commands:
            for line in commands:
                if line.startswith(b"use\t"):
                    display_choices.put(line[4:].rstrip(b"\r\n").decode("ascii"))
    finally:
        STOP.touch()


def watch(auto_select):
    STOP.unlink(missing_ok=True)
    threading.Thread(target=read_app_commands, daemon=True).start()
    os.environ["WT_LIBUSB_DLL"] = str(SOURCE / "libusb-1.0.dll")
    usb = USB(os.environ["WT_LIBUSB_DLL"])
    selected = None
    while not STOP.exists():
        try:
            selected = display_choices.get_nowait()
        except Empty:
            pass
        used = connect_if_ready(usb, selected, auto_select)
        if used is not None:
            # Reconnect to this source on replug. A later, different source gets a fresh selection.
            selected, auto_select = used, False
        for _ in range(10):
            if STOP.exists():
                break
            time.sleep(0.2)
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
            if WATCH:
                print("other_session", flush=True)
                sys.exit(2)
            raise RuntimeError("Another P4 desktop session is already running")
        if WATCH:
            watch(auto_select="--connect" in sys.argv[1:])
        else:
            serve()
    except Exception:
        state("error", reason=traceback.format_exc())
        raise
    finally:
        kernel.CloseHandle(mutex)
