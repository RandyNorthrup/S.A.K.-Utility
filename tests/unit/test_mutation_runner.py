"""Fail-closed execution and restoration contracts for the mutation runner."""

import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "mutation_runner", REPO_ROOT / "scripts" / "run_mutation_test.py"
)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class MutationRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name, value in {
            "REPO_ROOT": self.root,
            "SENTINEL_PATH": self.root / ".mutation-in-progress.json",
            "SNAPSHOT_DIR": self.root / ".mutation-snapshot",
            "RUN_LOG_DIR": None,
            "COMMAND_SEQUENCE": 0,
            "UNCONFIRMED_CHILD_PID": None,
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

    def test_assertion_failure_is_killed_and_restores_exact_bytes(self):
        self.assertEqual(self.run_catalog([True, True, True], [True, False]), 0)
        self.assert_restored()

    def test_noncompiling_mutant_is_error_not_coverage(self):
        with self.assertRaisesRegex(RUNNER.HarnessError, "did not compile"):
            self.run_catalog([True, False, True], [True])
        self.assert_restored()

    def test_failed_restoration_build_invalidates_run(self):
        with self.assertRaisesRegex(RUNNER.HarnessError, "Restored-source build failed"):
            self.run_catalog([True, True, False], [True, False])
        self.assertEqual(self.source.read_bytes(), self.original)
        self.assertTrue(RUNNER.SENTINEL_PATH.exists())
        self.assertEqual(RUNNER._recover(), 0)
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
        self.assertEqual(RUNNER._recover(), 0)
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
                patch.object(RUNNER.subprocess, "Popen") as start, \
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
        self.assertEqual(RUNNER._recover(), 2, "blind recovery must refuse unconfirmed children")

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
                patch.object(RUNNER.subprocess, "Popen") as start, \
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
        with patch.object(RUNNER.subprocess, "Popen") as start, \
                patch.object(RUNNER, "_terminate_child_tree") as terminate:
            child = start.return_value
            child.wait.side_effect = subprocess.TimeoutExpired("probe", 5)
            with self.assertRaises(RUNNER.HarnessError):
                RUNNER._run(["probe"], 5, self.root / "probe.log")
            child.wait.assert_called_once_with(timeout=5)
            terminate.assert_called_once()

    def test_failed_tree_kill_cannot_trust_parent_exit(self):
        parent = Mock(pid=12345)
        parent.poll.return_value = 0
        with patch.object(RUNNER.subprocess, "run", return_value=Mock(returncode=1)):
            with self.assertRaisesRegex(RUNNER.HarnessError, "Could not terminate child process tree"):
                RUNNER._terminate_child_tree(parent, io.BytesIO())
        parent.wait.assert_not_called()

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
        with self.assertRaisesRegex(RUNNER.HarnessError, "timed out"):
            RUNNER._run([sys.executable, "-c", parent_code], 2, log_path)
        self.assertIn("bounded-child-started", log_path.read_text(encoding="utf-8"))
        self.assertTrue(child_pid.exists())
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x1000, False, int(child_pid.read_text()))
        if handle:
            try:
                exit_code = ctypes.c_ulong()
                self.assertTrue(kernel.GetExitCodeProcess(handle, ctypes.byref(exit_code)))
                self.assertNotEqual(exit_code.value, 259, "child remains STILL_ACTIVE")
            finally:
                kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main()
