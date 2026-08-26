#!/usr/bin/env python3
"""Flake soak: find tests whose result depends on RUN ORDER or LOAD, not on the code.

R5-G18-8. The defect class this exists for is recorded in the remediation doc: a test function
(pauseResumeToggles) passed in isolation FORTY times in a row and then failed inside its full
binary, because ordering and load changed the timing. A flake hunt that only re-runs the single
failing function concludes, wrongly, that nothing is broken -- so the hunt has to compare the two
worlds rather than sample one of them.

Two modes, and they look for different things:

  soak       Run the whole ctest suite N times, optionally at several -j levels, and flag any
             test whose pass/fail is not identical across every round. This catches LOAD and
             inter-binary timing dependence. A test that passes 4 times and fails once is not
             "mostly fine"; it is a test whose result is not determined by the code.

  isolation  For every test binary, run each of its QtTest functions ALONE and compare with that
             function's result inside the whole-binary run. A function that passes alone but
             fails in-binary (or the reverse) depends on what its siblings left behind --
             static state, a stale singleton, a file on disk, a lingering thread. This is the
             pauseResumeToggles shape exactly.

Both modes FAIL CLOSED: any inconsistency exits 1 and names the offenders. A run that could not
enumerate anything also exits 1 -- a soak that silently tested nothing is a broken gate, not a
passing one (the same rule the clang-format and lizard gates learned).

This is deliberately NOT a pre-commit hook: a meaningful soak costs many minutes. It is a
harness to run against the suite periodically and after touching threading or shared state.

Usage:
  python scripts/run_flake_soak.py soak --rounds 3
  python scripts/run_flake_soak.py soak --rounds 2 --jobs 1,8
  python scripts/run_flake_soak.py isolation --filter test_flash
  python scripts/run_flake_soak.py isolation            # every binary; slow
"""

from __future__ import annotations

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build"
CONFIG = "Release"

# A QtTest binary's own lifecycle slots are not test functions; running them alone is either a
# no-op or an error, and they are not what an order-dependence hunt is looking at.
NON_TEST_SLOTS = {"initTestCase", "cleanupTestCase", "init", "cleanup",
                  "initTestCase_data"}

# One test process is bounded so a wedged binary cannot hang the whole soak. Generous: the
# slowest single suite in this repo runs ~25s, and an isolation run pays process startup per
# function.
SINGLE_TEST_TIMEOUT_S = 300
# A whole-suite ctest round. The full Release suite takes ~11-13 minutes; a stuck round must
# still end so the remaining rounds report.
SUITE_TIMEOUT_S = 3600


def fail(message: str) -> None:
    print(f"FLAKE SOAK FAILED: {message}")
    sys.exit(1)


def require_build_dir() -> None:
    if not (BUILD_DIR / "CTestTestfile.cmake").exists():
        fail(f"no configured build at {BUILD_DIR} (expected CTestTestfile.cmake). "
             "Configure and build first; this harness never builds for you, so it cannot "
             "silently soak a stale tree.")


def run_ctest_round(jobs: int) -> dict[str, bool]:
    """One whole-suite run. Returns {test name: passed}."""
    cmd = ["ctest", "-C", CONFIG, "--output-on-failure"]
    if jobs > 1:
        cmd += ["-j", str(jobs)]
    proc = subprocess.run(cmd, cwd=BUILD_DIR, capture_output=True, text=True,
                          timeout=SUITE_TIMEOUT_S, check=False)
    results: dict[str, bool] = {}
    # ctest's per-test line: "  1/250 Test  #1: test_name ...........   Passed    0.10 sec"
    line_re = re.compile(r"^\s*\d+/\d+\s+Test\s+#\d+:\s+(\S+)\s+\.*\s+(Passed|\*\*\*\w+|\*\*\*.*)")
    for line in proc.stdout.splitlines():
        m = line_re.match(line)
        if m:
            results[m.group(1)] = m.group(2) == "Passed"
    if not results:
        fail("ctest produced no per-test result lines; the harness cannot tell a clean suite "
             "from a suite that never ran. Check the build directory and ctest output format.")
    return results


def soak(rounds: int, job_levels: list[int]) -> int:
    require_build_dir()
    print(f"Soak: {rounds} round(s) at -j {job_levels}. "
          "Any test whose result is not identical every round is a flake.")
    # test name -> list of (round label, passed)
    observations: dict[str, list[tuple[str, bool]]] = collections.defaultdict(list)
    for jobs in job_levels:
        for round_index in range(rounds):
            label = f"-j{jobs} round {round_index + 1}"
            print(f"  running {label} ...", flush=True)
            results = run_ctest_round(jobs)
            failed_now = [name for name, ok in results.items() if not ok]
            print(f"    {len(results)} tests, {len(failed_now)} failed"
                  + (f": {', '.join(sorted(failed_now))}" if failed_now else ""))
            for name, ok in results.items():
                observations[name].append((label, ok))

    inconsistent = {name: obs for name, obs in observations.items()
                    if len({ok for _, ok in obs}) > 1}
    always_failing = sorted(name for name, obs in observations.items()
                            if all(not ok for _, ok in obs))

    print()
    if always_failing:
        # A test that fails EVERY round is broken, not flaky. Reported separately so a real
        # failure is never filed as "flake, will look later".
        print(f"CONSISTENTLY FAILING ({len(always_failing)}) -- these are broken, not flaky:")
        for name in always_failing:
            print(f"  {name}")
    if inconsistent:
        print(f"ORDER/LOAD DEPENDENT ({len(inconsistent)}):")
        for name, obs in sorted(inconsistent.items()):
            detail = ", ".join(f"{label}={'pass' if ok else 'FAIL'}" for label, ok in obs)
            print(f"  {name}: {detail}")
        return 1
    if always_failing:
        return 1
    print(f"SOAK CLEAN: {len(observations)} tests, identical results across every round.")
    return 0


def list_test_binaries(name_filter: str) -> list[tuple[str, Path]]:
    """(ctest name, executable path) for every registered test with an executable."""
    proc = subprocess.run(["ctest", "-C", CONFIG, "-N"], cwd=BUILD_DIR,
                          capture_output=True, text=True, timeout=300, check=False)
    names = re.findall(r"^\s*Test\s+#\d+:\s+(\S+)\s*$", proc.stdout, re.MULTILINE)
    if not names:
        fail("ctest -N listed no tests; refusing to report an empty isolation run as clean.")
    found: list[tuple[str, Path]] = []
    for name in names:
        if name_filter and name_filter not in name:
            continue
        exe = BUILD_DIR / CONFIG / f"{name}.exe"
        if exe.exists():
            found.append((name, exe))
    if name_filter and not found:
        fail(f"filter {name_filter!r} matched no test executable.")
    return found


def list_functions(exe: Path) -> list[str]:
    proc = subprocess.run([str(exe), "-functions"], capture_output=True, text=True,
                          timeout=120, check=False)
    functions = []
    for line in proc.stdout.splitlines():
        # QtTest prints "functionName()" per line.
        name = line.strip().removesuffix("()")
        if name and name not in NON_TEST_SLOTS and "(" not in name:
            functions.append(name)
    return functions


def run_one(exe: Path, function: str | None) -> bool:
    cmd = [str(exe)]
    if function:
        cmd.append(function)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=SINGLE_TEST_TIMEOUT_S, check=False)
    except subprocess.TimeoutExpired:
        # A hang is a failure, and specifically the kind an order-dependence hunt cares about.
        return False
    return proc.returncode == 0


def isolation(name_filter: str) -> int:
    require_build_dir()
    binaries = list_test_binaries(name_filter)
    print(f"Isolation: {len(binaries)} binaries. Each function is run ALONE and compared with "
          "its result inside the whole binary.")
    divergent: list[str] = []
    checked = 0
    for name, exe in binaries:
        functions = list_functions(exe)
        if not functions:
            print(f"  {name}: no functions enumerated -- SKIPPED (and reported, not hidden)")
            continue
        whole_ok = run_one(exe, None)
        # Which functions failed inside the whole binary is not directly observable from the
        # exit code, so the comparison is: if the binary as a whole passed, every function must
        # pass alone; if it failed, at least one function must fail alone. A function that
        # passes alone while its binary fails is the order-dependent shape.
        alone: dict[str, bool] = {}
        for function in functions:
            alone[function] = run_one(exe, function)
            checked += 1
        failed_alone = [f for f, ok in alone.items() if not ok]
        if whole_ok and failed_alone:
            divergent.append(f"{name}: binary PASSES but these fail ALONE: "
                             f"{', '.join(sorted(failed_alone))} (a function that needs a "
                             "sibling's leftover state to pass)")
        elif not whole_ok and not failed_alone:
            divergent.append(f"{name}: binary FAILS but every function passes ALONE "
                             "(order/load dependence -- the pauseResumeToggles shape)")
        print(f"  {name}: {len(functions)} functions, binary="
              f"{'pass' if whole_ok else 'FAIL'}, failing alone={len(failed_alone)}")

    print()
    if checked == 0:
        fail("no functions were run; an isolation sweep that tested nothing is not a pass.")
    if divergent:
        print(f"ORDER-DEPENDENT ({len(divergent)}):")
        for line in divergent:
            print(f"  {line}")
        return 1
    print(f"ISOLATION CLEAN: {checked} function runs agree with their whole-binary result.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)

    soak_parser = sub.add_parser("soak", help="repeat the whole suite and diff the results")
    soak_parser.add_argument("--rounds", type=int, default=3,
                             help="rounds per -j level (default 3)")
    soak_parser.add_argument("--jobs", default="8",
                             help="comma-separated -j levels, e.g. 1,8 (default 8)")

    iso_parser = sub.add_parser("isolation",
                                help="run each function alone and compare with the binary")
    iso_parser.add_argument("--filter", default="",
                            help="only binaries whose ctest name contains this substring")

    args = parser.parse_args()
    if args.mode == "soak":
        levels = [int(part) for part in args.jobs.split(",") if part.strip()]
        if not levels:
            fail("--jobs resolved to no levels.")
        if args.rounds < 2:
            fail("--rounds must be at least 2: a single round cannot show a difference.")
        return soak(args.rounds, levels)
    return isolation(args.filter)


if __name__ == "__main__":
    sys.exit(main())
