#!/usr/bin/env python3
"""Executable contract for exact Qt-slot cppcheck classification."""

from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CLASSIFIER_PATH = PROJECT_ROOT / "scripts" / "classify_cppcheck_qt_slots.py"
SPEC = importlib.util.spec_from_file_location("cppcheck_qt_slots", CLASSIFIER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load classifier: {CLASSIFIER_PATH}")
CLASSIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLASSIFIER)
RUNNER_PATH = PROJECT_ROOT / "scripts" / "run_cppcheck.ps1"


def finding(path: Path, line: int, member: str) -> str:
    return (
        f"{path}:{line}:10: style: Either there is a missing 'override', or the member "
        f"function '{member}' can be static. [functionStatic]\n"
    )


def line_of(source: str, needle: str) -> int:
    return next(
        index for index, line in enumerate(source.splitlines(), start=1) if needle in line
    )


class QtSlotClassifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "tests" / "unit").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_test_source(self, name: str, source: str) -> Path:
        path = self.root / "tests" / "unit" / name
        path.write_text(source, encoding="utf-8")
        return path

    def classify_one(self, diagnostic: str) -> tuple[list[str], int, int]:
        return CLASSIFIER.classify([diagnostic], self.root)

    def cppcheck_input_scope(self, *paths: Path) -> tuple[str, list[str]]:
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        if powershell is None:
            self.skipTest("Windows PowerShell is required for the cppcheck scope contract")
        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(RUNNER_PATH),
                "-PrintInputScope",
                *(str(path) for path in paths),
            ],
            cwd=PROJECT_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        scope_lines = [
            line.removeprefix("CPPCHECK_INPUT_SCOPE=")
            for line in result.stdout.splitlines()
            if line.startswith("CPPCHECK_INPUT_SCOPE=")
        ]
        self.assertEqual(len(scope_lines), 1, result.stdout + result.stderr)
        input_paths = [
            line.removeprefix("CPPCHECK_INPUT_PATH=")
            for line in result.stdout.splitlines()
            if line.startswith("CPPCHECK_INPUT_PATH=")
        ]
        return scope_lines[0], input_paths

    def test_new_out_of_class_qt_slot_is_classified_from_live_source(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
private:
    void helper();
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_example.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::cleanup"), "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [])
        self.assertEqual(classified, 1)
        self.assertEqual(remaining, 0)

    def test_namespaced_inline_slot_survives_literals_and_digit_separators(self) -> None:
        source = '''const char* noise = R"tag({ not code })tag";
namespace demo {
class NamespacedTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() { const int timeout = 600'000; }
};
}
'''
        path = self.write_test_source("test_namespaced.cpp", source)
        diagnostic = finding(
            path,
            line_of(source, "void cleanup"),
            "demo::NamespacedTest::cleanup",
        )
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [])
        self.assertEqual(classified, 1)
        self.assertEqual(remaining, 0)

    def test_ordinary_helper_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testCase();
private:
    int helper();
};

int ExampleTest::helper() { return 1; }
"""
        path = self.write_test_source("test_helper.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::helper"), "ExampleTest::helper")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_stale_definition_line_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_stale.cpp", source)
        stale_line = line_of(source, "ExampleTest::cleanup") - 1
        diagnostic = finding(path, stale_line, "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_ambiguous_duplicate_slot_declaration_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
    void cleanup(int value);
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_duplicate.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::cleanup"), "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_non_test_path_remains_actionable(self) -> None:
        source = """class Product : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void callback();
};
void Product::callback() {}
"""
        product_dir = self.root / "src"
        product_dir.mkdir()
        path = product_dir / "product.cpp"
        path.write_text(source, encoding="utf-8")
        diagnostic = finding(path, line_of(source, "Product::callback"), "Product::callback")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_repository_has_no_broad_function_static_silence(self) -> None:
        suppressions = (PROJECT_ROOT / "cppcheck_suppressions.txt").read_text(encoding="ascii")
        runner = (PROJECT_ROOT / "scripts" / "run_cppcheck.ps1").read_text(encoding="ascii")
        self.assertNotIn("functionStatic:*tests*", suppressions)
        self.assertIn("classify_cppcheck_qt_slots.py", runner)
        self.assertIn("Invoke-CapturedProcess", runner)
        self.assertNotIn("Start-Process -FilePath $CppcheckExe", runner)

    def test_cppcheck_resolves_qr_header_used_by_first_party_code(self) -> None:
        runner = RUNNER_PATH.read_text(encoding="ascii")
        root = "src/third_party/qrcodegen"
        self.assertTrue((PROJECT_ROOT / root / "qrcodegen.hpp").is_file())
        self.assertIn(f'"-I", (Join-Path $ProjectRoot "{root}")', runner)

    def test_cppcheck_reveal_resolves_qr_header_in_live_translation_unit(self) -> None:
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        self.assertIsNotNone(powershell, "Windows PowerShell is required for cppcheck")
        suppression_lines = (PROJECT_ROOT / "cppcheck_suppressions.txt").read_text(
            encoding="ascii"
        ).splitlines()
        suppressions_path = self.root / "qr_reveal_suppressions.txt"
        suppressions_path.write_text(
            "\n".join(line for line in suppression_lines if line.strip() != "missingInclude")
            + "\n",
            encoding="ascii",
        )
        errors_path = self.root / "qr_reveal_errors.txt"
        source = PROJECT_ROOT / "src" / "gui" / "wifi_manager_panel.cpp"
        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-NonInteractive",
                "-File",
                str(RUNNER_PATH),
                "-SuppressionsFile",
                str(suppressions_path),
                "-ErrorsFile",
                str(errors_path),
                "-SuppressFindingOutput",
                "-Files",
                str(source),
            ],
            cwd=PROJECT_ROOT,
            check=False,
            capture_output=True,
            text=True,
            timeout=45,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        findings = errors_path.read_text(encoding="utf-8", errors="replace")
        self.assertIn('Include file: "wifi_manager_panel.moc" not found.', findings)
        self.assertNotIn('Include file: "qrcodegen.hpp" not found.', findings)
        self.assertIn("Active checkers:", findings)

    def test_classified_diagnostics_fail_closed_on_read_or_tool_failure(self) -> None:
        runner = RUNNER_PATH.read_text(encoding="ascii")
        self.assertIn(
            "$errors = @(Get-Content -LiteralPath $ErrorsFile -ErrorAction Stop)",
            runner,
        )
        self.assertNotIn(
            "Get-Content -LiteralPath $ErrorsFile -ErrorAction SilentlyContinue",
            runner,
        )
        self.assertIn(
            "$hasCheckersReport -and $rawSignals.Count -eq $classifiedQtSlots",
            runner,
        )
        self.assertIn("$exitCode -eq 1 -and $remainingSignals.Count -eq 0", runner)

    def test_default_whole_project_scope_includes_test_translation_units(self) -> None:
        scope, paths = self.cppcheck_input_scope()
        self.assertEqual(scope, "whole-project-with-tests")
        self.assertEqual(
            {Path(path).resolve() for path in paths},
            {
                (PROJECT_ROOT / "src").resolve(),
                (PROJECT_ROOT / "include").resolve(),
                (PROJECT_ROOT / "tests").resolve(),
            },
        )

    def test_changed_source_keeps_fast_exact_file_scope(self) -> None:
        source = PROJECT_ROOT / "src" / "core" / "mbox_writer.cpp"
        scope, paths = self.cppcheck_input_scope(source)
        self.assertEqual(scope, "file-list")
        self.assertEqual(paths, [str(source)])

    def test_relative_source_resolves_against_powershell_location(self) -> None:
        # Set-Location moves PowerShell's location but not the process working directory, and
        # cppcheck (a child process) resolves a relative path against the latter. The runner
        # must hand cppcheck the file the caller named, so a relative path has to arrive
        # absolute, resolved from the location. The process directory here is a temp folder.
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        self.assertIsNotNone(powershell, "Windows PowerShell is required for the scope contract")
        relative = "src/core/mbox_writer.cpp"
        root_literal = str(PROJECT_ROOT).replace("'", "''")
        runner_literal = str(RUNNER_PATH).replace("'", "''")
        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                f"Set-Location -LiteralPath '{root_literal}'; "
                f"& '{runner_literal}' -PrintInputScope '{relative}'; exit $LASTEXITCODE",
            ],
            cwd=self.root,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        paths = [
            line.removeprefix("CPPCHECK_INPUT_PATH=")
            for line in result.stdout.splitlines()
            if line.startswith("CPPCHECK_INPUT_PATH=")
        ]
        self.assertEqual(len(paths), 1, result.stdout + result.stderr)
        self.assertTrue(Path(paths[0]).is_absolute(), paths[0])
        self.assertEqual(
            os.path.normcase(paths[0]),
            os.path.normcase(str(PROJECT_ROOT / "src" / "core" / "mbox_writer.cpp")),
        )

    def test_changed_header_expands_to_real_translation_units(self) -> None:
        header = PROJECT_ROOT / "include" / "sak" / "ai_assistant_panel.h"
        scope, paths = self.cppcheck_input_scope(header)
        self.assertEqual(scope, "whole-project-with-tests")
        self.assertEqual(
            {Path(path).resolve() for path in paths},
            {
                (PROJECT_ROOT / "src").resolve(),
                (PROJECT_ROOT / "include").resolve(),
                (PROJECT_ROOT / "tests").resolve(),
            },
        )

    def test_one_header_forces_safe_scope_for_mixed_batch(self) -> None:
        header = PROJECT_ROOT / "include" / "sak" / "per_user_customization_dialog.h"
        source = PROJECT_ROOT / "src" / "gui" / "per_user_customization_dialog.cpp"
        scope, paths = self.cppcheck_input_scope(source, header)
        self.assertEqual(scope, "whole-project-with-tests")
        resolved_paths = {str(Path(path).resolve()) for path in paths}
        self.assertIn(str((PROJECT_ROOT / "tests").resolve()), resolved_paths)

    def test_header_outside_first_party_roots_fails_closed(self) -> None:
        outside_header = PROJECT_ROOT / "temp" / "cppcheck_scope_outside.h"
        outside_header.parent.mkdir(exist_ok=True)
        outside_header.write_text("struct Outside { int value; };\n", encoding="ascii")
        try:
            powershell = shutil.which("powershell.exe") or shutil.which("powershell")
            if powershell is None:
                self.skipTest("Windows PowerShell is required for the cppcheck scope contract")
            result = subprocess.run(
                [
                    powershell,
                    "-NoProfile",
                    "-NonInteractive",
                    "-File",
                    str(RUNNER_PATH),
                    "-PrintInputScope",
                    str(outside_header),
                ],
                cwd=PROJECT_ROOT,
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("outside the first-party cppcheck scope", result.stdout + result.stderr)
        finally:
            outside_header.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
