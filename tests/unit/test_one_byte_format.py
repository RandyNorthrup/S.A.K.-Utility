"""Every size the application shows comes from sak::formatBytes.

Review 4 (Y10, X7): beside the two styles format_utils.h named, seven more formatters had been
written in panels and dialogs, and they disagreed. The AI page printed "(3 KB)" by integer
division beside "(527 B)", the backup wizard "4.9 GB" and "0.0 GB" for profiles the restore showed
as "4.88 GB" and "252.1 KB", and Advanced Search's metadata "242 bytes" beside its list's "242 B".
There is one style now, in include/sak/format_utils.h.

This reads every string literal under src/ and include/, comments left out, and refuses a literal
that puts a byte unit after a placeholder ("%1 MB") or after a number built in code (" MB"), which
is how a size is formatted by hand. Rates ("MB/s") are not sizes, and a spin box's suffix labels
the field. The few messages that name a setting in the unit it is set in are listed below, each
with what it is.
"""

from __future__ import annotations

from pathlib import Path
import re
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
CODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S,
)
# A C++14 digit separator (1'048'576) is not the start of a character literal.
DIGIT_SEPARATOR = re.compile(r"(?<=[0-9A-Fa-f])'(?=[0-9A-Fa-f])")
UNIT = r"(?:B|KB|MB|GB|TB|KiB|MiB|GiB|TiB)\b(?![/-])"
FORMATTED_SIZE = re.compile(r"%\d+ ?" + UNIT + r'|^"\s' + UNIT)
SOURCE_SUFFIXES = (".cpp", ".h")
THE_FORMATTER = "include/sak/format_utils.h"

# Settings shown in the unit the technician sets them in, not sizes formatted by hand.
SETTINGS_IN_THEIR_UNIT = {
    '"LAN transfer block size must be between 1 and %1 KB"': "the block-size field is in KB",
    '"Starting LAN transfer test to %1:%2 (%3s, %4 KB blocks)"': "the block-size field is in KB",
    '"The large-drive threshold (%1 GB) could not be applied, so every "':
        "the threshold field is in GB",
    '"The following target is larger than the %1 GB threshold:"': "the threshold field is in GB",
    '"The following targets are larger than the %1 GB threshold:"':
        "the threshold field is in GB",
    '"split into %2 MB parts"': "the split size is a whole number of MB, as DISM is given it",
    '"Create %1 MB %2 %3 partition labeled \\"%4\\"."': "the size field is in MB",
    '"File limit: %1 MB"': "the file-limit field is in MB",
    '" (hash of first %1 MB only)"': "the hashing cap is a whole number of MB",
}


def hand_formatted_sizes(root: Path) -> list[str]:
    """Every string literal under root's src/ and include/ that formats a size by hand."""
    found: list[str] = []
    for top in ("src", "include"):
        for path in sorted((root / top).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if relative == THE_FORMATTER:
                continue
            text = DIGIT_SEPARATOR.sub("_", path.read_text(encoding="utf-8", errors="replace"))
            for match in CODE.finditer(text):
                literal = match.group(0)
                if not literal.startswith('"') or not FORMATTED_SIZE.search(literal):
                    continue
                line_start = text.rfind("\n", 0, match.start()) + 1
                line_end = text.find("\n", match.end())
                if "setSuffix" in text[line_start:line_end] or literal in SETTINGS_IN_THEIR_UNIT:
                    continue
                line = text.count("\n", 0, match.start()) + 1
                found.append(f"{relative}:{line}: {literal}")
    return found


class OneByteFormatTests(unittest.TestCase):
    def scan(self, source: str, name: str = "panel.cpp") -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "src" / name).write_text(source, encoding="utf-8")
            return hand_formatted_sizes(root)

    def test_a_placeholder_with_a_unit_is_named(self) -> None:
        found = self.scan('void f() {\n    label(QString("%1 GB").arg(gb, 0, \'f\', 1));\n}\n')
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("src/panel.cpp:2:"), found[0])

    def test_every_unit_is_named(self) -> None:
        for literal in ('"%1 B"', '"(%2 KB)"', '"%3MB"', '"%1 TiB free"', '" MB)"'):
            with self.subTest(literal=literal):
                self.assertEqual(len(self.scan(f"auto x = {literal};\n")), 1)

    def test_a_rate_is_not_a_size(self) -> None:
        self.assertEqual(self.scan('auto x = QString("%1 MB/s");\n'), [])

    def test_a_b_tree_is_not_a_size(self) -> None:
        self.assertEqual(self.scan('auto x = QString("HFS+ %1 B-tree header");\n'), [])

    def test_a_field_suffix_is_not_a_size(self) -> None:
        self.assertEqual(self.scan('spin->setSuffix(tr(" MB"));\n'), [])

    def test_a_comment_is_not_shown(self) -> None:
        self.assertEqual(self.scan('// printed "(3 KB)"\n/* "%1 GB" */ int x;\n'), [])

    def test_a_digit_separator_does_not_hide_a_literal(self) -> None:
        found = self.scan('auto n = 1\'024; auto s = QString("%1 KB").arg(n);\n')
        self.assertEqual(len(found), 1)

    def test_the_formatter_itself_is_allowed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "include" / "sak").mkdir(parents=True)
            (root / "include" / "sak" / "format_utils.h").write_text(
                'auto x = QStringLiteral("%1 GB");\n', encoding="utf-8")
            self.assertEqual(hand_formatted_sizes(root), [])

    def test_this_tree_has_none(self) -> None:
        found = hand_formatted_sizes(REPO)
        self.assertEqual(found, [], "\n".join(found))


if __name__ == "__main__":
    unittest.main()
