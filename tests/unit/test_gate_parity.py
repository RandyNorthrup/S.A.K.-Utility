"""Regression tests for pre-commit-to-CI gate parity."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
GATE = PROJECT_ROOT / "scripts" / "check_gate_parity.ps1"


def run_gate(root: Path) -> subprocess.CompletedProcess[str]:
    pwsh = shutil.which("pwsh")
    if pwsh is None:
        raise RuntimeError("pwsh is required for the gate-parity regression test")
    return subprocess.run(
        [pwsh, "-NoProfile", "-NonInteractive", "-File", str(GATE),
         "-RepositoryRoot", str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


def make_fixture(directory: str) -> Path:
    root = Path(directory)
    (root / ".github" / "workflows").mkdir(parents=True)
    (root / "scripts").mkdir()
    shutil.copy2(PROJECT_ROOT / ".pre-commit-config.yaml", root)
    for workflow in (PROJECT_ROOT / ".github" / "workflows").glob("*.yml"):
        shutil.copy2(workflow, root / ".github" / "workflows")
    for script in ("cppcheck_whole_project_gate.ps1", "check_repository_hygiene.ps1",
                   "verify_signed_release.ps1"):
        shutil.copy2(PROJECT_ROOT / "scripts" / script, root / "scripts")
    return root


class GateParityTests(unittest.TestCase):
    def test_live_gate_matrix_passes(self) -> None:
        result = run_gate(PROJECT_ROOT)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_custom_ci_route_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = make_fixture(directory)
            workflow = root / ".github" / "workflows" / "build-release.yml"
            text = workflow.read_text(encoding="utf-8")
            needle = "& scripts/check_test_registration.ps1"
            self.assertIn(needle, text)
            workflow.write_text(text.replace(needle, "# registration gate removed", 1),
                                encoding="utf-8")
            result = run_gate(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("check_test_registration.ps1", result.stdout + result.stderr)

    def test_missing_builtin_ci_route_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = make_fixture(directory)
            hygiene = root / "scripts" / "check_repository_hygiene.ps1"
            text = hygiene.read_text(encoding="utf-8")
            needle = '    "check-json",\n'
            self.assertIn(needle, text)
            hygiene.write_text(text.replace(needle, "", 1), encoding="utf-8")
            result = run_gate(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("check-json", result.stdout + result.stderr)

    def test_missing_full_history_secret_route_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = make_fixture(directory)
            workflow = root / ".github" / "workflows" / "secret-scan.yml"
            text = workflow.read_text(encoding="utf-8")
            needle = "gitleaks detect"
            self.assertIn(needle, text)
            workflow.write_text(text.replace(needle, "gitleaks disabled", 1), encoding="utf-8")
            result = run_gate(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("gitleaks", (result.stdout + result.stderr).lower())

    def test_missing_local_route_for_ci_gate_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = make_fixture(directory)
            config = root / ".pre-commit-config.yaml"
            text = config.read_text(encoding="utf-8")
            start = text.index("      - id: startup-budget")
            end = text.index("      - id: accessibility-runtime", start)
            config.write_text(text[:start] + text[end:], encoding="utf-8")
            result = run_gate(root)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("check_startup_budget.ps1", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
