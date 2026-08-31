"""Executable regression tests for clang-tidy naming-gate health."""

import json
import base64
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
NAMING_GATE = PROJECT_ROOT / "scripts" / "clang_tidy_naming_gate.ps1"
RUN_CLANG_TIDY = PROJECT_ROOT / "scripts" / "run_clang_tidy.ps1"


def powershells() -> list[str]:
    shells = []
    for name in ("pwsh", "powershell"):
        path = shutil.which(name)
        if path is None:
            raise RuntimeError(f"{name} is required for clang-tidy gate regression tests")
        shells.append(path)
    return shells


def run_script(shell: str, script: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [shell, "-NoProfile", "-NonInteractive", "-File", str(script), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )


def make_gate_fixture(directory: str, log_text: str, translation_units: int) -> tuple[Path, Path]:
    root = Path(directory)
    (root / "build-tidy").mkdir(parents=True)
    (root / "build-tidy" / "compile_commands.json").write_text("[]", encoding="utf-8")
    stub = root / "stub_run_clang_tidy.ps1"
    encoded_log = base64.b64encode(log_text.encode("utf-8")).decode("ascii")
    stub.write_text(
        textwrap.dedent(
            f"""
            param(
                [string]$Root,
                [string]$Checks,
                [string]$ExcludeFilter,
                [string]$FirstPartyDir,
                [string]$LogPath
            )
            $target = Join-Path $Root $FirstPartyDir
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            $entries = @()
            foreach ($index in 1..{translation_units}) {{
                if ({translation_units} -gt 0) {{
                    $entries += @{{ directory = $Root; command = 'cl /c sample.cpp';
                        file = (Join-Path $Root "src/sample_$index.cpp") }}
                }}
            }}
            ConvertTo-Json -InputObject $entries | Set-Content -LiteralPath `
                (Join-Path $target 'compile_commands.json') -Encoding utf8
            $logBytes = [Convert]::FromBase64String('{encoded_log}')
            [IO.File]::WriteAllText($LogPath, [Text.Encoding]::UTF8.GetString($logBytes))
            """
        ).lstrip(),
        encoding="ascii",
    )
    return root, stub


def progress_log(count: int, extra: str = "") -> str:
    lines = [
        f"Running clang-tidy in 2 threads for {count} files out of {count} "
        "in compilation database ..."
    ]
    lines.extend(
        f"[{index:3}/{count}] clang-tidy.exe sample_{index}.cpp"
        for index in range(1, count + 1)
    )
    if extra:
        lines.append(extra)
    return "\n".join(lines)


class ClangTidyNamingGateTests(unittest.TestCase):
    def run_gate_case(
        self, shell: str, log_text: str, translation_units: int
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root, stub = make_gate_fixture(directory, log_text, translation_units)
            return run_script(
                shell,
                NAMING_GATE,
                "-Root",
                str(root),
                "-RunClangTidyScript",
                str(stub),
            )

    def test_analysis_database_strips_only_proven_driver_flags(self) -> None:
        for shell in powershells():
            with self.subTest(shell=Path(shell).name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source_dir = root / "src"
                include_dir = root / "include"
                source_dir.mkdir()
                include_dir.mkdir()
                (root / "build-tidy").mkdir()
                entries = [
                    {
                        "directory": str(root),
                        "command": (
                            "cl /GL /MP /Zc:preprocessor /external:anglebrackets "
                            "/permissive- /std:c++latest /c sample.cpp"
                        ),
                        "file": str(source_dir / "sample.cpp"),
                    },
                    {
                        "directory": str(root),
                        "arguments": [
                            "cl",
                            "/GL",
                            "/MP8",
                            "/Zc:preprocessor",
                            "/external:anglebrackets",
                            "/permissive-",
                            "/std:c++latest",
                            "/c",
                            "sample.h",
                        ],
                        "file": str(include_dir / "sample.h"),
                    },
                ]
                (root / "build-tidy" / "compile_commands.json").write_text(
                    json.dumps(entries), encoding="utf-8"
                )
                result = run_script(
                    shell,
                    RUN_CLANG_TIDY,
                    "-Root",
                    str(root),
                    "-PrepareOnly",
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                prepared = json.loads(
                    (root / "build-tidy-fp" / "compile_commands.json").read_text(encoding="utf-8")
                )
                rendered = json.dumps(prepared)
                for forbidden in ("/GL", "/MP", "/Zc:preprocessor", "/external:anglebrackets"):
                    self.assertNotIn(forbidden, rendered)
                self.assertIn("/permissive-", rendered)
                self.assertIn("/std:c++latest", rendered)

    def test_healthy_expected_translation_unit_count_passes(self) -> None:
        for shell in powershells():
            result = self.run_gate_case(shell, progress_log(3), 3)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("NAMING GATE PASSED", result.stdout)

    def test_zero_naming_findings_with_processing_error_fails(self) -> None:
        log = progress_log(1, "Error while processing sample_1.cpp.")
        for shell in powershells():
            result = self.run_gate_case(shell, log, 1)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("processing error", result.stdout.lower())

    def test_compiler_error_fails(self) -> None:
        log = progress_log(1, "sample.cpp:12:3: error: unknown type name 'broken'")
        for shell in powershells():
            result = self.run_gate_case(shell, log, 1)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("processing error", result.stdout.lower())

    def test_incomplete_translation_unit_count_fails(self) -> None:
        log = "\n".join(progress_log(3).splitlines()[:3])
        for shell in powershells():
            result = self.run_gate_case(shell, log, 3)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("completed 2 of 3", result.stdout.lower())

    def test_zero_translation_units_fails(self) -> None:
        for shell in powershells():
            result = self.run_gate_case(shell, progress_log(0), 0)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("zero translation units", result.stdout.lower())

    def test_naming_finding_fails(self) -> None:
        finding = "sample.cpp:4:7: warning: invalid case style for variable 'BadName'"
        for shell in powershells():
            result = self.run_gate_case(shell, progress_log(1, finding), 1)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("1 readability-identifier-naming finding", result.stdout)


if __name__ == "__main__":
    unittest.main()
