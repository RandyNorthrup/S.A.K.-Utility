"""No string the application shows says "file(s)".

UI review S24: "1 drive(s) available", "9 package(s)" and 146 more read as a placeholder nobody
filled in. Counts are spelled with sak::countOf ("1 file", "3 files"), and a label with no count
says what it means in plain words. The application loads no translations, so Qt's
tr("%n file(s)", "", n) shows "(s)" too.

This reads every string literal under src/ and include/, comments left out, so it answers for every
message the code can build: a runtime string is made of these literals. "http(s)" names a URL
scheme, not a plural, and is allowed.
"""

from __future__ import annotations

from pathlib import Path
import re
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]
TOKEN = re.compile(r"\w\((?:s|es|ies)\)")
URL_SCHEME = re.compile(r"\bhttp\(s\)")
CODE = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
    re.S,
)
SOURCE_SUFFIXES = (".cpp", ".h")


def placeholders(root: Path) -> list[str]:
    """Every string literal under root's src/ and include/ holding a plural placeholder."""
    found: list[str] = []
    for top in ("src", "include"):
        for path in sorted((root / top).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for match in CODE.finditer(text):
                literal = match.group(0)
                if literal.startswith('"') and TOKEN.search(URL_SCHEME.sub("", literal)):
                    line = text.count("\n", 0, match.start()) + 1
                    found.append(f"{path.relative_to(root).as_posix()}:{line}: {literal}")
    return found


class PluralPlaceholderTests(unittest.TestCase):
    def scan(self, source: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "src" / "panel.cpp").write_text(source, encoding="utf-8")
            return placeholders(root)

    def test_a_count_with_a_placeholder_is_named(self) -> None:
        found = self.scan('void f() {\n    status(tr("Found %1 file(s)").arg(n));\n}\n')
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("src/panel.cpp:2:"), found[0])

    def test_every_spelling_is_named(self) -> None:
        for literal in ('"%n item(s)"', '"%1 match(es)"', '"%1 director(ies)"', '"Drive(s):"'):
            with self.subTest(literal=literal):
                self.assertEqual(len(self.scan(f"auto x = {literal};\n")), 1)

    def test_a_literal_continued_on_the_next_line_is_read(self) -> None:
        found = self.scan('auto x = QStringLiteral("Scanned %1 "\n                         "drive(s)");\n')
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("src/panel.cpp:2:"), found[0])

    def test_a_url_scheme_is_not_a_plural(self) -> None:
        self.assertEqual(self.scan('auto x = QStringLiteral("needs an http(s) url");\n'), [])

    def test_a_comment_is_not_shown(self) -> None:
        self.assertEqual(
            self.scan('// "1 drive(s) available" read as a placeholder\n/* file(s) */ int x;\n'),
            [],
        )

    def test_real_plurals_pass(self) -> None:
        self.assertEqual(self.scan('auto x = countOf(n, tr("file"), tr("files"));\n'), [])

    def test_this_tree_has_none(self) -> None:
        found = placeholders(REPO)
        self.assertEqual(found, [], "\n".join(found))


if __name__ == "__main__":
    unittest.main()
