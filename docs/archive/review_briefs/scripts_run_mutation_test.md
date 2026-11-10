# Mutation runner execution and recovery review

Status: COMPLETE for the runner implementation described here. Archived after the September 19,
2026 full Release build and 281/281 CTest gate passed on the current code. Evidence:
build/wave36-full-build.log and build/wave36-full-ctest.log. Executing the remaining application
mutation catalogs is separate, still-open R5 work; this archive does not claim that coverage.

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
[R5](../CODEX_REVIEW_5_REMEDIATION.md). The subsequent recovery delta is recorded below.

Full-file follow-up on 2026-09-04 found two separate recovery gaps outside that executed-command
delta. Both were reproduced through the existing test fixture, with REPO_ROOT, sentinel and
snapshot redirected into a disposable temporary directory; no live project source was mutated.

- [x] [HIGH] Recovery did not refuse the live owner PID recorded in the sentinel. A fixture
  armed by the current still-running process restored its source and cleared the sentinel with
  exit zero. Recovery must establish that the owning run and its children cannot resume before
  touching source; an operator instruction alone is not an enforced guard.
  RESOLVED 2026-09-04: one process-owned Windows exclusive file handle serializes run and recovery
  before either admission check. Schema v2 binds the owner PID to its creation time. Every command
  starts suspended, is assigned to a non-breakaway Windows Job Object, then resumes. Normal exit,
  cancellation and timeout require empty job membership plus the root process wait before a
  quiescent receipt is persisted. An exited parent alone never proves orphaned descendants stopped.
- [x] [HIGH] Recovery enumerated the snapshots that happen to remain rather than requiring the
  sentinel's complete named file set. Removing the fixture's sole snapshot produced RECOVERED:
  0 file(s), exit zero, and marker removal while the source remained mutated. Validate the full
  manifest, safe target mapping and snapshot integrity before any restore or disarm.
  RESOLVED 2026-09-04: recovery prevalidates the entire nonempty manifest and exact snapshot set,
  canonical in-repository source paths, direct non-reparse/non-hardlinked files, byte lengths,
  SHA-256 hashes and allowed current-source hashes before its first source write. Hashed snapshot
  names prevent separator aliases. Atomic writes preserve exact bytes; disarm additionally requires
  every source to equal its recorded original. Missing, extra, changed, malformed or unbound records
  preserve the remaining originals and fail visibly.

The original 16 execution contracts remain covered. Current scoped verification: 50/50 tests pass
in 8.065 seconds (`build/mutation-recovery-tests-green.log`). Twenty-eight independent in-memory
negative drills each produced assertion failure with zero drill errors, then the unmodified runner
passed the suite again (`build/mutation-recovery-negative-drills.log`). The disposable fixtures
cover live-owner refusal, cross-process admission, complete-before-first-write validation, empty
and partial snapshots, hashes and mappings, source edits, reparse refusal, PID binding, every
uncertain command state, job containment, orphaned descendants and exact-byte disarm. No real
project mutation/build was launched and no real tool process was terminated for this proof.

Deliberate recovery boundary: legacy markers lack the verified process/hash contract and are not
automatically migrated. A command start or exit without a durable empty-job receipt remains
uncertain even if its PID is absent. Keep the marker, snapshots and logs for explicit verified
recovery; never clear uncertainty merely to force `--recover` through. Unbound snapshot directories
are retained, not guessed to be disposable.

Python compilation and scoped diff checks passed. The bounded recovery delta review found no
remaining issue. The prior Wave 31 gate did not certify these later changes; the September 19
full integration gate cited above now covers the current runner and its registered suite.

## Scoped Qt function execution (2026-09-06)

Catalogs may now declare qt_functions: 1..64 unique ASCII test-function identifiers, each at most
128 characters. Data-tag selectors and lifecycle/data-provider methods are refused. Omitting the
field still runs the original whole-target CTest selection. This is mutation-proof scope, not
permission to replace the mandatory full Release build and full CTest integration gate.

The scoped path discovers exactly one authoritative CTest registration with --show-only=json-v1,
using the existing bounded owned-job launcher. Its direct regular executable must match the catalog
target basename and have no reparse path. The exact command is extended only with function names
and a fresh private Qt XML logger. WORKING_DIRECTORY, ENVIRONMENT, ENVIRONMENT_MODIFICATION, TIMEOUT,
LABELS, PROCESSORS and RUN_SERIAL are preserved; unsupported properties, including fixture/dependency
and outcome-transform settings, are refused rather than discarded. A generated private CTest
registration is queried again and its complete command/context must round-trip exactly. Canonical
CTest registration files are never rewritten.

Fresh CTest JUnit and Qt XML must both show the exact execution. Every selected TestFunction and
both suite lifecycle records must be present and completed; unexpected/duplicate functions, missing
incidents, skips, expected-failure/blacklist outcomes, fatal messages and lifecycle failures invalidate
the run. All data-row incidents are checked, so passed rows cannot hide a failed or skipped row.
Only selected assertion failures with source locations count as kills. Qt and CTest verdicts must
agree. XML is bounded, direct-file checked and rejects DTD/entity declarations before parsing;
literal declaration text inside CDATA/comments is not mistaken for a declaration.

The function-only invocation runs all available data rows, following the pinned
[Qt 6.10 command-line contract](https://doc.qt.io/qt-6.10/qtest-overview.html#qt-test-command-line-arguments).
Qt evidence follows the pinned
[6.10.3 XML logger](https://github.com/qt/qtbase/blob/v6.10.3/src/testlib/qxmltestlogger.cpp).
CTest discovery/context copying uses its documented
[JSON object model](https://cmake.org/cmake/help/latest/manual/ctest.1.html#show-as-json-object-model).

Proof:

- Original 50-test baseline passed in 8.665 seconds. The extended 71-test suite passes, including
  real isolated CTest processes, literal semicolon/quote/dollar environment witnesses, data-row
  outcomes, exact executable/registration/report identity and the existing recovery/job contracts.
- Two existing native-process fixtures initially hit startup races under simultaneous build load;
  their unchanged focused replay passed. Their tests now establish bounded child readiness before
  starting the original 2/5-second execution checks. Production timeouts and containment are unchanged.
- Twenty-one independent in-memory guard reversals produced assertion failures with zero drill
  errors. Tracked runner bytes were unchanged. Evidence: build/wave32-qt-scoped-negative-drills.log.
- A read-only integration ran scriptBuilder_createImageRefusesExistingWithoutOverwrite in the
  actual built test_partition_manager_core executable through the scoped path. Exact registration,
  context and Qt XML checks passed in 1.34 seconds. Evidence: build/wave32-qt-scoped-real-integration.log
  and build/mutation-runs/qt-selector-integration-1788728503134486600. No main/catalog path, shared build
  or production-source mutation was invoked for this integration.

Hashed snapshots, exclusive admission, suspended job-contained launch, quiescence receipts,
compiler-success requirements and finally restoration remain in force. The scoped proof does not
certify unselected functions, browser/VM flows or the full current application build.

## Prelaunch journal transaction and incident diagnostics (2026-09-06)

The first live scoped capture-mode catalog stopped after two valid kills. Its third
mutant built, but the test command produced no output and retained a trailing
starting journal record with no unconfirmed PID. The old _command_begin appended
that record to shared RAM before persisting it. A prelaunch journal-write failure
could therefore cause finally cleanup to persist an uncertain command that had
never reached child construction. The cleanup guard correctly refused restoration,
but the CLI printed only that final error and omitted the original exception chain.

The incident's exact filesystem error was not retained and is not claimed known.
A separate private Windows fixture reproduces an atomic journal replacement denial
using a reader without FILE_SHARE_DELETE. It proves the prelaunch failure path,
not the identity of the reader or cause of the historical incident.

_command_begin now creates a copy-on-write candidate, persists it, then publishes
it to RUN_MANIFEST. Persistence failure cannot append uncommitted intent to shared
RAM or reach child construction. No retry, suppression, fabricated PID or invented
quiescence receipt was added. A write that commits and then raises still leaves
its durable starting record uncertain: recovery reads the disk and refuses it.
The existing durable-intent-before-launch and child-job requirements are unchanged.

The CLI retains its error verdict and exit 2, and additionally prints the complete
exception chain. Original persistence errors and every nested cleanup refusal are
therefore available together instead of only the last guard's message.

Scoped proof:

- Six new private-root tests against the old runner produced five assertion
  failures and no test errors; the commit-then-error refusal already passed.
  Evidence: build/wave32-runner-prelaunch-regression-red.log.
- All six pass with the fix. They cover failed intent without child construction,
  publication only after durability, full isolated-catalog exact source restoration
  after a prelaunch write failure, real Windows deny-delete replacement failure,
  postcommit uncertainty refusal, and original/inner/outer CLI exception diagnostics.
  Evidence: build/wave32-runner-prelaunch-regression-green.log.
- Three independent in-memory reversals fail the tests: removing copy-on-write,
  removing traceback rendering, and disabling exception-chain rendering. Zero
  drill errors; tracked runner bytes were unchanged by the drills. Evidence:
  build/wave32-runner-prelaunch-negative-drills.log.
- The complete extended runner suite passed: 77/77 tests in 14.541 seconds,
  including real private-root native/CTest fixtures and the existing admission,
  containment, recovery and scoped Qt evidence guards. Evidence:
  build/wave32-runner-prelaunch-full-unit.log. No shared build or actual project
  mutation catalog was invoked for this regression proof.

The historical uncertain journal is not migrated or relabeled as quiescent.
Its unchanged records, exact snapshot/mutant bytes and independent source/process
audits belong to build/wave32-mutation-incident-5452. Administrative source recovery
is not a completed catalog, and a stale mutant-built test executable is not proof
until rebuilt. The September 19 full Release/CTest gate cited above covers the current runner;
actual application catalog completion remains R5 work.
