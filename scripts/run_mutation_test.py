#!/usr/bin/env python3
"""Mutation-test harness (R5-G18-1).

Applies a catalog of single-site source mutations one at a time, rebuilds the
covering test target, runs it, and classifies each mutant:

  KILLED     the test target FAILED (or the mutant did not compile) -- the suite
             detected the change, which is what we want.
  SURVIVED   the test target still PASSED with the code deliberately broken -- the
             suite cannot tell the mutant from the original. Unless the mutant is
             declared behaviourally EQUIVALENT (same output for every input, with a
             written rationale), a survivor is a hole in the coverage.

A surviving mutant that was expected to be killed makes the harness exit non-zero,
so once a catalog is clean it can be wired into CI as a coverage ratchet.

Usage:
    python scripts/run_mutation_test.py scripts/mutation_catalogs/<catalog>.json

Catalog schema (JSON):
    {
      "build_dir":   "build",
      "config":      "Release",
      "target":      "test_linux_distro_catalog",
      "ctest_regex": "^test_linux_distro_catalog$",
      "mutants": [
        {"id": "...", "file": "src/core/foo.cpp",
         "find": "<exact source text, must occur EXACTLY once>",
         "replace": "<the mutated text>",
         "expect": "killed" | "equivalent",
         "why": "<rationale, required for equivalent>"}
      ]
    }

The original bytes of every touched file are snapshotted up front and always
restored -- on success, on a caught error, and on Ctrl-C -- so the working tree is
never left mutated. This edits tracked sources in place while it runs; do not run
it concurrently with a build you care about.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def _read(path: Path) -> str:
    """Read a source file preserving its exact bytes, including CRLF line endings
    (newline='' disables newline translation), so a mutate-then-restore round-trip
    leaves the working tree byte-identical rather than silently normalising EOLs."""
    with open(path, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def _write(path: Path, text: str) -> None:
    """Write a source file without translating line endings (see _read)."""
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)


def _run(cmd: list[str]) -> int:
    """Run a command from the repo root, discarding its output, return its code."""
    return subprocess.run(
        cmd, cwd=REPO_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    ).returncode


def _build(build_dir: str, config: str, target: str) -> bool:
    """Build one target. True on success."""
    return _run(["cmake", "--build", build_dir, "--config", config, "--target", target]) == 0


def _test_passes(build_dir: str, config: str, regex: str) -> bool:
    """Run the covering test(s). True iff every selected test passed."""
    return _run(["ctest", "--test-dir", build_dir, "-C", config, "-R", regex]) == 0


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    catalog_path = Path(sys.argv[1])
    if not catalog_path.is_absolute():
        catalog_path = REPO_ROOT / catalog_path
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))

    build_dir = catalog["build_dir"]
    config = catalog["config"]
    target = catalog["target"]
    regex = catalog["ctest_regex"]
    mutants = catalog["mutants"]

    # Snapshot every file this catalog will touch, so restore is total and safe.
    touched = sorted({m["file"] for m in mutants})
    originals: dict[str, str] = {}
    for rel in touched:
        p = REPO_ROOT / rel
        if not p.is_file():
            print(f"ERROR: source file not found: {rel}")
            return 2
        originals[rel] = _read(p)

    # Validate that each find-string occurs exactly once BEFORE mutating anything --
    # a zero or multi match would silently mutate the wrong site (or nothing).
    for m in mutants:
        text = originals[m["file"]]
        count = text.count(m["find"])
        if count != 1:
            print(f"ERROR: mutant {m['id']}: find-string occurs {count} times in "
                  f"{m['file']} (must be exactly 1). Fix the catalog.")
            return 2
        if m["expect"] not in ("killed", "equivalent"):
            print(f"ERROR: mutant {m['id']}: expect must be 'killed' or 'equivalent'.")
            return 2

    results: list[tuple[str, str, str]] = []  # (id, verdict, expect)
    holes: list[str] = []

    def restore_all() -> None:
        for rel, text in originals.items():
            _write(REPO_ROOT / rel, text)

    try:
        # Baseline: the unmutated tree must build and pass, or results are meaningless.
        print("=== baseline (unmutated) ===")
        if not _build(build_dir, config, target):
            print("ERROR: baseline build failed; cannot run mutation testing.")
            return 2
        if not _test_passes(build_dir, config, regex):
            print("ERROR: baseline test failed; cannot run mutation testing.")
            return 2
        print("baseline OK\n")

        for m in mutants:
            rel = m["file"]
            mutated = originals[rel].replace(m["find"], m["replace"], 1)
            _write(REPO_ROOT / rel, mutated)
            try:
                if not _build(build_dir, config, target):
                    verdict = "KILLED (build)"  # a non-compiling mutant is detected
                elif _test_passes(build_dir, config, regex):
                    verdict = "SURVIVED"
                else:
                    verdict = "KILLED"
            finally:
                _write(REPO_ROOT / rel, originals[rel])

            results.append((m["id"], verdict, m["expect"]))
            note = ""
            if verdict == "SURVIVED" and m["expect"] != "equivalent":
                holes.append(m["id"])
                note = "  <-- HOLE: suite does not detect this mutation"
            elif verdict == "SURVIVED" and m["expect"] == "equivalent":
                note = f"  (equivalent: {m['why']})"
            elif verdict.startswith("KILLED") and m["expect"] == "equivalent":
                note = "  (declared equivalent but a test distinguishes it -- fine)"
            print(f"{m['id']:<48} {verdict:<16}{note}")
    finally:
        restore_all()
        # Rebuild the target back to its pristine object so the tree is left green.
        _build(build_dir, config, target)

    print("\n=== summary ===")
    killed = sum(1 for _, v, _ in results if v.startswith("KILLED"))
    survived = sum(1 for _, v, _ in results if v == "SURVIVED")
    print(f"{len(results)} mutants: {killed} killed, {survived} survived "
          f"({len(holes)} unexpected holes)")
    if holes:
        print("HOLES (expected killed but survived): " + ", ".join(holes))
        return 1
    print("MUTATION RESULT: PASS (every non-equivalent mutant was killed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
