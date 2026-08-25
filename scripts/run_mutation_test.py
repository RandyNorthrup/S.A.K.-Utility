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

The converse is ALSO a failure, and used to be waved through as "fine". Declaring a
mutant equivalent is the claim that NO input distinguishes it from the original, so a
test that kills it is a PROOF that the written rationale is false -- and the rationale
is the only thing standing between "this mutation is provably harmless" and "nothing
tests this". Accepting a disproven claim silently is fail-open, and it hid a real one:
mbox_header_parser's value-extract-trim mutant was called redundant because the value
is trimmed again at insert, which ignored that whitespace left by the first trim is
carried INTO the fold join, where the outer trim can no longer reach it. It survived
only because no fixture folded a value with trailing whitespace. Both directions of
that mislabel were silent -- it survived while the corpus was thin, and once a fixture
reached it, the harness printed "fine". A MISLABELLED verdict now exits non-zero.

Usage:
    python scripts/run_mutation_test.py scripts/mutation_catalogs/<catalog>.json
    python scripts/run_mutation_test.py <catalog>.json --only-equivalents
    python scripts/run_mutation_test.py --recover

--only-equivalents runs just the mutants this catalog declares EQUIVALENT. A full
pass rebuilds the target once per mutant and takes minutes per catalog, but the
equivalence claims are a small minority of them and are the entries whose rationale
can rot silently as the suite grows -- a claim written when the corpus could not
reach the mutated site stays on the page unchallenged long after a new fixture does
reach it. Auditing only those is cheap enough to run whenever a suite gains coverage.

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

That restore is a finally block, which a HARD kill skips: a terminated run leaves a
deliberately broken production source sitting in the working tree, looking exactly
like an ordinary edit. Observed twice, and caught only because the operator happened
to run 'git status'. So the run is also ARMED on disk: the original bytes are written
to .mutation-snapshot/ and a .mutation-in-progress.json sentinel names the catalog,
the files at risk and the mutant currently applied. A clean exit clears both. If they
survive, the tree is presumed mutated and:

  - this harness REFUSES to start (a second run would snapshot the MUTATED bytes as
    if they were the original, making the damage permanent),
  - run_all_mutation_catalogs.py --validate fails, and that is the pre-commit hook,
    so a leftover mutation blocks the commit instead of riding along inside it,
  - 'python scripts/run_mutation_test.py --recover' restores from the snapshot.

Recovery restores the snapshot rather than telling you to run 'git checkout --',
because the snapshot is the true pre-run state: checkout would also silently discard
any uncommitted work that was in those files before the run started.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# On-disk record that a run is mid-flight. See the module docstring: the in-process
# restore cannot survive a hard kill, so the tree's true state is recorded here.
SENTINEL_PATH = REPO_ROOT / ".mutation-in-progress.json"
SNAPSHOT_DIR = REPO_ROOT / ".mutation-snapshot"


def _snapshot_path(rel: str) -> Path:
    """Flatten a repo-relative source path into one snapshot filename. '/' and '\\'
    both become '#' so nested sources cannot collide with a top-level one, and the
    snapshot directory stays flat (no directories to create or leave behind)."""
    return SNAPSHOT_DIR / (rel.replace("\\", "/").replace("/", "#"))


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


def _arm(catalog_name: str, originals: dict[str, str], mutant_id: str) -> None:
    """Record on disk that the tree is (about to be) mutated. Snapshots are written
    BEFORE the sentinel appears, so a sentinel always has a complete snapshot behind
    it -- the reverse order could advertise a recovery that does not exist."""
    SNAPSHOT_DIR.mkdir(exist_ok=True)
    for rel, text in originals.items():
        snap = _snapshot_path(rel)
        if not snap.is_file():
            _write(snap, text)
    payload = {
        "catalog": catalog_name,
        "pid": os.getpid(),
        "applied_mutant": mutant_id,
        "files": sorted(originals),
        "recover_with": "python scripts/run_mutation_test.py --recover",
    }
    SENTINEL_PATH.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _disarm() -> None:
    """Clear the on-disk record after the in-process restore has already run. The
    sentinel goes FIRST: if this is interrupted between the two, what is left is a
    stale snapshot with no sentinel, which is inert -- whereas a sentinel with no
    snapshot would block every later run with nothing to recover from."""
    SENTINEL_PATH.unlink(missing_ok=True)
    if SNAPSHOT_DIR.is_dir():
        for snap in SNAPSHOT_DIR.iterdir():
            snap.unlink()
        SNAPSHOT_DIR.rmdir()


def _refuse_if_armed() -> bool:
    """True (and explains) if a previous run left the tree presumed mutated. Starting
    anyway would snapshot the MUTATED bytes as the original and make the damage
    permanent, so this fails closed rather than assuming the tree is fine."""
    if not SENTINEL_PATH.is_file():
        return False
    print("ERROR: a previous mutation run did not finish. The working tree is "
          "presumed MUTATED -- one or more production sources still carry a "
          "deliberately broken edit.\n")
    print(SENTINEL_PATH.read_text(encoding="utf-8"))
    print("Refusing to start: mutating an already-mutated tree would snapshot the "
          "BROKEN bytes as the original and make the damage permanent.\n"
          "Restore the pre-run state with:\n"
          "    python scripts/run_mutation_test.py --recover")
    return True


def _discard_stale_snapshot() -> None:
    """Delete a snapshot directory left with NO sentinel beside it.

    _disarm removes the sentinel first, so an interruption between its two steps leaves
    exactly this shape. The sentinel is the authority on whether a mutation is in
    flight: without one, the tree is not mutated and these bytes are inert garbage --
    but they are DANGEROUS garbage, because _arm skips a snapshot that already exists,
    so a stale file would shadow the real original of a LATER run and recovery would
    restore the wrong bytes. Discard rather than trust."""
    if SENTINEL_PATH.is_file() or not SNAPSHOT_DIR.is_dir():
        return
    for snap in SNAPSHOT_DIR.iterdir():
        snap.unlink()
    SNAPSHOT_DIR.rmdir()


def _recover() -> int:
    """Restore every snapshotted file and clear the on-disk record."""
    if not SENTINEL_PATH.is_file():
        # No sentinel means no run was in flight, whatever else is lying around.
        if SNAPSHOT_DIR.is_dir():
            _discard_stale_snapshot()
            print("Nothing to recover: no mutation run is marked in progress. "
                  "Discarded a stale snapshot directory left with no sentinel.")
            return 0
        print("Nothing to recover: no mutation run is marked in progress.")
        return 0
    if not SNAPSHOT_DIR.is_dir():
        # Fail closed and loudly: the sentinel names files that were mutated, and the
        # bytes needed to undo that are gone. Say exactly which files to inspect.
        print(f"ERROR: {SENTINEL_PATH.name} exists but {SNAPSHOT_DIR.name}/ does not, "
              f"so the original bytes are unavailable. The files it names must be "
              f"restored by hand (git diff will show the single mutated line):\n")
        print(SENTINEL_PATH.read_text(encoding="utf-8"))
        return 2
    restored = 0
    for snap in sorted(SNAPSHOT_DIR.iterdir()):
        rel = snap.name.replace("#", "/")
        target = REPO_ROOT / rel
        if not target.is_file():
            print(f"ERROR: snapshot for '{rel}' has no file to restore to.")
            return 2
        if not _restore_file(target, _read(snap)):
            # Fail CLOSED and leave the sentinel and the snapshot exactly where they
            # are: the tree is still mutated, so the pre-commit hook must keep
            # refusing, and the bytes needed to finish the job must stay available.
            print(f"ERROR: could not write '{rel}' -- it is still locked by another "
                  f"process. Nothing has been lost: the snapshot is intact and the "
                  f"sentinel is still in place, so the commit stays blocked. Wait for "
                  f"the holding process to exit and run --recover again.")
            return 2
        print(f"restored {rel}")
        restored += 1
    _disarm()
    print(f"\nRECOVERED: {restored} file(s) restored to their pre-run state.")
    return 0


def _restore_file(path: Path, text: str, attempts: int = 30) -> bool:
    """Write @p text back to @p path, retrying while the file is LOCKED.

    A hard kill takes the harness down but not the compiler it spawned, and on Windows
    a cl.exe holding a source open for read blocks the write -- so the first recovery
    attempt after a kill raced a surviving build and died with a bare PermissionError,
    leaving the tree mutated. Found by the kill drill, not by reasoning. Retrying is
    correct rather than merely convenient: the lock is held by a process that is on its
    way out, and the snapshot is intact throughout, so waiting loses nothing. After the
    attempts are spent this returns False and the caller fails closed and says why --
    it never gives up quietly, and it never leaves a traceback as the explanation."""
    for attempt in range(attempts):
        try:
            _write(path, text)
            return True
        except PermissionError:
            if attempt == 0:
                print(f"  {path.name} is locked by another process (most likely a "
                      f"compiler left over from the interrupted run); waiting...")
            time.sleep(1)
    return False


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
    args = sys.argv[1:]
    if args == ["--recover"]:
        return _recover()
    only_equivalents = False
    if args and args[-1] == "--only-equivalents":
        only_equivalents = True
        args = args[:-1]
    if len(args) != 1:
        print(__doc__)
        return 2
    if _refuse_if_armed():
        return 2
    _discard_stale_snapshot()
    catalog_path = Path(args[0])
    if not catalog_path.is_absolute():
        catalog_path = REPO_ROOT / catalog_path
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))

    build_dir = catalog["build_dir"]
    config = catalog["config"]
    target = catalog["target"]
    regex = catalog["ctest_regex"]
    mutants = catalog["mutants"]
    if only_equivalents:
        # Filter AFTER reading the whole catalog, so the uniqueness/schema validation below
        # still runs over every entry: a stale find-string in a mutant this pass skips is
        # still a rotted catalog, and staying silent about it would trade one fail-open for
        # another. Only the build-and-run loop is narrowed.
        selected = [m for m in mutants if m["expect"] == "equivalent"]
        if not selected:
            print(f"{catalog_path.name}: no equivalence claims to audit.")
            return 0
    else:
        selected = mutants

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
    mislabelled: list[str] = []

    def restore_all() -> bool:
        """True iff every touched file is back to its original bytes. Uses the same
        lock-tolerant write as --recover: a compiler from the build this harness just
        ran can still hold a source open for a moment after cmake returns."""
        ok = True
        for rel, text in originals.items():
            if not _restore_file(REPO_ROOT / rel, text):
                print(f"ERROR: could not restore '{rel}' -- still locked. The tree is "
                      f"LEFT MUTATED; run --recover once the holding process exits.")
                ok = False
        return ok

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

        for m in selected:
            rel = m["file"]
            mutated = originals[rel].replace(m["find"], m["replace"], 1)
            # Arm BEFORE the write, and name the mutant, so a kill at any instant from
            # here on leaves a record that says exactly which edit is in the tree.
            _arm(catalog_path.name, originals, m["id"])
            _write(REPO_ROOT / rel, mutated)
            try:
                if not _build(build_dir, config, target):
                    verdict = "KILLED (build)"  # a non-compiling mutant is detected
                elif _test_passes(build_dir, config, regex):
                    verdict = "SURVIVED"
                else:
                    verdict = "KILLED"
            finally:
                _restore_file(REPO_ROOT / rel, originals[rel])

            results.append((m["id"], verdict, m["expect"]))
            note = ""
            if verdict == "SURVIVED" and m["expect"] != "equivalent":
                holes.append(m["id"])
                note = "  <-- HOLE: suite does not detect this mutation"
            elif verdict == "SURVIVED" and m["expect"] == "equivalent":
                note = f"  (equivalent: {m['why']})"
            elif verdict.startswith("KILLED") and m["expect"] == "equivalent":
                mislabelled.append(m["id"])
                note = ("  <-- MISLABELLED: declared equivalent, but a test DISTINGUISHES it, "
                        "which disproves the rationale. Reclassify as expect=killed (with a why "
                        "naming the input that separates them), or, if the kill is a build "
                        "failure, fix the catalog entry -- source that does not compile is not "
                        "an equivalent program")
            print(f"{m['id']:<48} {verdict:<16}{note}")
    finally:
        # Only clear the on-disk record if the tree is provably clean. If a lock beat
        # the retries, the sentinel and the snapshot MUST stay: they are what keeps the
        # pre-commit hook refusing and what --recover needs to finish the job.
        if restore_all():
            _disarm()
        # Rebuild the target back to its pristine object so the tree is left green.
        _build(build_dir, config, target)

    scope = ("equivalence claims only" if only_equivalents
             else "every mutant in the catalog")
    print(f"\n=== summary ({scope}) ===")
    killed = sum(1 for _, v, _ in results if v.startswith("KILLED"))
    survived = sum(1 for _, v, _ in results if v == "SURVIVED")
    print(f"{len(results)} mutants: {killed} killed, {survived} survived "
          f"({len(holes)} unexpected holes, {len(mislabelled)} mislabelled)")
    if holes:
        print("HOLES (expected killed but survived): " + ", ".join(holes))
    if mislabelled:
        print("MISLABELLED (declared equivalent but killed): " + ", ".join(mislabelled))
    if holes or mislabelled:
        return 1
    if only_equivalents:
        print("MUTATION RESULT: PASS (every equivalence claim survived the suite that "
              "would disprove it)")
    else:
        print("MUTATION RESULT: PASS (every non-equivalent mutant was killed, and every "
              "equivalence claim survived the suite that would disprove it)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
