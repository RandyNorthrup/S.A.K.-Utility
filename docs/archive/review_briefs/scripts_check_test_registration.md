# Test registration: wrapper-bound executable targets

Status: COMPLETE for this scoped registration, Windows-host and fixture-efficiency work. The
September 19 full Release build and 281/281 CTest gate passed on current code; evidence is
build/wave36-full-build.log and build/wave36-full-ctest.log. No CMake entry, certifier exception
or suppression was added by these repairs.

The registered test_partition_manager_live_certifier command invokes a PowerShell wrapper and
passes the built executable through -CertifierPath "$<TARGET_FILE:partition_manager_live_certifier>".
The old gate compared only add_test NAME against add_executable target names, falsely reporting
the helper as unregistered despite that explicit execution route.

The gate now reads uncommented CMake command statements, treating quoted/bracket arguments and
escaped characters as opaque and bounding parenthesis nesting. It derives registered names from
real add_test bodies rather than quoted examples. Executable targets are recognized either as
the direct COMMAND or the wrapper's exact -CertifierPath TARGET_FILE argument. Generator expressions
in unrelated output/data arguments do not establish execution. Existing source and foreach
coverage checks remain, with comments removed before inspection.

The registered false-green fixture contains 13 isolated structural controls: wrapper/direct-alias
positives, deleted/wrong target, non-executing argument, line/bracket comments, commented binding,
quoted fake registration and malformed/unbalanced/over-depth commands. Standalone and foreach
source registration remain positive controls in every case. This is a static route check, not
an evaluator for arbitrary CMake conditionals; configured CTest execution remains the live proof.

Proof on 2026-09-06:

- The unchanged HEAD gate fails the valid wrapped-target fixture.
- Independent in-memory guard reversals fail the fixture when wrapper binding is ignored,
  comment removal is disabled, arbitrary arguments count as executables, or quoted command text
  is read as an active registration. Tracked gate bytes are not modified by these drills.
- The full existing false-green suite passes under Windows PowerShell 5.1 and PowerShell 7 with
  the new cases. Logs are retained under
  build/wave32-package-rehearsal-20260906-095557-f2c8b75a/registration-falsegreen-ps51-final-private.log
  and registration-falsegreen-ps7-final-private.log. Secret-scan diagnostics remain private.
- The live registration gate passes with the existing four hardware-only certifier exceptions;
  partition_manager_live_certifier is not exempted.

Source: scripts/check_test_registration.ps1. Regression:
scripts/test_false_green_command_propagation.ps1, Test-WrappedTargetRegistration.

## PowerShell CTest launch policy audit (2026-09-06)

Four older registrations requested -ExecutionPolicy Bypass and omitted
-NonInteractive: test_partition_filesystem_probe_certifier,
test_file_management_live_certifier_gate, test_sak_apfs_writer_cli and
test_sak_hfs_writer_cli. Necessity was checked before changing them.

Authoritative configured CTest JSON was copied into a fresh private registration.
The only command changes were replacing the policy override with -NonInteractive
and assigning each script its own fresh OutputRoot. Command, working-directory and
both ENVIRONMENT_MODIFICATION entries round-tripped exactly through CTest discovery.
The same built tools and unchanged scripts then passed all four tests in 16.10
seconds, with zero skipped tests. Evidence: build/psflags-1ab44e49, including
registration-before.json, registration-tested.json, policy-audit-results.xml and
execution-capture.json. A malformed private property serialization was refused by
the round-trip check before any test launch and retained as registration-nonroundtrip.cmake.

The process/user/machine policy settings were unchanged: LocalMachine RemoteSigned;
MachinePolicy, UserPolicy, Process and CurrentUser Undefined. This is evidence that
the four overrides are unnecessary on the configured certification host, not a
policy override for machines whose administrator requires signed scripts.

All four CMake launches now use -NoProfile -NonInteractive -File. The existing
registration gate reuses its command tokenizer and checks the direct PowerShell
engines and three PowerShell path variables used by this tree. Exactly the two
canonical host flags must precede -File; script arguments, quoted examples and
comments cannot supply them. Extra host switches, including long/short/colon
execution-policy overrides, are refused. Bracket and quoted flag arguments remain
valid. The bounded source check does not evaluate arbitrary CMake wrappers or
resolve unknown variables. Other PowerShell launch sites outside CTest were not
audited by this four-registration change.

The new gate first rejected the live old flags, naming all four registrations,
then passed the full live tree after their flags changed. Logs:
old-four-registration-red.log and fixed-four-registration-green.log under the audit
directory. The existing false-green fixture retains all 13 wrapper-registration
controls and adds 24 launch-policy controls, including an independent old-flags,
missing-noninteractive and retained-override case for each of the four names.
Its complete suite passed under Windows PowerShell 5.1 and PowerShell 7; private
logs are full-false-green-ps51-private.log and full-false-green-ps7-private.log.
Removing the policy call from an isolated gate copy made the fixture fail for the
expected accepted-old-flags assertion (policy-guard-removal-red.log); no tracked
gate source was mutated. After the parent-owned helper rebuild, configured CTest
discovery confirmed exactly the four flag changes with every other command/context
value unchanged (registration-after.json).

No application startup, shared build or host sector/data I/O was performed. The
unchanged CLI safety negatives omit confirmation when naming PhysicalDrive0 or
the OS volume. PhysicalDrive0 is refused by string identity; the OS-volume alias
uses only a desiredAccess=0 handle and read-only volume-extent metadata query.
Image mutations and reports remained in owned fixture directories. This audit is
not filesystem hardware certification or proof of the full current Release suite.

## Windows host isolation and fixture efficiency (2026-09-08)

Native CTest retained PowerShell 7's module search path when launching Windows PowerShell 5.1;
its capture regression could not load Get-FileHash. The four existing 5.1 registrations now use
the existing CMake environment-isolation pattern to unset only PSModulePath. Four separate
copied-production launch reversals prove module resolution fails without that guard and passes
after exact restoration (build/ctest-module-drills-ef97006e). No test or timeout was removed.

The source-policy gate now follows the actual command in canonical CMake -E env wrappers;
environment assignment values and script data cannot supply host flags. The fixture retains
its 13 wrapper CLI controls and all 24 original policy cases, adds 23 wrapper/data cases, and runs
the exact supplied-GateText parser and guard in process. Three real full-script policy controls
retain CLI/wiring and nonzero-exit coverage. Portable AST preparation is shared only within one
supplied source, with fresh mock state for all 36 cases and explicit stale-preparation refusal.

The controller-evidence fixture similarly reuses immutable synthetic source bytes while keeping
eight source-mutating cases isolated. Every production validation still hashes current source
and enumerates headers. Independent final byte and directory-membership checks protect shared
fixture integrity. All 389 cases remain; no production validation cache was introduced.

False-green fixture roots now use fresh GUID/create-new ownership, exact temp containment and
the existing staging-path reparse guard. A refused existing-directory canary remains unchanged.
Success prints after cleanup; prior PID-named roots and failed artifacts were not touched.

Final focused timing evidence: controller CTest 48.92s under 120s; Windows PowerShell controller
389 cases in 69.641s including cleanup. Final false-green CTests 35.26s and 31.03s under 60s each.
Seven independent guard mutations produced intended reds and restored greens. Sources/logs are
bound in build/wave33-perf-final-owned-pure-drills.log and
build/wave33-perf-shared-oracle-mutations.log. Timing logs are
build/wave33-perf-final-ctest.log, build/wave33-perf-controller-ps51.log and
build/wave33-perf-owned-root-final-ctest.log. The later complete integration gate cited above
satisfies the former full Release/CTest requirement.

Current-source refresh: the four real CTest module-isolation guard triples passed under
build/ctest-module-drills-b3e3ab26. Nine independent pure fixture/policy guard triples passed on
each Windows host under perf-policy-pure-036810d1 and perf-policy-pure-faa3a906. The full current
false-green suites passed on both hosts within their unchanged 60-second bounds. The separate
current Windows PowerShell 5.1 controller run passed all 389 unique cases and immutable-source
checks in 44.994 seconds against its unchanged 120-second limit; receipt and log are retained
under build/wave37-controller-ps51-a671d1dc4ec04be998aaa1f65a060415. This closes the missing
current PS5 controller evidence; it does not certify disk or hardware operations.
