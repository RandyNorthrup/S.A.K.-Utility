"""The application spells "Wi-Fi" and "Ethernet" one way in what it shows.

Review 4 (Y11, Y19): "WiFi" stayed on the restore welcome page, the Network Management tabs, the
adapter table's Type column and a dozen messages beside "Wi-Fi" elsewhere; and the backup and
restore pages that keep every adapter's IP settings called them "ethernet adapters", lowercase,
counting Wi-Fi and Bluetooth adapters among them.

This reads every string literal under src/ and include/, comments left out. "WiFi" on its own is
the adapter type the data keeps (JSON exports, the AI tools, comparisons); sak::adapterTypeLabel
shows it as "Wi-Fi". A Settings page address ("ms-settings:network-ethernet") is not wording.
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
DIGIT_SEPARATOR = re.compile(r"(?<=[0-9A-Fa-f])'(?=[0-9A-Fa-f])")
WIFI = re.compile(r"\bWiFi\b")
LOWERCASE_ETHERNET = re.compile(r"(?<![-:])\bethernet\b")
ADAPTER_TYPE_TOKEN = '"WiFi"'
SOURCE_SUFFIXES = (".cpp", ".h")


def misspellings(root: Path) -> list[str]:
    """Every string literal under root's src/ and include/ with "WiFi" or "ethernet" in it."""
    found: list[str] = []
    for top in ("src", "include"):
        for path in sorted((root / top).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = DIGIT_SEPARATOR.sub("_", path.read_text(encoding="utf-8", errors="replace"))
            for match in CODE.finditer(text):
                literal = match.group(0)
                if not literal.startswith('"'):
                    continue
                wifi = literal != ADAPTER_TYPE_TOKEN and WIFI.search(literal)
                if wifi or LOWERCASE_ETHERNET.search(literal):
                    line = text.count("\n", 0, match.start()) + 1
                    found.append(f"{path.relative_to(root).as_posix()}:{line}: {literal}")
    return found


class UserTextSpellingTests(unittest.TestCase):
    def scan(self, source: str) -> list[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src").mkdir()
            (root / "src" / "panel.cpp").write_text(source, encoding="utf-8")
            return misspellings(root)

    def test_wifi_in_a_message_is_named(self) -> None:
        found = self.scan('void f() {\n    status(tr("Scanning WiFi networks..."));\n}\n')
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].startswith("src/panel.cpp:2:"), found[0])

    def test_lowercase_ethernet_is_named(self) -> None:
        self.assertEqual(len(self.scan('auto x = tr("Scanning ethernet adapters...");\n')), 1)

    def test_the_adapter_type_token_is_data(self) -> None:
        self.assertEqual(self.scan('bool w = type == QStringLiteral("WiFi");\n'), [])

    def test_a_settings_address_is_not_wording(self) -> None:
        self.assertEqual(self.scan('auto x = QStringLiteral("ms-settings:network-ethernet");\n'), [])

    def test_the_right_spellings_pass(self) -> None:
        self.assertEqual(self.scan('auto x = tr("Wi-Fi and Ethernet adapters");\n'), [])

    def test_a_comment_and_an_identifier_are_not_wording(self) -> None:
        self.assertEqual(self.scan('// "WiFi" in a comment\nvoid createWiFiTab();\n'), [])

    def test_this_tree_has_none(self) -> None:
        found = misspellings(REPO)
        self.assertEqual(found, [], "\n".join(found))


if __name__ == "__main__":
    unittest.main()
