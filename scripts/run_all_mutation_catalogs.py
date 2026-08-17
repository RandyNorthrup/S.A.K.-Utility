#!/usr/bin/env python3
"""Mutation-catalog coverage ratchet (R5-G18-1).

The per-catalog harness (run_mutation_test.py) proves that ONE catalog's
mutations are all detected. This wrapper is the ratchet over the whole SET of
catalogs, in two modes:

  --validate   FAST, no build. For every catalog it checks the schema, that each
               find-string still occurs EXACTLY once in the current source, and
               that the manifest and the on-disk catalog set agree. This is what
               a pre-commit hook runs on every commit: it catches a catalog that
               has silently ROTTED -- a source refactor that moves or renames the
               mutated text turns the mutation into a no-op that "passes" while
               testing nothing, and a deleted catalog silently drops coverage.
               Both fail closed here.

  (default)    FULL run. After the fast checks pass it invokes run_mutation_test
               on each catalog in turn and aggregates the verdicts. Rebuilding a
               target per mutant is slow (minutes per catalog), so this is the
               on-demand / CI gate, not a per-commit hook.

The manifest (scripts/mutation_catalogs/MANIFEST.txt, one catalog basename per
line, '#' comments allowed) is the ratchet's memory: coverage can only grow.
Adding a catalog file without listing it fails closed (forces registration);
deleting a listed catalog fails closed (coverage regressed). The set on disk
must equal the set in the manifest, exactly.

Usage:
    python scripts/run_all_mutation_catalogs.py            # full run (slow)
    python scripts/run_all_mutation_catalogs.py --validate # fast integrity only

Exit codes (match run_mutation_test.py): 0 = PASS, 1 = a surviving hole in a
full run, 2 = a structural/integrity failure (ratchet, schema, stale find).
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CATALOG_DIR = REPO_ROOT / "scripts" / "mutation_catalogs"
MANIFEST_PATH = CATALOG_DIR / "MANIFEST.txt"


def _read_source(path: Path) -> str:
    """Read a source file preserving exact bytes/CRLF, matching run_mutation_test
    (newline='') so the find-count here sees the identical text the harness will."""
    with open(path, "r", encoding="utf-8", newline="") as fh:
        return fh.read()


def _manifest_entries() -> list[str]:
    """The catalog basenames the manifest declares (blank/'#' lines dropped)."""
    lines = MANIFEST_PATH.read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    for line in lines:
        stripped = line.strip()
        if stripped and not stripped.startswith("#"):
            out.append(stripped)
    return out


def _check_ratchet() -> list[str]:
    """The manifest set and the on-disk *.json set must match exactly. Returns a
    list of human-readable problems (empty == the ratchet holds)."""
    problems: list[str] = []
    if not MANIFEST_PATH.is_file():
        return [f"manifest not found: {MANIFEST_PATH}"]
    declared = set(_manifest_entries())
    present = {p.name for p in CATALOG_DIR.glob("*.json")}
    for name in sorted(declared - present):
        problems.append(f"manifest lists '{name}' but the catalog file is missing "
                        f"(coverage regressed -- restore it or drop it deliberately)")
    for name in sorted(present - declared):
        problems.append(f"catalog '{name}' exists but is not in MANIFEST.txt "
                        f"(register it so the ratchet tracks it)")
    return problems


def _validate_mutant(mutant: dict, source_text: str) -> str | None:
    """Validate one mutant against its (already-read) source. Returns an error
    string, or None if the mutant is well-formed and its find-string is unique."""
    for key in ("id", "file", "find", "replace", "expect"):
        if key not in mutant:
            return f"mutant missing required key '{key}'"
    if mutant["expect"] not in ("killed", "equivalent"):
        return f"mutant {mutant['id']}: expect must be 'killed' or 'equivalent'"
    if mutant["expect"] == "equivalent" and not mutant.get("why", "").strip():
        return f"mutant {mutant['id']}: an equivalent mutant requires a 'why' rationale"
    count = source_text.count(mutant["find"])
    if count != 1:
        return (f"mutant {mutant['id']}: find-string occurs {count} times in "
                f"{mutant['file']} (must be exactly 1 -- catalog has gone stale)")
    return None


def _validate_catalog(catalog_path: Path) -> list[str]:
    """Structurally validate one catalog. Returns a list of problems (empty ==
    valid). Does not build or run anything."""
    problems: list[str] = []
    try:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        return [f"{catalog_path.name}: cannot parse: {exc}"]
    for key in ("build_dir", "config", "target", "ctest_regex", "mutants"):
        if key not in catalog:
            problems.append(f"{catalog_path.name}: missing top-level key '{key}'")
    mutants = catalog.get("mutants") or []
    if not mutants:
        problems.append(f"{catalog_path.name}: no mutants declared")
    sources: dict[str, str] = {}
    for mutant in mutants:
        rel = mutant.get("file", "")
        if rel not in sources:
            src_path = REPO_ROOT / rel
            if not src_path.is_file():
                problems.append(f"{catalog_path.name}: source not found: {rel}")
                continue
            sources[rel] = _read_source(src_path)
        err = _validate_mutant(mutant, sources[rel])
        if err:
            problems.append(f"{catalog_path.name}: {err}")
    return problems


def _validate_all(catalogs: list[Path]) -> int:
    """Run the ratchet + per-catalog structural validation. Exit 0 or 2."""
    problems = _check_ratchet()
    for catalog_path in catalogs:
        catalog_problems = _validate_catalog(catalog_path)
        if catalog_problems:
            problems.extend(catalog_problems)
        else:
            print(f"OK    {catalog_path.name}")
    if problems:
        print("\n=== mutation-catalog integrity FAILURES ===")
        for problem in problems:
            print(f"  {problem}")
        return 2
    print(f"\nMUTATION CATALOG INTEGRITY: PASS ({len(catalogs)} catalogs, ratchet holds)")
    return 0


def _run_full(catalogs: list[Path]) -> int:
    """Fast checks, then invoke the harness on each catalog. Aggregate verdicts."""
    validate_rc = _validate_all(catalogs)
    if validate_rc != 0:
        print("\nStructural validation failed; not running the (slow) mutation pass.")
        return validate_rc
    worst = 0
    summary: list[tuple[str, int]] = []
    for catalog_path in catalogs:
        print(f"\n########## {catalog_path.name} ##########")
        rel = catalog_path.relative_to(REPO_ROOT).as_posix()
        rc = subprocess.run(
            [sys.executable, "scripts/run_mutation_test.py", rel], cwd=REPO_ROOT
        ).returncode
        summary.append((catalog_path.name, rc))
        worst = max(worst, rc)
    print("\n=== aggregate summary ===")
    for name, rc in summary:
        verdict = "PASS" if rc == 0 else ("HOLE" if rc == 1 else "ERROR")
        print(f"  {verdict:<6} {name}")
    if worst == 0:
        print(f"\nALL CATALOGS PASS ({len(catalogs)} catalogs)")
    else:
        print(f"\nMUTATION RATCHET FAIL (worst exit {worst})")
    return worst


def main() -> int:
    args = sys.argv[1:]
    validate_only = False
    if args == ["--validate"]:
        validate_only = True
    elif args:
        print(__doc__)
        return 2
    if not CATALOG_DIR.is_dir():
        print(f"ERROR: catalog directory not found: {CATALOG_DIR}")
        return 2
    catalogs = sorted(CATALOG_DIR.glob("*.json"))
    if not catalogs:
        print(f"ERROR: no catalogs in {CATALOG_DIR}")
        return 2
    if validate_only:
        return _validate_all(catalogs)
    return _run_full(catalogs)


if __name__ == "__main__":
    sys.exit(main())
