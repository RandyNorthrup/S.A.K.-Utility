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

# On-disk binary-format and crypto modules. These legitimately use raw hex field
# offsets and magic/signature constants throughout (APFS/HFS on-disk structures,
# FileVault keybag + AES/PBKDF2 crypto). Hand-naming every offset here is high-risk
# churn against the Apple-certified byte-exact output -- a single mis-named offset
# writes wrong bytes and fails fsck -- so these files are exempt from the
# magic-number rule, mirroring the file allowlist the GUI magic-number checker
# (scripts/check_gui_magic_numbers.ps1) already uses for layout code.
EXEMPT_FILES = {
    "src/core/partition_apfs_writer.cpp",
    "include/sak/partition_apfs_writer.h",
    "src/core/partition_apfs_file_system_reader.cpp",
    "include/sak/apfs_compression.h",
    # The lzbitmap/lzvn/lzfse codecs and the resource-fork wrapper are the same
    # byte-exact category as apfs_compression.h: bit-shift amounts, byte masks
    # (0xFF), on-disk field offsets (trailer + 24) and Apple magic values
    # (0x636D7066 'cmpf'). Naming each offset is high-risk churn against the
    # Apple-certified compression output, so they join the format exemption.
    "include/sak/apfs_lzbitmap.h",
    "include/sak/apfs_lzbitmap_encode.h",
    "include/sak/apfs_lzbitmap_codec.h",
    "include/sak/apfs_resource_fork.h",
    "src/core/apfs_keybag.cpp",
    "src/core/apfs_crypto.cpp",
    "include/sak/partition_hfs_internal.h",
    "include/sak/partition_hfs_core.h",
    "include/sak/partition_hfs_case_folding.h",
}

NUMERIC_LITERAL_RE = re.compile(
    r"""
    (?<![A-Za-z0-9_])
    -?
    (?:
        0[xX][0-9A-Fa-f']+
        |
        0[bB][01']+
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
STATIC_ASSERT_RE = re.compile(r"\bstatic_assert\b")


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
    if (
        STATIC_ASSERT_RE.search(stripped)
        or stripped.startswith("using ")
        or stripped.startswith("typedef ")
    ):
        return True

    if state.enum_depth > 0:
        state.enum_depth = max(state.enum_depth + code.count("{") - code.count("}"), 0)
        return True
    if state.constant_depth > 0:
        state.constant_depth = update_depth(state.constant_depth, code)
        return True

    if ENUM_RE.search(stripped):
        # `enum` used as an elaborated-type-specifier that opens a scope (e.g.
        # `void f(enum Mode m) {` or `enum State next() {`) is NOT an enum body: the brace
        # belongs to a function. Only treat it as an enum definition when no parenthesis
        # precedes the opening brace, so a real function body is still scanned.
        brace = code.find("{")
        paren = code.find("(")
        elaborated_use = paren >= 0 and (brace < 0 or paren < brace)
        if not elaborated_use:
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
    try:
        text = path.read_text(encoding="utf-8-sig", errors="strict")
    except (UnicodeDecodeError, OSError) as exc:
        # A file we cannot decode or read may hide numeric literals behind the lost bytes;
        # fail closed by reporting it instead of silently skipping its contents.
        return [Violation(path, 1, "unreadable-source", f"{type(exc).__name__}: {exc}")]
    line_number = 0
    for line_number, line in enumerate(text.splitlines(), start=1):
        code = strip_non_code(line, lex_state)
        if is_constant_or_enum_context(code, skip_state):
            continue
        for match in NUMERIC_LITERAL_RE.finditer(code):
            literal = normalized_literal(match.group(0))
            if literal in ALLOWED_LITERALS:
                continue
            violations.append(Violation(path, line_number, literal, line.strip()))
    # An unterminated block comment or raw string is a malformed source file whose lexical
    # state would otherwise suppress every following line; surface it rather than returning a
    # clean result for a file the compiler would reject.
    if lex_state.in_block_comment:
        violations.append(
            Violation(path, line_number, "unterminated-block-comment",
                      "block comment not closed at end of file")
        )
    elif lex_state.raw_string_delimiter is not None:
        violations.append(
            Violation(path, line_number, "unterminated-raw-string",
                      "raw string literal not closed at end of file")
        )
    return violations


def scan(root: Path, scan_roots: list[str]) -> tuple[list[Violation], int, int]:
    violations: list[Violation] = []
    scanned_files = 0
    roots_found = 0
    for scan_root in scan_roots:
        current_root = root / scan_root
        if not current_root.is_dir():
            continue
        roots_found += 1
        for path in current_root.rglob("*"):
            if not path.is_file():
                continue
            try:
                # Exclusions and the exempt list are matched on the repo-relative path, not
                # the absolute one: a checkout that lives beneath an ancestor named "build"
                # or "external" must not have every source file silently excluded.
                relative = path.resolve().relative_to(root)
            except ValueError:
                # A symlink/junction escaping the repository root is not part of this
                # checkout; skip it rather than scan foreign bytes.
                continue
            if not should_scan(relative):
                continue
            if relative.as_posix() in EXEMPT_FILES:
                continue
            scanned_files += 1
            violations.extend(scan_file(path))
    return violations, scanned_files, roots_found


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

    violations, scanned_files, roots_found = scan(root, scan_roots)
    if roots_found == 0:
        print(
            f"Magic-number check failed: none of the source roots {list(scan_roots)} exist "
            f"under {root}. Run from the repository root or pass a correct --root."
        )
        return 1
    if scanned_files == 0:
        print(
            f"Magic-number check failed: zero source files scanned under {root}. This usually "
            "means a wrong --root or an excluded ancestor directory."
        )
        return 1
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
