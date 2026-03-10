#!/usr/bin/env python3
"""Lizard complexity checker for S.A.K. Utility — strictest TigerStyle settings.

Enforces:
  - Cyclomatic complexity (CCN) ≤ 10
  - Function length ≤ 70 lines (NLOC)
  - Parameter count ≤ 5
  - No warnings tolerated (exit 1 on any violation)

Usage:
    python scripts/run_lizard.py [files...]
    python scripts/run_lizard.py                   # scans src/ include/ tests/
    python scripts/run_lizard.py --strict           # same as default (always strict)

Called by pre-commit hooks and CI. Exit code 0 = clean, 1 = violations found.
"""

import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# TigerStyle thresholds — strictest settings
# ---------------------------------------------------------------------------
MAX_CCN = 10            # Cyclomatic complexity (lizard default: 15)
MAX_FUNC_LENGTH = 70    # Function length in NLOC (TigerStyle hard limit)
MAX_PARAMS = 5          # Parameter count
MAX_FILE_NLOC = 500     # File-level NLOC warning threshold

# Directories to scan when no files are specified
DEFAULT_DIRS = ["src", "include", "tests"]

# Paths to exclude from analysis
EXCLUDE_PATTERNS = [
    "*/third_party/*",
    "*/3rdparty/*",
    "*/external/*",
    "*/build/*",
    "*/.venv/*",
    "*/moc_*",
    "*/qrc_*",
    "*/ui_*",
]


def find_lizard() -> str:
    """Locate the lizard executable."""
    project_root = Path(__file__).resolve().parent.parent
    venv_lizard = project_root / ".venv" / "Scripts" / "lizard.exe"
    if venv_lizard.exists():
        return str(venv_lizard)
    return "lizard"


def build_command(files: list[str]) -> list[str]:
    """Build the lizard command with strictest thresholds."""
    lizard_exe = find_lizard()

    command = [
        lizard_exe,
        "-l", "cpp",
        f"-C{MAX_CCN}",
        f"-L{MAX_FUNC_LENGTH}",
        f"-a{MAX_PARAMS}",
        "-w",               # Warnings-only output format (cleaner)
        "-i", "0",           # Zero tolerance — any warning is a failure
    ]

    for pattern in EXCLUDE_PATTERNS:
        command.extend(["-x", pattern])

    if files:
        command.extend(files)
    else:
        command.extend(DEFAULT_DIRS)

    return command


def main() -> int:
    # Filter out --strict flag (always strict, flag accepted for compatibility)
    args = [arg for arg in sys.argv[1:] if arg != "--strict"]

    # Filter to only C++ files if specific files are passed
    cpp_extensions = {".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx"}
    if args:
        cpp_files = [
            f for f in args
            if Path(f).suffix.lower() in cpp_extensions
        ]
        if not cpp_files:
            return 0  # No C++ files to check
        files = cpp_files
    else:
        files = []

    command = build_command(files)

    result = subprocess.run(command, capture_output=False)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
