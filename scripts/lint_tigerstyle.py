#!/usr/bin/env python3
"""TigerStyle lint checker for S.A.K. Utility.

Checks C++ source files for TigerStyle compliance:
  - Max line length (100 chars, raw strings exempt)
  - Function length (<=70 lines)
  - Nesting depth (<=3 levels)
  - Assertion density (warn if <2 per function)
  - catch(...) without explanatory comment

Usage:
    python scripts/lint_tigerstyle.py [--strict] [paths...]

Exit code 0 if all checks pass (warnings are ok), 1 if errors found.
"""

import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
MAX_LINE_LENGTH = 100
MAX_FUNCTION_LINES = 70
MAX_NESTING_DEPTH = 3
MIN_ASSERTIONS_PER_FUNCTION = 2

# Patterns for PowerShell interpolation strings that cannot be split
# (accepted exceptions for line-length rule)
UNSPLITTABLE_RE = re.compile(
    r'\\"\$\(.*\\\"\$\{',  # PS variable interpolation with \"
)

RAW_STRING_OPEN = re.compile(r'R"([A-Za-z_]*)\(')
FUNCTION_DEF = re.compile(
    r'^(?!.*\b(?:if|else|for|while|switch|catch|do|return|emit)\b)'
    r'[A-Za-z_][\w:<>*&\s,]*\([^;]*\)\s*(?:const\s*)?(?:override\s*)?'
    r'(?:noexcept\s*)?(?:final\s*)?\{?\s*$'
)
ASSERT_RE = re.compile(
    r'\b(?:Q_ASSERT|Q_ASSERT_X|assert|QVERIFY|QCOMPARE|QVERIFY2)\b'
)
CATCH_ALL_RE = re.compile(r'\bcatch\s*\(\s*\.\.\.\s*\)')


class Severity(Enum):
    ERROR = "ERROR"
    WARNING = "WARNING"


@dataclass
class Violation:
    file: str
    line: int
    severity: Severity
    rule: str
    message: str


@dataclass
class FunctionInfo:
    name: str
    start_line: int
    end_line: int = 0
    assertion_count: int = 0
    max_nesting: int = 0


# ---------------------------------------------------------------------------
# Checkers
# ---------------------------------------------------------------------------

def check_line_length(filepath: str, lines: list[str]) -> list[Violation]:
    """Check that no line exceeds MAX_LINE_LENGTH (raw strings exempt)."""
    violations: list[Violation] = []
    in_raw = False
    raw_delim = ""

    for i, line in enumerate(lines, 1):
        stripped = line.rstrip("\n")

        # Track raw string boundaries
        if not in_raw:
            m = RAW_STRING_OPEN.search(stripped)
            if m:
                raw_delim = m.group(1)
                # Check if raw string closes on same line
                close_pattern = f"){raw_delim}\""
                after_open = stripped[m.end():]
                if close_pattern not in after_open:
                    in_raw = True
                    continue  # Skip this line (raw string start)
                # Single-line raw string: exempt the whole line
                continue
        else:
            close_pattern = f"){raw_delim}\""
            if close_pattern in stripped:
                in_raw = False
            continue  # Inside raw string: exempt

        if len(stripped) > MAX_LINE_LENGTH:
            # Allow NOLINT suppression for accepted exceptions
            if "// NOLINT" in stripped or "// NOLINT(line-length)" in stripped:
                continue
            violations.append(Violation(
                file=filepath, line=i, severity=Severity.ERROR,
                rule="line-length",
                message=(f"Line is {len(stripped)} chars "
                         f"(max {MAX_LINE_LENGTH})")
            ))

    return violations


# Regex to match regular C++ string literal content (double-quoted strings)
# Handles escaped characters including \" inside strings.
_STRING_LITERAL_RE = re.compile(r'"(?:[^"\\]|\\.)*"')

# Regex to match character literal content (single-quoted chars)
_CHAR_LITERAL_RE = re.compile(r"'(?:[^'\\]|\\.)*'")


def _strip_string_braces(line: str) -> str:
    """Remove braces that appear inside string/char literals or comments.

    Returns a version of the line where braces inside quotes and after
    ``//`` are replaced so they are not counted as C++ scope braces.
    """
    # Strip content inside string literals (replaces all content with empty)
    result = _STRING_LITERAL_RE.sub('""', line)
    # Strip content inside character literals
    result = _CHAR_LITERAL_RE.sub("''", result)
    # Strip line comments (braces in comments are not code)
    comment_pos = result.find("//")
    if comment_pos >= 0:
        result = result[:comment_pos]
    return result


def _find_functions(lines: list[str]) -> list[FunctionInfo]:
    """Find function bodies by tracking brace depth.

    Handles raw strings and regular string literals so that braces
    inside PowerShell/SQL/JSON embedded strings are not mis-counted.
    """
    functions: list[FunctionInfo] = []
    brace_depth = 0
    current_func: FunctionInfo | None = None
    func_brace_depth = 0
    in_raw = False
    raw_delim = ""

    for i, line in enumerate(lines, 1):
        stripped = line.strip()

        # ── Raw string tracking ──────────────────────────────────────
        if in_raw:
            close_pattern = f"){raw_delim}\""
            if close_pattern in stripped:
                in_raw = False
            # Inside raw string: skip brace counting entirely, but
            # still count assertions if inside a function.
            if current_func:
                current_func.assertion_count += len(ASSERT_RE.findall(line))
            continue

        # Check if this line opens a raw string
        raw_match = RAW_STRING_OPEN.search(stripped)
        if raw_match:
            raw_delim = raw_match.group(1)
            close_pattern = f"){raw_delim}\""
            after_open = stripped[raw_match.end():]
            if close_pattern not in after_open:
                # Multi-line raw string starts here
                in_raw = True
                # Count braces only BEFORE the raw string opening
                prefix = stripped[:raw_match.start()]
                code_line = _strip_string_braces(prefix)
                opens = code_line.count("{")
                closes = code_line.count("}")
                if current_func:
                    current_func.assertion_count += len(
                        ASSERT_RE.findall(line))
                    nesting = (brace_depth - func_brace_depth - 1
                               + max(0, opens - closes))
                    if nesting > current_func.max_nesting:
                        current_func.max_nesting = nesting
                    brace_depth += opens - closes
                    if brace_depth <= func_brace_depth:
                        current_func.end_line = i
                        functions.append(current_func)
                        current_func = None
                else:
                    brace_depth += opens - closes
                continue
            # Single-line raw string: strip it before counting braces
            # Replace the whole raw string with empty placeholder
            stripped = (stripped[:raw_match.start()]
                        + stripped[stripped.index(close_pattern)
                                  + len(close_pattern):])

        # ── Skip pure comments and preprocessor ──────────────────────
        if stripped.startswith("//") or stripped.startswith("#"):
            if current_func:
                current_func.assertion_count += len(ASSERT_RE.findall(line))
            continue

        # ── Count braces excluding those in string literals ──────────
        code_line = _strip_string_braces(line)
        opens = code_line.count("{")
        closes = code_line.count("}")

        if current_func is None:
            # Look for function definition
            if FUNCTION_DEF.match(stripped) or (
                brace_depth == 1 and stripped == "{"
                and i > 1 and FUNCTION_DEF.match(lines[i - 2].strip())
            ):
                if "{" in code_line:
                    current_func = FunctionInfo(
                        name=stripped.split("(")[0].strip().split()[-1]
                        if "(" in stripped else "unknown",
                        start_line=i
                    )
                    func_brace_depth = brace_depth
            brace_depth += opens - closes
            if current_func is None and brace_depth == 1 and opens > 0:
                # Namespace or class opening brace
                pass
        else:
            # Inside a function
            current_func.assertion_count += len(ASSERT_RE.findall(line))

            # Track nesting relative to function
            nesting = brace_depth - func_brace_depth - 1 + max(0, opens - closes)
            if nesting > current_func.max_nesting:
                current_func.max_nesting = nesting

            brace_depth += opens - closes

            if brace_depth <= func_brace_depth:
                current_func.end_line = i
                functions.append(current_func)
                current_func = None

    return functions


def check_function_length(
    filepath: str, functions: list[FunctionInfo]
) -> list[Violation]:
    """Check that no function exceeds MAX_FUNCTION_LINES."""
    violations: list[Violation] = []
    for func in functions:
        length = func.end_line - func.start_line + 1
        if length > MAX_FUNCTION_LINES:
            violations.append(Violation(
                file=filepath, line=func.start_line,
                severity=Severity.ERROR, rule="function-length",
                message=(f"Function '{func.name}' is {length} lines "
                         f"(max {MAX_FUNCTION_LINES})")
            ))
    return violations


def check_nesting_depth(
    filepath: str, functions: list[FunctionInfo]
) -> list[Violation]:
    """Check that no function exceeds MAX_NESTING_DEPTH."""
    violations: list[Violation] = []
    for func in functions:
        if func.max_nesting > MAX_NESTING_DEPTH:
            violations.append(Violation(
                file=filepath, line=func.start_line,
                severity=Severity.ERROR, rule="nesting-depth",
                message=(f"Function '{func.name}' has nesting depth "
                         f"{func.max_nesting} (max {MAX_NESTING_DEPTH})")
            ))
    return violations


def check_assertion_density(
    filepath: str, functions: list[FunctionInfo]
) -> list[Violation]:
    """Warn if a function has fewer than MIN_ASSERTIONS_PER_FUNCTION."""
    violations: list[Violation] = []
    for func in functions:
        length = func.end_line - func.start_line + 1
        # Only check functions with meaningful length (>10 lines)
        if length > 10 and func.assertion_count < MIN_ASSERTIONS_PER_FUNCTION:
            violations.append(Violation(
                file=filepath, line=func.start_line,
                severity=Severity.WARNING, rule="assertion-density",
                message=(f"Function '{func.name}' has "
                         f"{func.assertion_count} assertion(s) "
                         f"(recommend >= {MIN_ASSERTIONS_PER_FUNCTION})")
            ))
    return violations


def check_catch_all(
    filepath: str, lines: list[str]
) -> list[Violation]:
    """Check that catch(...) has an explanatory comment.

    Accepts comments on the same line, the previous line, or the
    first line inside the catch body (the next line after the opening brace).
    """
    violations: list[Violation] = []
    for i, line in enumerate(lines, 1):
        if CATCH_ALL_RE.search(line):
            # Check this line, the previous line, and the next line
            has_comment = "//" in line
            if i > 1 and not has_comment:
                has_comment = "//" in lines[i - 2]
            if i < len(lines) and not has_comment:
                has_comment = "//" in lines[i]  # next line (body)
            if not has_comment:
                violations.append(Violation(
                    file=filepath, line=i, severity=Severity.ERROR,
                    rule="catch-all-comment",
                    message="catch(...) without explanatory comment"
                ))
    return violations


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Directories excluded from linting (third-party code we do not own)
EXCLUDED_DIRS = {"third_party", "3rdparty", "external"}


def _is_excluded(filepath: str) -> bool:
    """Return True if the file is inside an excluded directory."""
    parts = Path(filepath).parts
    return any(part in EXCLUDED_DIRS for part in parts)


def collect_files(paths: list[str]) -> list[str]:
    """Collect all .cpp/.h files from given paths, excluding third-party."""
    result: list[str] = []
    for p in paths:
        path = Path(p)
        if path.is_file() and path.suffix in (".cpp", ".h"):
            if not _is_excluded(str(path)):
                result.append(str(path))
        elif path.is_dir():
            for ext in ("*.cpp", "*.h"):
                for f in path.rglob(ext):
                    if not _is_excluded(str(f)):
                        result.append(str(f))
    return sorted(set(result))


def lint_file(filepath: str, strict: bool) -> list[Violation]:
    """Run all checks on a single file."""
    with open(filepath, encoding="utf-8") as f:
        lines = f.readlines()

    violations: list[Violation] = []
    violations.extend(check_line_length(filepath, lines))
    violations.extend(check_catch_all(filepath, lines))

    functions = _find_functions(lines)
    violations.extend(check_function_length(filepath, functions))
    violations.extend(check_nesting_depth(filepath, functions))
    violations.extend(check_assertion_density(filepath, functions))

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(
        description="TigerStyle lint checker for C++ source files"
    )
    parser.add_argument(
        "paths", nargs="*", default=["src", "include", "tests"],
        help="Files or directories to check (default: src include tests)"
    )
    parser.add_argument(
        "--strict", action="store_true",
        help="Treat warnings as errors"
    )
    args = parser.parse_args()

    files = collect_files(args.paths)
    if not files:
        print("No C++ files found.")
        return 0

    all_violations: list[Violation] = []
    for filepath in files:
        all_violations.extend(lint_file(filepath, args.strict))

    # Print violations
    errors = 0
    warnings = 0
    for v in all_violations:
        tag = v.severity.value
        print(f"{v.file}:{v.line}: {tag}: [{v.rule}] {v.message}")
        if v.severity == Severity.ERROR:
            errors += 1
        else:
            warnings += 1

    # Summary
    print(f"\n{'=' * 60}")
    print(f"Files checked: {len(files)}")
    print(f"Errors: {errors}  Warnings: {warnings}")

    if errors > 0:
        print("FAILED — fix errors above")
        return 1

    if warnings > 0 and args.strict:
        print("FAILED (strict mode) — fix warnings above")
        return 1

    print("PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
