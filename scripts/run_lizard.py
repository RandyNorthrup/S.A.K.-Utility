#!/usr/bin/env python3
"""Lizard complexity checker for S.A.K. Utility — TigerStyle settings.

Hard limits (block commit):
  - Cyclomatic complexity (CCN) ≤ 10
  - Parameter count ≤ 5

Soft limit (advisory warning, does not block):
  - Function length ≤ 70 lines (NLOC)

Length-only violations are printed as recommendations but do not cause
a non-zero exit code. This prevents artificial function splitting in
low-complexity UI setup or data-initialization code where the length
comes from declarative content rather than logic.

Usage:
    python scripts/run_lizard.py [files...]
    python scripts/run_lizard.py                   # scans src/ include/ tests/

Called by pre-commit hooks and CI. Exit code 0 = clean, 1 = violations found.
"""

import re
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# TigerStyle thresholds
# ---------------------------------------------------------------------------
MAX_CCN = 10            # Cyclomatic complexity (hard limit)
MAX_PARAMS = 5          # Parameter count (hard limit)
MAX_FUNC_LENGTH = 70    # Function length in NLOC (soft recommendation)

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

# Regex to extract metrics from a lizard warning line
WARNING_RE = re.compile(
    r"(\d+) NLOC, (\d+) CCN, \d+ token, (\d+) PARAM, (\d+) length"
)


def find_lizard() -> str:
    """Locate the lizard executable."""
    project_root = Path(__file__).resolve().parent.parent
    venv_lizard = project_root / ".venv" / "Scripts" / "lizard.exe"
    if venv_lizard.exists():
        return str(venv_lizard)
    return "lizard"


def build_command(files: list[str]) -> list[str]:
    """Build the lizard command."""
    lizard_exe = find_lizard()

    command = [
        lizard_exe,
        "-l", "cpp",
        f"-C{MAX_CCN}",
        f"-L{MAX_FUNC_LENGTH}",
        f"-a{MAX_PARAMS}",
        "-w",
        "-i", "0",
    ]

    for pattern in EXCLUDE_PATTERNS:
        command.extend(["-x", pattern])

    if files:
        command.extend(files)
    else:
        command.extend(DEFAULT_DIRS)

    return command


def classify_warning(line: str) -> str | None:
    """Classify a lizard warning line.

    Returns:
        "hard"  — CCN or PARAM violation (blocks commit)
        "soft"  — length-only violation (advisory)
        None    — not a warning line
    """
    match = WARNING_RE.search(line)
    if not match:
        return None

    ccn = int(match.group(2))
    params = int(match.group(3))
    length = int(match.group(4))

    has_ccn = ccn > MAX_CCN
    has_params = params > MAX_PARAMS
    has_length = length > MAX_FUNC_LENGTH

    if has_ccn or has_params:
        return "hard"
    if has_length:
        return "soft"
    return None


def main() -> int:
    args = [arg for arg in sys.argv[1:] if arg != "--strict"]

    cpp_extensions = {".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx"}
    if args:
        cpp_files = [
            f for f in args
            if Path(f).suffix.lower() in cpp_extensions
        ]
        if not cpp_files:
            return 0
        files = cpp_files
    else:
        files = []

    command = build_command(files)
    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr

    hard_errors: list[str] = []
    soft_warnings: list[str] = []

    for line in output.splitlines():
        kind = classify_warning(line)
        if kind == "hard":
            hard_errors.append(line)
        elif kind == "soft":
            soft_warnings.append(line)

    if hard_errors:
        print(f"=== {len(hard_errors)} HARD violation(s) "
              f"(CCN>{MAX_CCN} or PARAM>{MAX_PARAMS}) ===")
        for line in hard_errors:
            print(line)

    if soft_warnings:
        print(f"\n--- {len(soft_warnings)} length advisory warning(s) "
              f"(>{MAX_FUNC_LENGTH} lines, non-blocking) ---")
        for line in soft_warnings:
            print(line)

    if hard_errors:
        print(f"\nFAILED: {len(hard_errors)} hard violation(s) must be fixed.")
        return 1

    if soft_warnings:
        print(f"\nPASSED (with {len(soft_warnings)} length advisory warning(s))")
    else:
        print("\nPASSED — all functions within limits.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
