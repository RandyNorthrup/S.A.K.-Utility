# Mutation runner execution and recovery review

Scope: the 2026-09-04 execution, CTest evidence, recovery and log-retention delta in
`scripts/run_mutation_test.py` and `tests/unit/test_mutation_runner.py`.

- [x] [HIGH] A timed-out command could leave interactive child processes running without bounded
  cleanup, and build failures could be reported as killed mutants.
  RESOLVED: hidden Windows child processes have finite waits and tree termination. A killed mutant
  requires a successful build plus executed CTest failure evidence. Timeouts, skipped or missing
  tests, malformed evidence, build failures and restore failures invalidate the run.
- [x] [HIGH] Failed child termination still allowed source restoration, marker removal and another
  build while the prior child tree could remain active.
  RESOLVED: an unconfirmed child blocks source changes and rebuilds, preserves the snapshot, and is
  recorded in the sentinel. Blind recovery refuses that state. The same guard covers the baseline
  and the final restored-source build.
- [x] [HIGH] A failed tree kill was accepted when the immediate parent had already exited, even
  though descendants could remain active.
  RESOLVED: every nonzero tree-termination exit is an error. Parent exit alone is not evidence that
  its descendants stopped.
- [x] [MEDIUM] Later mutants could overwrite the Qt assertion report that explained an earlier
  failure.
  RESOLVED: each command retains its own process log and CTest XML; changed Qt assertion reports
  are copied beside that command's log. Unchanged reports are not relabeled as fresh evidence.

Independent verification on 2026-09-04: all 16 runner tests passed. The failed-termination finding
was reproduced with an isolated temporary fixture; the exited-parent finding was reproduced with
real Windows helper processes, whose exact child was then terminated successfully. The tests now
cover both guards, retained recovery state after final-build timeout, and fresh versus stale Qt
reports. Bounded final review found no remaining issue in this delta.

This remains scoped runner evidence. Integrated local gate 2026-09-04: Release build exit 0;
CTest 276/276 passed in 1052.17 seconds (`build/wave31-full-ctest-final.xml`). Runtime skip audit
passed with 27 reviewed skips. Remaining work and certification boundaries are tracked in
[R5](../CODEX_REVIEW_5_REMEDIATION.md); the recovery findings below remain INCOMPLETE.

Full-file follow-up on 2026-09-04 found two separate recovery gaps outside that executed-command
delta. Both were reproduced through the existing test fixture, with REPO_ROOT, sentinel and
snapshot redirected into a disposable temporary directory; no live project source was mutated.

- [ ] [HIGH] Recovery does not refuse the live owner PID recorded in the sentinel. A fixture
  armed by the current still-running process restored its source and cleared the sentinel with
  exit zero. Recovery must establish that the owning run and its children cannot resume before
  touching source; an operator instruction alone is not an enforced guard.
- [ ] [HIGH] Recovery enumerates the snapshots that happen to remain rather than requiring the
  sentinel's complete named file set. Removing the fixture's sole snapshot produced RECOVERED:
  0 file(s), exit zero, and marker removal while the source remained mutated. Validate the full
  manifest, safe target mapping and snapshot integrity before any restore or disarm.

These findings remain INCOMPLETE. Existing run-time termination guards and their proof above are
retained; the script is not marked fully reviewed or recovery-certified in the ledger.
