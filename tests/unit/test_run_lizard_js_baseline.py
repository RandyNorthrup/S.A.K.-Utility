"""Regression tests for the JavaScript complexity baseline ratchet."""

import contextlib
import importlib.util
import io
import re
import tempfile
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

    def test_javascript_parse_desync_is_rejected(self) -> None:
        source = """function downloadOptions(rawArgs) {
  const args = rawArgs || {};
  const url = args.url ? String(args.url) : "";
  if (!/^https?:/i.test(url)) {
    return { error: "bad url" };
  }
  const opts = { url };
  if (typeof args.filename === "string" && args.filename.trim().length > 0) {
    const fn = args.filename.trim();
    if (/^([a-zA-Z]:|\\\\|\\/)/.test(fn) || fn.indexOf("..") >= 0) {
      return { error: "bad filename" };
    }
    opts.filename = fn;
  }
  const timeoutMs = Number.isFinite(args.timeout_ms) ? args.timeout_ms : 30000;
  return { opts, timeoutMs: Math.min(120000, Math.max(1000, timeoutMs)) };
}
function downloadReply() { return true; }
async function handleDownload() { return false; }
function fetchStateReply() { return true; }
function httpAuthCredentials() { return true; }
function httpAuthOriginError() { return true; }
async function resolveTabOrigin() { return true; }
async function handleHttpAuth() { return true; }
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trigger.js"
            path.write_text(source, encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                status = RUN_LIZARD.check_javascript_parse_desync([str(path)])
        self.assertEqual(status, 1)
        self.assertIn("functions UNMEASURED", output.getvalue())

    def test_precommit_lizard_hook_includes_javascript(self) -> None:
        config = (PROJECT_ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        start = config.index("- id: lizard-complexity")
        end = config.index("\n      # -------------------------------------------------------------------", start)
        hook = config[start:end]
        self.assertRegex(hook, r"types_or:\s*\[[^\]]*\bjavascript\b")
        self.assertRegex(hook, re.compile(r"files:.*\bjs\b"))


if __name__ == "__main__":
    unittest.main()
