#!/usr/bin/env python3
"""Classify exact cppcheck functionStatic findings for Qt meta-object slots.

Qt invokes methods declared under Q_SLOTS through generated meta-object code. Cppcheck does not
model that call edge and recommends making otherwise stateless slots static, which would remove
them from Qt's meta-object table. This classifier does not suppress functionStatic at invocation.
It sees every raw finding and removes one from the actionable stream only after proving all of
these facts from the live source:

* the path is under this repository's tests directory;
* cppcheck named a concrete Class::method at the reported definition line;
* that exact class has one top-level declaration of the method; and
* the declaration is inside a Q_SLOTS access section.

Anything ambiguous remains actionable. Ordinary test helpers therefore cannot hide behind the
Qt limitation.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


FUNCTION_STATIC_RE = re.compile(
    r"^(?P<path>.+):(?P<line>\d+):(?P<column>\d+):.*"
    r"member function '(?P<member>[^']+)' can be static\. \[functionStatic\]\s*$"
)
CLASS_RE_TEMPLATE = r"\b(?:class|struct)\s+{class_name}\b[^;{{]*{{"
ACCESS_RE = re.compile(
    r"\b(?:(?:public|protected|private)\s+Q_SLOTS\s*:|"
    r"(?:public|protected|private)\s*:|Q_SIGNALS\s*:)"
)


def _blank_range(chars: list[str], start: int, end: int) -> None:
    for index in range(start, end):
        if chars[index] not in "\r\n":
            chars[index] = " "


def mask_non_code(source: str) -> str:
    """Blank comments and literals while preserving offsets, braces, and newlines."""

    chars = list(source)
    index = 0
    length = len(source)
    while index < length:
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            if end < 0:
                end = length
            _blank_range(chars, index, end)
            index = end
            continue

        if source.startswith("/*", index):
            close = source.find("*/", index + 2)
            end = length if close < 0 else close + 2
            _blank_range(chars, index, end)
            index = end
            continue

        if source.startswith('R"', index):
            delimiter_end = source.find("(", index + 2, min(length, index + 20))
            if delimiter_end >= 0:
                delimiter = source[index + 2 : delimiter_end]
                if not re.search(r"[\s\\()]", delimiter):
                    terminator = ")" + delimiter + '"'
                    close = source.find(terminator, delimiter_end + 1)
                    end = length if close < 0 else close + len(terminator)
                    _blank_range(chars, index, end)
                    index = end
                    continue

        # Apostrophes between alphanumeric characters are C++ digit separators (600'000,
        # 0xAB'CD), not character-literal delimiters. Treating one as a quote can blank
        # thousands of lines until the next apostrophe and make a real slot unclassifiable.
        if (
            source[index] == "'"
            and index > 0
            and index + 1 < length
            and source[index - 1].isalnum()
            and source[index + 1].isalnum()
        ):
            index += 1
            continue

        if source[index] in {'"', "'"}:
            quote = source[index]
            end = index + 1
            while end < length:
                if source[end] == "\\":
                    end += 2
                    continue
                end += 1
                if source[end - 1] == quote:
                    break
            _blank_range(chars, index, min(end, length))
            index = end
            continue

        index += 1

    return "".join(chars)


def matching_brace(masked: str, opening: int) -> int | None:
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def brace_depths(text: str) -> list[int]:
    depths: list[int] = []
    depth = 0
    for char in text:
        depths.append(depth)
        if char == "{":
            depth += 1
        elif char == "}":
            depth = max(0, depth - 1)
    return depths


def method_is_unique_qt_slot(masked: str, class_name: str, method_name: str) -> bool:
    class_re = re.compile(CLASS_RE_TEMPLATE.format(class_name=re.escape(class_name)))
    class_matches = list(class_re.finditer(masked))
    if len(class_matches) != 1:
        return False

    opening = class_matches[0].end() - 1
    closing = matching_brace(masked, opening)
    if closing is None:
        return False

    body = masked[opening + 1 : closing]
    depths = brace_depths(body)
    method_re = re.compile(rf"\b{re.escape(method_name)}\s*\(")
    method_matches = [match for match in method_re.finditer(body) if depths[match.start()] == 0]
    if len(method_matches) != 1:
        return False

    labels = [match for match in ACCESS_RE.finditer(body) if depths[match.start()] == 0]
    method_offset = method_matches[0].start()
    preceding = [label for label in labels if label.end() <= method_offset]
    if not preceding:
        return False
    return "Q_SLOTS" in preceding[-1].group(0)


def finding_is_qt_slot(line: str, project_root: Path) -> bool:
    match = FUNCTION_STATIC_RE.match(line)
    if not match:
        return False

    finding_path = Path(match.group("path"))
    if not finding_path.is_absolute():
        finding_path = project_root / finding_path
    finding_path = finding_path.resolve()
    try:
        finding_path.relative_to((project_root / "tests").resolve())
    except ValueError:
        return False

    member_parts = [part for part in match.group("member").lstrip(":").split("::") if part]
    if len(member_parts) < 2:
        return False
    class_name, method_name = member_parts[-2:]

    source = finding_path.read_text(encoding="utf-8")
    source_lines = source.splitlines()
    definition_line = int(match.group("line"))
    if definition_line < 1 or definition_line > len(source_lines):
        return False
    if not re.search(rf"\b{re.escape(method_name)}\s*\(", source_lines[definition_line - 1]):
        return False

    return method_is_unique_qt_slot(mask_non_code(source), class_name, method_name)


def classify(lines: list[str], project_root: Path) -> tuple[list[str], int, int]:
    actionable: list[str] = []
    classified = 0
    remaining_function_static = 0
    for line in lines:
        if finding_is_qt_slot(line, project_root):
            classified += 1
            continue
        actionable.append(line)
        if line.rstrip().endswith("[functionStatic]"):
            remaining_function_static += 1
    return actionable, classified, remaining_function_static


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--project-root", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    raw_lines = args.input.read_text(encoding="utf-8-sig").splitlines(keepends=True)
    actionable, classified, remaining = classify(raw_lines, args.project_root.resolve())
    args.output.write_text("".join(actionable), encoding="utf-8", newline="")
    print(f"CLASSIFIED_QT_SLOT_FUNCTION_STATIC={classified}")
    print(f"ACTIONABLE_FUNCTION_STATIC={remaining}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
