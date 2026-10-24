"""Bounded Windows capture fixture; no network or device access."""

import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

SELF_EXPIRY_SECONDS = 8
ACK_SECONDS = 2
POLL_SECONDS = 0.005
MAX_RECEIPT_DELAY_MS = 10000


def wait_ack(path: Path) -> None:
    deadline = time.monotonic() + ACK_SECONDS
    while not path.exists():
        if time.monotonic() >= deadline:
            raise TimeoutError("Outer fixture did not bind output process identities")
        time.sleep(POLL_SECONDS)


def output_probe(mode: str, directory: Path) -> None:
    # This watchdog cannot be held up by a blocked native pipe write or stdin.
    expiry = threading.Timer(SELF_EXPIRY_SECONDS, os._exit, args=(93,))
    expiry.daemon = True
    expiry.start()
    receipt_delay_ms = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    if not 0 <= receipt_delay_ms <= MAX_RECEIPT_DELAY_MS:
        raise ValueError("Output receipt delay is outside the fixture bound")
    if receipt_delay_ms:
        time.sleep(receipt_delay_ms / 1000)
    receipt(directory / "parent.json")
    if mode == "output-hold-stdout":
        subprocess.Popen(
            [sys.executable, __file__, "holder-output-failure", str(directory)],
            stdin=subprocess.DEVNULL, stdout=sys.stdout, stderr=sys.stderr,
            close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW,
        )
    wait_ack(directory / "output-ready.ack")
    if mode in ("output-wait", "output-block-stdin"):
        wait_ack(directory / "output-release.ack")
    stdout_count, stderr_count = int(sys.argv[3]), int(sys.argv[4])
    for stream, marker, count in ((sys.stdout, "O", stdout_count),
                                  (sys.stderr, "E", stderr_count)):
        while count > 0:
            chunk = min(4096, count)
            stream.write(marker * chunk)
            stream.flush()
            count -= chunk
    if mode in ("output-wait", "output-block-stdin"):
        time.sleep(SELF_EXPIRY_SECONDS)
    elif mode != "output-exit" and mode != "output-hold-stdout":
        raise ValueError("Unknown output fixture mode")


def receipt(path: Path) -> None:
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    times = [wintypes.FILETIME() for _ in range(4)]
    if not kernel.GetProcessTimes(kernel.GetCurrentProcess(), *(ctypes.byref(t) for t in times)):
        raise ctypes.WinError(ctypes.get_last_error())
    created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
    staging = path.with_suffix(".tmp")
    with staging.open("x", encoding="ascii") as stream:
        json.dump({"pid": os.getpid(), "created": str(created)}, stream)
    staging.rename(path)


def main() -> None:
    mode, directory = sys.argv[1], Path(sys.argv[2])
    if mode.startswith("output-"):
        output_probe(mode, directory)
        return
    if mode.startswith("holder-"):
        receipt(directory / "child.json")
        if mode == "holder-output-failure":
            wait_ack(directory / "output-drain.ack")
            sys.stderr.write("E" * 8193)
            sys.stderr.flush()
        time.sleep(SELF_EXPIRY_SECONDS)
        return
    receipt(directory / "parent.json")
    if mode == "no-read":
        time.sleep(SELF_EXPIRY_SECONDS)
        return
    if mode not in ("hold-stdout", "hold-stderr", "hold-stdin",
                    "parent-exit-before-stop", "parent-exit-before-taskkill"):
        raise ValueError("Unknown capture fixture mode")
    # Completion cases inherit only their selected pipe; race children inherit none.
    subprocess.Popen(
        [sys.executable, __file__, "holder-" + mode, str(directory)],
        stdin=sys.stdin if mode == "hold-stdin" else subprocess.DEVNULL,
        stdout=sys.stdout if mode == "hold-stdout" else subprocess.DEVNULL,
        stderr=sys.stderr if mode == "hold-stderr" else subprocess.DEVNULL,
        close_fds=True,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    deadline = time.monotonic() + ACK_SECONDS
    while not (directory / "bound.ack").exists():
        if time.monotonic() >= deadline:
            raise TimeoutError("Outer fixture did not bind parent and child identities")
        time.sleep(POLL_SECONDS)
    if mode.startswith("parent-exit-"):
        deadline = time.monotonic() + SELF_EXPIRY_SECONDS
        while not (directory / "parent-release.ack").exists():
            if time.monotonic() >= deadline:
                raise TimeoutError("Admitted-timeout parent release did not arrive")
            time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
