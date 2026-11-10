"""Fail-closed execution and restoration contracts for the mutation runner."""

import ctypes
import importlib.util
import ast
from contextlib import redirect_stderr, redirect_stdout
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import textwrap
import unittest
import xml.etree.ElementTree as ET
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
CATALOGS = importlib.import_module("run_all_mutation_catalogs")

SPEC = importlib.util.spec_from_file_location(
    "mutation_runner", REPO_ROOT / "scripts" / "run_mutation_test.py"
)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)

PROCESS_TERMINATE_ACCESS = 0x0001
PROBE_EXIT_WAIT_MS = 5000


class MutationRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        for name, value in {
            "REPO_ROOT": self.root,
            "SENTINEL_PATH": self.root / ".mutation-in-progress.json",
            "SNAPSHOT_DIR": self.root / ".mutation-snapshot",
            "RUN_LOG_DIR": None,
            "COMMAND_SEQUENCE": 0,
            "UNCONFIRMED_CHILD_PID": None,
            "RUN_MANIFEST": None,
        }.items():
            replacement = patch.object(RUNNER, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)

    def catalog(self):
        self.source = self.root / "probe.cpp"
        self.original = b"int probe() { return 1; }\r\n"
        self.source.write_bytes(self.original)
        path = self.root / "catalog.json"
        path.write_text(json.dumps({
            "build_dir": "build", "config": "Release", "target": "probe",
            "ctest_regex": "^probe$", "mutants": [{
                "id": "wrong-result", "file": "probe.cpp", "find": "return 1",
                "replace": "return 0", "expect": "killed", "why": "wrong result",
            }],
        }), encoding="utf-8")
        return path

    def run_catalog(self, builds, tests):
        path = self.catalog()
        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", side_effect=builds), \
                patch.object(RUNNER, "_test_passes", side_effect=tests):
            return RUNNER.main()

    def assert_restored(self):
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertFalse(RUNNER.SENTINEL_PATH.exists())
        self.assertFalse(RUNNER.SNAPSHOT_DIR.exists())

    def wait_after_probe_ready(self, witness, on_ready=None):
        """Separate bounded process startup from the execution/termination contract tested."""
        original_wait = RUNNER._WindowsCommand.wait
        first_wait = True

        def ready_wait(command, timeout):
            nonlocal first_wait
            if first_wait:
                first_wait = False
                startup_deadline = time.monotonic() + 30
                while not (witness.is_file() and witness.stat().st_size):
                    if time.monotonic() >= startup_deadline:
                        raise subprocess.TimeoutExpired("fixture readiness", 30)
                    time.sleep(0.01)
                if on_ready is not None:
                    on_ready(command)
            return original_wait(command, timeout)

        return ready_wait

    def pin_probe_process(self, witness, job):
        """Keep an exact process object, so PID reuse cannot pass or fail a tree check."""
        pid = int(witness.read_text().strip())
        handle = RUNNER.KERNEL.OpenProcess(
            RUNNER.PROCESS_QUERY_ACCESS | PROCESS_TERMINATE_ACCESS, False, pid)
        self.assertTrue(handle, f"Could not pin owned probe process {pid}")
        self.addCleanup(self.close_probe_process, handle)
        self.assertEqual(RUNNER.KERNEL.WaitForSingleObject(handle, 0), RUNNER.WAIT_TIMEOUT,
                         "Owned probe exited before the cleanup contract was tested")
        is_in_job = RUNNER.KERNEL.IsProcessInJob
        is_in_job.argtypes = [RUNNER.wintypes.HANDLE, RUNNER.wintypes.HANDLE,
                              ctypes.POINTER(RUNNER.wintypes.BOOL)]
        is_in_job.restype = RUNNER.wintypes.BOOL
        in_job = RUNNER.wintypes.BOOL()
        self.assertTrue(is_in_job(handle, job, ctypes.byref(in_job)),
                        "Could not query grandchild job membership")
        self.assertTrue(in_job.value, "Grandchild escaped the owned command job")
        return handle

    @staticmethod
    def close_probe_process(handle):
        try:
            status = RUNNER.KERNEL.WaitForSingleObject(handle, PROBE_EXIT_WAIT_MS)
            if status == RUNNER.WAIT_TIMEOUT:
                try:
                    RUNNER._winapi.TerminateProcess(handle, 1)
                except PermissionError:
                    # Windows may reject a second termination of an already-exiting
                    # process. Its pinned handle still has to become signaled.
                    pass
                if RUNNER.KERNEL.WaitForSingleObject(handle, PROBE_EXIT_WAIT_MS) != \
                        RUNNER.WAIT_SIGNALED:
                    raise AssertionError("Owned probe did not stop during fixture cleanup")
            elif status != RUNNER.WAIT_SIGNALED:
                raise AssertionError("Could not wait for owned probe during fixture cleanup")
        finally:
            RUNNER.KERNEL.CloseHandle(handle)

    def recover_stopped_owner(self):
        # This fixture stands in for a runner process that already exited. Live-owner refusal
        # is exercised independently using the real current process identity below.
        with patch.object(RUNNER, "_process_identity", return_value=None):
            return RUNNER._recover()

    def arm_mutated_fixture(self, second=False):
        self.catalog()
        originals = {"probe.cpp": self.original.decode("utf-8")}
        if second:
            self.other = self.root / "other.cpp"
            self.other.write_bytes(self.original)
            originals["other.cpp"] = self.original.decode("utf-8")
        RUNNER._arm("catalog.json", originals, "wrong-result")
        mutated = self.original.decode("utf-8").replace("return 1", "return 0")
        RUNNER._record_mutation("probe.cpp", mutated)
        RUNNER._write(self.source, mutated)

    def update_manifest(self, update):
        manifest = json.loads(RUNNER.SENTINEL_PATH.read_text())
        update(manifest)
        RUNNER._write_manifest(manifest)

    def assert_recovery_refused_without_write(self, reason):
        source_before = self.source.read_bytes()
        marker_before = RUNNER.SENTINEL_PATH.read_bytes()
        snapshots_before = {path.name: path.read_bytes() for path in RUNNER.SNAPSHOT_DIR.iterdir()
                            if path.is_file()}
        with patch.object(RUNNER, "_process_identity", return_value=None):
            with self.assertRaises((RUNNER.HarnessError, OSError)) as rejected:
                RUNNER._recover()
        self.assertEqual(self.source.read_bytes(), source_before)
        self.assertTrue(RUNNER.SENTINEL_PATH.is_file(), "refusal erased recovery authority")
        self.assertEqual(RUNNER.SENTINEL_PATH.read_bytes(), marker_before)
        self.assertEqual({path.name: path.read_bytes() for path in RUNNER.SNAPSHOT_DIR.iterdir()
                          if path.is_file()}, snapshots_before)
        self.assertRegex(str(rejected.exception), reason)

    def test_recovery_refuses_live_owner_without_touching_source(self):
        self.arm_mutated_fixture()
        before = self.source.read_bytes()
        with self.assertRaisesRegex(RUNNER.HarnessError, "owner is still running"):
            RUNNER._recover()
        self.assertEqual(self.source.read_bytes(), before)
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())

    def test_recovery_complete_dead_owner_restores_original_bytes(self):
        self.arm_mutated_fixture()
        self.assertEqual(self.recover_stopped_owner(), 0)
        self.assert_restored()

    def test_recovery_refuses_zero_snapshots_for_nonempty_manifest(self):
        self.arm_mutated_fixture()
        RUNNER._snapshot_path("probe.cpp").unlink()
        self.assert_recovery_refused_without_write("required recovery snapshot")

    def test_recovery_refuses_empty_manifest_even_with_empty_snapshot_directory(self):
        self.arm_mutated_fixture()
        RUNNER._snapshot_path("probe.cpp").unlink()
        self.update_manifest(lambda manifest: manifest.update(files=[]))
        self.assert_recovery_refused_without_write("nonempty complete source manifest")

    def test_recovery_prevalidates_last_snapshot_before_first_source_write(self):
        self.arm_mutated_fixture(second=True)
        mutated = self.original.decode("utf-8").replace("return 1", "return 0")
        RUNNER._record_mutation("other.cpp", mutated)
        RUNNER._write(self.other, mutated)
        # The first manifest entry is valid; corruption is in the LAST entry.
        RUNNER._snapshot_path("probe.cpp").unlink()
        other_before = self.other.read_bytes()
        self.assert_recovery_refused_without_write("required recovery snapshot")
        self.assertEqual(self.other.read_bytes(), other_before)

    def test_recovery_refuses_snapshot_hash_mismatch(self):
        self.arm_mutated_fixture()
        snapshot = RUNNER._snapshot_path("probe.cpp")
        snapshot.write_bytes(self.original.replace(b"return 1", b"return 9"))
        self.assert_recovery_refused_without_write("snapshot hash mismatch")

    def test_recovery_refuses_snapshot_size_mismatch(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest["files"][0].update(size=len(self.original)+1))
        self.assert_recovery_refused_without_write("wrong size")

    def test_recovery_refuses_extra_snapshot_without_source_write(self):
        self.arm_mutated_fixture()
        (RUNNER.SNAPSHOT_DIR / "unlisted.snapshot").write_bytes(b"unlisted originals")
        self.assert_recovery_refused_without_write("does not match the complete manifest")

    def test_recovery_refuses_duplicate_source_records(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest["files"].append(dict(manifest["files"][0])))
        self.assert_recovery_refused_without_write("Duplicate source")

    def test_recovery_refuses_snapshot_mapping_substitution(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest["files"][0].update(snapshot="other.snapshot"))
        self.assert_recovery_refused_without_write("invalid snapshot mapping")

    def test_recovery_refuses_unrelated_source_edit(self):
        self.arm_mutated_fixture()
        self.source.write_bytes(b"a new unrelated user edit\r\n")
        self.assert_recovery_refused_without_write("Source changed outside")

    def test_recovery_refuses_legacy_marker_without_migration(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest.pop("schema_version"))
        self.assert_recovery_refused_without_write("Legacy/unbound recovery marker")

    def test_recovery_refuses_unbound_snapshot_directory(self):
        self.arm_mutated_fixture()
        RUNNER.SENTINEL_PATH.unlink()
        snapshot = RUNNER._snapshot_path("probe.cpp")
        before = snapshot.read_bytes()
        with self.assertRaisesRegex(RUNNER.HarnessError, "Unbound snapshot directory retained"):
            RUNNER._recover()
        self.assertEqual(snapshot.read_bytes(), before)

    def test_recovery_requires_process_instance_binding(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest["owner"].pop("created"))
        self.assert_recovery_refused_without_write("bound PID and creation time")

    def test_recovery_rejects_out_of_range_process_binding(self):
        self.arm_mutated_fixture()
        for owner in ({"pid": 1 << 32, "created": 1}, {"pid": 1, "created": 1 << 64}):
            with self.subTest(owner=owner):
                self.update_manifest(lambda manifest: manifest.update(owner=owner))
                self.assert_recovery_refused_without_write("bound PID and creation time")

    def test_recovery_rejects_malformed_or_nonfile_marker(self):
        self.arm_mutated_fixture()
        RUNNER.SENTINEL_PATH.write_bytes(b'{"schema_version":')
        self.assert_recovery_refused_without_write("not valid UTF-8 JSON")
        RUNNER.SENTINEL_PATH.unlink()
        RUNNER.SENTINEL_PATH.mkdir()
        with self.assertRaisesRegex(RUNNER.HarnessError, "manifest is missing"):
            RUNNER._recover()
        self.assertIn(b"return 0", self.source.read_bytes())

    def test_recovery_accepts_complete_quiescent_command_receipt(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest.update(commands=[
            {"state": "quiescent", "pid": 12345, "created": 1, "active_processes": 0}]))
        self.assertEqual(self.recover_stopped_owner(), 0)
        self.assert_restored()

    def test_real_owned_command_persists_bound_quiescent_receipt(self):
        self.arm_mutated_fixture()
        log = self.root / "recorded-command.log"
        self.assertEqual(RUNNER._run([sys.executable, "-c", "print('owned command')"], 5, log), 0)
        command = json.loads(RUNNER.SENTINEL_PATH.read_text())["commands"][-1]
        self.assertEqual(command["state"], "quiescent")
        self.assertEqual(command["active_processes"], 0)
        self.assertIn("pid", command)
        self.assertIn("created", command)
        self.assertGreater(command["pid"], 0)
        self.assertGreater(command["created"], 0)
        self.assertEqual(self.recover_stopped_owner(), 0)
        self.assert_restored()

    def test_process_query_uncertainty_refuses_recovery(self):
        self.arm_mutated_fixture()
        before = self.source.read_bytes()
        with patch.object(RUNNER, "_process_identity", side_effect=RUNNER.HarnessError("query denied")):
            with self.assertRaisesRegex(RUNNER.HarnessError, "query denied"):
                RUNNER._recover()
        self.assertEqual(self.source.read_bytes(), before)
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())

    def assert_uncertain_command_refused(self, state):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest.update(commands=[
            {"state": state, "pid": 12345, "created": 1, "active_processes": 0}]))
        self.assert_recovery_refused_without_write("Command start or exit is uncertain")

    def test_dead_parent_absence_does_not_clear_uncertain_command(self):
        self.assert_uncertain_command_refused("root_exited")

    def test_uncertain_start_is_not_a_quiescent_receipt(self):
        self.assert_uncertain_command_refused("starting")

    def test_running_command_is_not_a_quiescent_receipt(self):
        self.assert_uncertain_command_refused("running")

    def test_recovery_requires_empty_job_completion_receipt(self):
        self.arm_mutated_fixture()
        self.update_manifest(lambda manifest: manifest.update(commands=[
            {"state": "quiescent", "pid": 12345, "created": 1, "active_processes": 1}]))
        self.assert_recovery_refused_without_write("empty-job completion evidence")

    def test_source_path_refuses_escape_absolute_ads_and_hardlink(self):
        self.arm_mutated_fixture()
        for path in ("../outside.cpp", "C:/outside.cpp", "//server/share/file.cpp",
                     "probe.cpp:stream", "./probe.cpp", "folder\\probe.cpp"):
            with self.subTest(path=path):
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._source_path(path)
        alias = self.root / "alias.cpp"
        os.link(self.source, alias)
        with self.assertRaisesRegex(RUNNER.HarnessError, "direct regular file"):
            RUNNER._source_path("alias.cpp")

    def test_recovery_refuses_junction_redirected_snapshot_directory(self):
        self.arm_mutated_fixture()
        original_directory = self.root.parent / "saved-snapshots"
        RUNNER.SNAPSHOT_DIR.rename(original_directory)
        RUNNER._winapi.CreateJunction(str(original_directory), str(RUNNER.SNAPSHOT_DIR))
        before = self.source.read_bytes()
        try:
            with patch.object(RUNNER, "_process_identity", return_value=None):
                with self.assertRaisesRegex(RUNNER.HarnessError, "Reparse path"):
                    RUNNER._recover()
            self.assertEqual(self.source.read_bytes(), before)
            self.assertTrue(RUNNER.SENTINEL_PATH.exists())
        finally:
            if RUNNER.SNAPSHOT_DIR.exists():
                RUNNER.SNAPSHOT_DIR.rmdir()

    def test_snapshot_names_do_not_alias_path_separators_with_hash_characters(self):
        self.assertNotEqual(RUNNER._snapshot_path("a/b.cpp"), RUNNER._snapshot_path("a#b.cpp"))

    def test_disarm_refuses_source_that_is_still_mutated(self):
        self.arm_mutated_fixture()
        with self.assertRaisesRegex(RUNNER.HarnessError, "every original byte is restored"):
            RUNNER._disarm()
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())
        self.assertIn(b"return 0", self.source.read_bytes())

    def test_uncertain_command_start_is_durable_before_launch(self):
        self.arm_mutated_fixture()

        def fail_launch(*args):
            marker = json.loads(RUNNER.SENTINEL_PATH.read_text())
            self.assertTrue(marker["commands"], "command intent was not durable before launch")
            self.assertEqual(marker["commands"][-1]["state"], "starting")
            raise RUNNER.HarnessError("launch outcome uncertain")

        with patch.object(RUNNER._WindowsCommand, "start", side_effect=fail_launch):
            with self.assertRaisesRegex(RUNNER.HarnessError, "launch outcome uncertain"):
                RUNNER._run(["probe"], 1, self.root / "launch.log")
        self.assert_recovery_refused_without_write("Command start or exit is uncertain")

    def test_failed_command_intent_does_not_publish_ram_state_or_create_child(self):
        self.arm_mutated_fixture()
        previous = json.loads(json.dumps(RUNNER.RUN_MANIFEST))
        marker_before = RUNNER.SENTINEL_PATH.read_bytes()
        failure = OSError("prelaunch-journal-write-failed")
        with patch.object(RUNNER, "_write_manifest", side_effect=failure), \
                patch.object(RUNNER, "_WindowsCommand") as command:
            with self.assertRaises(OSError) as rejected:
                RUNNER._run(["probe"], 1, self.root / "never-launched.log")
        self.assertIs(rejected.exception, failure)
        command.assert_not_called()
        self.assertIsNone(RUNNER.UNCONFIRMED_CHILD_PID)
        self.assertEqual(RUNNER.RUN_MANIFEST, previous)
        self.assertEqual(RUNNER.SENTINEL_PATH.read_bytes(), marker_before)

    def test_command_intent_is_published_only_after_durable_write(self):
        self.arm_mutated_fixture()
        previous = json.loads(json.dumps(RUNNER.RUN_MANIFEST))
        original_write = RUNNER._write_manifest

        def inspect_commit(candidate):
            self.assertEqual(RUNNER.RUN_MANIFEST, previous,
                             "uncommitted command intent became visible in RAM")
            self.assertEqual(candidate["commands"], [{"state": "starting"}])
            original_write(candidate)

        with patch.object(RUNNER, "_write_manifest", side_effect=inspect_commit):
            RUNNER._command_begin()
        self.assertEqual(RUNNER.RUN_MANIFEST["commands"], [{"state": "starting"}])
        self.assertEqual(json.loads(RUNNER.SENTINEL_PATH.read_bytes()), RUNNER.RUN_MANIFEST)

    def test_catalog_prelaunch_write_failure_restores_source_and_original_error(self):
        path = self.catalog()
        original_write = RUNNER._write_manifest
        failure = OSError("catalog-prelaunch-journal-write-failed")

        def reject_new_command(candidate):
            if candidate["commands"]:
                raise failure
            original_write(candidate)

        def reject_mutant_test(*args):
            RUNNER._run(["probe"], 1, self.root / "never-launched.log")

        outcomes = iter([True, reject_mutant_test])

        def next_test(*args):
            outcome = next(outcomes)
            return outcome(*args) if callable(outcome) else outcome

        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", return_value=True) as build, \
                patch.object(RUNNER, "_test_passes", side_effect=next_test), \
                patch.object(RUNNER, "_write_manifest", side_effect=reject_new_command), \
                patch.object(RUNNER, "_WindowsCommand") as command:
            with self.assertRaises((RUNNER.HarnessError, OSError)) as rejected:
                RUNNER.main()
        self.assertIs(rejected.exception, failure)
        command.assert_not_called()
        self.assertEqual(build.call_count, 3, "restored-source build was not reached")
        self.assert_restored()

    def test_windows_deny_delete_marker_refuses_intent_without_creating_child(self):
        self.arm_mutated_fixture()
        previous = json.loads(json.dumps(RUNNER.RUN_MANIFEST))
        marker_before = RUNNER.SENTINEL_PATH.read_bytes()
        # A reader that omits FILE_SHARE_DELETE really prevents atomic replacement
        # on Windows. This handle owns only this test's private temporary marker.
        handle = RUNNER.KERNEL.CreateFileW(
            str(RUNNER.SENTINEL_PATH), 0x80000000, 0x3, None, 3, 0x80, None)
        self.assertNotEqual(handle, RUNNER.INVALID_HANDLE)
        try:
            with patch.object(RUNNER, "_WindowsCommand") as command:
                with self.assertRaises(PermissionError) as rejected:
                    RUNNER._run(["probe"], 1, self.root / "blocked-marker.log")
                command.assert_not_called()
        finally:
            self.assertTrue(RUNNER.KERNEL.CloseHandle(handle))
        self.assertIn(rejected.exception.winerror, (5, 32))
        self.assertIsNone(RUNNER.UNCONFIRMED_CHILD_PID)
        self.assertEqual(RUNNER.RUN_MANIFEST, previous)
        self.assertEqual(RUNNER.SENTINEL_PATH.read_bytes(), marker_before)
        self.assertFalse(list(self.root.glob(".mutation-in-progress.json.tmp-*")))

    def test_command_intent_commit_then_error_keeps_uncertain_recovery_refusal(self):
        self.arm_mutated_fixture()
        original_write = RUNNER._write_manifest
        failure = OSError("command-intent-postcommit-failed")

        def commit_then_fail(candidate):
            original_write(candidate)
            raise failure

        with patch.object(RUNNER, "_write_manifest", side_effect=commit_then_fail), \
                patch.object(RUNNER, "_WindowsCommand") as command:
            with self.assertRaises(OSError) as rejected:
                RUNNER._run(["probe"], 1, self.root / "postcommit.log")
        self.assertIs(rejected.exception, failure)
        command.assert_not_called()
        self.assertIsNone(RUNNER.UNCONFIRMED_CHILD_PID)
        marker = json.loads(RUNNER.SENTINEL_PATH.read_bytes())
        self.assertEqual(marker["commands"], [{"state": "starting"}])
        self.assert_recovery_refused_without_write("Command start or exit is uncertain")

    def test_cli_retains_original_and_all_cleanup_exception_diagnostics(self):
        def fail_with_cleanup_chain():
            try:
                try:
                    raise OSError("original-prelaunch-persistence-diagnostic")
                finally:
                    raise RUNNER.HarnessError("inner-cleanup-refusal-diagnostic")
            finally:
                raise RUNNER.HarnessError("outer-cleanup-refusal-diagnostic")

        # Execute the actual entry block, not a second copy of its error renderer.
        # main is replaced before execution: no real catalog, build or admission.
        source = Path(RUNNER.__file__).read_text(encoding="utf-8")
        entry = ast.parse(source).body[-1]
        self.assertIsInstance(entry, ast.If)
        namespace = dict(vars(RUNNER), __name__="__main__", main=fail_with_cleanup_chain)
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as terminated:
                exec(compile(ast.Module(body=[entry], type_ignores=[]), RUNNER.__file__, "exec"),
                     namespace)
        self.assertEqual(terminated.exception.code, 2)
        self.assertIn("MUTATION RESULT: ERROR:", stdout.getvalue())
        for diagnostic in ("original-prelaunch-persistence-diagnostic",
                           "inner-cleanup-refusal-diagnostic", "outer-cleanup-refusal-diagnostic"):
            self.assertIn(diagnostic, stderr.getvalue())

    def test_native_child_is_suspended_until_job_assignment(self):
        marker = self.root / "child-ran.txt"
        code = f"from pathlib import Path;import time;Path({str(marker)!r}).write_text('ran');time.sleep(5)"
        command = RUNNER._WindowsCommand()
        assign = RUNNER.KERNEL.AssignProcessToJobObject

        def checked_assignment(job, process):
            time.sleep(1)
            self.assertFalse(marker.exists(), "child ran before containment was assigned")
            return assign(job, process)

        with (self.root / "suspended.log").open("wb") as output:
            try:
                with patch.object(RUNNER.KERNEL, "AssignProcessToJobObject", side_effect=checked_assignment):
                    command.start([sys.executable, "-c", code], output)
                self.assertEqual(command.active_processes(), 1)
            finally:
                command.stop_and_confirm()
                # Independent fixture cleanup remains effective when a negative drill breaks
                # job assignment itself; this handle belongs only to this harmless probe.
                if command.process_handle and RUNNER.KERNEL.WaitForSingleObject(
                        command.process_handle, 0) == RUNNER.WAIT_TIMEOUT:
                    RUNNER._winapi.TerminateProcess(command.process_handle, 1)
                    command.wait(5)
                command.close()

    def test_child_job_quiescence_is_confirmed_after_termination(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # Job observations are injected; no native handle is closed.
        with patch.object(command, "active_processes", side_effect=[1, 1, 0]) as active, \
                patch.object(command, "pin_active_processes", return_value=[]), \
                patch.object(RUNNER.KERNEL, "TerminateJobObject", return_value=1):
            command.stop_and_confirm()
        self.assertEqual(active.call_count, 3, "termination acceptance is not empty-job evidence")

    def test_incomplete_job_member_list_refuses_cleanup(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # Query is injected; no native handle is closed.

        def incomplete_list(_job, _kind, buffer, _size, _returned):
            info = ctypes.cast(buffer, ctypes.POINTER(RUNNER._JobProcessIds)).contents
            info.assigned = 2
            info.returned = 1
            return 1

        with patch.object(RUNNER.KERNEL, "QueryInformationJobObject",
                          side_effect=incomplete_list), \
                self.assertRaisesRegex(RUNNER.HarnessError, "bounded buffer"):
            command.pin_active_processes()

    def test_job_member_query_failure_refuses_cleanup(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # Query is injected; no native handle is closed.

        def denied_query(*_arguments):
            ctypes.set_last_error(5)
            return 0

        with patch.object(RUNNER.KERNEL, "QueryInformationJobObject",
                          side_effect=denied_query), \
                self.assertRaisesRegex(RUNNER.HarnessError,
                                       "Job process ID enumeration failed.*error 5"):
            command.pin_active_processes()

    def test_foreign_pid_cannot_be_pinned_as_job_member(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # Every native observation is injected.

        def one_member(_job, _kind, buffer, _size, _returned):
            info = ctypes.cast(buffer, ctypes.POINTER(RUNNER._JobProcessIds)).contents
            info.assigned = 1
            info.returned = 1
            info.process_ids[0] = 1234
            return 1

        with patch.object(RUNNER.KERNEL, "QueryInformationJobObject",
                          side_effect=one_member), \
                patch.object(RUNNER.KERNEL, "OpenProcess", return_value=42), \
                patch.object(RUNNER.KERNEL, "WaitForSingleObject",
                             return_value=RUNNER.WAIT_TIMEOUT), \
                patch.object(RUNNER.KERNEL, "IsProcessInJob", return_value=1), \
                patch.object(RUNNER.KERNEL, "CloseHandle") as close, \
                self.assertRaisesRegex(RUNNER.HarnessError, "live owned job process"):
            command.pin_active_processes()
        close.assert_called_once_with(42)

    def test_unsignaled_pinned_member_refuses_cleanup_and_closes_handle(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # Job and member handles are injected, not native handles.
        with patch.object(command, "active_processes", side_effect=[1, 1]), \
                patch.object(command, "pin_active_processes", return_value=[42]), \
                patch.object(RUNNER.KERNEL, "TerminateJobObject", return_value=1), \
                patch.object(RUNNER.KERNEL, "WaitForSingleObject",
                             return_value=RUNNER.WAIT_TIMEOUT), \
                patch.object(RUNNER.KERNEL, "CloseHandle") as close, \
                self.assertRaisesRegex(RUNNER.HarnessError,
                                       "member did not terminate before deadline"):
            command.stop_and_confirm()
        close.assert_called_once_with(42)

    def test_admission_is_cross_process_and_releases_with_owner_handle(self):
        ready, stop = self.root / "ready", self.root / "stop"
        module_path = REPO_ROOT / "scripts/run_mutation_test.py"
        code = (
            "import importlib.util,sys,time\nfrom pathlib import Path\n"
            f"sys.path.insert(0,str(Path({str(module_path)!r}).parent))\n"
            f"spec=importlib.util.spec_from_file_location('probe',{str(module_path)!r})\n"
            "module=importlib.util.module_from_spec(spec)\nspec.loader.exec_module(module)\n"
            f"module.REPO_ROOT=Path({str(self.root)!r})\n"
            "with module._admission():\n"
            f" Path({str(ready)!r}).write_text('ready')\n"
            f" while not Path({str(stop)!r}).exists(): time.sleep(0.01)\n"
        )
        child = subprocess.Popen([sys.executable, "-c", code], creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            deadline = time.monotonic() + 5
            while not ready.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertTrue(ready.exists())
            with self.assertRaisesRegex(RUNNER.HarnessError, "Exclusive mutation"):
                RUNNER._recover()
        finally:
            stop.write_text("stop")
            child.wait(timeout=5)
        with RUNNER._admission():
            pass

    def test_admission_blocks_second_run_and_recovery_before_marker_exists(self):
        catalog = self.catalog()
        with RUNNER._admission():
            with self.assertRaisesRegex(RUNNER.HarnessError, "Exclusive mutation"):
                RUNNER._recover()
            with patch.object(sys, "argv", ["runner", str(catalog)]), \
                    patch.object(RUNNER, "_build") as build:
                with self.assertRaisesRegex(RUNNER.HarnessError, "Exclusive mutation"):
                    RUNNER.main()
                build.assert_not_called()
        self.assertFalse(RUNNER.SENTINEL_PATH.exists())
        with RUNNER._admission():
            pass  # Closing the owning handle released admission; no stale-file guessing needed.

    def test_normal_root_exit_still_drains_orphaned_descendant_job(self):
        grandchild_code = "import time;time.sleep(30)"
        child_code = (
            "import subprocess,sys;"
            f"child=subprocess.Popen([sys.executable,'-c',{grandchild_code!r}],"
            "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL);print(child.pid,flush=True)"
        )
        parent_code = (
            "import subprocess,sys;"
            f"print(subprocess.check_output([sys.executable,'-c',{child_code!r}]).decode().strip(),flush=True)"
        )
        log = self.root / "orphan-job.log"
        pinned = []
        with patch.object(RUNNER._WindowsCommand, "wait",
                          self.wait_after_probe_ready(
                              log, lambda command: pinned.append(
                                  self.pin_probe_process(log, command.job)))):
            self.assertEqual(RUNNER._run([sys.executable, "-c", parent_code], 5, log), 0)
        self.assertEqual(len(pinned), 1)
        self.assertEqual(RUNNER.KERNEL.WaitForSingleObject(pinned[0], 0), RUNNER.WAIT_SIGNALED,
                         "owned grandchild remained after root exit")

    def test_assertion_failure_is_killed_and_restores_exact_bytes(self):
        self.assertEqual(self.run_catalog([True, True, True], [True, False]), 0)
        self.assert_restored()

    def test_lf_catalog_mutates_crlf_source_without_changing_eols(self):
        path = self.catalog()
        self.original = b"int probe() {\r\n    return 1;\r\n}\r\n"
        self.source.write_bytes(self.original)
        catalog = json.loads(path.read_text(encoding="utf-8"))
        mutant = catalog["mutants"][0]
        mutant["find"] = "return 1;\n}"
        mutant["replace"] = "return 0;\n}"
        path.write_text(json.dumps(catalog), encoding="utf-8")
        self.assertIsNone(CATALOGS._validate_mutant(mutant, self.original.decode("utf-8")))

        observations = []

        def inspect_mutation(*_args):
            data = self.source.read_bytes()
            observations.append(data)
            if len(observations) == 1:
                self.assertEqual(data, self.original)
                return True
            self.assertEqual(data, b"int probe() {\r\n    return 0;\r\n}\r\n")
            return False

        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", return_value=True), \
                patch.object(RUNNER, "_test_passes", side_effect=inspect_mutation):
            self.assertEqual(RUNNER.main(), 0)
        self.assertEqual(len(observations), 2)
        self.assert_restored()

    def test_catalog_line_endings_refuse_mixed_and_bare_cr(self):
        mutant = {"id": "eol", "file": "probe.cpp", "find": "return 1;\n}",
                  "replace": "return 0;\n}", "expect": "killed"}
        mixed = "int probe() {\r\n    return 1;\n}\r\n"
        self.assertIn("mixed", CATALOGS._validate_mutant(mutant, mixed))
        with self.assertRaisesRegex(ValueError, "mixed"):
            RUNNER.adapt_mutation_text(mixed, mutant["find"], mutant["replace"])
        mutant["find"] = "return 1;\r}"
        self.assertIn("bare-CR", CATALOGS._validate_mutant(mutant, "return 1;\n}"))

    def test_catalog_crlf_needle_adapts_to_lf_source(self):
        find, replacement = RUNNER.adapt_mutation_text(
            "int probe() {\n    return 1;\n}\n", "return 1;\r\n}", "return 0;\r\n}")
        self.assertEqual(find, "return 1;\n}")
        self.assertEqual(replacement, "return 0;\n}")

    def test_noncompiling_mutant_is_error_not_coverage(self):
        with self.assertRaisesRegex(RUNNER.HarnessError, "did not compile"):
            self.run_catalog([True, False, True], [True])
        self.assert_restored()

    def test_arm_failure_is_not_masked_by_restoration_or_build(self):
        path = self.catalog()
        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_arm", side_effect=RUNNER.HarnessError("snapshot creation failed")), \
                patch.object(RUNNER, "_build") as build:
            with self.assertRaisesRegex(RUNNER.HarnessError, "snapshot creation failed"):
                RUNNER.main()
        build.assert_not_called()
        self.assertEqual(self.source.read_bytes(), self.original)

    def test_failed_restoration_build_invalidates_run(self):
        with self.assertRaisesRegex(RUNNER.HarnessError, "Restored-source build failed"):
            self.run_catalog([True, True, False], [True, False])
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())
        self.assertEqual(self.recover_stopped_owner(), 0)
        self.assert_restored()

    def test_test_infrastructure_error_restores_source(self):
        with self.assertRaisesRegex(RUNNER.HarnessError, "test timed out"):
            self.run_catalog([True, True, True], [True, RUNNER.HarnessError("test timed out")])
        self.assert_restored()

    def test_survivor_fails_coverage(self):
        self.assertEqual(self.run_catalog([True, True, True], [True, True]), 1)
        self.assert_restored()

    def test_failed_source_restore_retains_snapshot_and_does_not_build(self):
        path = self.catalog()
        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", return_value=True) as build, \
                patch.object(RUNNER, "_test_passes", side_effect=[True, False]), \
                patch.object(RUNNER, "_restore_file", return_value=False):
            with self.assertRaisesRegex(RUNNER.HarnessError, "Source restoration failed"):
                RUNNER.main()
        self.assertEqual(build.call_count, 2)
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())
        self.assertEqual(self.recover_stopped_owner(), 0)
        self.assert_restored()

    def test_failed_child_termination_retains_mutant_and_snapshot(self):
        path = self.catalog()

        def timed_out_test(*args):
            RUNNER._run(["probe"], 5, self.root / "failed-termination.log")

        calls = iter([True, timed_out_test])

        def next_test_result(*args):
            step = next(calls)
            return step(*args) if callable(step) else step

        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", return_value=True) as build, \
                patch.object(RUNNER, "_test_passes", side_effect=next_test_result), \
                patch.object(RUNNER, "_WindowsCommand") as start, \
                patch.object(RUNNER, "_terminate_child_tree", side_effect=RUNNER.HarnessError("active")):
            start.return_value.pid = 12345
            start.return_value.wait.side_effect = subprocess.TimeoutExpired("probe", 5)
            with self.assertRaisesRegex(RUNNER.HarnessError, "Child tree may still be active"):
                RUNNER.main()
        self.assertEqual(build.call_count, 2, "restoration build must not race live children")
        self.assertIn(b"return 0", self.source.read_bytes())
        self.assertTrue(RUNNER.SNAPSHOT_DIR.exists())
        marker = json.loads(RUNNER.SENTINEL_PATH.read_text())
        self.assertEqual(marker["unconfirmed_child_pid"], 12345)
        with patch.object(RUNNER, "_process_identity", return_value=None):
            with self.assertRaisesRegex(RUNNER.HarnessError, "Unconfirmed child"):
                RUNNER._recover()

    def test_restoration_build_timeout_keeps_recovery_record(self):
        path = self.catalog()
        build_calls = 0

        def build_result(*args):
            nonlocal build_calls
            build_calls += 1
            if build_calls < 3:
                return True
            RUNNER._run(["probe"], 5, self.root / "restoration-timeout.log")

        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", side_effect=build_result), \
                patch.object(RUNNER, "_test_passes", side_effect=[True, False]), \
                patch.object(RUNNER, "_WindowsCommand") as start, \
                patch.object(RUNNER, "_terminate_child_tree", side_effect=RUNNER.HarnessError("active")):
            start.return_value.pid = 12345
            start.return_value.wait.side_effect = subprocess.TimeoutExpired("probe", 5)
            with self.assertRaisesRegex(RUNNER.HarnessError, "Child tree may still be active"):
                RUNNER.main()
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertEqual(build_calls, 3)
        self.assertTrue(RUNNER.SNAPSHOT_DIR.exists())
        marker = json.loads(RUNNER.SENTINEL_PATH.read_text())
        self.assertEqual(marker["unconfirmed_child_pid"], 12345)

    def write_ctest(self, code):
        executable = str(Path(sys.executable)).replace("\\", "/")
        self.root.joinpath("CTestTestfile.cmake").write_text(
            f'add_test(probe "{executable}" "-c" "{code}")\n', encoding="utf-8"
        )

    def qt_report(self, path, functions, outcomes=None):
        outcomes = outcomes or {}
        report = ET.Element("TestCase", name="FixtureQtTest")
        for name in ["initTestCase"] + functions + ["cleanupTestCase"]:
            function = ET.SubElement(report, "TestFunction", name=name)
            ET.SubElement(function, "Incident", type=outcomes.get(name, "pass"),
                          file="fixture.cpp", line="17")
            ET.SubElement(function, "Duration", msecs="0")
        ET.SubElement(report, "Duration", msecs="0")
        ET.ElementTree(report).write(path, encoding="utf-8", xml_declaration=True)

    def scoped_registration(self, properties=None):
        return {"name": "probe", "command": [sys.executable], "properties": properties or {
            "WORKING_DIRECTORY": str(self.root)}}

    def write_qt_fixture(self, properties=None):
        script = self.root / "fake_qt.py"
        script.write_text(textwrap.dedent('''\
            import os
            from pathlib import Path
            import sys
            import xml.etree.ElementTree as ET
            functions, outputs = [], []
            arguments = iter(sys.argv[1:])
            for argument in arguments:
                if argument == "-o":
                    outputs.append(next(arguments).rsplit(",", 1))
                else:
                    functions.append(argument)
            if "unknown" in functions:
                print("Unknown test function: unknown", flush=True)
                raise SystemExit(1)
            if os.environ.get("SCOPED_ENV") != 'left;"${literal}"|tail':
                raise SystemExit("registered environment changed")
            if not Path("working-directory-witness").is_file():
                raise SystemExit("registered working directory changed")
            report = ET.Element("TestCase", name="FixtureQtTest")
            names = ["initTestCase"] + functions + ["cleanupTestCase"]
            if "extra" in functions:
                names.append("unrequested")
            if "duplicate" in functions:
                names.append("duplicate")
            failures = 0
            for name in names:
                if name == "absent":
                    continue
                function = ET.SubElement(report, "TestFunction", name=name)
                outcome = "pass"
                if name in ("fails", "falseGreen") or (name == "cleanupTestCase" and "lifecycle" in functions):
                    outcome = "fail"
                    failures += 1
                if name == "skipped":
                    outcome = "skip"
                if name == "unsupported":
                    outcome = "xfail"
                if name != "empty":
                    ET.SubElement(function, "Incident", type=outcome, file="fixture.cpp", line="17")
                if name == "fatal":
                    ET.SubElement(function, "Message", type="qfatal")
                ET.SubElement(function, "Duration", msecs="0")
            ET.SubElement(report, "Duration", msecs="0")
            for destination, kind in outputs:
                if kind == "xml" and "noXml" not in functions:
                    ET.ElementTree(report).write(destination, encoding="utf-8", xml_declaration=True)
                elif kind == "txt" and destination != "-":
                    Path(destination).write_text("executed " + ",".join(functions), encoding="utf-8")
            raise SystemExit(0 if "falseGreen" in functions else bool(failures or "mismatch" in functions))
            '''), encoding="utf-8")
        work = self.root / "working directory"
        work.mkdir()
        (work / "working-directory-witness").write_text("present", encoding="utf-8")
        report_dir = self.root / "test_results"
        report_dir.mkdir()
        command = [sys.executable, str(script), "-o", f"{report_dir / 'probe.txt'},txt", "-o", "-,txt"]
        context = properties or {
            "WORKING_DIRECTORY": str(work),
            "ENVIRONMENT": ['SCOPED_ENV=left;"${literal}"'],
            "ENVIRONMENT_MODIFICATION": ["SCOPED_ENV=string_append:|tail"],
            "TIMEOUT": 10.0, "LABELS": ["scope;literal", "proof"],
            "PROCESSORS": 1, "RUN_SERIAL": True,
        }
        literal = RUNNER._cmake_literal
        contents = "add_test(" + " ".join(literal(value) for value in ["probe"] + command) + ")\n"
        for name, value in context.items():
            contents += f"set_tests_properties(probe PROPERTIES {name} {literal(RUNNER._cmake_property(value))})\n"
        self.root.joinpath("CTestTestfile.cmake").write_text(contents, encoding="utf-8")

    def run_qt_fixture(self, functions):
        return RUNNER._test_passes(str(self.root), "Release", "^probe$", functions,
                                   Path(sys.executable).stem)

    def test_qt_catalog_selector_validation_is_bounded_and_exact(self):
        self.assertIsNone(RUNNER._catalog_qt_functions({}))
        self.assertEqual(RUNNER._catalog_qt_functions({"qt_functions": ["one", "two_2"]}),
                         ["one", "two_2"])
        invalid = (None, [], "one", ["one", "one"], ["one:row"], ["one()"], ["-functions"],
                   ["initTestCase"], ["cleanupTestCase"], ["init"], ["cleanup"], ["one_data"],
                   [True], [""], ["a" * 129], ["f" + str(index) for index in range(65)])
        for functions in invalid:
            with self.subTest(functions=functions):
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._catalog_qt_functions({"qt_functions": functions})

    def test_invalid_qt_catalog_refuses_before_arming_or_build(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        catalog["qt_functions"] = []
        path.write_text(json.dumps(catalog), encoding="utf-8")
        with patch.object(sys, "argv", ["runner", str(path)]), patch.object(RUNNER, "_build") as build:
            with self.assertRaises(RUNNER.HarnessError):
                RUNNER.main()
            build.assert_not_called()
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertFalse(RUNNER.SENTINEL_PATH.exists())

    def test_qt_catalog_passes_selectors_and_target_to_baseline_and_mutant(self):
        path = self.catalog()
        catalog = json.loads(path.read_text())
        catalog["qt_functions"] = ["one"]
        path.write_text(json.dumps(catalog), encoding="utf-8")
        with patch.object(sys, "argv", ["runner", str(path)]), \
                patch.object(RUNNER, "_build", return_value=True), \
                patch.object(RUNNER, "_test_passes", side_effect=[True, False]) as tests:
            self.assertEqual(RUNNER.main(), 0)
        self.assertEqual(tests.call_args_list[0].args, ("build", "Release", "^probe$", ["one"], "probe"))
        self.assertEqual(tests.call_args_list[1].args, tests.call_args_list[0].args)
        self.assert_restored()

    def test_real_scoped_ctest_preserves_context_and_distinguishes_assertions(self):
        self.write_qt_fixture()
        registration_before = self.root.joinpath("CTestTestfile.cmake").read_bytes()
        self.assertTrue(self.run_qt_fixture(["passes", "another"]))
        self.assertFalse(self.run_qt_fixture(["fails"]))
        self.assertEqual(self.root.joinpath("CTestTestfile.cmake").read_bytes(), registration_before)
        self.assertEqual(len(list(RUNNER.RUN_LOG_DIR.glob("*.qt.xml"))), 2)
        self.assertEqual(len(list(RUNNER.RUN_LOG_DIR.glob("*.qttest.txt"))), 2)

    def test_scoped_qt_unknown_absent_and_unexecuted_functions_are_errors(self):
        self.write_qt_fixture()
        for function in ("unknown", "absent", "empty", "extra", "duplicate"):
            with self.subTest(function=function):
                with self.assertRaises(RUNNER.HarnessError):
                    self.run_qt_fixture([function])

    def test_scoped_qt_skip_lifecycle_fatal_and_unsupported_outcomes_are_errors(self):
        self.write_qt_fixture()
        for function in ("skipped", "lifecycle", "fatal", "unsupported"):
            with self.subTest(function=function):
                with self.assertRaises(RUNNER.HarnessError):
                    self.run_qt_fixture([function])

    def test_scoped_qt_exit_and_assertion_evidence_must_agree(self):
        self.write_qt_fixture()
        for function in ("mismatch", "falseGreen"):
            with self.subTest(function=function):
                with self.assertRaisesRegex(RUNNER.HarnessError, "assertion results disagree"):
                    self.run_qt_fixture([function])

    def test_scoped_context_refuses_unsupported_duplicate_or_invalid_properties(self):
        valid = [{"name": "WORKING_DIRECTORY", "value": str(self.root)}]
        for extra in ({"name": "WILL_FAIL", "value": True}, {"name": "FIXTURES_REQUIRED", "value": ["setup"]},
                      {"name": "WORKING_DIRECTORY", "value": str(self.root)},
                      {"name": "TIMEOUT", "value": 0}, {"name": "TIMEOUT", "value": float("inf")},
                      {"name": "RUN_SERIAL", "value": "true"},
                      {"name": "ENVIRONMENT", "value": "A=B"}):
            with self.subTest(extra=extra):
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._scoped_properties(valid + [extra])
        with self.assertRaisesRegex(RUNNER.HarnessError, "working directory"):
            RUNNER._scoped_properties([])

    def test_scoped_registration_round_trip_refuses_changed_context(self):
        first = self.scoped_registration()
        changed = {**first, "command": first["command"] + ["one", "-o", "wrong.xml,xml"]}
        with patch.object(RUNNER, "_ctest_registration", side_effect=[first, changed]):
            with self.assertRaisesRegex(RUNNER.HarnessError, "round trip"):
                RUNNER._prepare_qt_selection(str(self.root), "Release", "probe", Path(sys.executable).stem,
                                             ["one"], RUNNER._next_log_path("test"))

    def test_scoped_registration_requires_direct_matching_regular_binary(self):
        cases = (([sys.executable], "unrelated_target", "directly execute"),
                 (["relative.exe"], "relative", "directly execute"),
                 ([str(self.root / "missing.exe")], "missing", "regular file"))
        for command, target, reason in cases:
            with self.subTest(command=command):
                registration = {**self.scoped_registration(), "command": command}
                with patch.object(RUNNER, "_ctest_registration", return_value=registration):
                    with self.assertRaisesRegex(RUNNER.HarnessError, reason):
                        RUNNER._prepare_qt_selection(str(self.root), "Release", "probe", target,
                                                     ["one"], RUNNER._next_log_path("test"))

    def test_real_scoped_ctest_refuses_wrong_catalog_target(self):
        self.write_qt_fixture()
        with self.assertRaisesRegex(RUNNER.HarnessError, "directly execute the catalog target"):
            RUNNER._test_passes(str(self.root), "Release", "^probe$", ["passes"], "wrong_target")

    def test_real_scoped_ctest_refuses_junction_binary(self):
        self.write_qt_fixture()
        alias = self.root / "runtime-alias"
        RUNNER._winapi.CreateJunction(str(Path(sys.executable).parent), str(alias))
        registration = self.root / "CTestTestfile.cmake"
        registration.write_text(registration.read_text(encoding="utf-8").replace(
            sys.executable, str(alias / Path(sys.executable).name)), encoding="utf-8")
        try:
            with self.assertRaisesRegex(RUNNER.HarnessError, "Reparse path"):
                self.run_qt_fixture(["passes"])
        finally:
            alias.rmdir()

    def test_real_scoped_ctest_cannot_reuse_preexisting_qt_evidence(self):
        self.write_qt_fixture()
        RUNNER.RUN_LOG_DIR = self.root / "retained"
        RUNNER.RUN_LOG_DIR.mkdir()
        self.qt_report(RUNNER.RUN_LOG_DIR / "0001-test.qt.xml", ["noXml"])
        with self.assertRaisesRegex(RUNNER.HarnessError, "fresh unused path"):
            self.run_qt_fixture(["noXml"])

    def test_scoped_registration_refuses_reparse_binary_and_stale_xml_path(self):
        registration = self.scoped_registration()
        log = RUNNER._next_log_path("test")
        with patch.object(RUNNER, "_ctest_registration", return_value=registration), \
                patch.object(RUNNER, "_assert_direct_path", side_effect=RUNNER.HarnessError("Reparse")):
            with self.assertRaisesRegex(RUNNER.HarnessError, "Reparse"):
                RUNNER._prepare_qt_selection(str(self.root), "Release", "probe", Path(sys.executable).stem,
                                             ["one"], log)
        log.with_suffix(".qt.xml").write_text("stale", encoding="utf-8")
        with patch.object(RUNNER, "_ctest_registration", return_value=registration):
            with self.assertRaisesRegex(RUNNER.HarnessError, "fresh unused path"):
                RUNNER._prepare_qt_selection(str(self.root), "Release", "probe", Path(sys.executable).stem,
                                             ["one"], log)

    def test_scoped_ctest_requires_exactly_one_discovered_test(self):
        self.write_qt_fixture()
        with self.assertRaisesRegex(RUNNER.HarnessError, "exactly one"):
            RUNNER._ctest_registration(str(self.root), "Release", "^absent$")
        with self.root.joinpath("CTestTestfile.cmake").open("a", encoding="utf-8") as stream:
            stream.write(f'add_test(other {RUNNER._cmake_literal(sys.executable)} "-c" "pass")\n')
        with self.assertRaisesRegex(RUNNER.HarnessError, "exactly one"):
            RUNNER._ctest_registration(str(self.root), "Release", "probe|other")

    def test_scoped_discovery_rejects_failed_malformed_and_oversize_results(self):
        for result, content in ((1, b"{}"), (0, b"not-json"), (0, b'{"kind":"other"}')):
            def discovered(cmd, timeout_seconds, log_path):
                log_path.write_bytes(content)
                return result
            with self.subTest(result=result, content=content), patch.object(RUNNER, "_run", side_effect=discovered):
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._ctest_registration(str(self.root), "Release", "probe")
        with patch.object(RUNNER, "_run", side_effect=RUNNER.HarnessError("discovery timed out")):
            with self.assertRaisesRegex(RUNNER.HarnessError, "timed out"):
                RUNNER._ctest_registration(str(self.root), "Release", "probe")
        with patch.object(RUNNER, "MAX_CTEST_JSON_BYTES", 1), \
                patch.object(RUNNER, "_run", side_effect=discovered):
            with self.assertRaisesRegex(RUNNER.HarnessError, "byte limit"):
                RUNNER._ctest_registration(str(self.root), "Release", "probe")

    def test_scoped_ctest_rejects_missing_wrong_skipped_crashed_or_timedout_result(self):
        qt_report = self.root / "fresh.qt.xml"
        bodies = ('<testcase name="wrong"/>', '<testcase name="probe"><skipped/></testcase>',
                  '<testcase name="probe"><failure message="SEGFAULT"/></testcase>',
                  '<testcase name="probe"><failure message="Timeout"/></testcase>')
        for body in bodies:
            def executed(cmd, timeout_seconds, log_path):
                log_path.with_suffix(".xml").write_text(f"<testsuite>{body}</testsuite>", encoding="utf-8")
                self.qt_report(qt_report, ["one"])
                return 0 if 'name="wrong"' in body else 1
            with self.subTest(body=body), \
                    patch.object(RUNNER, "_prepare_qt_selection", return_value=(str(self.root), "probe", qt_report, "probe")), \
                    patch.object(RUNNER, "_run", side_effect=executed):
                with self.assertRaises(RUNNER.HarnessError):
                    self.run_qt_fixture(["one"])

    def test_qt_function_scope_checks_all_data_rows_without_tag_selection(self):
        report_path = self.root / "data-rows.xml"
        self.qt_report(report_path, ["dataDriven"])
        report = ET.parse(report_path).getroot()
        function = report.find("./TestFunction[@name='dataDriven']")
        first = function.find("Incident")
        ET.SubElement(first, "DataTag").text = "row-one"
        second = ET.SubElement(function, "Incident", type="pass", file="fixture.cpp", line="18")
        ET.SubElement(second, "DataTag").text = "row-two"
        for outcome in ("pass", "fail", "skip"):
            second.set("type", outcome)
            ET.ElementTree(report).write(report_path, encoding="utf-8", xml_declaration=True)
            if outcome == "skip":
                with self.assertRaisesRegex(RUNNER.HarnessError, "skipped"):
                    RUNNER._qt_assertion_failures(report_path, ["dataDriven"])
            else:
                self.assertEqual(RUNNER._qt_assertion_failures(report_path, ["dataDriven"]), int(outcome == "fail"))
        function.set("name", "dataDriven:row-one")
        ET.ElementTree(report).write(report_path, encoding="utf-8", xml_declaration=True)
        with self.assertRaisesRegex(RUNNER.HarnessError, "exact requested set"):
            RUNNER._qt_assertion_failures(report_path, ["dataDriven"])

    def test_qt_xml_refuses_dtd_entities_oversize_and_utf16(self):
        report = self.root / "unsafe.xml"
        for payload in (b'<!DOCTYPE TestCase [<!ENTITY x "value">]><TestCase/>',
                        b'<!ENTITY x SYSTEM "file:///never-read"><TestCase/>',
                        '<!DOCTYPE TestCase><TestCase/>'.encode("utf-16")):
            with self.subTest(payload=payload):
                report.write_bytes(payload)
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._test_xml(report, "Qt")
        report.write_bytes(b"<TestCase>" + b"x" * 101 + b"</TestCase>")
        with patch.object(RUNNER, "MAX_TEST_XML_BYTES", 100):
            with self.assertRaisesRegex(RUNNER.HarnessError, "byte limit"):
                RUNNER._test_xml(report, "Qt")

    def test_qt_xml_declaration_text_in_cdata_is_not_an_actual_declaration(self):
        report = self.root / "description.xml"
        self.qt_report(report, ["one"], {"one": "fail"})
        source = report.read_text(encoding="utf-8").replace('<Duration msecs="0" />',
            '<Description><![CDATA[Literal <!DOCTYPE sample> text]]></Description><Duration msecs="0" />', 1)
        report.write_text(source, encoding="utf-8")
        self.assertEqual(RUNNER._qt_assertion_failures(report, ["one"]), 1)

    def test_qt_xml_requires_location_completion_and_scoped_incidents(self):
        report = self.root / "evidence.xml"
        self.qt_report(report, ["one"], {"one": "fail"})
        original = report.read_text(encoding="utf-8")
        for broken in (original.replace('line="17"', 'line="0"'),
                       original.replace('<Duration msecs="0" />', ''),
                       original.replace('</TestCase>', '<Incident type="skip"/></TestCase>')):
            with self.subTest(broken=broken):
                report.write_text(broken, encoding="utf-8")
                with self.assertRaises(RUNNER.HarnessError):
                    RUNNER._qt_assertion_failures(report, ["one"])

    def test_real_ctest_pass_and_failed_assertion_are_distinct(self):
        self.write_ctest("raise SystemExit(0)")
        self.assertTrue(RUNNER._test_passes(str(self.root), "Release", "^probe$"))
        self.write_ctest("raise SystemExit(1)")
        self.assertFalse(RUNNER._test_passes(str(self.root), "Release", "^probe$"))
        self.assertTrue(any(RUNNER.RUN_LOG_DIR.glob("*.log")))

    def test_real_ctest_timeout_is_error_not_killed(self):
        self.write_ctest("import time\ntime.sleep(30)")
        with patch.object(RUNNER, "TEST_CASE_TIMEOUT_SECONDS", 1):
            with self.assertRaisesRegex(RUNNER.HarnessError, "infrastructure failure"):
                RUNNER._test_passes(str(self.root), "Release", "^probe$")

    def test_real_ctest_missing_test_is_error(self):
        self.write_ctest("raise SystemExit(0)")
        with self.assertRaises(RUNNER.HarnessError):
            RUNNER._test_passes(str(self.root), "Release", "^absent$")

    def test_empty_success_report_is_error(self):
        def empty_result(cmd, timeout_seconds, log_path):
            log_path.with_suffix(".xml").write_text("<testsuite/>", encoding="utf-8")
            return 0

        with patch.object(RUNNER, "_run", side_effect=empty_result):
            with self.assertRaisesRegex(RUNNER.HarnessError, "no matching tests"):
                RUNNER._test_passes(str(self.root), "Release", "^probe$")

    def test_fresh_qt_assertions_are_retained_but_stale_reports_are_not(self):
        report_dir = self.root / "build" / "test_results"
        report_dir.mkdir(parents=True)
        stale = report_dir / "stale.txt"
        stale.write_text("old result", encoding="utf-8")

        def executed_result(cmd, timeout_seconds, log_path):
            (report_dir / "fresh.txt").write_text("FAIL! exact assertion", encoding="utf-8")
            log_path.with_suffix(".xml").write_text(
                '<testsuite><testcase name="fresh"/><testcase name="stale"/></testsuite>',
                encoding="utf-8",
            )
            return 0

        with patch.object(RUNNER, "_run", side_effect=executed_result):
            self.assertTrue(RUNNER._test_passes("build", "Release", "probe"))
        retained = list(RUNNER.RUN_LOG_DIR.glob("*.qttest.txt"))
        self.assertEqual(len(retained), 1)
        self.assertIn("fresh", retained[0].name)
        self.assertEqual(retained[0].read_text(), "FAIL! exact assertion")

    def test_run_supplies_finite_timeout_and_terminates_child_tree(self):
        with patch.object(RUNNER, "_WindowsCommand") as start, \
                patch.object(RUNNER, "_terminate_child_tree") as terminate:
            child = start.return_value
            child.wait.side_effect = subprocess.TimeoutExpired("probe", 5)
            with self.assertRaises(RUNNER.HarnessError):
                RUNNER._run(["probe"], 5, self.root / "probe.log")
            child.wait.assert_called_once_with(timeout=5)
            terminate.assert_called_once()

    def test_failed_tree_kill_cannot_trust_parent_exit(self):
        command = RUNNER._WindowsCommand()
        command.job = 1  # No real handle is used: both job API observations below are injected.
        with patch.object(command, "active_processes", return_value=1), \
                patch.object(command, "pin_active_processes", return_value=[]), \
                patch.object(RUNNER.KERNEL, "TerminateJobObject", return_value=0):
            with self.assertRaisesRegex(RUNNER.HarnessError, "TerminateJobObject failed"):
                RUNNER._terminate_child_tree(command)

    def test_command_timeout_retains_diagnostic_and_cleans_child_tree(self):
        child_pid = self.root / "child.pid"
        child_code = "import time;time.sleep(60)"
        parent_code = (
            "import pathlib,subprocess,sys,time;"
            f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}]);"
            f"pathlib.Path({str(child_pid)!r}).write_text(str(child.pid));"
            "print('bounded-child-started',flush=True);time.sleep(60)"
        )
        log_path = self.root / "timeout.log"
        pinned = []
        with patch.object(RUNNER._WindowsCommand, "wait",
                          self.wait_after_probe_ready(
                              child_pid, lambda command: pinned.append(
                                  self.pin_probe_process(child_pid, command.job)))):
            with self.assertRaisesRegex(RUNNER.HarnessError, "timed out"):
                RUNNER._run([sys.executable, "-c", parent_code], 2, log_path)
        self.assertIn("bounded-child-started", log_path.read_text(encoding="utf-8"))
        self.assertTrue(child_pid.exists())
        self.assertEqual(len(pinned), 1)
        self.assertEqual(RUNNER.KERNEL.WaitForSingleObject(pinned[0], 0), RUNNER.WAIT_SIGNALED,
                         "owned grandchild remained after command timeout")


if __name__ == "__main__":
    unittest.main()
