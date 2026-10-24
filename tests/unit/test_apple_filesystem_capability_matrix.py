import ast
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = PROJECT_ROOT / "scripts" / "check_apple_filesystem_capability_matrix.ps1"
COMMERCIAL_SCRIPT = PROJECT_ROOT / "scripts" / "check_partition_manager_commercial_gate_matrix.ps1"
MATRIX = Path("certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json")
COMMERCIAL_MATRIX = Path("certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json")
LIVE_PRODUCTION_AUDIT = Path("docs/PRODUCTION_GRADE_AUDIT.md")
ARCHIVED_PRODUCTION_AUDIT = Path("docs/archive/PRODUCTION_GRADE_AUDIT.md")
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")
# Code, UI, tests and the matrix only. No documentation is copied: both gates must pass in a
# fixture root that holds no prose, which is the proof that neither reads it (owner ruling
# 2026-10-24 retired the doc-citation and doc-wording checks).
COMMERCIAL_FIXTURE_FILES = (
    COMMERCIAL_MATRIX,
    Path("src/gui/partition_manager_panel.cpp"),
    Path("src/core/partition_script_builder.cpp"),
    Path("src/core/partition_safety_validator.cpp"),
    Path("tests/unit/test_partition_manager_panel.cpp"),
    Path("tests/unit/test_partition_manager_core.cpp"),
)


class AppleFilesystemCapabilityMatrixTests(unittest.TestCase):
    def test_child_gate_launches_inherit_execution_policy(self):
        targets = (
            (Path(__file__), self.__class__.__name__, ("run_gate", "run_commercial_gate")),
            (
                PROJECT_ROOT / "tests/unit/test_cppcheck_qt_slot_classifier.py",
                "QtSlotClassifierTests",
                ("cppcheck_input_scope", "test_header_outside_first_party_roots_fails_closed"),
            ),
        )
        for path, class_name, methods in targets:
            tree = ast.parse(path.read_text(encoding="utf-8"))
            test_class = next(
                node
                for node in tree.body
                if isinstance(node, ast.ClassDef) and node.name == class_name
            )
            for name in methods:
                method = next(
                    node
                    for node in test_class.body
                    if isinstance(node, ast.FunctionDef) and node.name == name
                )
                literals = {
                    node.value
                    for node in ast.walk(method)
                    if isinstance(node, ast.Constant) and isinstance(node.value, str)
                }
                self.assertNotIn("-ExecutionPolicy", literals, f"{path.name}:{name}")
                self.assertNotIn("Bypass", literals, f"{path.name}:{name}")

    def setUp(self):
        if POWERSHELL is None:
            self.fail("PowerShell is required for the Apple capability matrix test")
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for relative in (MATRIX,):
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(PROJECT_ROOT / relative, destination)

    def tearDown(self):
        self.temp.cleanup()

    def run_gate(self):
        return subprocess.run(
            [
                POWERSHELL,
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(SCRIPT),
                "-ProjectRoot",
                str(self.root),
            ],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def run_commercial_gate(self):
        for relative in COMMERCIAL_FIXTURE_FILES:
            destination = self.root / relative
            if destination.exists():
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(PROJECT_ROOT / relative, destination)
        return subprocess.run(
            [
                POWERSHELL,
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(COMMERCIAL_SCRIPT),
                "-ProjectRoot",
                str(self.root),
            ],
            cwd=PROJECT_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

    def read_matrix(self):
        return json.loads((self.root / MATRIX).read_text(encoding="utf-8"))

    def write_matrix(self, matrix):
        (self.root / MATRIX).write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")

    def test_live_shape_passes(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("17 capabilities, 2 track gates.", result.stdout)

    def test_missing_capability_fails(self):
        matrix = self.read_matrix()
        matrix["capabilities"] = [row for row in matrix["capabilities"] if row["id"] != "A-a"]
        self.write_matrix(matrix)
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_certified_row_without_evidence_fails(self):
        matrix = self.read_matrix()
        next(row for row in matrix["capabilities"] if row["id"] == "H-a")["evidence_refs"] = []
        self.write_matrix(matrix)
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_fusion_cannot_be_flipped_to_certified_without_scope_change(self):
        matrix = self.read_matrix()
        fusion = next(row for row in matrix["capabilities"] if row["id"] == "A-i")
        fusion["status"] = "certified"
        fusion["certified_on"] = "2026-08-31"
        fusion["evidence_refs"] = [{"type": "unit_test", "id": "forged"}]
        self.write_matrix(matrix)
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_historical_audit_is_archived_and_not_a_live_gate_dependency(self):
        self.assertFalse((PROJECT_ROOT / LIVE_PRODUCTION_AUDIT).exists())
        self.assertTrue((PROJECT_ROOT / ARCHIVED_PRODUCTION_AUDIT).is_file())

        for script in (
            Path("scripts/check_partition_manager_release_claims.ps1"),
            Path("scripts/check_partition_manager_commercial_gate_matrix.ps1"),
        ):
            text = (PROJECT_ROOT / script).read_text(encoding="utf-8")
            self.assertNotIn("PRODUCTION_GRADE_AUDIT.md", text)

    def test_current_commercial_mapping_does_not_require_a_hardware_promotion_claim(self):
        result = self.run_commercial_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("feature groups mapped to", result.stdout)

    def test_commercial_gate_requires_hardware_wipe_execution_code(self):
        self.run_commercial_gate()  # stage the fixture copies
        builder = self.root / "src" / "core" / "partition_script_builder.cpp"
        text = builder.read_text(encoding="utf-8")
        pattern = "Clear-Disk -Number %1 -RemoveData -RemoveOEM"
        self.assertIn(pattern, text)
        builder.write_text(text.replace(pattern, "removed wipe engine"), encoding="utf-8")
        result = self.run_commercial_gate()
        self.assertNotEqual(result.returncode, 0)
        diagnostic = result.stdout + result.stderr
        self.assertIn("Physical hardware wipe", diagnostic)
        self.assertIn("partition_script_builder.cpp", diagnostic)

    def test_commercial_gate_requires_removable_disk_guard(self):
        self.run_commercial_gate()
        validator = self.root / "src" / "core" / "partition_safety_validator.cpp"
        text = validator.read_text(encoding="utf-8")
        self.assertIn("disk.is_removable", text)
        validator.write_text(text.replace("disk.is_removable", "disk.removed_guard"), encoding="utf-8")
        result = self.run_commercial_gate()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("USB removable destructive proof", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
