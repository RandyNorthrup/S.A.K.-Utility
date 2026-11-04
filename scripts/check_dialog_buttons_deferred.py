"""Refuses a button or menu action that opens a dialog inside its own press.

Qt's accessibility press on a button or menu item runs what it does inside the UI Automation call.
A dialog opened there holds that call, and every later one, until it closes, so a screen reader
cannot read the dialog it has just opened. Queuing the connection lets the press return first.

This reads the source, so it answers for what it can follow: a connect of a button's clicked or an
action's triggered to a slot or lambda under src/gui, and the functions those call, a few levels
down, by name. A dialog reached through a function pointer or a signal of its own is not seen.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

MODAL_CALL = re.compile(
    r"(?:\.|->)exec\s*\("
    r"|\bQMessageBox::(?:information|warning|critical|question|about)\b"
    r"|\bQFileDialog::get\w+\b"
    r"|\bQInputDialog::get\w+\b"
    r"|\bQColorDialog::get\w+\b"
    r"|\bQFontDialog::get\w+\b"
    r"|\bshow(?:Warning|Information|Question|Critical|Error)Logged\b"
)
CONNECT = re.compile(
    r"\bconnect\(\s*(?P<sender>[^,]+?),\s*&\s*(?P<type>QPushButton|QToolButton|QAbstractButton|"
    r"QCommandLinkButton|QAction)::(?P<signal>clicked|triggered)\b"
)
FUNCTION = re.compile(
    r"^(?:[\w:<>\*&,]+[ \t\*&]+)*(?:\w+::)*(?P<name>\w+)\s*\([^;{}]*\)\s*(?:const\s*)?(?:noexcept\s*)?\{",
    re.M,
)
SLOT = re.compile(r"&\s*(?:\w+::)*(?P<name>\w+)\s*(?:,\s*Qt::\w+\s*)?\)$")
CALL = re.compile(r"\b(\w+)\s*\(")
NOT_FUNCTIONS = frozenset({"if", "for", "while", "switch", "return", "catch", "sizeof"})
MAX_DEPTH = 3


def block_end(text: str, start: int, opener: str, closer: str) -> int:
    """Index just past the close that balances an opener already passed at start."""
    depth = 1
    index = start
    while index < len(text) and depth > 0:
        character = text[index]
        depth += character == opener
        depth -= character == closer
        index += 1
    return index


def strip_comments_and_strings(text: str) -> str:
    """Blanks comments and string contents, keeping every offset where it was."""
    def blank(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    pattern = r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\''
    return re.sub(pattern, blank, text, flags=re.S)


def function_bodies(text: str) -> dict[str, list[str]]:
    bodies: dict[str, list[str]] = {}
    for match in FUNCTION.finditer(text):
        name = match.group("name")
        if name in NOT_FUNCTIONS:
            continue
        bodies.setdefault(name, []).append(text[match.end():block_end(text, match.end(), "{", "}")])
    return bodies


def reaches_dialog(text: str, bodies: dict[str, list[str]], seen: set[str], depth: int) -> str:
    """The first dialog-opening call this code reaches, by name, or an empty string."""
    direct = MODAL_CALL.search(text)
    if direct is not None:
        return direct.group(0)
    if depth >= MAX_DEPTH:
        return ""
    for name in set(CALL.findall(text)):
        if name in seen or name not in bodies:
            continue
        seen.add(name)
        for body in bodies[name]:
            found = reaches_dialog(body, bodies, seen, depth + 1)
            if found:
                return found
    return ""


def press_handler_opens_dialog(call: str, bodies: dict[str, list[str]]) -> str:
    slot = SLOT.search(call)
    if "[" not in call and slot is not None:
        for body in bodies.get(slot.group("name"), []):
            found = reaches_dialog(body, bodies, {slot.group("name")}, 1)
            if found:
                return found
        return ""
    return reaches_dialog(call, bodies, set(), 1)


def scan_file(path: Path, text: str, bodies: dict[str, list[str]]) -> list[dict[str, object]]:
    findings: list[dict[str, object]] = []
    for match in CONNECT.finditer(text):
        end = block_end(text, match.end(), "(", ")")
        call = text[match.start():end]
        if "QueuedConnection" in call:
            continue
        reached = press_handler_opens_dialog(call, bodies)
        if reached:
            findings.append(
                {
                    "path": path,
                    "line": text.count("\n", 0, match.start()) + 1,
                    "sender": match.group("sender").strip(),
                    "reached": reached.strip(),
                    "start": match.start(),
                    "end": end,
                }
            )
    return findings


def scan(root: Path) -> list[dict[str, object]]:
    """Every press handler under root/src/gui that opens a dialog before its press returns."""
    sources = sorted((root / "src" / "gui").glob("*.cpp"))
    if not sources:
        raise SystemExit(f"No GUI sources found under {root}")
    texts = {path: strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
             for path in sources}
    everywhere: dict[str, list[str]] = {}
    for text in texts.values():
        for name, found in function_bodies(text).items():
            everywhere.setdefault(name, []).extend(found)
    findings: list[dict[str, object]] = []
    for path, text in texts.items():
        findings.extend(scan_file(path, text, everywhere))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    arguments = parser.parse_args()
    findings = scan(arguments.root)
    for finding in findings:
        relative = Path(finding["path"]).relative_to(arguments.root).as_posix()
        print(
            f"{relative}:{finding['line']}: {finding['sender']} opens a dialog inside its press "
            f"({finding['reached']}). Add Qt::QueuedConnection."
        )
    if findings:
        print(f"{len(findings)} press handler(s) would block a screen reader.")
        return 1
    print("No button or menu action opens a dialog inside its press.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
