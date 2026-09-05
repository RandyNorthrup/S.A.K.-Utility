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


def make_gate_fixture(
    directory: str,
    log_text: str,
    translation_units: int,
    core_log_text: str | None = None,
    core_translation_units: int = 1,
    manifest_entries: list[str] | None = None,
    create_manifest_sources: bool = True,
) -> tuple[Path, Path, Path]:
    root = Path(directory)
    (root / "build-tidy").mkdir(parents=True)
    (root / "build-tidy" / "compile_commands.json").write_text("[]", encoding="utf-8")
    stub = root / "stub_run_clang_tidy.ps1"
    if core_log_text is None:
        core_log_text = progress_log(core_translation_units)
    if manifest_entries is None:
        manifest_entries = ["src/core/core_sample_1.cpp"]
    manifest = root / "core-clean.txt"
    manifest.write_text("\n".join(manifest_entries) + ("\n" if manifest_entries else ""), encoding="ascii")
    if create_manifest_sources:
        for entry in manifest_entries:
            source = root / entry
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("// fixture\n", encoding="ascii")
    encoded_log = base64.b64encode(log_text.encode("utf-8")).decode("ascii")
    encoded_core_log = base64.b64encode(core_log_text.encode("utf-8")).decode("ascii")
    stub.write_text(
        textwrap.dedent(
            f"""
            param(
                [string]$Root,
                [string]$Checks,
                [string]$ExcludeFilter,
                [string]$IncludeFilter,
                [string]$ConfigFile,
                [string]$FirstPartyDir,
                [string]$LogPath
            )
            $isCore = -not [string]::IsNullOrWhiteSpace($IncludeFilter)
            $count = if ($isCore) {{ {core_translation_units} }} else {{ {translation_units} }}
            $target = Join-Path $Root $FirstPartyDir
            New-Item -ItemType Directory -Force -Path $target | Out-Null
            $entries = @()
            foreach ($index in 1..$count) {{
                if ($count -gt 0) {{
                    $relative = if ($isCore) {{ "src/core/core_sample_$index.cpp" }} `
                        else {{ "src/gui/sample_$index.cpp" }}
                    $entries += @{{ directory = $Root; command = 'cl /c sample.cpp';
                        file = (Join-Path $Root $relative) }}
                }}
            }}
            ConvertTo-Json -InputObject $entries | Set-Content -LiteralPath `
                (Join-Path $target 'compile_commands.json') -Encoding utf8
            $encoded = if ($isCore) {{ '{encoded_core_log}' }} else {{ '{encoded_log}' }}
            $logBytes = [Convert]::FromBase64String($encoded)
            $logText = [Text.Encoding]::UTF8.GetString($logBytes).Replace('__ROOT__', $Root)
            [IO.File]::WriteAllText($LogPath, $logText)
            """
        ).lstrip(),
        encoding="ascii",
    )
    return root, stub, manifest


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
        self,
        shell: str,
        log_text: str,
        translation_units: int,
        *,
        core_log_text: str | None = None,
        core_translation_units: int = 1,
        manifest_entries: list[str] | None = None,
        create_manifest_sources: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root, stub, manifest = make_gate_fixture(
                directory,
                log_text,
                translation_units,
                core_log_text,
                core_translation_units,
                manifest_entries,
                create_manifest_sources,
            )
            return run_script(
                shell,
                NAMING_GATE,
                "-Root",
                str(root),
                "-RunClangTidyScript",
                str(stub),
                "-CoreManifestPath",
                str(manifest),
            )

    def test_analysis_database_strips_only_proven_driver_flags(self) -> None:
        for shell in powershells():
            with self.subTest(shell=Path(shell).name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                core_dir = root / "src" / "core"
                gui_dir = root / "src" / "gui"
                include_dir = root / "include"
                core_dir.mkdir(parents=True)
                gui_dir.mkdir(parents=True)
                include_dir.mkdir()
                (core_dir / "core_sample.cpp").write_text("int core_sample = 0;\n", encoding="ascii")
                (gui_dir / "gui_sample.cpp").write_text("int gui_sample = 0;\n", encoding="ascii")
                (include_dir / "sample.h").write_text("#pragma once\n", encoding="ascii")
                (root / "build-tidy").mkdir()
                entries = [
                    {
                        "directory": str(root),
                        "command": (
                            "cl /GL /MP /Zc:preprocessor /external:anglebrackets "
                            "/permissive- /std:c++latest /c sample.cpp"
                        ),
                        "file": str(core_dir / "core_sample.cpp"),
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
                        "file": str(gui_dir / "gui_sample.cpp"),
                    },
                    {
                        "directory": str(root),
                        "command": "cl /GL /permissive- /std:c++latest /c sample.h",
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

                core_result = run_script(
                    shell,
                    RUN_CLANG_TIDY,
                    "-Root",
                    str(root),
                    "-PrepareOnly",
                    "-FirstPartyDir",
                    "build-tidy-core",
                    "-IncludeFilter",
                    "/src/core/",
                )
                self.assertEqual(core_result.returncode, 0, core_result.stdout + core_result.stderr)
                core_entries = json.loads(
                    (root / "build-tidy-core" / "compile_commands.json").read_text(encoding="utf-8")
                )
                self.assertIsInstance(core_entries, list)
                self.assertEqual(len(core_entries), 1)
                self.assertIn("/src/core/", core_entries[0]["file"].replace("\\", "/"))

    def test_analysis_database_prunes_deleted_source_entries(self) -> None:
        for shell in powershells():
            with self.subTest(shell=Path(shell).name), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source_dir = root / "src" / "gui"
                source_dir.mkdir(parents=True)
                live = source_dir / "live.cpp"
                missing = source_dir / "removed.cpp"
                live.write_text("int live = 0;\n", encoding="ascii")
                (root / "build-tidy").mkdir()
                entries = [
                    {"directory": str(root), "command": "cl /c live.cpp", "file": str(live)},
                    {
                        "directory": str(root),
                        "command": "cl /c removed.cpp",
                        "file": str(missing),
                    },
                ]
                (root / "build-tidy" / "compile_commands.json").write_text(
                    json.dumps(entries), encoding="utf-8"
                )

                result = run_script(shell, RUN_CLANG_TIDY, "-Root", str(root), "-PrepareOnly")
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("stale missing-source entries pruned: 1", result.stdout)
                prepared = json.loads(
                    (root / "build-tidy-fp" / "compile_commands.json").read_text(encoding="utf-8")
                )
                self.assertIsInstance(prepared, list)
                self.assertEqual([entry["file"] for entry in prepared], [str(live)])

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
        finding = (
            "sample.cpp:4:7: warning: invalid case style for variable 'BadName' "
            "[readability-identifier-naming]"
        )
        for shell in powershells():
            result = self.run_gate_case(shell, progress_log(1, finding), 1)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("1 finding(s) outside src/core", result.stdout)

    def test_core_processing_error_fails(self) -> None:
        core_log = progress_log(1, "Error while processing core_sample_1.cpp.")
        for shell in powershells():
            result = self.run_gate_case(
                shell, progress_log(1), 1, core_log_text=core_log
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("processing errors", result.stdout.lower())

    def test_core_compiler_error_fails(self) -> None:
        core_log = progress_log(
            1, "core_sample_1.cpp:3:2: error: unknown type name 'broken' [clang-diagnostic-error]"
        )
        for shell in powershells():
            result = self.run_gate_case(
                shell, progress_log(1), 1, core_log_text=core_log
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("processing errors", result.stdout.lower())

    def test_unhealthy_scan_removes_stale_core_summary(self) -> None:
        for shell in powershells():
            with self.subTest(shell=Path(shell).name), tempfile.TemporaryDirectory() as directory:
                root, stub, manifest = make_gate_fixture(
                    directory,
                    progress_log(1, "Error while processing sample.cpp."),
                    1,
                )
                summary = root / "build-tidy-core" / "core-naming-summary.json"
                summary.parent.mkdir(parents=True)
                summary.write_text('{"healthy":true}', encoding="ascii")
                result = run_script(
                    shell,
                    NAMING_GATE,
                    "-Root",
                    str(root),
                    "-RunClangTidyScript",
                    str(stub),
                    "-CoreManifestPath",
                    str(manifest),
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(summary.exists())

    def test_healthy_scan_publishes_machine_readable_core_summary(self) -> None:
        for shell in powershells():
            with self.subTest(shell=Path(shell).name), tempfile.TemporaryDirectory() as directory:
                root, stub, manifest = make_gate_fixture(directory, progress_log(1), 1)
                result = run_script(
                    shell,
                    NAMING_GATE,
                    "-Root",
                    str(root),
                    "-RunClangTidyScript",
                    str(stub),
                    "-CoreManifestPath",
                    str(manifest),
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(
                    "non-core 1/1 TUs clean; core 1/1 TUs healthy",
                    result.stdout,
                )
                summary_path = root / "build-tidy-core" / "core-naming-summary.json"
                summary = json.loads(summary_path.read_text(encoding="utf-8"))
                self.assertTrue(summary["healthy"])
                self.assertEqual(summary["translation_units"], 1)
                self.assertEqual(summary["finding_count"], 0)
                self.assertEqual(summary["clean_translation_units"], 1)

    def test_core_incomplete_translation_unit_count_fails(self) -> None:
        core_log = "\n".join(progress_log(3).splitlines()[:3])
        for shell in powershells():
            result = self.run_gate_case(
                shell,
                progress_log(1),
                1,
                core_log_text=core_log,
                core_translation_units=3,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("completed 2 of 3", result.stdout.lower())

    def test_core_finding_outside_clean_manifest_is_measured(self) -> None:
        finding = (
            "__ROOT__/src/core/core_sample_2.cpp:4:7: error: invalid case style for variable "
            "'BadName' [readability-identifier-naming,-warnings-as-errors]"
        )
        for shell in powershells():
            result = self.run_gate_case(
                shell,
                progress_log(1),
                1,
                core_log_text=progress_log(2, finding),
                core_translation_units=2,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 measured finding(s)", result.stdout)

    def test_core_finding_in_clean_manifest_fails(self) -> None:
        finding = (
            "__ROOT__/src/core/core_sample_1.cpp:4:7: error: invalid case style for variable "
            "'BadName' [readability-identifier-naming,-warnings-as-errors]"
        )
        for shell in powershells():
            result = self.run_gate_case(
                shell,
                progress_log(1),
                1,
                core_log_text=progress_log(1, finding),
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("clean-tu ratchet regressed", result.stdout.lower())

    def test_core_manifest_missing_source_fails(self) -> None:
        for shell in powershells():
            result = self.run_gate_case(
                shell,
                progress_log(1),
                1,
                manifest_entries=["src/core/missing.cpp"],
                create_manifest_sources=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("manifest source missing", result.stdout.lower())

    def test_core_manifest_empty_fails(self) -> None:
        for shell in powershells():
            result = self.run_gate_case(
                shell, progress_log(1), 1, manifest_entries=[]
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("manifest is empty", result.stdout.lower())


if __name__ == "__main__":
    unittest.main()
