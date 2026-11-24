"""Bounded Windows JPEG sender. Wire layout follows firmware/main/usb_screen_protocol.h."""
import ctypes as C
import json
import os
import struct
import subprocess
import threading
import time
import zlib

FRAME = struct.Struct("<4sHHIIHHHHII")
ACK = struct.Struct("<4sHH8I")
SEQUENCE_MASK = 0xFFFFFFFF


def publish(path, values):
    staging = path.with_suffix(".pending")
    staging.write_text(json.dumps(values, indent=2), encoding="utf-8")
    for attempt in range(10):
        try:
            staging.replace(path)
            return
        except PermissionError:  # A Windows reader may briefly hold the previous file.
            if attempt == 9:
                raise
            time.sleep(0.015)


def run(usb, handle, root, target, max_jpeg):
    stop = root / "stop.request"
    args = [str(root / "P4Desktop.exe"), target["name"]]
    args.extend(str(target[field]) for field in ("x", "y", "width", "height"))
    condition = threading.Condition()
    sent = received = 0
    done = False
    errors = []
    with (root / "capture.log").open("w") as log, subprocess.Popen(
        args, cwd=root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=log, creationflags=subprocess.CREATE_NO_WINDOW,
    ) as capture:
        def receive():
            nonlocal received
            carry = b""
            last_presented = None
            last_write = -1.0
            buffer = C.create_string_buffer(4096)
            try:
                while True:
                    with condition:
                        condition.wait_for(lambda: received < sent or done)
                        if done and received == sent:
                            break
                    length = usb.transfer(handle, 0x81, buffer, len(buffer))
                    if not length:
                        raise OSError("Empty P4 receipt")
                    carry += buffer.raw[:length]
                    while len(carry) >= ACK.size:
                        magic, version, size, seq, status, rx, decoded, presented, *_ = ACK.unpack_from(carry)
                        carry = carry[ACK.size:]
                        if (magic, version, size, seq, status) != (
                            b"WP4A", 1, ACK.size, (received + 1) & SEQUENCE_MASK, 0
                        ):
                            raise OSError(f"Unexpected P4 receipt: sequence {seq}, status {status}")
                        if last_presented is not None and presented != (last_presented + 1) & SEQUENCE_MASK:
                            raise OSError("P4 presentation counter jumped")
                        last_presented = presented
                        with condition:
                            received += 1
                            condition.notify_all()
                        now = time.monotonic()
                        if now - last_write >= 1:
                            publish(root / "stream.json", {
                                "pid": os.getpid(), "capture_pid": capture.pid, "unix_time": time.time(),
                                "sent": sent, "ack_ok": received, "board_received": rx,
                                "board_decoded": decoded, "board_presented": presented, "display": target,
                            })
                            last_write = now
                if carry:
                    raise OSError("Partial P4 receipt")
            except Exception as error:
                with condition:
                    errors.append(error)
                    condition.notify_all()

        reader = threading.Thread(target=receive)
        reader.start()
        try:
            while not stop.exists():
                length = capture.stdout.read(4)
                if not length:
                    break
                if len(length) != 4:
                    raise OSError("Incomplete JPEG length")
                size, = struct.unpack("<I", length)
                if not 4 <= size <= max_jpeg:
                    raise OSError(f"Unexpected JPEG length {size}")
                jpeg = capture.stdout.read(size)
                if len(jpeg) != size or jpeg[:2] != b"\xff\xd8" or jpeg[-2:] != b"\xff\xd9":
                    raise OSError("Incomplete JPEG")
                with condition:
                    if not condition.wait_for(lambda: sent - received < 3 or errors, timeout=3):
                        raise TimeoutError("P4 presentation stalled")
                    if errors:
                        raise errors[0]
                    sent += 1
                    sequence = sent & SEQUENCE_MASK
                    condition.notify_all()
                packet = FRAME.pack(b"WP4D", 1, FRAME.size, sequence, size, 800, 1280, 1, 0, zlib.crc32(jpeg), 0) + jpeg
                if usb.transfer(handle, 0x01, packet, len(packet)) != len(packet):
                    raise OSError("Short USB write")
                if len(packet) % 512 == 0 and usb.transfer(handle, 0x01, None, 0) != 0:
                    raise OSError("Invalid empty USB packet")
            if not stop.exists() and capture.wait() != 0:
                raise RuntimeError("Desktop capture failed: " + (root / "capture.log").read_text().strip())
        finally:
            if capture.poll() is None:
                capture.terminate()
            capture.wait(timeout=3)
            capture.stdout.close()
            with condition:
                done = True
                condition.notify_all()
            reader.join()
        if errors:
            raise errors[0]
