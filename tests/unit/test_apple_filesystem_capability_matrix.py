import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = PROJECT_ROOT / "scripts" / "check_apple_filesystem_capability_matrix.ps1"
MATRIX = Path("certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json")
CLAIM_DOCS = (
    Path("docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md"),
    Path("docs/PARTITION_MANAGER_CERTIFICATION.md"),
    Path("docs/PRODUCTION_GRADE_AUDIT.md"),
    Path("docs/RELEASE_READINESS.md"),
)
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")


class AppleFilesystemCapabilityMatrixTests(unittest.TestCase):
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
                "-ExecutionPolicy",
                "Bypass",
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

    def read_matrix(self):
        return json.loads((self.root / MATRIX).read_text(encoding="utf-8"))

    def write_matrix(self, matrix):
        (self.root / MATRIX).write_text(json.dumps(matrix, indent=2) + "\n", encoding="utf-8")

    def test_live_shape_passes(self):
        result = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("17 capabilities", result.stdout)

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


if __name__ == "__main__":
    unittest.main()
