#!/usr/bin/env python3
"""Lizard complexity checker for S.A.K. Utility -- TigerStyle settings.

Hard limits (block commit -- warnings are errors in this repo):
  - Cyclomatic complexity (CCN) <= 10
  - Parameter count <= 5
  - Function length <= 70 lines

All three are blocking; there are no advisory-only violations.

TWO LANGUAGES, ONE SET OF THRESHOLDS
C++ is held at zero violations. JavaScript -- browser/extension/background.js, ~3300 lines
of privileged code that drives a real user's browser over CDP -- had never been seen by any
gate at all (R5-G21-9), and arrived with two dozen functions over the limit. Excluding it
would have left the situation exactly as it was, so instead it is held by a RATCHET against
an enumerated baseline:

  1. a JavaScript violation that is not in the baseline FAILS -- no new debt;
  2. a baselined function that gets WORSE on any metric FAILS -- no drift;
  3. a baseline entry that no longer violates FAILS until it is deleted -- so the list can
     only ever shrink, and cannot quietly become a permanent exclusion.

Rule 3 is the point. A baseline nobody is forced to prune is just an exclusion list with
extra steps.

Usage:
    python scripts/run_lizard.py [files...]
    python scripts/run_lizard.py                   # scans src/ include/ tests/ browser/

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
MAX_FUNC_LENGTH = 70    # Function length in lines (hard limit)

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

# Directories scanned for JavaScript when no files are specified.
JS_DIRS = ["browser"]

JS_EXTENSIONS = {".js", ".mjs"}

# The repository root, derived from this script's own location so the gate scans the real tree
# regardless of the caller's working directory.
PROJECT_ROOT = Path(__file__).resolve().parent.parent

# The enumerated JavaScript baseline. See the module docstring: this list may shrink and
# may not grow, and a stale entry is itself a failure.
BASELINE_PATH = Path(__file__).resolve().parent / "lizard_js_baseline.txt"

# Regex to extract metrics from a lizard warning line
WARNING_RE = re.compile(
    r"(\d+) NLOC, (\d+) CCN, \d+ token, (\d+) PARAM, (\d+) length"
)

# "browser/extension/background.js:339: warning: onHostMessage has 49 NLOC, ..."
WARNING_LOCATION_RE = re.compile(r"^(.+?):(\d+): warning: (\S+) has ")


def find_lizard() -> str:
    """Locate the lizard executable."""
    venv_lizard = PROJECT_ROOT / ".venv" / "Scripts" / "lizard.exe"
    if venv_lizard.exists():
        return str(venv_lizard)
    return "lizard"


def build_command(files: list[str], language: str, default_dirs: list[str]) -> list[str]:
    """Build the lizard command for one language."""
    lizard_exe = find_lizard()

    command = [
        lizard_exe,
        "-l", language,
        f"-C{MAX_CCN}",
        f"-L{MAX_FUNC_LENGTH}",
        f"-a{MAX_PARAMS}",
        "-w",
        "-i", "0",
    ]

    for pattern in EXCLUDE_PATTERNS:
        command.extend(["-x", pattern])

    # Everything after "--" is a positional path, so a file whose name begins with "-" cannot be
    # mistaken for a lizard option (argument injection through a crafted filename).
    command.append("--")
    if files:
        command.extend(files)
    else:
        command.extend(default_dirs)

    return command


def assert_scan_is_not_empty(files: list[str], language: str, default_dirs: list[str],
                             extensions: set[str]) -> None:
    """Abort unless this run will actually analyze at least one file.

    lizard exits 0 when the paths it is given are missing or hold nothing it recognises, so a
    whole-tree run launched from the wrong working directory -- or against a renamed tree --
    produces no warnings, no non-zero exit, and a confident "PASSED -- all functions within
    limits." That is a gate certifying a scan it never performed. The explicit-files case is
    NOT checked here: pre-commit legitimately passes a commit with no files of this language,
    and main() already returns 0 for that."""
    if files:
        return
    found = 0
    for directory in default_dirs:
        root = PROJECT_ROOT / directory
        if not root.is_dir():
            raise SystemExit(
                f"ABORT: lizard ({language}) default scan directory {directory!r} does not "
                f"exist under {PROJECT_ROOT}; refusing to certify a scan of nothing.")
        for path in root.rglob("*"):
            if path.is_file() and path.suffix.lower() in extensions:
                found += 1
                if found:
                    break
        if found:
            break
    if found == 0:
        raise SystemExit(
            f"ABORT: lizard ({language}) found no source files under {default_dirs}; a scan "
            "that analyzes zero files is BROKEN, not passing.")


def run_lizard(files: list[str], language: str, default_dirs: list[str]) -> list[str]:
    """Run lizard and return the lines that are hard violations."""
    extensions = JS_EXTENSIONS if language == "javascript" else {
        ".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx"}
    assert_scan_is_not_empty(files, language, default_dirs, extensions)
    command = build_command(files, language, default_dirs)
    try:
        result = subprocess.run(
            command, capture_output=True, text=True, cwd=str(PROJECT_ROOT))
    except FileNotFoundError as exc:
        raise SystemExit(
            f"ABORT: lizard executable not found ({command[0]!r}); the complexity gate cannot "
            "run. Install lizard into .venv or on PATH.") from exc
    output = result.stdout + result.stderr
    hard = [line for line in output.splitlines() if classify_warning(line) == "hard"]
    # lizard runs with "-i 0", so it exits non-zero only to report threshold violations, and
    # every such violation is a parseable warning line. A non-zero exit with nothing parseable
    # means lizard itself failed (bad arguments, a crash, or a changed output format) -- fail
    # closed rather than read the empty result as a clean pass.
    if result.returncode != 0 and not hard:
        raise SystemExit(
            f"ABORT: lizard ({language}) exited {result.returncode} with no parseable "
            f"violations; refusing to certify this run.\n{output.strip()}")
    return hard


def parse_violation(line: str) -> tuple[str, int, int, int] | None:
    """Return (key, ccn, params, length) for a warning line, or None.

    The key is "function@path" WITHOUT the line number, because a function moving down the
    file is not a change in its complexity and must not require a baseline edit.
    """
    metrics = WARNING_RE.search(line)
    location = WARNING_LOCATION_RE.match(line)
    if not metrics or not location:
        return None
    path = location.group(1).replace("\\", "/")
    return (f"{location.group(3)}@{path}",
            int(metrics.group(2)), int(metrics.group(3)), int(metrics.group(4)))


def load_baseline() -> dict[str, tuple[int, int, int]]:
    """Read the enumerated JavaScript baseline: key -> (ccn, params, length)."""
    if not BASELINE_PATH.exists():
        return {}
    baseline: dict[str, tuple[int, int, int]] = {}
    for raw in BASELINE_PATH.read_text(encoding="ascii").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 4:
            raise SystemExit(
                f"ABORT: malformed baseline row in {BASELINE_PATH.name}: {raw!r}. "
                "Expected 'function@path | ccn | params | length'.")
        baseline[parts[0]] = (int(parts[1]), int(parts[2]), int(parts[3]))
    return baseline


def check_javascript(files: list[str]) -> int:
    """Hold JavaScript against the baseline ratchet. Returns an exit code."""
    violations = {}
    for line in run_lizard(files, "javascript", JS_DIRS):
        parsed = parse_violation(line)
        if parsed:
            violations[parsed[0]] = (parsed[1], parsed[2], parsed[3])

    baseline = load_baseline()
    failures: list[str] = []

    for key, (ccn, params, length) in sorted(violations.items()):
        if key not in baseline:
            failures.append(
                f"NEW violation (not in the baseline): {key} "
                f"has {ccn} CCN, {params} PARAM, {length} length")
            continue
        base_ccn, base_params, base_length = baseline[key]
        if ccn > base_ccn or params > base_params or length > base_length:
            failures.append(
                f"WORSE than its baseline: {key} was {base_ccn} CCN / {base_params} PARAM / "
                f"{base_length} length, is now {ccn} / {params} / {length}")

    # A baseline scan is only meaningful over the whole tree; when pre-commit passes a subset
    # of files, a function that simply was not scanned must not read as one that was fixed.
    if not files:
        for key in sorted(set(baseline) - set(violations)):
            failures.append(
                f"STALE baseline entry: {key} no longer violates. Delete its row from "
                f"{BASELINE_PATH.name} -- the baseline may only shrink.")

    if failures:
        print(f"=== JavaScript complexity: {len(failures)} problem(s) ===")
        for failure in failures:
            print(f"  {failure}")
        print()
        print("The JavaScript baseline is a ratchet, not an exclusion list: entries may be")
        print("removed as functions are fixed, and nothing may be added to it.")
        return 1

    if violations:
        print(f"JavaScript: {len(violations)} known violation(s), none new, none worse.")
    return 0


def classify_warning(line: str) -> str | None:
    """Classify a lizard warning line.

    Returns:
        "hard"  -- CCN, PARAM, or length violation (blocks commit)
        None    -- not a warning line
    """
    match = WARNING_RE.search(line)
    if not match:
        return None

    ccn = int(match.group(2))
    params = int(match.group(3))
    length = int(match.group(4))

    if ccn > MAX_CCN or params > MAX_PARAMS or length > MAX_FUNC_LENGTH:
        return "hard"
    return None


def check_cpp(files: list[str]) -> int:
    """C++ is held at zero violations. Returns an exit code."""
    hard_errors = run_lizard(files, "cpp", DEFAULT_DIRS)
    if hard_errors:
        print(f"=== {len(hard_errors)} C++ violation(s) "
              f"(CCN>{MAX_CCN}, PARAM>{MAX_PARAMS}, or length>{MAX_FUNC_LENGTH}) ===")
        for line in hard_errors:
            print(line)
        print(f"\nFAILED: {len(hard_errors)} violation(s) must be fixed.")
        return 1
    return 0


def main() -> int:
    args = [arg for arg in sys.argv[1:] if arg != "--strict"]

    cpp_extensions = {".cpp", ".h", ".hpp", ".cxx", ".cc", ".hxx"}
    if args:
        cpp_files = [f for f in args if Path(f).suffix.lower() in cpp_extensions]
        js_files = [f for f in args if Path(f).suffix.lower() in JS_EXTENSIONS]
        if not cpp_files and not js_files:
            return 0
    else:
        cpp_files = []
        js_files = []

    status = 0
    # Both languages always run to completion: reporting only the first failing language
    # would hide the second until the first was fixed, which turns one commit into two.
    if cpp_files or not args:
        status |= check_cpp(cpp_files)
    if js_files or not args:
        status |= check_javascript(js_files)

    if status == 0:
        print("\nPASSED -- all functions within limits.")
    return status


if __name__ == "__main__":
    sys.exit(main())
