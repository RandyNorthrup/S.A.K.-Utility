#!/usr/bin/env python3
"""Mutation-test harness (R5-G18-1).

Applies a catalog of single-site source mutations one at a time, rebuilds the
covering test target, runs it, and classifies each mutant:

  KILLED     the mutant compiled and the test target FAILED -- the suite
             detected the change, which is what we want.
  ERROR      a build, timeout, missing test, or restoration failure invalidated
             the run. Infrastructure failures never count as killed mutants.
  SURVIVED   the test target still PASSED with the code deliberately broken -- the
             suite cannot tell the mutant from the original. Unless the mutant is
             declared behaviourally EQUIVALENT (same output for every input, with a
             written rationale), a survivor is a hole in the coverage.

A surviving mutant that was expected to be killed makes the harness exit non-zero,
so once a catalog is clean it can be wired into CI as a coverage ratchet.

The converse is ALSO a failure, and used to be waved through as "fine". Declaring a
mutant equivalent is the claim that NO input distinguishes it from the original, so a
test that kills it is a PROOF that the written rationale is false -- and the rationale
is the only thing standing between "this mutation is provably harmless" and "nothing
tests this". Accepting a disproven claim silently is fail-open, and it hid a real one:
mbox_header_parser's value-extract-trim mutant was called redundant because the value
is trimmed again at insert, which ignored that whitespace left by the first trim is
carried INTO the fold join, where the outer trim can no longer reach it. It survived
only because no fixture folded a value with trailing whitespace. Both directions of
that mislabel were silent -- it survived while the corpus was thin, and once a fixture
reached it, the harness printed "fine". A MISLABELLED verdict now exits non-zero.

Usage:
    python scripts/run_mutation_test.py scripts/mutation_catalogs/<catalog>.json
    python scripts/run_mutation_test.py <catalog>.json --only-equivalents
    python scripts/run_mutation_test.py --recover

--only-equivalents runs just the mutants this catalog declares EQUIVALENT. A full
pass rebuilds the target once per mutant and takes minutes per catalog, but the
equivalence claims are a small minority of them and are the entries whose rationale
can rot silently as the suite grows -- a claim written when the corpus could not
reach the mutated site stays on the page unchallenged long after a new fixture does
reach it. Auditing only those is cheap enough to run whenever a suite gains coverage.

Catalog schema (JSON):
    {
      "build_dir":   "build",
      "config":      "Release",
      "target":      "test_linux_distro_catalog",
      "ctest_regex": "^test_linux_distro_catalog$",
      "qt_functions": ["testNamedContract"],
      "mutants": [
        {"id": "...", "file": "src/core/foo.cpp",
         "find": "<exact source text, must occur EXACTLY once>",
         "replace": "<the mutated text>",
         "expect": "killed" | "equivalent",
         "why": "<rationale, required for equivalent>"}
      ]
    }

qt_functions is optional. When present it names 1..64 distinct Qt test functions,
not data tags or lifecycle methods. Exactly one direct-executable CTest registration
must match. Its command and supported runtime properties are copied into an isolated
CTest directory, never edited in the real build. Fresh CTest and Qt XML evidence must
agree, and each selected function must execute without skipped/infrastructure outcomes.
This narrows mutation proof only; the full Release build and CTest gate remain required.

The original bytes of every touched file are snapshotted up front. Restoration
requires complete hash-verified originals and proven child-job quiescence. An
uncertain command or damaged manifest leaves source, snapshots and the marker
intact for explicit investigation; it never guesses that restoration is safe.
This edits tracked sources in place while it runs; do not run it concurrently
with a build you care about. Run and recovery share a kernel-owned exclusive lock.

That restore is a finally block, which a HARD kill skips: a terminated run leaves a
deliberately broken production source sitting in the working tree, looking exactly
like an ordinary edit. Observed twice, and caught only because the operator happened
to run 'git status'. So the run is also ARMED on disk: the original bytes are written
to .mutation-snapshot/ and a .mutation-in-progress.json sentinel names the catalog,
the files at risk and the mutant currently applied. A clean exit clears both. If they
survive, the tree is presumed mutated and:

  - this harness REFUSES to start (a second run would snapshot the MUTATED bytes as
    if they were the original, making the damage permanent),
  - run_all_mutation_catalogs.py --validate fails, and that is the pre-commit hook,
    so a leftover mutation blocks the commit instead of riding along inside it,
  - 'python scripts/run_mutation_test.py --recover' restores from the snapshot.

Recovery restores the snapshot rather than telling you to run 'git checkout --',
because the snapshot is the true pre-run state: checkout would also silently discard
any uncommitted work that was in those files before the run started.
Recovery refuses a live owner process instance, every uncertain command start/exit,
legacy markers without verified process/hash bindings, and incomplete or changed
snapshot sets. PID absence alone never proves that orphaned descendants stopped.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
import traceback
import xml.etree.ElementTree as ET
import ctypes
from ctypes import wintypes
from contextlib import contextmanager
import hashlib
import msvcrt
from pathlib import Path, PureWindowsPath
import re
import tempfile
import _winapi
import math

from mutation_catalog_text import adapt_mutation_text

REPO_ROOT = Path(__file__).resolve().parent.parent

# On-disk record that a run is mid-flight. See the module docstring: the in-process
# restore cannot survive a hard kill, so the tree's true state is recorded here.
SENTINEL_PATH = REPO_ROOT / ".mutation-in-progress.json"
SNAPSHOT_DIR = REPO_ROOT / ".mutation-snapshot"
BUILD_TIMEOUT_SECONDS = 1800
TEST_CASE_TIMEOUT_SECONDS = 300
TEST_COMMAND_TIMEOUT_SECONDS = 1800
TERMINATION_TIMEOUT_SECONDS = 30
RUN_LOG_DIR: Path | None = None
COMMAND_SEQUENCE = 0
UNCONFIRMED_CHILD_PID: int | None = None
RUN_MANIFEST: dict | None = None
MANIFEST_VERSION = 2
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_SNAPSHOT_BYTES = 64 * 1024 * 1024
MAX_SNAPSHOT_TOTAL_BYTES = 256 * 1024 * 1024
MAX_MANIFEST_FILES = 4096
MAX_COMMAND_RECORDS = 4096
MAX_JOB_PROCESS_HANDLES = 1024
MAX_WINDOWS_PID = (1 << 32) - 1
MAX_FILETIME = (1 << 64) - 1
MAX_QT_FUNCTIONS = 64
MAX_QT_FUNCTION_NAME = 128
MAX_CTEST_JSON_BYTES = 4 * 1024 * 1024
MAX_TEST_XML_BYTES = 16 * 1024 * 1024
MAX_CTEST_CONTEXT_ITEMS = 256
MAX_CTEST_CONTEXT_TEXT = 32767
TEST_DISCOVERY_TIMEOUT_SECONDS = 30
QT_LIFECYCLE_FUNCTIONS = frozenset(("initTestCase", "cleanupTestCase"))
QT_RESERVED_FUNCTIONS = QT_LIFECYCLE_FUNCTIONS | {"init", "cleanup", "initMain"}
SCOPED_LIST_PROPERTIES = frozenset(("ENVIRONMENT", "ENVIRONMENT_MODIFICATION", "LABELS"))
SCOPED_TEST_PROPERTIES = SCOPED_LIST_PROPERTIES | {
    "WORKING_DIRECTORY", "TIMEOUT", "PROCESSORS", "RUN_SERIAL"}
REPARSE_ATTRIBUTE = 0x400
PROCESS_QUERY_ACCESS = 0x1000 | 0x100000
ERROR_INVALID_PARAMETER = 87
WAIT_SIGNALED = 0
WAIT_TIMEOUT = 258
JOB_LIMIT_KILL_ON_CLOSE = 0x2000
JOB_EXTENDED_LIMITS = 9
JOB_BASIC_ACCOUNTING = 1
JOB_BASIC_PROCESS_ID_LIST = 3
CREATE_SUSPENDED = 0x4
INVALID_HANDLE = ctypes.c_void_p(-1).value
KERNEL = ctypes.WinDLL("kernel32", use_last_error=True)


class _JobLimits(ctypes.Structure):
    _fields_ = [("process_time", ctypes.c_longlong), ("job_time", ctypes.c_longlong),
                ("flags", wintypes.DWORD), ("min_working", ctypes.c_size_t),
                ("max_working", ctypes.c_size_t), ("active_limit", wintypes.DWORD),
                ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
                ("scheduling", wintypes.DWORD)]


class _ExtendedJobLimits(ctypes.Structure):
    _fields_ = [("basic", _JobLimits), ("io", ctypes.c_ulonglong * 6),
                ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]


class _JobAccounting(ctypes.Structure):
    _fields_ = [("times", ctypes.c_longlong * 4), ("page_faults", wintypes.DWORD),
                ("total_processes", wintypes.DWORD), ("active_processes", wintypes.DWORD),
                ("terminated_processes", wintypes.DWORD)]


class _JobProcessIds(ctypes.Structure):
    _fields_ = [("assigned", wintypes.DWORD), ("returned", wintypes.DWORD),
                ("process_ids", ctypes.c_size_t * MAX_JOB_PROCESS_HANDLES)]


for _name, _arguments, _result in [
    ("CreateFileW", [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE], wintypes.HANDLE),
    ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
    ("OpenProcess", [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
    ("IsProcessInJob", [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)],
     wintypes.BOOL),
    ("GetProcessTimes", [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4,
     wintypes.BOOL),
    ("WaitForSingleObject", [wintypes.HANDLE, wintypes.DWORD], wintypes.DWORD),
    ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
    ("SetInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD],
     wintypes.BOOL),
    ("QueryInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                   wintypes.DWORD, ctypes.c_void_p], wintypes.BOOL),
    ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
    ("TerminateJobObject", [wintypes.HANDLE, wintypes.UINT], wintypes.BOOL),
    ("ResumeThread", [wintypes.HANDLE], wintypes.DWORD),
]:
    _function = getattr(KERNEL, _name)
    _function.argtypes = _arguments
    _function.restype = _result


class HarnessError(RuntimeError):
    """An infrastructure failure that cannot establish mutation evidence."""


def _windows_error(operation: str) -> HarnessError:
    return HarnessError(f"{operation} failed (Windows error {ctypes.get_last_error()})")


def _assert_direct_path(path: Path) -> None:
    """Reject links/junctions before trusting a source or recovery pathname."""
    current = path.absolute()
    while True:
        try:
            info = current.lstat()
        except FileNotFoundError:
            info = None
        if info is not None and getattr(info, "st_file_attributes", 0) & REPARSE_ATTRIBUTE:
            raise HarnessError(f"Reparse path is not allowed for mutation recovery: {current}")
        if current.parent == current:
            break
        current = current.parent


@contextmanager
def _admission():
    """One kernel-owned, non-inheritable lock covers run and recovery admission."""
    directory = REPO_ROOT / "build"
    _assert_direct_path(directory)
    directory.mkdir(exist_ok=True)
    lock = directory / ".mutation-run.lock"
    _assert_direct_path(lock)
    handle = KERNEL.CreateFileW(str(lock), 0xC0000000, 0, None, 4, 0x00200080, None)
    if handle == INVALID_HANDLE:
        raise _windows_error("Exclusive mutation run/recovery admission")
    try:
        _assert_direct_path(lock)
        yield
    finally:
        KERNEL.CloseHandle(handle)


def _process_identity(pid: int) -> dict | None:
    """None means the process is absent; access/query errors remain uncertainty."""
    handle = KERNEL.OpenProcess(PROCESS_QUERY_ACCESS, False, pid)
    if not handle:
        if ctypes.get_last_error() == ERROR_INVALID_PARAMETER:
            return None
        raise _windows_error(f"OpenProcess({pid})")
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not KERNEL.GetProcessTimes(handle, *(ctypes.byref(item) for item in times)):
            raise _windows_error(f"GetProcessTimes({pid})")
        wait = KERNEL.WaitForSingleObject(handle, 0)
        if wait not in (WAIT_SIGNALED, WAIT_TIMEOUT):
            raise _windows_error(f"Process wait query({pid})")
        creation = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
        return {"pid": pid, "created": creation, "running": wait == WAIT_TIMEOUT}
    finally:
        KERNEL.CloseHandle(handle)


def _owner_identity() -> dict:
    identity = _process_identity(os.getpid())
    if not identity:
        raise HarnessError("Could not bind the mutation owner process instance")
    return {"pid": identity["pid"], "created": identity["created"]}


class _WindowsCommand:
    """Launch suspended, assign a non-breakaway child job, then release the thread."""
    def __init__(self):
        self.pid = 0
        self.process_handle = None
        self.thread_handle = None
        self.job = None
        self.assigned = False

    def start(self, cmd: list[str], output) -> None:
        executable = shutil.which(cmd[0])
        if not executable:
            raise HarnessError(f"Required command is unavailable: {cmd[0]}")
        self.job = KERNEL.CreateJobObjectW(None, None)
        if not self.job:
            raise _windows_error("CreateJobObject")
        limits = _ExtendedJobLimits()
        limits.basic.flags = JOB_LIMIT_KILL_ON_CLOSE
        if not KERNEL.SetInformationJobObject(self.job, JOB_EXTENDED_LIMITS,
                                              ctypes.byref(limits), ctypes.sizeof(limits)):
            raise _windows_error("SetInformationJobObject")
        handles = []
        try:
            with open(os.devnull, "rb") as input_file:
                owner = _winapi.GetCurrentProcess()
                for stream in (input_file, output):
                    handles.append(_winapi.DuplicateHandle(
                        owner, msvcrt.get_osfhandle(stream.fileno()), owner, 0, True,
                        _winapi.DUPLICATE_SAME_ACCESS))
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESTDHANDLES
                startup.hStdInput, startup.hStdOutput = handles
                startup.hStdError = startup.hStdOutput
                startup.lpAttributeList = {"handle_list": handles}
                self.process_handle, self.thread_handle, self.pid, _ = _winapi.CreateProcess(
                    executable, subprocess.list2cmdline(cmd), None, None, True,
                    subprocess.CREATE_NO_WINDOW | CREATE_SUSPENDED, None, str(REPO_ROOT), startup)
            if not KERNEL.AssignProcessToJobObject(self.job, self.process_handle):
                raise _windows_error("AssignProcessToJobObject")
            self.assigned = True
            _command_started(self.pid)
            if KERNEL.ResumeThread(self.thread_handle) == 0xFFFFFFFF:
                raise _windows_error("ResumeThread")
            _winapi.CloseHandle(self.thread_handle)
            self.thread_handle = None
        finally:
            for handle in handles:
                _winapi.CloseHandle(handle)

    def wait(self, timeout: float) -> int:
        result = KERNEL.WaitForSingleObject(self.process_handle, max(0, int(timeout * 1000)))
        if result == WAIT_TIMEOUT:
            raise subprocess.TimeoutExpired("owned Windows command", timeout)
        if result != WAIT_SIGNALED:
            raise _windows_error("WaitForSingleObject")
        return _winapi.GetExitCodeProcess(self.process_handle)

    def active_processes(self) -> int:
        info = _JobAccounting()
        if not KERNEL.QueryInformationJobObject(self.job, JOB_BASIC_ACCOUNTING,
                                                ctypes.byref(info), ctypes.sizeof(info), None):
            raise _windows_error("QueryInformationJobObject")
        return info.active_processes

    def pin_active_processes(self) -> list[int]:
        """Pin exact live job members before termination; PIDs alone can be recycled."""
        info = _JobProcessIds()
        if not KERNEL.QueryInformationJobObject(self.job, JOB_BASIC_PROCESS_ID_LIST,
                                                ctypes.byref(info), ctypes.sizeof(info), None):
            raise _windows_error("Job process ID enumeration")
        if info.assigned > MAX_JOB_PROCESS_HANDLES or info.returned < info.assigned:
            raise HarnessError("Owned child job process list exceeded its bounded buffer")
        handles = []
        try:
            for index in range(info.returned):
                pid = int(info.process_ids[index])
                if not pid:
                    raise HarnessError("Owned child job returned an invalid process ID")
                handle = KERNEL.OpenProcess(PROCESS_QUERY_ACCESS, False, pid)
                if not handle:
                    if ctypes.get_last_error() == ERROR_INVALID_PARAMETER:
                        continue  # Member exited between enumeration and handle open.
                    raise _windows_error(f"OpenProcess({pid}) for owned job")
                status = KERNEL.WaitForSingleObject(handle, 0)
                if status == WAIT_SIGNALED:
                    KERNEL.CloseHandle(handle)
                    continue
                in_job = wintypes.BOOL()
                if status != WAIT_TIMEOUT or not KERNEL.IsProcessInJob(
                        handle, self.job, ctypes.byref(in_job)) or not in_job.value:
                    KERNEL.CloseHandle(handle)
                    raise HarnessError("Could not bind a live owned job process instance")
                handles.append(handle)
            if not handles and self.active_processes():
                raise HarnessError("Owned child job has active processes but no pinned handles")
            return handles
        except BaseException:
            for handle in handles:
                KERNEL.CloseHandle(handle)
            raise

    def stop_and_confirm(self) -> None:
        deadline = time.monotonic() + TERMINATION_TIMEOUT_SECONDS
        pinned = []
        if self.process_handle and not self.assigned:
            status = KERNEL.WaitForSingleObject(self.process_handle, 0)
            if status == WAIT_TIMEOUT:
                _winapi.TerminateProcess(self.process_handle, 1)
            elif status != WAIT_SIGNALED:
                raise _windows_error("Unassigned child process query")
        try:
            if self.job:
                if self.active_processes():
                    pinned = self.pin_active_processes()
                    if self.active_processes() and not KERNEL.TerminateJobObject(self.job, 1):
                        raise _windows_error("TerminateJobObject")
                for handle in pinned:
                    remaining_ms = max(0, int((deadline - time.monotonic()) * 1000))
                    status = KERNEL.WaitForSingleObject(handle, remaining_ms)
                    if status == WAIT_TIMEOUT:
                        raise HarnessError("Owned job member did not terminate before deadline")
                    if status != WAIT_SIGNALED:
                        raise _windows_error("Owned job member wait")
                while self.active_processes():
                    if time.monotonic() >= deadline:
                        raise HarnessError("Owned child job did not become quiescent")
                    time.sleep(0.01)
            if self.process_handle:
                self.wait(max(0, deadline - time.monotonic()))
        finally:
            for handle in pinned:
                KERNEL.CloseHandle(handle)

    def close(self) -> None:
        if self.process_handle and not self.assigned:
            self.stop_and_confirm()
        for name in ("thread_handle", "process_handle", "job"):
            handle = getattr(self, name)
            if handle:
                _winapi.CloseHandle(handle)
                setattr(self, name, None)


def _require_children_stopped() -> None:
    uncertain = RUN_MANIFEST and any(
        item["state"] != "quiescent" for item in RUN_MANIFEST["commands"])
    if UNCONFIRMED_CHILD_PID is not None or uncertain:
        if RUN_MANIFEST is not None:
            RUN_MANIFEST["unconfirmed_child_pid"] = UNCONFIRMED_CHILD_PID
            _write_manifest(RUN_MANIFEST)
        raise HarnessError("Child tree may still be active; source and snapshot left intact")


def _command_begin() -> None:
    global RUN_MANIFEST
    if RUN_MANIFEST is None:
        return
    if len(RUN_MANIFEST["commands"]) >= MAX_COMMAND_RECORDS:
        raise HarnessError("Mutation command record limit reached")
    # A failed journal write happens before any child exists. Do not let an
    # uncommitted RAM append become a fabricated uncertain launch during cleanup.
    # A durable starting record still precedes launch and remains fail-closed if
    # persistence commits but subsequently raises: recovery reads the disk record.
    candidate = {**RUN_MANIFEST,
                 "commands": [*RUN_MANIFEST["commands"], {"state": "starting"}]}
    _write_manifest(candidate)
    RUN_MANIFEST = candidate


def _command_started(pid: int) -> None:
    if RUN_MANIFEST is None:
        return
    identity = _process_identity(pid)
    if not identity:
        raise HarnessError("New command process identity is unavailable")
    RUN_MANIFEST["commands"][-1].update(
        {"state": "running", "pid": pid, "created": identity["created"]})
    _write_manifest(RUN_MANIFEST)


def _command_state(state: str) -> None:
    if RUN_MANIFEST is not None:
        RUN_MANIFEST["commands"][-1]["state"] = state
        if state == "quiescent":
            RUN_MANIFEST["commands"][-1]["active_processes"] = 0
        _write_manifest(RUN_MANIFEST)


def _source_path(relative: str) -> Path:
    if not isinstance(relative, str) or not relative or "\\" in relative:
        raise HarnessError("Manifest source path must be a canonical relative path")
    windows = PureWindowsPath(relative)
    parts = relative.split("/")
    if windows.drive or windows.root or ":" in relative or any(
            part in ("", ".", "..") or part.endswith((" ", ".")) for part in parts):
        raise HarnessError(f"Unsafe recovery source path: {relative!r}")
    if any(ord(char) < 32 for char in relative) or relative.startswith(".mutation-"):
        raise HarnessError(f"Reserved recovery source path: {relative!r}")
    path = REPO_ROOT / relative
    if not path.absolute().is_relative_to(REPO_ROOT.absolute()):
        raise HarnessError(f"Recovery source escapes repository: {relative!r}")
    _assert_direct_path(path)
    if not path.is_file() or path.stat().st_nlink != 1:
        raise HarnessError(f"Recovery source is not one direct regular file: {relative}")
    return path


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _bounded_bytes(path: Path, limit: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise HarnessError(f"Recovery input exceeds its byte limit: {path}")
    return data


def _atomic_bytes(path: Path, data: bytes) -> None:
    _assert_direct_path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        _assert_direct_path(path)
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def _write_manifest(manifest: dict) -> None:
    encoded = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise HarnessError("Recovery manifest exceeds its byte limit")
    _atomic_bytes(SENTINEL_PATH, encoded)


def _positive_integer(value) -> bool:
    return type(value) is int and value > 0


def _valid_process_binding(record) -> bool:
    return isinstance(record, dict) and _positive_integer(record.get("pid")) and \
        record["pid"] <= MAX_WINDOWS_PID and _positive_integer(record.get("created")) and \
        record["created"] <= MAX_FILETIME


def _validate_process_records(manifest: dict, recovering: bool) -> None:
    owner = manifest.get("owner")
    if not _valid_process_binding(owner):
        raise HarnessError("Recovery owner lacks a bound PID and creation time")
    if recovering:
        current = _process_identity(owner["pid"])
        if current and current["created"] == owner["created"] and current["running"]:
            raise HarnessError("Mutation owner is still running; recovery refused")
    commands = manifest.get("commands")
    if not isinstance(commands, list) or len(commands) > MAX_COMMAND_RECORDS:
        raise HarnessError("Invalid mutation command manifest")
    if manifest.get("unconfirmed_child_pid") is not None:
        raise HarnessError("Unconfirmed child record retained; recovery refused")
    for command in commands:
        if not isinstance(command, dict) or command.get("state") != "quiescent":
            raise HarnessError("Command start or exit is uncertain; recovery refused")
        if not _valid_process_binding(command) or \
                type(command.get("active_processes")) is not int or command["active_processes"] != 0:
            raise HarnessError("Command lacks verified empty-job completion evidence")
        current = _process_identity(command["pid"])
        if current and current["created"] == command["created"] and current["running"]:
            raise HarnessError("A recorded command process is still running; recovery refused")


def _validated_manifest(recovering: bool = False) -> tuple[dict, list[tuple[Path, bytes]]]:
    """Validate the complete set, hashes, paths and authority before the first write."""
    _assert_direct_path(SENTINEL_PATH)
    _assert_direct_path(SNAPSHOT_DIR)
    if not SENTINEL_PATH.is_file() or SENTINEL_PATH.stat().st_nlink != 1 or \
            SENTINEL_PATH.stat().st_size > MAX_MANIFEST_BYTES:
        raise HarnessError("Recovery manifest is missing or exceeds its limit")
    try:
        manifest = json.loads(_bounded_bytes(SENTINEL_PATH, MAX_MANIFEST_BYTES))
    except (ValueError, UnicodeError) as error:
        raise HarnessError("Recovery manifest is not valid UTF-8 JSON; originals retained") from error
    if not isinstance(manifest, dict) or manifest.get("schema_version") != MANIFEST_VERSION:
        raise HarnessError("Legacy/unbound recovery marker: preserve snapshots; no automatic "
                           "migration is safe without verified original and process records")
    _validate_process_records(manifest, recovering)
    entries = manifest.get("files")
    if not isinstance(entries, list) or not entries or len(entries) > MAX_MANIFEST_FILES:
        raise HarnessError("Recovery requires a nonempty complete source manifest")
    if not SNAPSHOT_DIR.is_dir():
        raise HarnessError("Complete recovery snapshot directory is missing")
    expected_names, seen_paths, restored = set(), set(), []
    total_bytes = 0
    for entry in entries:
        if not isinstance(entry, dict):
            raise HarnessError("Invalid recovery file record")
        target = _source_path(entry.get("path"))
        key = str(target).casefold()
        snapshot = _snapshot_path(entry["path"])
        if key in seen_paths or entry.get("snapshot") != snapshot.name:
            raise HarnessError("Duplicate source or invalid snapshot mapping")
        seen_paths.add(key)
        expected_names.add(snapshot.name)
        size, digest = entry.get("size"), entry.get("sha256")
        allowed = entry.get("allowed_current_sha256")
        if type(size) is not int or not 0 <= size <= MAX_SNAPSHOT_BYTES or \
                not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest) or \
                not isinstance(allowed, list) or not 1 <= len(allowed) <= 2 or digest not in allowed or \
                any(not isinstance(item, str) or not re.fullmatch(r"[0-9a-f]{64}", item)
                    for item in allowed):
            raise HarnessError("Invalid snapshot byte/hash contract")
        total_bytes += size
        if total_bytes > MAX_SNAPSHOT_TOTAL_BYTES:
            raise HarnessError("Recovery snapshot byte budget exceeded")
        _assert_direct_path(snapshot)
        if not snapshot.is_file() or snapshot.stat().st_nlink != 1 or snapshot.stat().st_size != size:
            raise HarnessError("A required recovery snapshot is missing or has the wrong size")
        data = _bounded_bytes(snapshot, size)
        if _digest(data) != digest:
            raise HarnessError("Recovery snapshot hash mismatch")
        if target.stat().st_size > MAX_SNAPSHOT_BYTES or \
                _digest(_bounded_bytes(target, MAX_SNAPSHOT_BYTES)) not in allowed:
            raise HarnessError(f"Source changed outside the recorded mutation: {entry['path']}")
        data.decode("utf-8")
        restored.append((target, data))
    if {path.name for path in SNAPSHOT_DIR.iterdir()} != expected_names:
        raise HarnessError("Snapshot directory does not match the complete manifest")
    return manifest, restored


def _record_mutation(relative: str, text: str) -> None:
    _validated_manifest()
    for entry in RUN_MANIFEST["files"]:
        if entry["path"] == relative:
            entry["allowed_current_sha256"] = list(dict.fromkeys([entry["sha256"],
                                                                  _digest(text.encode("utf-8"))]))
            _write_manifest(RUN_MANIFEST)
            return
    raise HarnessError("Mutation source is absent from the complete manifest")


def _next_log_path(label: str) -> Path:
    global RUN_LOG_DIR, COMMAND_SEQUENCE
    if RUN_LOG_DIR is None:
        RUN_LOG_DIR = REPO_ROOT / "build" / "mutation-runs" / f"{time.time_ns()}-{os.getpid()}"
        RUN_LOG_DIR.mkdir(parents=True)
        print(f"Mutation logs: {RUN_LOG_DIR}", flush=True)
    COMMAND_SEQUENCE += 1
    return RUN_LOG_DIR / f"{COMMAND_SEQUENCE:04d}-{label}.log"


def _terminate_child_tree(process: _WindowsCommand) -> None:
    """A job's empty membership proves quiescence even after intermediate parents exit."""
    process.stop_and_confirm()


def _snapshot_path(rel: str) -> Path:
    """Bind a flat snapshot name to one canonical source path without delimiter aliases."""
    return SNAPSHOT_DIR / (_digest(rel.encode("utf-8")) + ".snapshot")


def _read(path: Path) -> str:
    """Read a source file preserving its exact bytes, including CRLF line endings
    (newline='' disables newline translation), so a mutate-then-restore round-trip
    leaves the working tree byte-identical rather than silently normalising EOLs."""
    return _bounded_bytes(path, MAX_SNAPSHOT_BYTES).decode("utf-8")


def _write(path: Path, text: str) -> None:
    """Write a source file without translating line endings (see _read)."""
    _atomic_bytes(path, text.encode("utf-8"))


def _arm(catalog_name: str, originals: dict[str, str], mutant_id: str) -> None:
    """Record on disk that the tree is (about to be) mutated. Snapshots are written
    BEFORE the sentinel appears, so a sentinel always has a complete snapshot behind
    it -- the reverse order could advertise a recovery that does not exist."""
    global RUN_MANIFEST
    if RUN_MANIFEST is None:
        if not originals or len(originals) > MAX_MANIFEST_FILES:
            raise HarnessError("A mutation run needs a nonempty bounded source manifest")
        entries = []
        total_bytes = 0
        for relative, text in sorted(originals.items()):
            _source_path(relative)
            data = text.encode("utf-8")
            total_bytes += len(data)
            if len(data) > MAX_SNAPSHOT_BYTES or total_bytes > MAX_SNAPSHOT_TOTAL_BYTES:
                raise HarnessError("Mutation snapshot byte budget exceeded")
            entries.append({"path": relative, "snapshot": _snapshot_path(relative).name,
                            "size": len(data), "sha256": _digest(data),
                            "allowed_current_sha256": [_digest(data)]})
        _assert_direct_path(SNAPSHOT_DIR)
        SNAPSHOT_DIR.mkdir(exist_ok=False)
        for relative, text in originals.items():
            _write(_snapshot_path(relative), text)
        RUN_MANIFEST = {"schema_version": MANIFEST_VERSION, "owner": _owner_identity(),
                        "catalog": catalog_name, "files": entries, "commands": []}
    RUN_MANIFEST["applied_mutant"] = mutant_id
    _write_manifest(RUN_MANIFEST)


def _disarm() -> None:
    """Clear the on-disk record after the in-process restore has already run. The
    sentinel goes FIRST: if this is interrupted between the two, what is left is a
    retained unbound snapshot directory, never automatically deleted or reused.
    The complete snapshot set and every restored original are verified first."""
    global RUN_MANIFEST
    manifest, restored = _validated_manifest()
    if any(_bounded_bytes(target, MAX_SNAPSHOT_BYTES) != data for target, data in restored):
        raise HarnessError("Refusing to disarm before every original byte is restored")
    SENTINEL_PATH.unlink()
    for entry in manifest["files"]:
        (SNAPSHOT_DIR / entry["snapshot"]).unlink()
    SNAPSHOT_DIR.rmdir()
    RUN_MANIFEST = None


def _refuse_if_armed() -> bool:
    """True (and explains) if a previous run left the tree presumed mutated. Starting
    anyway would snapshot the MUTATED bytes as the original and make the damage
    permanent, so this fails closed rather than assuming the tree is fine."""
    if not SENTINEL_PATH.exists() and not SENTINEL_PATH.is_symlink():
        return False
    print("ERROR: a previous mutation run did not finish. The working tree is "
          "presumed MUTATED -- one or more production sources still carry a "
          "deliberately broken edit.\n")
    if SENTINEL_PATH.is_file():
        print("Recovery marker retained at " + str(SENTINEL_PATH))
    print("Refusing to start: mutating an already-mutated tree would snapshot the "
          "BROKEN bytes as the original and make the damage permanent.\n"
          "Restore the pre-run state with:\n"
          "    python scripts/run_mutation_test.py --recover")
    return True


def _discard_stale_snapshot() -> None:
    """Never guess that unbound originals are garbage after an interrupted owner."""
    if not SENTINEL_PATH.exists() and SNAPSHOT_DIR.exists():
        raise HarnessError("Unbound snapshot directory retained; verify its originals manually "
                           "before removing it. No automatic deletion or migration is safe.")


def _recover() -> int:
    with _admission():
        return _recover_locked()


def _recover_locked() -> int:
    """Restore only a complete verified manifest after owner and child-job quiescence."""
    if not SENTINEL_PATH.exists() and not SENTINEL_PATH.is_symlink():
        _discard_stale_snapshot()
        print("Nothing to recover: no mutation run is marked in progress.")
        return 0
    _, restored = _validated_manifest(recovering=True)
    for target, data in restored:
        if not _restore_file(target, data.decode("utf-8")):
            raise HarnessError(f"Could not restore {target}; complete originals retained")
        if _bounded_bytes(target, MAX_SNAPSHOT_BYTES) != data:
            raise HarnessError(f"Restored source verification failed: {target}")
        print(f"restored {target.relative_to(REPO_ROOT)}")
    _disarm()
    print(f"\nRECOVERED: {len(restored)} file(s) restored to their pre-run state.")
    return 0


def _restore_file(path: Path, text: str, attempts: int = 30) -> bool:
    """Write @p text back to @p path, retrying while the file is LOCKED.

    A hard kill takes the harness down but not the compiler it spawned, and on Windows
    a cl.exe holding a source open for read blocks the write -- so the first recovery
    attempt after a kill raced a surviving build and died with a bare PermissionError,
    leaving the tree mutated. Found by the kill drill, not by reasoning. Retrying is
    correct rather than merely convenient: the lock is held by a process that is on its
    way out, and the snapshot is intact throughout, so waiting loses nothing. After the
    attempts are spent this returns False and the caller fails closed and says why --
    it never gives up quietly, and it never leaves a traceback as the explanation."""
    for attempt in range(attempts):
        try:
            _write(path, text)
            return True
        except PermissionError:
            if attempt == 0:
                print(f"  {path.name} is locked by another process (most likely a "
                      f"compiler left over from the interrupted run); waiting...")
            time.sleep(1)
    return False


def _run(cmd: list[str], timeout_seconds: int, log_path: Path) -> int:
    """Capture diagnostics and bound the child without killing this restore owner."""
    global UNCONFIRMED_CHILD_PID
    print(f"Running {cmd[0]} (limit {timeout_seconds}s; log {log_path.name})", flush=True)
    with log_path.open("wb") as output:
        _command_begin()
        UNCONFIRMED_CHILD_PID = -1
        process = _WindowsCommand()
        try:
            process.start(cmd, output)
            UNCONFIRMED_CHILD_PID = process.pid
            try:
                result = process.wait(timeout=timeout_seconds)
            except (subprocess.TimeoutExpired, KeyboardInterrupt) as error:
                _terminate_child_tree(process)
                _command_state("quiescent")
                UNCONFIRMED_CHILD_PID = None
                raise HarnessError(f"Command interrupted or timed out; see {log_path}") from error
            _command_state("root_exited")
            _terminate_child_tree(process)
            _command_state("quiescent")
            UNCONFIRMED_CHILD_PID = None
        finally:
            process.close()
    if result != 0:
        print(f"Command exited {result}; diagnostics: {log_path}", flush=True)
    return result


def _build(build_dir: str, config: str, target: str) -> bool:
    """Build one target. True on success."""
    return _run(["cmake", "--build", build_dir, "--config", config, "--target", target],
                BUILD_TIMEOUT_SECONDS, _next_log_path("build")) == 0


def _catalog_qt_functions(catalog: dict) -> list[str] | None:
    if "qt_functions" not in catalog:
        return None
    functions = catalog["qt_functions"]
    if not isinstance(functions, list) or not 1 <= len(functions) <= MAX_QT_FUNCTIONS:
        raise HarnessError("qt_functions must be a nonempty bounded list")
    for function in functions:
        if not isinstance(function, str) or len(function) > MAX_QT_FUNCTION_NAME or \
                not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", function) or \
                function in QT_RESERVED_FUNCTIONS or function.endswith("_data"):
            raise HarnessError("qt_functions requires plain test-function names, not data/lifecycle selectors")
    if len(set(functions)) != len(functions):
        raise HarnessError("qt_functions contains duplicate selectors")
    return list(functions)


def _context_text(value) -> bool:
    return isinstance(value, str) and len(value) <= MAX_CTEST_CONTEXT_TEXT and "\0" not in value


def _scoped_properties(records) -> dict:
    if not isinstance(records, list) or len(records) > len(SCOPED_TEST_PROPERTIES):
        raise HarnessError("Invalid or unsupported scoped CTest properties")
    properties = {}
    for record in records:
        if not isinstance(record, dict) or set(record) != {"name", "value"}:
            raise HarnessError("Invalid scoped CTest property record")
        name, value = record["name"], record["value"]
        if not isinstance(name, str) or name not in SCOPED_TEST_PROPERTIES or name in properties:
            raise HarnessError("Unsupported or duplicate scoped CTest property")
        valid = False
        if name in SCOPED_LIST_PROPERTIES:
            valid = isinstance(value, list) and len(value) <= MAX_CTEST_CONTEXT_ITEMS and \
                all(_context_text(item) for item in value)
        elif name == "WORKING_DIRECTORY":
            valid = _context_text(value) and Path(value).is_absolute() and Path(value).is_dir()
        elif name == "TIMEOUT":
            valid = type(value) in (int, float) and math.isfinite(value) and \
                0 < value <= TEST_COMMAND_TIMEOUT_SECONDS
        elif name == "PROCESSORS":
            valid = type(value) is int and 0 < value <= MAX_CTEST_CONTEXT_ITEMS
        elif name == "RUN_SERIAL":
            valid = type(value) is bool
        if not valid:
            raise HarnessError(f"Invalid scoped CTest property value: {name}")
        properties[name] = value
    if "WORKING_DIRECTORY" not in properties:
        raise HarnessError("Scoped CTest registration lacks an explicit working directory")
    return properties


def _ctest_registration(build_dir: str, config: str, regex: str) -> dict:
    log_path = _next_log_path("ctest-registration")
    result = _run(["ctest", "--test-dir", build_dir, "-C", config, "-R", regex,
                   "--show-only=json-v1"], TEST_DISCOVERY_TIMEOUT_SECONDS, log_path)
    if result != 0:
        raise HarnessError(f"CTest registration discovery failed; see {log_path}")
    try:
        metadata = json.loads(_bounded_bytes(log_path, MAX_CTEST_JSON_BYTES).decode("utf-8"))
    except (OSError, ValueError, UnicodeError) as error:
        raise HarnessError("CTest registration discovery produced invalid JSON") from error
    if not isinstance(metadata, dict) or metadata.get("kind") != "ctestInfo" or \
            metadata.get("version") != {"major": 1, "minor": 0} or \
            not isinstance(metadata.get("tests"), list) or len(metadata["tests"]) != 1:
        raise HarnessError("qt_functions requires exactly one authoritative CTest registration")
    test = metadata["tests"][0]
    if not isinstance(test, dict) or not isinstance(test.get("name"), str) or \
            not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,127}", test["name"]) or \
            ("config" in test and test["config"] != config):
        raise HarnessError("Invalid scoped CTest name or configuration")
    command = test.get("command")
    if not isinstance(command, list) or not 1 <= len(command) <= MAX_CTEST_CONTEXT_ITEMS or \
            not all(_context_text(argument) for argument in command):
        raise HarnessError("Invalid scoped CTest command")
    return {"name": test["name"], "command": command,
            "properties": _scoped_properties(test.get("properties"))}


def _cmake_literal(value: str) -> str:
    delimiter = "="
    while f"]{delimiter}]" in value:
        delimiter += "="
    return f"[{delimiter}[{value}]{delimiter}]"


def _cmake_property(value) -> str:
    if isinstance(value, list):
        return ";".join(item.replace(";", r"\;") for item in value)
    if type(value) is bool:
        return "TRUE" if value else "FALSE"
    return str(value)


def _prepare_qt_selection(build_dir: str, config: str, regex: str, target: str,
                          functions: list[str], log_path: Path) -> tuple[str, str, Path, str]:
    registration = _ctest_registration(build_dir, config, regex)
    binary = Path(registration["command"][0])
    if not isinstance(target, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", target) or \
            not binary.is_absolute() or binary.suffix.lower() != ".exe" or binary.stem != target:
        raise HarnessError("Scoped Qt command must directly execute the catalog target")
    _assert_direct_path(binary)
    if not binary.is_file():
        raise HarnessError("Scoped Qt executable is not a regular file")
    qt_report = log_path.with_suffix(".qt.xml")
    if qt_report.exists() or qt_report.is_symlink():
        raise HarnessError("Scoped Qt report must be a fresh unused path")
    command = registration["command"] + functions + ["-o", f"{qt_report},xml"]
    scope = log_path.with_name(log_path.stem + "-ctest")
    _assert_direct_path(scope)
    scope.mkdir(exist_ok=False)
    name = registration["name"]
    contents = "add_test(" + " ".join(_cmake_literal(value) for value in [name] + command) + ")\n"
    for property_name, value in registration["properties"].items():
        contents += (f"set_tests_properties({_cmake_literal(name)} PROPERTIES {property_name} "
                     f"{_cmake_literal(_cmake_property(value))})\n")
    _atomic_bytes(scope / "CTestTestfile.cmake", contents.encode("utf-8"))
    scoped_regex = "^" + re.escape(name) + "$"
    copied = _ctest_registration(str(scope), config, scoped_regex)
    if copied != {**registration, "command": command}:
        raise HarnessError("Scoped CTest command/context changed during registration round trip")
    return str(scope), scoped_regex, qt_report, name


def _test_xml(path: Path, kind: str) -> ET.Element:
    try:
        _assert_direct_path(path)
        if not path.is_file() or path.stat().st_nlink != 1:
            raise HarnessError(f"{kind} produced no valid result report: {path}")
        text = _bounded_bytes(path, MAX_TEST_XML_BYTES).decode("utf-8")
        # CDATA/comments are text, not declarations. Reject executable XML declarations
        # before ElementTree sees them, including case variants and internal entities.
        markup = re.sub(r"<!--.*?-->|<!\[CDATA\[.*?\]\]>", "", text, flags=re.S)
        if "\0" in text:
            raise HarnessError(f"{kind} result report is not valid UTF-8 XML")
        if re.search(r"<!\s*(?:DOCTYPE|ENTITY)\b", markup, re.I):
            raise HarnessError(f"DTD/entity declarations are forbidden in {kind} result report")
        return ET.fromstring(text)
    except (OSError, UnicodeError, ET.ParseError) as error:
        raise HarnessError(f"{kind} produced no valid result report: {path}") from error


def _qt_assertion_failures(path: Path, functions: list[str]) -> int:
    report = _test_xml(path, "Qt")
    if report.tag != "TestCase" or not report.get("name") or report.find("Duration") is None:
        raise HarnessError("Qt result lacks a completed named TestCase")
    entries = report.findall("TestFunction")
    if len(list(report.iter("Incident"))) != sum(len(entry.findall("Incident")) for entry in entries):
        raise HarnessError("Qt report contains an incident outside a named function")
    names = [entry.get("name") for entry in entries]
    if len(names) != len(set(names)) or set(names) != set(functions) | QT_LIFECYCLE_FUNCTIONS:
        raise HarnessError("Qt selected functions were absent, duplicated or not the exact requested set")
    for message in report.iter("Message"):
        if message.get("type") not in {"qdebug", "qinfo", "qwarn", "qcritical", "info", "warn"}:
            raise HarnessError("Qt report contains a fatal or unknown message")
    failures = 0
    for entry in entries:
        incidents = entry.findall("Incident")
        if not incidents or entry.find("Duration") is None:
            raise HarnessError("Qt selected function has no completed execution evidence")
        for incident in incidents:
            outcome = incident.get("type")
            if outcome not in ("pass", "fail"):
                raise HarnessError("Qt selected function skipped or reported an unsupported outcome")
            if outcome == "fail":
                if entry.get("name") in QT_LIFECYCLE_FUNCTIONS:
                    raise HarnessError("Qt lifecycle failure is not selected-function coverage")
                if not incident.get("file") or not re.fullmatch(r"[1-9][0-9]*", incident.get("line", "")):
                    raise HarnessError("Qt failure lacks an assertion source location")
                failures += 1
    return failures


def _test_passes(build_dir: str, config: str, regex: str,
                 qt_functions: list[str] | None = None, target: str | None = None) -> bool:
    """Require executed tests; timeout, skip, and absent evidence are run errors."""
    log_path = _next_log_path("test")
    report_path = log_path.with_suffix(".xml")
    qt_report_dir = REPO_ROOT / build_dir / "test_results"
    prior_reports = {path.name: (path.stat().st_mtime_ns, path.stat().st_size)
                     for path in qt_report_dir.glob("*.txt")}
    execution_dir, execution_regex = build_dir, regex
    qt_report, expected_name = None, None
    if qt_functions is not None:
        functions = _catalog_qt_functions({"qt_functions": qt_functions})
        execution_dir, execution_regex, qt_report, expected_name = _prepare_qt_selection(
            build_dir, config, regex, target, functions, log_path)
    result = _run(
        ["ctest", "--test-dir", execution_dir, "-C", config, "-R", execution_regex,
         "--no-tests=error", "--timeout", str(TEST_CASE_TIMEOUT_SECONDS),
         "--output-on-failure", "--output-junit", str(report_path)],
        TEST_COMMAND_TIMEOUT_SECONDS, log_path,
    )
    cases = list(_test_xml(report_path, "CTest").iter("testcase"))
    if not cases:
        raise HarnessError(f"CTest executed no matching tests; see {log_path}")
    if expected_name is not None and (len(cases) != 1 or cases[0].get("name") != expected_name):
        raise HarnessError("Scoped CTest did not execute exactly the registered test")
    _retain_qt_reports(build_dir, cases, log_path, prior_reports)
    failures = 0
    for case in cases:
        if case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
            raise HarnessError(f"CTest skipped a required test; see {log_path}")
        failure = case.find("failure")
        if failure is not None:
            if failure.get("message") != "Failed":
                raise HarnessError(f"CTest infrastructure failure: {failure.attrib}; see {log_path}")
            failures += 1
    if (result == 0) != (failures == 0):
        raise HarnessError(f"CTest exit/result disagreement; see {log_path}")
    if qt_report is not None and (result == 0) != (_qt_assertion_failures(qt_report, functions) == 0):
        raise HarnessError(f"CTest and selected Qt assertion results disagree; see {log_path}")
    return result == 0


def _retain_qt_reports(build_dir: str, cases: list[ET.Element], log_path: Path,
                       prior_reports: dict[str, tuple[int, int]]) -> None:
    """Keep fresh Qt assertion output before a later mutant overwrites it."""
    for case in cases:
        name = case.get("name", "")
        if not name or Path(name).name != name:
            raise HarnessError("Invalid testcase name in CTest result report")
        source = REPO_ROOT / build_dir / "test_results" / f"{name}.txt"
        if source.is_file():
            current = (source.stat().st_mtime_ns, source.stat().st_size)
            if prior_reports.get(source.name) != current:
                shutil.copyfile(source, log_path.with_name(f"{log_path.stem}-{name}.qttest.txt"))


def main() -> int:
    global RUN_MANIFEST, UNCONFIRMED_CHILD_PID
    with _admission():
        try:
            return _main_locked()
        finally:
            RUN_MANIFEST = None
            UNCONFIRMED_CHILD_PID = None


def _main_locked() -> int:
    args = sys.argv[1:]
    if args == ["--recover"]:
        return _recover_locked()
    only_equivalents = False
    if args and args[-1] == "--only-equivalents":
        only_equivalents = True
        args = args[:-1]
    if len(args) != 1:
        print(__doc__)
        return 2
    if _refuse_if_armed():
        return 2
    _discard_stale_snapshot()
    catalog_path = Path(args[0])
    if not catalog_path.is_absolute():
        catalog_path = REPO_ROOT / catalog_path
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))

    build_dir = catalog["build_dir"]
    config = catalog["config"]
    target = catalog["target"]
    regex = catalog["ctest_regex"]
    qt_functions = _catalog_qt_functions(catalog)
    mutants = catalog["mutants"]
    if only_equivalents:
        # Filter AFTER reading the whole catalog, so the uniqueness/schema validation below
        # still runs over every entry: a stale find-string in a mutant this pass skips is
        # still a rotted catalog, and staying silent about it would trade one fail-open for
        # another. Only the build-and-run loop is narrowed.
        selected = [m for m in mutants if m["expect"] == "equivalent"]
        if not selected:
            print(f"{catalog_path.name}: no equivalence claims to audit.")
            return 0
    else:
        selected = mutants

    # Snapshot every file this catalog will touch, so restore is total and safe.
    touched = sorted({m["file"] for m in mutants})
    originals: dict[str, str] = {}
    for rel in touched:
        p = _source_path(rel)
        if p.stat().st_size > MAX_SNAPSHOT_BYTES:
            raise HarnessError(f"Mutation source exceeds its byte limit: {rel}")
        originals[rel] = _read(p)

    # Validate that each find-string occurs exactly once BEFORE mutating anything --
    # a zero or multi match would silently mutate the wrong site (or nothing).
    for m in mutants:
        text = originals[m["file"]]
        try:
            find, _ = adapt_mutation_text(text, m["find"], m["replace"])
        except ValueError as exc:
            print(f"ERROR: mutant {m['id']}: {exc} in {m['file']}.")
            return 2
        count = text.count(find)
        if count != 1:
            print(f"ERROR: mutant {m['id']}: find-string occurs {count} times in "
                  f"{m['file']} (must be exactly 1). Fix the catalog.")
            return 2
        if m["expect"] not in ("killed", "equivalent"):
            print(f"ERROR: mutant {m['id']}: expect must be 'killed' or 'equivalent'.")
            return 2

    results: list[tuple[str, str, str]] = []  # (id, verdict, expect)
    holes: list[str] = []
    mislabelled: list[str] = []

    def restore_all() -> bool:
        """True iff every touched file is back to its original bytes. Uses the same
        lock-tolerant write as --recover: a compiler from the build this harness just
        ran can still hold a source open for a moment after cmake returns."""
        _, restore_set = _validated_manifest()
        ok = True
        for target, data in restore_set:
            if not _restore_file(target, data.decode("utf-8")):
                print(f"ERROR: could not restore '{target}' -- still locked. The tree is "
                      f"LEFT MUTATED; run --recover once the holding process exits.")
                ok = False
        return ok

    # Arming writes only recovery records, never production source. If it fails, preserve its
    # exact failure and any partial originals instead of masking it with a recovery attempt.
    _arm(catalog_path.name, originals, "baseline")
    try:
        # Baseline: the unmutated tree must build and pass, or results are meaningless.
        print("=== baseline (unmutated) ===")
        if not _build(build_dir, config, target):
            print("ERROR: baseline build failed; cannot run mutation testing.")
            return 2
        if not _test_passes(build_dir, config, regex, qt_functions, target):
            print("ERROR: baseline test failed; cannot run mutation testing.")
            return 2
        print("baseline OK\n")

        for m in selected:
            print(f"Mutant {len(results) + 1}/{len(selected)}: {m['id']}", flush=True)
            rel = m["file"]
            find, replacement = adapt_mutation_text(originals[rel], m["find"], m["replace"])
            mutated = originals[rel].replace(find, replacement, 1)
            # Arm BEFORE the write, and name the mutant, so a kill at any instant from
            # here on leaves a record that says exactly which edit is in the tree.
            _arm(catalog_path.name, originals, m["id"])
            _record_mutation(rel, mutated)
            _write(REPO_ROOT / rel, mutated)
            try:
                if not _build(build_dir, config, target):
                    raise HarnessError(f"Mutant {m['id']} did not compile; fix the catalog")
                elif _test_passes(build_dir, config, regex, qt_functions, target):
                    verdict = "SURVIVED"
                else:
                    verdict = "KILLED"
            finally:
                _require_children_stopped()
                _validated_manifest()
                if not _restore_file(REPO_ROOT / rel, originals[rel]):
                    raise HarnessError(f"Could not restore {rel}; mutation marker retained")

            results.append((m["id"], verdict, m["expect"]))
            note = ""
            if verdict == "SURVIVED" and m["expect"] != "equivalent":
                holes.append(m["id"])
                note = "  <-- HOLE: suite does not detect this mutation"
            elif verdict == "SURVIVED" and m["expect"] == "equivalent":
                note = f"  (equivalent: {m['why']})"
            elif verdict.startswith("KILLED") and m["expect"] == "equivalent":
                mislabelled.append(m["id"])
                note = ("  <-- MISLABELLED: declared equivalent, but a test DISTINGUISHES it, "
                        "which disproves the rationale. Reclassify as expect=killed (with a why "
                        "naming the input that separates them), or, if the kill is a build "
                        "failure, fix the catalog entry -- source that does not compile is not "
                        "an equivalent program")
            print(f"{m['id']:<48} {verdict:<16}{note}", flush=True)
    finally:
        _require_children_stopped()
        # Only clear the on-disk record if the tree is provably clean. If a lock beat
        # the retries, the sentinel and the snapshot MUST stay: they are what keeps the
        # pre-commit hook refusing and what --recover needs to finish the job.
        if not restore_all():
            raise HarnessError("Source restoration failed; mutation marker retained")
        # Rebuild the target back to its pristine object so the tree is left green.
        try:
            if not _build(build_dir, config, target):
                raise HarnessError("Restored-source build failed; mutation run is not certified")
        finally:
            _require_children_stopped()
        _disarm()

    scope = ("equivalence claims only" if only_equivalents
             else "every mutant in the catalog")
    print(f"\n=== summary ({scope}) ===")
    killed = sum(1 for _, v, _ in results if v.startswith("KILLED"))
    survived = sum(1 for _, v, _ in results if v == "SURVIVED")
    print(f"{len(results)} mutants: {killed} killed, {survived} survived "
          f"({len(holes)} unexpected holes, {len(mislabelled)} mislabelled)")
    if holes:
        print("HOLES (expected killed but survived): " + ", ".join(holes))
    if mislabelled:
        print("MISLABELLED (declared equivalent but killed): " + ", ".join(mislabelled))
    if holes or mislabelled:
        return 1
    if only_equivalents:
        print("MUTATION RESULT: PASS (every equivalence claim survived the suite that "
              "would disprove it)")
    else:
        print("MUTATION RESULT: PASS (every non-equivalent mutant was killed, and every "
              "equivalence claim survived the suite that would disprove it)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (HarnessError, OSError, subprocess.SubprocessError) as error:
        print(f"MUTATION RESULT: ERROR: {error}", flush=True)
        # Finally guards may refuse restoration while handling an earlier I/O or
        # launch failure. Preserve the complete primary/cleanup exception chain;
        # the final guard message alone cannot identify the original incident.
        traceback.print_exception(error)
        sys.exit(2)
