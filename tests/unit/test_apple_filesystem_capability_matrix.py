import ast
import json
import re
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
CLAIM_DOCS = (
    Path("docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md"),
    Path("docs/PARTITION_MANAGER_CERTIFICATION.md"),
    Path("docs/RELEASE_READINESS.md"),
)
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")
COMMERCIAL_FIXTURE_FILES = (
    COMMERCIAL_MATRIX,
    Path("src/gui/partition_manager_panel.cpp"),
    Path("src/core/partition_script_builder.cpp"),
    Path("docs/archive/PARTITION_MANAGER_PANEL_PLAN.md"),
    Path("docs/PARTITION_MANAGER_CERTIFICATION.md"),
    Path("README.md"),
    Path("CHANGELOG.md"),
    Path("tests/unit/test_partition_manager_panel.cpp"),
    Path("tests/unit/test_partition_manager_core.cpp"),
)


class AppleFilesystemCapabilityMatrixTests(unittest.TestCase):
    def test_child_gate_launches_inherit_execution_policy(self):
        targets = (
            (Path(__file__), self.__class__.__name__, ("run_gate", "run_commercial_gate")),
            (
                PROJECT_ROOT / "tests/unit/test_active_doc_references.py",
                "ActiveDocReferenceTests",
                ("run_gate",),
            ),
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
        for relative in (MATRIX, *CLAIM_DOCS):
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
        self.assertIn("17 capabilities, 2 track gates, 3 claim documents", result.stdout)

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

    def test_owner_document_status_drift_fails(self):
        path = self.root / CLAIM_DOCS[0]
        text = path.read_text(encoding="utf-8")
        row = next(line for line in text.splitlines() if line.startswith("| H-a |"))
        self.assertIn("CERTIFIED", row)
        path.write_text(text.replace(row, row.replace("CERTIFIED", "BROKEN", 1)), encoding="utf-8")
        self.assertNotEqual(self.run_gate().returncode, 0)

    def test_claim_document_without_matrix_citation_fails(self):
        path = self.root / CLAIM_DOCS[-1]
        text = path.read_text(encoding="utf-8")
        citation = "certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json"
        self.assertIn(citation, text)
        path.write_text(text.replace(citation, "certification/missing.json"), encoding="utf-8")
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

        matrix = json.loads((PROJECT_ROOT / MATRIX).read_text(encoding="utf-8"))
        self.assertNotIn(LIVE_PRODUCTION_AUDIT.as_posix(), matrix["claim_documents"])
        self.assertNotIn(ARCHIVED_PRODUCTION_AUDIT.as_posix(), matrix["claim_documents"])

        for script in (
            Path("scripts/check_partition_manager_release_claims.ps1"),
            Path("scripts/check_partition_manager_commercial_gate_matrix.ps1"),
        ):
            text = (PROJECT_ROOT / script).read_text(encoding="utf-8")
            self.assertNotIn("PRODUCTION_GRADE_AUDIT.md", text)

    def test_commercial_gate_uses_current_release_claims_after_audit_archive(self):
        changelog = self.root / "CHANGELOG.md"
        text = (PROJECT_ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
        changelog.write_text(text.replace("dynamic-to-basic conversion", "removed claim"),
                             encoding="utf-8")
        result = self.run_commercial_gate()
        self.assertNotEqual(result.returncode, 0)
        diagnostic = result.stdout + result.stderr
        self.assertIn("Release-facing certification boundary", diagnostic)
        self.assertIn("dynamic-to-basic", diagnostic)
        self.assertIn("CHANGELOG.md", diagnostic)

    def test_current_commercial_mapping_does_not_require_a_hardware_promotion_claim(self):
        result = self.run_commercial_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("feature groups mapped to", result.stdout)

    def test_commercial_gate_requires_hardware_wipe_scope_after_audit_archive(self):
        certification = self.root / "docs" / "PARTITION_MANAGER_CERTIFICATION.md"
        text = (PROJECT_ROOT / certification.relative_to(self.root)).read_text(encoding="utf-8")
        certification.parent.mkdir(parents=True, exist_ok=True)
        scope = "Non-system hardware wipe on disposable physical disk"
        self.assertEqual(text.count(scope), 1)
        certification.write_text(
            text.replace(scope, "removed hardware scope"),
            encoding="utf-8",
        )
        result = self.run_commercial_gate()
        self.assertNotEqual(result.returncode, 0)
        diagnostic = result.stdout + result.stderr
        self.assertIn("Physical hardware wipe", diagnostic)
        # Both PowerShell hosts wrap error records at different word boundaries.
        # Require every expected word in order, allowing only layout between them.
        self.assertRegex(diagnostic, r"\s+(?:\|\s*)?".join(map(re.escape, scope.split())))
        self.assertIn("PARTITION_MANAGER_CERTIFICATION.md", diagnostic)

    def test_commercial_gate_requires_hardware_wipe_evidence_id_in_document(self):
        certification = self.root / "docs" / "PARTITION_MANAGER_CERTIFICATION.md"
        text = (PROJECT_ROOT / certification.relative_to(self.root)).read_text(encoding="utf-8")
        evidence_id = "external.hardware-wipe"
        self.assertGreater(text.count(evidence_id), 0)
        certification.write_text(text.replace(evidence_id, "removed.hardware-wipe"), encoding="utf-8")
        result = self.run_commercial_gate()
        self.assertNotEqual(result.returncode, 0)
        diagnostic = result.stdout + result.stderr
        self.assertIn("Physical hardware wipe", diagnostic)
        self.assertIn(evidence_id, diagnostic)


if __name__ == "__main__":
    unittest.main()
