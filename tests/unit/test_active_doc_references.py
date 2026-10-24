"""Adversarial tests for tracked active-document local references."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = PROJECT_ROOT / "scripts" / "check_active_doc_references.ps1"
POWERSHELLS = tuple(
    dict.fromkeys(shell for name in ("pwsh", "powershell") if (shell := shutil.which(name)))
)


class ActiveDocReferenceTests(unittest.TestCase):
    def setUp(self):
        if not POWERSHELLS:
            self.fail("PowerShell is required for active-document reference tests")
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        subprocess.run(
            ["git", "-C", str(self.root), "config", "core.autocrlf", "false"], check=True
        )

    def tearDown(self):
        self.temp.cleanup()

    def write(self, relative: str, text: str, *, tracked: bool = True) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        if tracked:
            subprocess.run(["git", "-C", str(self.root), "add", "--", relative], check=True)

    def run_gate(self, powershell: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                powershell,
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

    def assert_failed_with(self, expected: str) -> None:
        for powershell in POWERSHELLS:
            with self.subTest(powershell=powershell):
                result = self.run_gate(powershell)
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn(expected, output)

    def assert_gate_passes(self, expected: str) -> None:
        for powershell in POWERSHELLS:
            with self.subTest(powershell=powershell):
                result = self.run_gate(powershell)
                output = result.stdout + result.stderr
                self.assertEqual(result.returncode, 0, output)
                self.assertIn(expected, result.stdout)

    def test_active_links_may_target_parked_buckets_without_scanning_them(self):
        self.write("docs/archive/DONE.md", "[ignored broken](missing.md)\n")
        self.write("docs/future/PARKED.md", "`docs/MISSING.md`\n")
        self.write(
            "docs/ACTIVE.md",
            "[done](archive/DONE.md) [parked](future/PARKED.md)\n"
            "`docs/archive/DONE.md`\n",
        )
        self.assert_gate_passes("1 documents, 3 local references")

    def test_missing_markdown_target_fails(self):
        self.write("docs/ACTIVE.md", "[missing](MISSING.md)\n")
        self.assert_failed_with("missing local target")

    def test_untracked_existing_target_fails(self):
        self.write("docs/ACTIVE.md", "[draft](DRAFT.md)\n")
        self.write("docs/DRAFT.md", "draft\n", tracked=False)
        self.assert_failed_with("untracked local target")

    def test_case_mismatch_fails_even_on_case_insensitive_filesystems(self):
        self.write("docs/Target.md", "target\n")
        self.write("docs/ACTIVE.md", "[wrong case](target.md)\n")
        self.assert_failed_with("case mismatch")

    def test_repository_escape_fails(self):
        self.write("docs/ACTIVE.md", "[escape](../../outside.md)\n")
        self.assert_failed_with("path escapes repository")

    def test_external_fragment_and_fenced_targets_are_ignored(self):
        self.write("docs/TARGET.md", "target\n")
        self.write(
            "docs/ACTIVE.md",
            "[local](TARGET.md) [web](https://example.com/missing.md) [heading](#part)\n"
            "```markdown\n[ignored](MISSING.md)\n`docs/ALSO_MISSING.md`\n```\n",
        )
        self.assert_gate_passes("1 local references")

    def test_zero_active_documents_fails_closed(self):
        self.write("docs/archive/DONE.md", "[ignored](MISSING.md)\n")
        self.assert_failed_with("zero tracked top-level docs/*.md")

    def test_zero_local_references_fails_closed(self):
        self.write("docs/ACTIVE.md", "[web](https://example.com) [heading](#part)\n")
        self.assert_failed_with("zero local references")

    def test_missing_backticked_repository_path_fails(self):
        self.write("docs/ACTIVE.md", "Gate lives at `docs/MISSING.md`.\n")
        self.assert_failed_with("missing local target")

    def test_docs_prefixed_markdown_link_is_not_reinterpreted_from_root(self):
        self.write("docs/TARGET.md", "target\n")
        self.write("docs/ACTIVE.md", "[wrong relative form](docs/TARGET.md)\n")
        self.assert_failed_with("resolves to 'docs/docs/TARGET.md'")

    def test_reference_definition_target_is_checked(self):
        self.write("docs/ACTIVE.md", "See [missing][ref].\n\n[ref]: MISSING.md\n")
        self.assert_failed_with("missing local target")


if __name__ == "__main__":
    unittest.main()
