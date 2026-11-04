"""What scripts/check_dialog_buttons_deferred.py catches, on a planted source tree."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_dialog_buttons_deferred.py"
SPEC = importlib.util.spec_from_file_location("check_dialog_buttons_deferred", SCRIPT)
GATE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = GATE
SPEC.loader.exec_module(GATE)


class DialogButtonsDeferredTests(unittest.TestCase):
    def scan(self, source: str) -> list[dict[str, object]]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gui = root / "src" / "gui"
            gui.mkdir(parents=True)
            (gui / "panel.cpp").write_text(source, encoding="utf-8")
            return GATE.scan(root)

    def test_a_slot_that_opens_a_dialog_is_named(self) -> None:
        found = self.scan(
            "void Panel::setup() {\n"
            "    connect(m_button, &QPushButton::clicked, this, &Panel::onPress);\n"
            "}\n"
            "void Panel::onPress() {\n"
            "    QDialog dialog;\n"
            "    dialog.exec();\n"
            "}\n"
        )
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0]["sender"], "m_button")
        self.assertEqual(found[0]["line"], 2)

    def test_a_queued_connection_is_fine(self) -> None:
        self.assertEqual(
            self.scan(
                "void Panel::setup() {\n"
                "    connect(m_button, &QPushButton::clicked, this, &Panel::onPress,\n"
                "            Qt::QueuedConnection);\n"
                "}\n"
                "void Panel::onPress() { QDialog d; d.exec(); }\n"
            ),
            [],
        )

    def test_a_lambda_that_opens_a_dialog_is_named(self) -> None:
        found = self.scan(
            "void Panel::setup() {\n"
            "    connect(m_button, &QToolButton::clicked, this, [this]() {\n"
            '        QMessageBox::warning(this, "t", "m");\n'
            "    });\n"
            "}\n"
        )
        self.assertEqual(len(found), 1)

    def test_a_dialog_a_few_calls_down_is_found(self) -> None:
        found = self.scan(
            "void Panel::setup() {\n"
            "    connect(m_action, &QAction::triggered, this, &Panel::onPress);\n"
            "}\n"
            "void Panel::onPress() { first(); }\n"
            "void Panel::first() { second(); }\n"
            "QString second() { return QFileDialog::getOpenFileName(); }\n"
        )
        self.assertEqual(len(found), 1)

    def test_a_press_that_opens_nothing_is_left_alone(self) -> None:
        self.assertEqual(
            self.scan(
                "void Panel::setup() {\n"
                "    connect(m_button, &QPushButton::clicked, this, &Panel::onPress);\n"
                "}\n"
                "void Panel::onPress() { refresh(); }\n"
                "void Panel::refresh() {}\n"
            ),
            [],
        )

    def test_a_comment_or_a_string_is_not_a_dialog(self) -> None:
        self.assertEqual(
            self.scan(
                "void Panel::setup() {\n"
                "    connect(m_button, &QPushButton::clicked, this, &Panel::onPress);\n"
                "}\n"
                "void Panel::onPress() {\n"
                "    // dialog.exec() used to be here\n"
                '    log("QMessageBox::warning");\n'
                "}\n"
            ),
            [],
        )

    def test_another_signal_is_not_a_press(self) -> None:
        self.assertEqual(
            self.scan(
                "void Panel::setup() {\n"
                "    connect(m_box, &QComboBox::activated, this, &Panel::onPick);\n"
                "}\n"
                "void Panel::onPick() { QDialog d; d.exec(); }\n"
            ),
            [],
        )

    def test_this_tree_has_none(self) -> None:
        self.assertEqual(GATE.scan(SCRIPT.parents[1]), [])


if __name__ == "__main__":
    unittest.main()
