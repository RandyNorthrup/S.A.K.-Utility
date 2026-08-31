#!/usr/bin/env python3
"""Executable contract for exact Qt-slot cppcheck classification."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[2]
CLASSIFIER_PATH = PROJECT_ROOT / "scripts" / "classify_cppcheck_qt_slots.py"
SPEC = importlib.util.spec_from_file_location("cppcheck_qt_slots", CLASSIFIER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load classifier: {CLASSIFIER_PATH}")
CLASSIFIER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CLASSIFIER)


def finding(path: Path, line: int, member: str) -> str:
    return (
        f"{path}:{line}:10: style: Either there is a missing 'override', or the member "
        f"function '{member}' can be static. [functionStatic]\n"
    )


def line_of(source: str, needle: str) -> int:
    return next(
        index for index, line in enumerate(source.splitlines(), start=1) if needle in line
    )


class QtSlotClassifierTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "tests" / "unit").mkdir(parents=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write_test_source(self, name: str, source: str) -> Path:
        path = self.root / "tests" / "unit" / name
        path.write_text(source, encoding="utf-8")
        return path

    def classify_one(self, diagnostic: str) -> tuple[list[str], int, int]:
        return CLASSIFIER.classify([diagnostic], self.root)

    def test_new_out_of_class_qt_slot_is_classified_from_live_source(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
private:
    void helper();
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_example.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::cleanup"), "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [])
        self.assertEqual(classified, 1)
        self.assertEqual(remaining, 0)

    def test_namespaced_inline_slot_survives_literals_and_digit_separators(self) -> None:
        source = '''const char* noise = R"tag({ not code })tag";
namespace demo {
class NamespacedTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup() { const int timeout = 600'000; }
};
}
'''
        path = self.write_test_source("test_namespaced.cpp", source)
        diagnostic = finding(
            path,
            line_of(source, "void cleanup"),
            "demo::NamespacedTest::cleanup",
        )
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [])
        self.assertEqual(classified, 1)
        self.assertEqual(remaining, 0)

    def test_ordinary_helper_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void testCase();
private:
    int helper();
};

int ExampleTest::helper() { return 1; }
"""
        path = self.write_test_source("test_helper.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::helper"), "ExampleTest::helper")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_stale_definition_line_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_stale.cpp", source)
        stale_line = line_of(source, "ExampleTest::cleanup") - 1
        diagnostic = finding(path, stale_line, "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_ambiguous_duplicate_slot_declaration_remains_actionable(self) -> None:
        source = """class ExampleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup();
    void cleanup(int value);
};

void ExampleTest::cleanup() {}
"""
        path = self.write_test_source("test_duplicate.cpp", source)
        diagnostic = finding(path, line_of(source, "ExampleTest::cleanup"), "ExampleTest::cleanup")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_non_test_path_remains_actionable(self) -> None:
        source = """class Product : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void callback();
};
void Product::callback() {}
"""
        product_dir = self.root / "src"
        product_dir.mkdir()
        path = product_dir / "product.cpp"
        path.write_text(source, encoding="utf-8")
        diagnostic = finding(path, line_of(source, "Product::callback"), "Product::callback")
        actionable, classified, remaining = self.classify_one(diagnostic)
        self.assertEqual(actionable, [diagnostic])
        self.assertEqual(classified, 0)
        self.assertEqual(remaining, 1)

    def test_repository_has_no_broad_function_static_silence(self) -> None:
        suppressions = (PROJECT_ROOT / "cppcheck_suppressions.txt").read_text(encoding="ascii")
        runner = (PROJECT_ROOT / "scripts" / "run_cppcheck.ps1").read_text(encoding="ascii")
        self.assertNotIn("functionStatic:*tests*", suppressions)
        self.assertIn("classify_cppcheck_qt_slots.py", runner)
        self.assertIn("Invoke-CapturedProcess", runner)
        self.assertNotIn("Start-Process -FilePath $CppcheckExe", runner)


if __name__ == "__main__":
    unittest.main()
