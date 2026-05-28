#!/usr/bin/env python3
"""Fail on non-constant numeric literals in production C++ code.

The scanner intentionally ignores comments, string/character/raw-string
literals, preprocessor lines, enum definitions, and named const/constexpr
initializers. Bare -1, 0, and 1 are allowed because they are conventional
sentinel/identity values; everything else must be named.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


CPP_EXTENSIONS = {".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}
DEFAULT_ROOTS = ("src", "include")
EXCLUDED_PARTS = {
    ".git",
    ".venv",
    "build",
    "third_party",
    "3rdparty",
    "external",
}
ALLOWED_LITERALS = {"-1", "-1.0", "0", "0.0", "1", "1.0"}

NUMERIC_LITERAL_RE = re.compile(
    r"""
    (?<![A-Za-z0-9_])
    -?
    (?:
        0[xX][0-9A-Fa-f']+
        |
        \d[\d']*(?:\.\d[\d']*)?(?:[eE][+-]?\d[\d']*)?
    )
    (?:[uUlLfF]{0,3})
    """,
    re.VERBOSE,
)
RAW_STRING_START_RE = re.compile(r'R"([A-Za-z0-9_]*)\(')
CONSTANT_DECL_RE = re.compile(r"\b(?:inline\s+)?(?:static\s+)?(?:constexpr|consteval|constinit|const)\b")
K_NAMED_CONSTANT_RE = re.compile(r"\bk[A-Z][A-Za-z0-9_]*\b")
ENUM_RE = re.compile(r"\benum\b")


def is_numeric_separator_context(previous: str, next_char: str) -> bool:
    return previous.isdigit() and next_char.isdigit()


def is_hex_separator_context(previous: str, next_char: str) -> bool:
    return previous in "0123456789abcdefABCDEF" and next_char in "0123456789abcdefABCDEF"


def is_const_variable_context(stripped: str) -> bool:
    if not CONSTANT_DECL_RE.search(stripped):
        return False
    assignment_index = stripped.find("=")
    paren_index = stripped.find("(")
    if assignment_index >= 0 and (paren_index < 0 or assignment_index < paren_index):
        return True
    return bool(K_NAMED_CONSTANT_RE.search(stripped))


@dataclass
class LexState:
    in_block_comment: bool = False
    raw_string_delimiter: str | None = None


@dataclass
class SkipState:
    enum_depth: int = 0
    constant_depth: int = 0


@dataclass
class Violation:
    path: Path
    line_number: int
    literal: str
    line: str


def repo_path(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root).as_posix()


def should_scan(path: Path) -> bool:
    return path.suffix.lower() in CPP_EXTENSIONS and not any(
        part in EXCLUDED_PARTS for part in path.parts
    )


def strip_non_code(line: str, state: LexState) -> str:
    output: list[str] = []
    index = 0
    while index < len(line):
        if state.raw_string_delimiter is not None:
            terminator = f"){state.raw_string_delimiter}\""
            end = line.find(terminator, index)
            if end < 0:
                return "".join(output)
            index = end + len(terminator)
            state.raw_string_delimiter = None
            output.append(" ")
            continue

        char = line[index]
        if state.in_block_comment:
            if char == "*" and index + 1 < len(line) and line[index + 1] == "/":
                state.in_block_comment = False
                index += 2
            else:
                index += 1
            continue

        if char == "/" and index + 1 < len(line):
            next_char = line[index + 1]
            if next_char == "/":
                break
            if next_char == "*":
                state.in_block_comment = True
                index += 2
                continue

        raw_match = RAW_STRING_START_RE.match(line, index)
        if raw_match:
            state.raw_string_delimiter = raw_match.group(1)
            index = raw_match.end()
            output.append(" ")
            continue

        if char == "'" and index > 0 and index + 1 < len(line):
            if is_numeric_separator_context(line[index - 1], line[index + 1]) or (
                is_hex_separator_context(line[index - 1], line[index + 1])
            ):
                output.append(char)
                index += 1
                continue

        if char in {"'", '"'}:
            quote = char
            output.append(" ")
            index += 1
            while index < len(line):
                if line[index] == "\\":
                    index += 2
                    continue
                if line[index] == quote:
                    index += 1
                    break
                index += 1
            output.append(" ")
            continue

        output.append(char)
        index += 1

    return "".join(output)


def update_depth(current: int, code: str) -> int:
    depth = current + code.count("{") + code.count("(") - code.count("}") - code.count(")")
    if ";" in code and depth <= 0:
        return 0
    return max(depth, 0)


def is_constant_or_enum_context(code: str, state: SkipState) -> bool:
    stripped = code.strip()
    if not stripped:
        return True
    if stripped.startswith("#"):
        return True
    if "static_assert" in stripped or stripped.startswith("using ") or stripped.startswith("typedef "):
        return True

    if state.enum_depth > 0:
        state.enum_depth = max(state.enum_depth + code.count("{") - code.count("}"), 0)
        return True
    if state.constant_depth > 0:
        state.constant_depth = update_depth(state.constant_depth, code)
        return True

    if ENUM_RE.search(stripped):
        state.enum_depth = max(code.count("{") - code.count("}"), 0)
        return True
    if is_const_variable_context(stripped):
        state.constant_depth = update_depth(0, code)
        return True
    return False


def normalized_literal(raw_literal: str) -> str:
    literal = raw_literal.replace("'", "")
    if literal.lower().startswith("0x"):
        return re.sub(r"[uUlL]+$", "", literal)
    literal = re.sub(r"[uUlLfF]+$", "", literal)
    return literal


def scan_file(path: Path) -> list[Violation]:
    violations: list[Violation] = []
    lex_state = LexState()
    skip_state = SkipState()
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8-sig", errors="ignore").splitlines(), start=1
    ):
        code = strip_non_code(line, lex_state)
        if is_constant_or_enum_context(code, skip_state):
            continue
        for match in NUMERIC_LITERAL_RE.finditer(code):
            literal = normalized_literal(match.group(0))
            if literal in ALLOWED_LITERALS:
                continue
            violations.append(Violation(path, line_number, literal, line.strip()))
    return violations


def scan(root: Path, scan_roots: list[str]) -> list[Violation]:
    violations: list[Violation] = []
    for scan_root in scan_roots:
        current_root = root / scan_root
        if not current_root.exists():
            continue
        for path in current_root.rglob("*"):
            if path.is_file() and should_scan(path):
                violations.extend(scan_file(path))
    return violations


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=".", help="Repository root")
    parser.add_argument(
        "--include-tests",
        action="store_true",
        help="Also scan tests/ after production code is clean",
    )
    parser.add_argument("--max-report", type=int, default=200)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    scan_roots = list(DEFAULT_ROOTS)
    if args.include_tests:
        scan_roots.append("tests")

    violations = scan(root, scan_roots)
    if violations:
        print(f"Magic-number check failed: {len(violations)} violation(s).")
        for violation in violations[: args.max_report]:
            print(
                f"{repo_path(violation.path, root)}:{violation.line_number}:"
                f"{violation.literal}: {violation.line}"
            )
        if len(violations) > args.max_report:
            remaining = len(violations) - args.max_report
            print(f"... {remaining} more violation(s) not shown.")
        return 1

    print("Magic-number check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
