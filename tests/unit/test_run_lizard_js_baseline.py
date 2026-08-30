"""Regression tests for the JavaScript complexity baseline ratchet."""

import contextlib
import importlib.util
import io
import unittest
from pathlib import Path
from unittest import mock


PROJECT_ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = PROJECT_ROOT / "scripts" / "run_lizard.py"
SPEC = importlib.util.spec_from_file_location("sak_run_lizard", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"Could not load {MODULE_PATH}")
RUN_LIZARD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_LIZARD)


class JavascriptBaselineTests(unittest.TestCase):
    def test_violation_path_is_canonical(self) -> None:
        line = (
            "./browser/extension/background.js:10: warning: handleWaitFor has "
            "20 NLOC, 12 CCN, 50 token, 2 PARAM, 38 length"
        )
        parsed = RUN_LIZARD.parse_violation(line)
        self.assertIsNotNone(parsed)
        self.assertEqual(
            parsed[0], "handleWaitFor@browser/extension/background.js")

    def test_explicit_file_scope_requires_stale_row_removal(self) -> None:
        baseline = {
            "handleWaitFor@browser/extension/background.js": (12, 2, 38),
            "other@browser/extension/other.js": (11, 0, 20),
        }
        with mock.patch.object(RUN_LIZARD, "load_baseline", return_value=baseline), \
                mock.patch.object(RUN_LIZARD, "run_lizard", return_value=[]), \
                contextlib.redirect_stdout(io.StringIO()):
            status = RUN_LIZARD.check_javascript(
                ["browser/extension/background.js"])
        self.assertEqual(status, 1)

    def test_unscanned_baseline_rows_are_not_called_stale(self) -> None:
        baseline = {"other@browser/extension/other.js": (11, 0, 20)}
        with mock.patch.object(RUN_LIZARD, "load_baseline", return_value=baseline), \
                mock.patch.object(RUN_LIZARD, "run_lizard", return_value=[]), \
                contextlib.redirect_stdout(io.StringIO()):
            status = RUN_LIZARD.check_javascript(
                ["browser/extension/background.js"])
        self.assertEqual(status, 0)

    def test_missing_explicit_source_fails_closed(self) -> None:
        with self.assertRaises(SystemExit) as raised:
            RUN_LIZARD.assert_scan_is_not_empty(
                ["browser/extension/definitely_missing.js"],
                "javascript",
                RUN_LIZARD.JS_DIRS,
                RUN_LIZARD.JS_EXTENSIONS,
            )
        self.assertIn("refusing to certify a partial scan", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
