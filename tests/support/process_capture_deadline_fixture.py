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

POLL_SECONDS = 0.005
MAX_RECEIPT_DELAY_MS = 10000

# Each wait for the outer test's acknowledgement must outlast the window the outer side
# (scripts/test_process_capture.ps1) gives itself to produce it. Otherwise a slow but valid outer
# side loses the race and the fixture exits 1 for a reason that is not the case under test: on the
# 317211fc nightly, stdout-exact under Windows PowerShell 5.1 failed with "expected '0', got '1'"
# because this wait was 2 s while the outer side was still inside its 10 s. With each wait above
# its outer window, an outer side that is truly too slow fails first on its own message.
#  - The output cases bind identities within $identitySetupLimitSeconds = 10 s and write
#    output-ready.ack, output-release.ack and output-drain.ack inside that setup or a capture
#    whose timeout is at most 10 s.
#  - The deadline cases write bound.ack and parent-release.ack inside their 30 s outer watchdog.
# Each margin is 5 s: well above the outer side's own polling (5-10 ms) and scheduling stalls
# seen on a loaded runner, and small next to the 60 s ctest TIMEOUT these tests run under.
OUTER_OUTPUT_WINDOW_SECONDS = 10
OUTER_DEADLINE_WINDOW_SECONDS = 30
ACK_MARGIN_SECONDS = 5
OUTPUT_ACK_SECONDS = OUTER_OUTPUT_WINDOW_SECONDS + ACK_MARGIN_SECONDS
BIND_ACK_SECONDS = OUTER_DEADLINE_WINDOW_SECONDS + ACK_MARGIN_SECONDS
# How long a fixture lives when nobody stops it. Every case kills the processes it holds, so this
# bounds only a fixture the outer side lost track of. It must outlast the longest valid output
# run (the largest receipt delay, two output acknowledgements and the writes) and the deadline
# parent's release wait and holders, which must stay alive until the outer side has checked them
# inside its 30 s watchdog: 10 + 2 * 15 + 5 = 45 s.
SELF_EXPIRY_SECONDS = MAX_RECEIPT_DELAY_MS // 1000 + 2 * OUTPUT_ACK_SECONDS + ACK_MARGIN_SECONDS


def wait_ack(path: Path) -> None:
    deadline = time.monotonic() + OUTPUT_ACK_SECONDS
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
    deadline = time.monotonic() + BIND_ACK_SECONDS
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
