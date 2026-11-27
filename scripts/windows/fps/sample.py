"""Sample the native presentation counters and passively read FUSB from WSL."""
import argparse
import csv
import json
import os
from pathlib import Path
import re
import select
import time

UART_FIELDS = ("rx_fps", "decoded_fps", "present_fps", "refresh_hz", "total_rx", "total_decoded", "total_present",
               "crc_errors", "header_errors", "decode_errors", "present_errors", "dropped", "timeouts", "ack_timeouts")


def sample(session, fd, ack, uart, duration):
    # Discard bytes buffered before the measurement, including the first incomplete line.
    while select.select([fd], [], [], 0)[0]:
        if not os.read(fd, 16384):
            raise OSError("FUSB closed")
    pending = b""
    synchronized = False
    last = None
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        try:
            state = json.loads((session / "stream.json").read_text())
            if state["unix_time"] != last:
                ack.writerow([state["unix_time"], time.time(), state["pid"], state["capture_pid"], state["sent"],
                              state["ack_ok"], state["board_received"], state["board_decoded"], state["board_presented"]])
                last = state["unix_time"]
        except (FileNotFoundError, PermissionError):
            pass
        if not select.select([fd], [], [], 0.13)[0]:
            continue
        data = os.read(fd, 16384)
        if not data:
            raise OSError("FUSB closed")
        lines = (pending + data).split(b"\n")
        pending = lines.pop()
        for line in lines:
            if not synchronized:
                synchronized = True
                continue
            text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line.decode(errors="replace"))
            if "USB_SCREEN rx_fps=" not in text:
                continue
            fields = dict(re.findall(r"([a-zA-Z_]+)=([\d.]+)", text))
            stamp = re.search(r"\((\d+)\)", text)
            if stamp and all(key in fields for key in UART_FIELDS):
                uart.writerow([time.time(), stamp[1], *(fields[key] for key in UART_FIELDS)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("session", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("phase", choices=["current", "controlled"])
    parser.add_argument("seconds", type=float)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    fd = os.open(args.port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        with (args.output / (args.phase + "-ack.csv")).open("x", newline="", buffering=1) as af, (
            args.output / (args.phase + "-uart.csv")
        ).open("x", newline="", buffering=1) as uf:
            ack, uart = csv.writer(af), csv.writer(uf)
            ack.writerow(["windows_unix_s", "reader_unix_s", "worker_pid", "capture_pid", "sent", "ack_ok",
                          "board_rx", "board_decoded", "board_presented"])
            uart.writerow(["reader_unix_s", "board_ms", *UART_FIELDS])
            sample(args.session, fd, ack, uart, args.seconds)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
