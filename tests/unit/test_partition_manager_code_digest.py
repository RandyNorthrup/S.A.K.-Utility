"""The code digest evidence is bound to: what it covers, and what it deliberately does not."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "partition_manager_code_digest.py"
SPEC = importlib.util.spec_from_file_location("partition_manager_code_digest", SCRIPT)
DIGEST = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = DIGEST
SPEC.loader.exec_module(DIGEST)


def git(root: Path, *arguments: str) -> None:
    subprocess.run(
        ["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.invalid",
         "-c", "core.autocrlf=false", *arguments],
        check=True,
        capture_output=True,
    )


class CodeDigestTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        git(self.root, "init")
        git(self.root, "config", "core.autocrlf", "false")
        for relative, text in {
            "src/core/a.cpp": b"int a;\nint b;\n",
            "src/elevated/helper.cpp": b"int helper;\n",
            "src/gui/partition_manager_panel.cpp": b"int panel;\n",
            "tests/unit/t.cpp": b"int t;\n",
            "scripts/s.ps1": b"exit 0\n",
            "CMakeLists.txt": b"project(x)\n",
            "VERSION": b"1.2.3\n",
            "docs/d.md": b"words\n",
            "docs/d.txt": b"words\n",
            "README.md": b"words\n",
            "artifacts/e.json": b"{}\n",
            ".github/workflows/ci.yml": b"on: push\n",
            ".gitignore": b"*.tmp\n",
        }.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text)
        git(self.root, "add", "-A")
        git(self.root, "commit", "-m", "start")

    def digest(self) -> str:
        return DIGEST.describe(self.root)["code_digest"]

    def test_shipped_code_changes_the_digest(self) -> None:
        for relative in ("src/core/a.cpp", "src/elevated/helper.cpp",
                         "src/gui/partition_manager_panel.cpp", "CMakeLists.txt", "VERSION",
                         "scripts/s.ps1", "tests/unit/t.cpp"):
            before = self.digest()
            path = self.root / relative
            path.write_bytes(path.read_bytes() + b"// changed\n")
            self.assertNotEqual(self.digest(), before, relative)

    def test_docs_and_evidence_do_not_change_the_digest(self) -> None:
        before = self.digest()
        for relative in ("docs/d.md", "docs/d.txt", "README.md", "artifacts/e.json",
                         ".github/workflows/ci.yml", ".gitignore"):
            path = self.root / relative
            path.write_bytes(path.read_bytes() + b"more\n")
        self.assertEqual(self.digest(), before)

    def test_line_endings_do_not_change_the_digest(self) -> None:
        before = self.digest()
        path = self.root / "src/core/a.cpp"
        path.write_bytes(path.read_bytes().replace(b"\n", b"\r\n"))
        self.assertEqual(self.digest(), before)

    def test_a_clone_with_other_line_endings_gets_the_same_digest(self) -> None:
        for setting in ("true", "false"):
            with tempfile.TemporaryDirectory() as other:
                clone = Path(other) / "clone"
                subprocess.run(
                    ["git", "clone", "-q", "-c", f"core.autocrlf={setting}", str(self.root), str(clone)],
                    check=True,
                    capture_output=True,
                )
                self.assertEqual(DIGEST.describe(clone)["code_digest"], self.digest(), setting)

    def test_a_renamed_file_changes_the_digest(self) -> None:
        before = self.digest()
        git(self.root, "mv", "src/core/a.cpp", "src/core/b.cpp")
        self.assertNotEqual(self.digest(), before)

    def test_an_added_tracked_file_changes_the_digest(self) -> None:
        before = self.digest()
        (self.root / "src/core/new.cpp").write_bytes(b"int n;\n")
        self.assertEqual(self.digest(), before, "an untracked file is not in the build's sources")
        git(self.root, "add", "src/core/new.cpp")
        self.assertNotEqual(self.digest(), before)

    def test_it_says_when_code_is_uncommitted(self) -> None:
        self.assertFalse(DIGEST.describe(self.root)["code_dirty"])
        (self.root / "docs/d.md").write_bytes(b"changed\n")
        self.assertFalse(DIGEST.describe(self.root)["code_dirty"], "docs are not code")
        (self.root / "src/core/a.cpp").write_bytes(b"int c;\n")
        self.assertTrue(DIGEST.describe(self.root)["code_dirty"])

    def test_a_missing_tracked_file_stops_it(self) -> None:
        (self.root / "src/core/a.cpp").unlink()
        with self.assertRaises(DIGEST.DigestError):
            DIGEST.describe(self.root)

    def test_a_folder_that_is_not_a_repository_stops_it(self) -> None:
        with tempfile.TemporaryDirectory() as other:
            with self.assertRaises(DIGEST.DigestError):
                DIGEST.describe(Path(other))

    def test_the_header_names_all_three(self) -> None:
        header = DIGEST.header_text(DIGEST.describe(self.root))
        self.assertIn("#define SAK_CODE_DIGEST", header)
        self.assertIn("#define SAK_CODE_COMMIT", header)
        self.assertIn("#define SAK_CODE_DIRTY false", header)

    def test_an_unchanged_header_keeps_its_timestamp(self) -> None:
        path = self.root / "out" / "h.h"
        DIGEST.write_if_changed(path, "one")
        first = path.stat().st_mtime_ns
        DIGEST.write_if_changed(path, "one")
        self.assertEqual(path.stat().st_mtime_ns, first)
        DIGEST.write_if_changed(path, "two")
        self.assertEqual(path.read_text(encoding="utf-8"), "two")


if __name__ == "__main__":
    unittest.main()
