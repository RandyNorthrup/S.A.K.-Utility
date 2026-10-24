"""Executable document-accuracy diagnostics on native Windows PowerShell hosts."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
GATE = PROJECT_ROOT / "scripts" / "check_doc_accuracy.ps1"


class DocumentAccuracyTests(unittest.TestCase):
    def setUp(self):
        self.shells = []
        for name in ("powershell", "pwsh"):
            shell = shutil.which(name)
            self.assertIsNotNone(shell, f"{name} is required for this Windows gate")
            self.shells.append(shell)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for directory in ("scripts", "tests/unit", "docs"):
            self.root.joinpath(directory).mkdir(parents=True)
        shutil.copyfile(GATE, self.root / "scripts" / GATE.name)
        self.root.joinpath("tests/unit/test_probe.cpp").write_text(
            "// Directory-inventory fixture; never compiled.\n", encoding="ascii"
        )
        self.root.joinpath("tests/CMakeLists.txt").write_text(
            "add_test(NAME test_probe COMMAND probe)\n", encoding="ascii"
        )
        self.write_readme()
        self.write_remediation()

    def write_readme(self, count=1):
        self.root.joinpath("tests/README.md").write_text(
            f"{count} registered CTest tests across 1 C++ test source files.\n"
            "Unit tests (1 C++ files, including actions/).\n"
            "End-to-end workflow tests (0 files).\n"
            "```\n+-- test_probe.cpp\n```\n"
            "| Test | Description |\n| test_probe | Fixture |\n"
            "**Registered Tests:** 1\n",
            encoding="ascii",
        )

    def write_remediation(self, done=1):
        self.root.joinpath("docs/CODEX_REVIEW_5_REMEDIATION.md").write_text(
            f"Tally: {done} [x] / 0 [~] / 0 [ ]\n- [x] Completed fixture\n",
            encoding="ascii",
        )

    def run_gate(self, shell):
        return subprocess.run(
            [shell, "-NoProfile", "-NonInteractive", "-File",
             str(self.root / "scripts" / GATE.name)],
            capture_output=True, text=True, check=False, timeout=30,
        )

    def assert_failure_documents(self, documents, violation):
        for shell in self.shells:
            with self.subTest(shell=shell):
                result = self.run_gate(shell)
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, 1, output)
                self.assertIn(violation, output)
                self.assertIn(f"Fix {documents} to match; do not weaken this gate.", output)

    def test_matching_documents_pass(self):
        for shell in self.shells:
            with self.subTest(shell=shell):
                result = self.run_gate(shell)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("Doc-accuracy gate passed", result.stdout)

    def test_remediation_only_failure_names_remediation(self):
        self.write_remediation(done=0)
        self.assert_failure_documents(
            "docs/CODEX_REVIEW_5_REMEDIATION.md", "tally claims 0 [x], file has 1"
        )

    def test_readme_only_failure_names_readme(self):
        self.write_readme(count=2)
        self.assert_failure_documents("tests/README.md", "README says 2, reality is 1")

    def test_both_failures_name_both_documents(self):
        self.write_readme(count=2)
        self.write_remediation(done=0)
        self.assert_failure_documents(
            "tests/README.md, docs/CODEX_REVIEW_5_REMEDIATION.md", "tally claims 0 [x]"
        )


if __name__ == "__main__":
    unittest.main()
