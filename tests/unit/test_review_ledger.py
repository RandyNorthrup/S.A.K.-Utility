"""Keep every first-party source language visible in the R5 review denominator."""

import importlib.util
from pathlib import Path
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("review_ledger", REPO_ROOT / "scripts/review_ledger.py")
LEDGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LEDGER)


class ReviewLedgerTests(unittest.TestCase):
    def test_first_party_c_shim_is_reviewable(self):
        self.assertEqual(LEDGER.classify("src/core/sak_lzvn_shim.c"), "src")

    def test_python_test_guards_are_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/unit/test_gate_parity.py"), "tests")
        self.assertEqual(LEDGER.classify("tests/unit/test_mutation_runner.py"), "tests")

    def test_powershell_tests_are_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/unit/test_pack_extension.ps1"), "tests")

    def test_windows_launchers_are_reviewable(self):
        self.assertEqual(LEDGER.classify("scripts/launch_partition_manager_vm_gate.cmd"), "scripts")
        self.assertEqual(LEDGER.classify("scripts/launch.bat"), "scripts")

    def test_c_fixture_is_reviewable(self):
        self.assertEqual(LEDGER.classify("tests/fixtures/cppcheck_c_language_probe.c"), "tests")

    def test_existing_languages_remain_reviewable(self):
        paths = {
            "include/sak/file_hash.h": "include", "include/sak/types.hpp": "include",
            "src/core/file_hash.cpp": "src", "tests/unit/test_file_hash.cpp": "tests",
            "tests/unit/test_browser.mjs": "tests", "scripts/run_mutation_test.py": "scripts",
            "scripts/check_doc_accuracy.ps1": "scripts", "browser/extension/background.js": "browser",
        }
        for path, group in paths.items():
            with self.subTest(path=path):
                self.assertEqual(LEDGER.classify(path), group)

    def test_resources_and_generated_output_are_not_source_units(self):
        for path in ("docs/future/PLAN.md", "build/generated.cpp", "temp/probe.py",
                     "resources/workflow.json", "third_party/vendor.c", "tests/README.md",
                     "src_snapshot/old.cpp"):
            with self.subTest(path=path):
                self.assertIsNone(LEDGER.classify(path))


if __name__ == "__main__":
    unittest.main()
