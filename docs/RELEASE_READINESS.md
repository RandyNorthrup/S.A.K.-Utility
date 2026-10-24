# Release Readiness

Current HFS+/APFS driver capability status is owned by
`certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`. Historical milestone text below is not a
second status owner; release-facing Apple filesystem claims must mirror the gated matrix.

## Release bar (owner ruling 2026-10-24)

The next release ships when every box below is checked on the release bytes. This replaces the R5
definition of done (G10), whose open-ended items ("zero findings anywhere", "no surviving mutant
anywhere", full review-ledger coverage) could never close.

- [ ] CI (tier T2, see AGENTS.md) green on `main` for the release commit.
- [ ] Startup does not freeze the GUI: the window stays responsive while fonts load, including on
      a machine with thousands of installed fonts; stock Windows 11 VM cold start within budget.
- [ ] Clean-package smoke and startup end-to-end smoke pass (tier T3).
- [ ] Shipped binaries are Authenticode-signed, and the Azure signing credential is confirmed valid.
- [ ] Windows 11 VM smoke passes on the release bytes.
- [ ] No known crash, data-loss or unpatched-security item remains in the list below.
- [x] Hardware-only claims (partition external gates, Apple kernel, Windows 10) are labeled
      uncertified by the computed claim level in `certification/`.

DESIGN DECISION (owner 2026-10-24): Windows 10 rig runs, the 18 external partition hardware gates,
Apple kernel/hardware certification, review-ledger coverage, mutation surface and clang-tidy naming
debt are not release requirements. Their claims stay labeled uncertified; the archived R5 document
keeps their history.

### Open crash, data-loss and security items (must be empty to release)

1. Startup GUI freeze: a cold DirectWrite font census blocks the GUI thread on machines with many
   installed fonts (`src/gui/main_window.cpp` createStatusBar, `src/main.cpp` warmup). Detail:
   `docs/GUI_STARTUP_FIELD_NOTES.md`.
2. `ProcessRunner::finishContainedOrFailFast` (`src/core/process_runner.cpp` ~189) calls qFatal on
   a cleanup deadline during a tool run -- the same crash shape fixed for MCP in a8b64bbd. Apply
   the same report-and-contain treatment through `sak::ContainedProcessQuarantine`.
3. Other fail-fast teardown paths to classify as true invariant or load-dependent:
   `~WindowsProcessJob` (`src/core/windows_process_job.cpp` ~381-402), `WorkerBase::stopAndJoin`
   (`src/core/worker_base.cpp` ~46), AI panel destruction (`src/gui/ai_assistant_panel.cpp`
   ~3866, ~3892).
4. Dependency security updates (verify each advisory ID at its source before acting): Qt 6.10.3
   (6.10 support ended 2026-10-07; JSON-parsing advisories), iPerf3 3.21 (server-side
   use-after-free; the app runs iperf3 as a server), Chocolatey 2.7.2's bundled 7-Zip, zlib 1.3.1,
   liblzma 5.8.2, and the hfsprogs OpenSSL 3.6 build (end of life 2026-11-01).
5. Downloaded third-party binaries without a hash pin: iperf3, smartmontools, and the Trusted
   Signing client library.
6. 78 unverified findings (18 HIGH) in 14 review briefs under `docs/review_briefs/` that no ledger
   unit binds. Triage each against current code; two sampled HIGH/MEDIUM findings were already
   fixed.

### Open release infrastructure

- Tiered gates: the local tiers (T1 commit hooks, T1p pre-push, manual-stage T2-T4 twins), the
  meta-gate retirements, CTest labels, resource locks and parallel runs are in place
  (2026-10-24). Open: rewrite the CI workflows to run T2 on every push to `dev` by label.
- Intermittent, cause unidentified: once in a `-j 8` full suite (2026-10-24)
  `test_advanced_uninstall_controller` failed because `ConfigManager::sync` got a settings access
  error on the shared `data/config/Utility.ini`, at the serial tail with no other CTest test
  running; seven serial reruns of the same sequence passed and Defender real-time protection is
  off. If a user's machine can hold that file the same way, a settings save fails visibly
  rather than silently, but it fails.
- One shared core library so first-party sources compile once instead of once per test target.
- Done 2026-10-24: the Current Certification State block in `docs/PARTITION_MANAGER_CERTIFICATION.md`
  is generated from the computed claim level and both certification matrices, the T3 check fails
  when it is stale, and the regex-over-prose claim checks are retired.
- `vcpkg.json` with a pinned baseline and a CI cache key that tracks it; one clang-format major
  version locally and in CI.
- Done 2026-10-24: certifier and writer-CLI script tests delete their run folder when they pass,
  keep failures for diagnosis, and cap leftover failed runs at the newest three
  (`scripts/run_folder_retention.ps1`). They once held over 1 TB under `build/test_results`.
- Refresh `THIRD_PARTY_LICENSES.md` against what actually ships (Mesa opengl32sw, D3Dcompiler,
  MSVC runtime, 7-Zip, cygwin1.dll and others are missing; Qt section names 6.5).

## Current gate boundary: Wave 69 (2026-10-24)

Commits 6e71d676 (held waves 32-68), a8b64bbd (MCP containment), 7567e23f (test environment
fixes) and ac21ea2d (secret-scan tool resolution) were pushed to `origin/dev`. Gate on the final
tree: all-target Release build exit 0, 249/249 native CTests (build/wave69-full-ctest.stdout.log)
and 51/51 script CTests after the script fixes (build/wave69c-script-ctest.stdout.log). No
package, VM, accessibility or signing gate has run on these bytes.

## Previous gate boundary: Wave 68 source candidate (2026-09-25)

W67c is the last complete local gate: Release build and 300/300 CTests passed,
but its fresh native package smoke timed out. Wave 68 has only targeted app
builds and copied portable-runner contract tests; its native 30-second smoke
still fails. Font warmup/census removal and a narrower alias-only removal
were both restored under the owner's keep-only-if-verified condition. A
targeted Release app rebuild passed for restored source, but no complete
source gate followed. Current-binary package, accessibility, Windows 11 VM,
Windows 10, hardware, security and signing gates remain open. The archived R5
document holds item history; `docs/GUI_STARTUP_FIELD_NOTES.md` owns the timing detail. Future
plans under `docs/future/` remain parked.

## Historical package/VM proof: Wave 44 (2026-09-23)

The archived R5 document holds item history. Wave 44 passed a full
VS2022 Release build and all 287 registered CTests. Fresh portable ZIP staging,
static/runtime dependency checks, native Windows startup from staged and
extracted folders, and exact 159-file ZIP/extract hash comparison passed. On
the Windows 11 x64 VM, the transferred ZIP hash matched, 159 files extracted,
package startup smoke passed, and the app's accessibility audit reported
`SAK_ACCESSIBILITY_AUDIT_OK missing=0 checked=1950`. Evidence is under
`build/wave44-*`, including `build/wave44-vm-windows-startup-evidence.md`.
The retained Wave44 ZIP and `build/wave44-real-package-archive.log` still hold
its archive and binary hashes. A Wave45 staging-test attempt removed the old
global `Release/SHA256SUMS.txt`; it must not be cited as a current manifest.

Wave 45 changes ProcessRunner and the elevated helper after these package/VM
receipts. Wave 44 is historical proof, not proof of the current binary.
Neither wave certifies interactive
console UI, Windows 10, raw disks, external Apple media, hardware, live model
calls, signing, or the whole release. Seven vendor `/w` override warnings and
their 48 revealed strict diagnostics remain open, as do ProcessRunner process-
tree certification and portable-staging path replacement. The 25 reviewed local Qt
skips are not treated as executed external or destructive tests.

## Wave 48 local/Windows 11 scope (2026-09-24, not release certification)

The current-source VS2022 Release build and all 287 CTests passed. An earlier
full run failed 1/287 when integer-rounded MCP cleanup timed out before its
deadline; passing the original QDeadlineTimer to the semaphore fixed that
defect. Focused test passed 20 repeats and a copied-source early-return drill
went green/red/restored-green. Skip audit found 25 reviewed skips, none stale.
Whole-tree cppcheck, stale-inline-suppression reveal, dead-code probe, full
repository secret scan, and extracted-package Gitleaks scan passed.

Archive now builds and verifies candidate ZIP/checksum before publication,
retains prior outputs for checked rollback, and refuses incoherent prior
manifests or unresolved recovery markers. Owned PS7/5.1 fixtures exercised
compression and separate ZIP/checksum publish failures, same-name and new-name
releases, and copied-source rollback/prior-binding red drills. Fresh 157-file
ZIP had exact extraction hashes, no bundled logs, static/runtime check pass,
and native startup pass. Windows 11 VM matched ZIP/app hashes, passed native
startup and accessibility audit (`SAK_ACCESSIBILITY_AUDIT_OK missing=0
checked=1950`), and left no owned process or crash/temp/log items. Exact
evidence is `build/wave48-transaction-package-vm-evidence.md`.

One other fresh local stage of this same app binary timed out under the
unchanged 30-second native startup gate. Its later local/VM successes do not
erase that failure. Cold/intermittent startup is INCOMPLETE and blocks a
reliable release claim. Two-file archive publication is not crash-atomic;
interrupted transactions fail closed pending explicit recovery. The staging
path-swap race also remains open. Wave47 is historical for the older app and
archive script, not proof of current bytes.

Subsequent smoke-only startup phase diagnostics now flush the before/after Qt
and font-query boundaries, and the portable runner requires ordered markers.
Its 41 mocked contracts and copied-guard red drill pass. These newer source
bytes completed an all-target Release build and fresh 157-file stage/static
check. The first native stage then timed out under the unchanged 30-second
limit while complete CTest was concurrently running. Its flushed log stops at
the first GUI-thread font-family query after successful Qt initialization.
The complete CTest subsequently passed 287/287. Two independent fresh stages
of the same app hash passed the native smoke under PowerShell 7 and Windows
PowerShell 5.1 after CTest ended. See `build/wave49-startup-phase-evidence.md`.
The same W49 ZIP passed exact 157-file local extraction hashes, static/native
smoke, and extracted-directory Gitleaks. Windows 11 VM transfer/extraction
hashes matched, native smoke passed, and accessibility audit reported
`missing=0 checked=1950`; two runtime log items were retained after the audit,
so post-audit pristine state is not claimed. The earlier first-run timeout is
not resolved. Wave48 binary receipts are historical for these bytes.
Another uncontended fresh default-backend extract also timed out at the first
font query. An explicit-GDI arm passed, but a following A/B/B/A same-binary
comparison passed all four arms; no backend switch is justified yet. The
unchanged 30-second native startup reliability gate remains a release blocker.
After those W49 binary receipts, source-only cleanup removed the ineffective
DirectWrite override and escape hatch. Its old-code red/new-code green source
contract passed on both PowerShell hosts. W50 then passed a full Release
build, 287/287 CTests, current ZIP hash/extraction, and Windows 11 VM
native/accessibility checks. The first fresh local ZIP extraction still timed
out at the unchanged 30-second native gate: its forced font-family query
took 28839 ms. See `build/wave50-baseline-evidence.md`. W49 receipts are
historical for W50 bytes; the underlying timeout remains unresolved.
The owner-approved W51 lazy-font candidate compiled and passed 12 matched
fresh-ZIP native A/B/B/A runs, but whole-startup means were nearly equal and
neither arm reproduced the rare timeout. Under the owner's keep-only-if-
verified condition, it was reverted to exact W50 source hashes. The current
all-target build now exits 0, but the rebuilt app SHA256 is
`128403965F367238348DFA64B7CC0703FB68A6014E6F7169A9BBBBF656D6612C`.
The version header embeds build date/time; W50 package/VM receipts do not
certify these new bytes. W52 now has exact 157-file local extraction hashes,
static/runtime checks, an extracted-directory Gitleaks pass, and Windows 11
VM hash-matched native startup plus accessibility audit (`missing=0
checked=1950`). Final complete CTest passed 287/287; skip audit found 25
reviewed skips. The uncontended fresh local ZIP-extract native smoke passed in
22.823 seconds under the unchanged 30-second limit. See
`build/wave52-restored-evidence.md`. Earlier first-run timeouts remain a
release blocker; this later pass does not establish reliable startup.
Wave53 then removed five unnecessary nested test-launch policy overrides;
the app hash stayed unchanged, so W52 package/VM proof still binds to its
binary. The complete Release build and 287/287 CTest passed for the latest
test-script bytes; skip audit found 25 reviewed skips, none stale. Other
policy overrides and the startup reliability blocker remain open.
Wave54 removed nine additional test-only policy overrides with focused
red/green guards and real PS7/PS5 and Python test passes. The app binary is
unchanged; its full Release build and 287/287 CTests passed before Wave55.
Wave55 then repaired the portable-stage regression fixture's unbounded copy
of historical Release packages/ZIPs. Its unchanged 240-second CTest had
approached timeout at 221.77 seconds; the bounded fixture passed a focused
registered run in 84.74 seconds, and the full PS7/PS5 scripts passed with
independent exclusion drills. The first full gate failed 1/287 when a
different combined false-green test reached its 60-second timeout; staging
passed in 105.64 seconds under the same slow run. Portable and Ledger scopes
were then registered separately on PS7/PS5 without increasing their limits;
focused runs pass. The new full Release build and 289/289 CTests passed; in
that slow full run the split scopes took 16.55-22.83 seconds and bounded
staging 94.76 seconds under their unchanged limits. Skip audit found 25
reviewed skips. The actual production path-swap/crash-recovery risks are
unaffected.
Wave56 removed two more unneeded policy overrides from the review-unit and
release-readiness child launchers. Exact red/green guards plus isolated
success/failure and review timeout fixtures passed on PS7/PS5. No destructive
readiness workflow was invoked. The latest all-target Release build and
289/289 CTests passed, with 25 reviewed skips and none stale; hardware/VM
policy overrides remain individually open.
Wave59 removed the unnecessary Bypass flag from the VirtualBox guest-admin
inline probe. Its exact guard was red/green and the automation-VM security
fixture passed under PS7/PS5. No provisioning or destructive VM path was
executed. Its all-target Release build passed, but full CTest failed 1/290
when the second 18,043,854,848-byte APFS scratch image could not resize on
the system temporary drive. The failure log is retained at
`build/wave59-partition-core-failure.txt`; this is not a green release gate.
Wave60 routes that large test's images through the explicit
`SAK_TEST_IMAGE_SCRATCH_ROOT`. A focused run on D: (`Storage 4TB`, exFAT)
allocated both images but hit Qt Test's 300-second function timeout. The
failed-run images were removed after exact path/content checks; the log is
retained at `build/wave60-apfs-focused.txt`. With the documented Qt Test
timeout raised to 900000 ms, the same focused case passed on D: in 518182 ms;
its temporary directory cleaned itself up. The subsequent full D: diagnostic
was stopped after 174/290 tests: its partition-core test had 46 failed
assertions, and 13 test executables had failed under an all-exFAT temporary
environment. This is not a complete gate. The same full partition-core test
passed 322 assertions, zero failures on E: NTFS in 138731 ms. A direct
capability-based copy fix now has a successful targeted build, old-red/new-
green 8 KiB copy proof on D: exFAT, 4/4 APFS reader fuzz assertions on D:,
and 322 passing partition-core assertions on E: NTFS. It is not yet a full
gate: complete latest-source Release/CTest on E:, capacity/failure-cleanup
proof, and current-binary VM/package checks remain open. A separate new-image
sizing path still marks sparse only as a best effort, then resizes without a
non-sparse capacity preflight; its partial-image failure cleanup is also open
under R5-IDX-37. The copy fix does not close that create path.
Wave61 local gate then passed an all-target Release build and all 290 CTests
with temporary files on E: NTFS, in 1904.85 seconds. The skip audit found
5242 passed Qt functions and 25 reviewed skips, none stale. The built app
SHA256 is `8DD34358DBD3A5C2F9FDA5C5BEA2513689D9AA651AC0842856DF82C0F68D0C4A`.
The D: exFAT copy regression and APFS reader fuzz case passed on the fixed
binary. The fresh W61 package staged 157 files; ZIP SHA256
`3F4D4D893C5966F11277C2A9745E3FA762C83EBA226B5740D926166B555817D6`
matched on `WIN-11-VM`, and all 157 extracted member hashes matched locally
and on the VM. Local and VM native startup passed. The VM accessibility audit
exited zero with `missing=0 checked=1950`; its fetched status artifact hash
matched. The VM retained two log items/three extra runtime files after the
audit, not a pristine post-audit package. Details:
`build/wave61-local-and-vm-evidence.md`. Dense-capacity/failure-cleanup
drills, the separate APFS new-image capacity defect, reliable cold startup,
hardware and release gates remain open. This is local/package/Windows 11 VM
proof only, not full-app certification.
R5-IDX-38 also blocks file-backed APFS and HFS+ durability claims: the current
`flushDeviceBuffers` QFile branch flushes Qt buffers but does not call the
Windows `FlushFileBuffers` API for the OS file cache. Current green tests and
package startup do not prove those image writes durable across power loss.
New APFS image-format paths also close/hash the QFile without a checked OS
flush; a hash of cached bytes is not durable-write evidence.
Wave62 removed four blanket execution-policy bypasses from the certification
gap report's suggested commands. The real generated-list assertion went red
on the old text and the full certification-tool self-test passed under PS7
and Windows PowerShell 5.1 without overrides. No VHD or hardware command was
run. Host-side runbook commands now inherit policy; fresh extraction uses a
single-use path rather than deleting a fixed folder. The two UNC in-guest
Bypass examples still require separate proof or a direct fix. A fresh
complete Release/CTest gate for the changed script/test bytes is still
required; the W61 app/ZIP/VM proof binds unchanged binaries only.
That W62 all-target build exited zero, but complete CTest failed 1/290:
`test_process_capture` reached its existing 60-second timeout. A focused
registered rerun also timed out at a different subcase. W63 splits its core,
ownership, deadline, output, stdin and environment contracts into 12
separately bounded PS7/PS5 CTests (300 total), with no limit increase or
assertion drop. The focused 12/12 run passed. An initial CMake loop gave a
false static doc count (292 vs 300 live); explicit registrations now make
both doc-accuracy and live CTest JSON count 300. Full Release/CTest and skip
audit were still open at that point. W62 is not a green gate.
The first explicit W63 scoped run failed 1/12 in PS7 output. A repeated
output run showed the fixture's two-second Python receipt setup expired
before `parent.json` appeared; the test had hidden that original exception.
Controlled three-second parent and termination-utility receipt delays proved
the old waits red and bounded ten-second fixture-only waits green. Neither
production capture deadlines nor the 60-second CTest limits changed. Focused
PS7/PS5 output scopes passed after the fix. Each was then repeated five
times (10/10 green) under unchanged limits.
The final W63 all-target Release build exited zero and all 300 CTests passed
in 1613.35 seconds. Skip audit: 5242 passed Qt functions, 25 reviewed skips,
none stale. The app SHA stayed
`8DD34358DBD3A5C2F9FDA5C5BEA2513689D9AA651AC0842856DF82C0F68D0C4A`,
but the rebuilt helper SHA is now
`467AFBB372A9E8327531F3C8F0F7355E05B1FFD17380C504463A2BD098F1D778`.
Fresh W63 package/VM checks now bind the new helper: ZIP SHA256
`142B5B089986DA7C856FED559F67CF50A1447FB7A73E170EEA13742798FF3927`,
157/157 extracted member hashes matched locally and on Windows 11 VM,
native startup passed in both places, and the VM accessibility audit reported
`missing=0 checked=1950` with a fetched matching status hash. Two VM log
items/three extra runtime files remained after audit; see
`build/wave63-local-and-vm-evidence.md`. R5-IDX-35 through R5-IDX-38,
R5-IDX-40 and external/release gates remain open.
Wave64 has a gated shared APFS/HFS+ QFile flush fix. A read-only handle
test went red on the old Qt-only flush and green when the helper called
Windows `FlushFileBuffers` after Qt flush. The newly created APFS format path
still needs its own checked flush. The full partition-core test passed 322
assertions on E: and targeted cppcheck passed. The all-target Release build
then exited zero and all 300 CTests passed in 1924.96 seconds, with 25
reviewed skips and none stale. Current app/helper SHA256 values are
`3EE453FF4F755DAE13BB5D5421DD13C2E5A9B7A6A3B469F55F12BDA14B988FD3`
and `67B7E4546D11841B9F605046E4DE0EDEE14F43607FBA5992D475759F2EE0C622`.
Whole-project cppcheck passed with zero normal findings, 183/186 active
checkers and zero stale inline suppressions; dead-code probing found zero
high-confidence unique definitions. Fresh W64 ZIP SHA256
`04966536B01DBDA45D9D861F5EC229B255615FD8327E1AAD7A8DEAC72AB63BD5`
had 157/157 matching extracted files locally and on Windows 11 VM. Native
startup and VM accessibility (`missing=0 checked=1950`) passed on current
app/helper hashes. Two VM log items/three extra runtime files remained
after audit; see `build/wave64-local-and-vm-evidence.md`. This is not
physical power-loss certification, and the new APFS image-format path still
lacks its own checked OS flush.
Staging took 142.70 seconds under 240, and archive recovery took 171.41
under 180, versus 65.18/73.28 in W63; this sharply reduced timing
margin is open as R5-IDX-41, not a reason to raise limits. An isolated
focused rerun passed in 77.34/104.81 seconds, so the slow-run cause remains
unidentified. W63 package/VM proof is historical for the W64 binaries.
Wave65 moves the `/dev/sda` raw-looking-input assertion out of an inactive
non-Windows test arm and removes only that dead branch. A compile-successful
mutation of the production classifier made the now-active Windows test red;
exact source restoration rebuilt it green. This is a test-coverage fix, not
Linux application support. Full Release/CTest and current-package binding
were still open at that point.
The caller audit then found both Windows raw-device openers passing `/dev/...`
through to `CreateFileW`. Separate non-existent-path read/write tests were
red on the old OS error, and a whitespace-padded `/dev/sda` classifier test
was red. A shared canonical raw-open check now refuses these before native
open. Uppercase `/DEV/...`, backslash `\dev\...`, and padded mixed-case
spellings then made the classifier/read/write tests red on the old code;
normalized case-insensitive classification now routes them to that same
refusal. All three targeted tests and the full 324-function partition-core
test pass on E:, with no scratch residue. The all-target Release build and
300/300 CTests passed in 2361.54 seconds; skip audit found 5244 passed Qt
functions and 25 reviewed skips, none stale. Current app/helper SHA256 are
`9F52E92C8C35EAAE6047CC664B724BDF8E92983E7DB81D5FD2EC8DB621A8AC96`
and `332CFC4B96BDCAA73A72B30B9F880932F3C1290B6BBCBB8613D368C91B354DB0`.
Whole-project cppcheck passed with 183/186 active checkers, zero normal
findings and zero stale inline suppressions; dead-code probing found zero
high-confidence unique definitions. Fresh W65 ZIP SHA256
`7406BD0CEB3688E4DF2BBE1A4239B95D05E5AE2EF7B6A0B822CC4750EC4CDE45`
had 157/157 matching extracted members locally and on Windows 11 VM.
Native startup and accessibility (`missing=0 checked=1950`) passed on
current app/helper hashes. Two VM log items/three extra runtime files
remained after audit; see `build/wave65-local-and-vm-evidence.md`. W64
binary receipts are historical. The raw-path item is closed, but APFS/HFS+
capacity/durability, staging reliability and external/release gates remain.
W66's read-only cppcheck reveal removed the two broad missing-include
exceptions only in a private scan. It exposed 260 project and 5753 system
include diagnostics; 252 project names were generated Qt MOC files, while
the eight non-MOC names resolved under the real generated/vendor include
roots. The live suppressions and gate were not changed. Direct include
routing and a narrow, red-drilled MOC classifier remain open under R5-IDX-33.
The subsequent narrow QR include-root change removed its private-reveal
diagnostic, while a live LZFSE root candidate exposed a finding in vendored
code and was rolled back without a suppression. The all-target W66 Release
build and complete 300/300 CTest passed; whole-tree cppcheck, dead-code and
skip audits passed too (`build/wave66-local-evidence.md`). The rebuilt
app/helper hashes exactly match the W65 package-bound binaries, and neither
changed QA file is a ZIP member. W65 native/VM runtime receipts still bind
those binaries; the unproved LZFSE/generated/system include routing and
global algorithm-style exception remain under R5-IDX-33.
W67 subsequently changed `ai_command_guard.cpp`, a test source and the
registered QR reveal test. Focused builds/tests and private analyzer reveals
pass, but complete W67 CTest timed out `test_review_ledger` at 180 seconds
(299/300 passed, exit 8); the full gate is RED and package/VM binding for
these code bytes remains OPEN. W65/W66 runtime receipts are historical for the current
source candidate. A transient cppcheck capture-file cleanup lock remains
unfixed. The R5 tracking document was reconstructed from a verified Wave 41
Git object after a full-disk write failure; its recovery overlay, not older
historical checked markers, controls current claims. The three certifier
self-tests also retained unbounded image run folders in `build/test_results`;
successful-run cleanup is open and external evidence under `artifacts/` was
not deleted.
W67b then passed `test_review_ledger` in 85.99 seconds in the complete suite,
but `test_mutation_runner` failed its exact descendant-exit postcondition:
the complete gate remained 299/300, exit 8. Bounded Job member handle pinning
and fail-closed negative guards are now a focused-tested candidate under
R5-IDX-45. The full gate and current package/VM binding remain open.
W67c then passed the complete all-target Release build and 300/300 CTests
(skip audit: 5244 pass, 25 reviewed skips, none stale). Whole-tree cppcheck
and dead-code gates passed; the algorithm-style reveal fell to 280 without a
new suppression. A fresh 157-file stage passed static/runtime checks, but
its default 30-second native startup smoke timed out at the first font
database query (`build/wave67c-package-native.log`). One log remained, no app
process. Current package/VM and release readiness stay RED/OPEN; do not cite
W65 native/VM receipts for the changed W67c app binary.
Historical Wave57 adds explicit recovery for versioned interrupted archive transactions:
normal publication still refuses stale markers, while an operator-requested
recovery validates lock/snapshot/output hashes and either keeps a complete
candidate pair or restores the prior pair. Owned hard-crash/tamper fixtures
pass under PS7/PS5. Archive and staging scopes are separately registered
(290 CTests), with unchanged limits. Full Release build and 290/290 CTests
passed. The current-script W57 ZIP has exact 157-file local extraction,
static/runtime/native smoke and Gitleaks passes; Windows 11 VM ZIP/app/helper
hashes matched, with native smoke and accessibility audit passing. See
`build/wave57-archive-recovery-evidence.md`. Staging path replacement,
physical crash durability, elevated-helper live action and reliable cold
startup are not certified by those checks.

Interrupted archive recovery is operator-requested, not automatic. After
confirming no publisher is running, keep the exact lock and transaction
directory intact and invoke the archive script with the original build root
and package name plus `-RecoverInterrupted`. The command refuses an absent,
active, unversioned, wrong-name or tampered record; never delete the marker
or force a new archive over an unproved pair. Recheck the resulting ZIP and
`SHA256SUMS.txt` hashes before using them. `-CleanMutableRuntimeData` cannot
be combined with recovery. This runbook is for versioned process-interrupted
transactions only, not a power-loss certification or a path-swap workaround.
No W51 candidate full-suite or VM certification is credited.

This proves only local plus one VM path. Windows 10, visible interactive UI,
raw-disk and adapter APPLY, external media/hardware, live model, signing, and
the release matrix remain open. ProcessRunner cancellation and timeout cleanup
have separate copied-source red drills with outer Job containment; the
elevated-helper live action path has not been exercised on the VM. Do not
archive R5-P7-20 or mark the release certified on this scope.

## PUBLISHED CREDENTIAL: macOS cert-rig VM password (accepted risk, SETTLED)

The section that stood here described this as a PRE-PUSH blocker and stated that the finding was
being left failing rather than allowlisted. Both statements stopped being true on 2026-08-26 and
nothing updated them, which is exactly the staleness R5-IDX-7 tracks -- and worse here, because a
reader checking the project's security posture would have been told a control was armed that had
been deliberately, and correctly, stood down. Recorded as R5-IDX-20.

- WHAT: the macOS cert-rig VM password and its SSH hostkey fingerprint, committed in
  `docs/APFS_LIVE_RECERT_FOLLOWONS.md` (introduced at commit 08035cf1, flagged by
  `scripts/scan_secrets.ps1` on a full-history run as `generic-api-key`).
- WORKING TREE: redacted 2026-08-25. The doc points at the UNTRACKED `temp/creds.md`, so no
  FUTURE commit carries the credential. The historical blob is untouched and always will be.
- PUBLISHED. The branch was pushed to `origin/dev` on 2026-08-27 and commit 08035cf1 is an
  ancestor of it, so the credential is on GitHub now. This was an owner decision, not an
  accident: the VM is reachable only from inside his LAN, so he accepted the exposure as low
  risk rather than rewrite 1594 commits ("scrubbing the history sounds bad"). Options (2)
  rewrite history and (3) never push are therefore both spent.
- ALLOWLISTED, with authorization. The fingerprint
  `08035cf1...:docs/APFS_LIVE_RECERT_FOLLOWONS.md:generic-api-key:109` is entry 4 in
  `.gitleaksignore`, recorded there as a REAL credential accepted as low risk rather than
  dressed up as a false positive. It is one exact `commit:file:rule:line` fingerprint, so it
  silences that one historical finding and nothing else; a path glob or a bare rule name there
  would disarm the scanner for every future secret in the file, and that is still forbidden.
- WHAT NOW STOPS THE NEXT ONE: the `gitleaks-staged` pre-commit hook, added alongside the
  allowlist entry, scans the staged diff before a commit exists. The full-history scan could
  only ever report a credential that had already been committed; this one refuses it at the
  point it would enter history.
- NOT BEING ROTATED, and that is a decision rather than an omission. The owner ruled on
  2026-08-27 that the password stays: the certification-rig VMs are reachable only from this
  machine's local network, are not exposed on any external route, and hold nothing beyond the
  rigs themselves. The published string is a credential to a LAN-only target, which is the same
  basis on which the allowlist entry was authorized the day before. Nothing is waiting on
  anyone, and no agent should re-raise it as an open action.
- WHAT WOULD CHANGE THAT: if a cert rig is ever reachable from outside the LAN, or reused for
  anything beyond certification, the risk this decision accepted no longer holds and the
  password must be rotated before that happens. That is the condition to check, not the calendar.
- Tracked as R5-IDX-20, now closed.

## Automated Gates

Required before a release candidate is published:

- Full Release build succeeds.
- Full CTest suite passes with zero failures.
- Secret scan passes.
- Blocking-pattern guard passes.
- Accessibility, raw style-token, raw style-literal, GUI magic-number, global magic-number, logged dialog, Partition Manager certification-matrix-integrity/commercial-destructive-feature-matrix/external-checklist/external-lab-package/certification-gap-report/VHD-preflight/certification-artifact-bundle/feature-matrix/evidence-payload/release-claim/strict-handoff, and Lizard gates pass.
- Third-party license audit passes.
- Full driver-level APFS (A1-A8) and HFS+/HFSX (H1-H8) write is Apple-certified - Apple `fsck_apfs`/`fsck_hfs` + macOS-kernel mount at every milestone, with physical-USB destructive + crash-interruption + rollback at the A8/H8 track gates (evidence `external.apfs-a8-physical`, `external.hfs-h8-physical`). Capability status is owned by `certification/APPLE_FILESYSTEM_CAPABILITY_MATRIX.json`; the archived driver plan is its narrative realization record; this is a distinct scope from the Partition Manager destructive-operations claim level.
- Partition Manager non-native filesystem tools remain blocked unless
  `tools/filesystem/manifest.json` has pinned id/version/upstream/license/source-hash/binary-hash/file-system/operation metadata,
  matching bundled binary hashes, and hashed `runtime_files` for companion
  DLL/license/source/helper artifacts. CMake requires the manifest at configure
  time, `scripts/check_partition_filesystem_tool_manifest.ps1` rejects missing
  metadata, path traversal, missing binaries, hash mismatches, and unmanifested
  files under `tools/filesystem`, `scripts/check_third_party_licenses.ps1`
  requires license coverage for any approved manifest tool, and portable
  staging/smoke checks require the manifest in the release package. Current
  manifest approval includes e2fsprogs 1.47.4 `e2fsck` for ext2/ext3/ext4
  `check-read-only` and confirmed `repair`, `mke2fs` for confirmed `format`,
  and `resize2fs` for confirmed `resize`; it also includes hfsprogs
  540.1.linux3-6+sak-msys `newfs_hfs` for HFS+/HFSX confirmed sparse-staged
  `format` and `fsck_hfs` for HFS+/HFSX confirmed sparse-staged `repair`.
  Confirmed ext format, repair, adjacent grow, and same-start shrink now route
  through Pending Operations, safety validation, Apply review, elevated
  PowerShell execution, manifest/hash revalidation, and explicit confirmation,
  with destructive VM raw-media proof and destructive physical USB proof passed
  on 2026-06-05. HFS+/HFSX format/repair routes through the same queue,
  raw-target identity, manifest/hash, Apply review, and confirmation model with
  sparse staging, image-level tool proof, and physical raw-partition HFS mutation
  proof recorded. ext includes
  read-only directory listing, selected-file extraction, bounded recursive
  directory export through original parser code, and allowlisted elevated
  read-only raw-probe retry when the normal process cannot open
  `\\.\PhysicalDriveN`; Linux swap includes read-only header
  version/page-size/UUID/label metadata plus confirmed original SWAPSPACE2 v1
  format through Pending Operations, raw target identity, Apply review,
  generated UUID/label metadata, page-size selection, and explicit
  confirmation; XFS/Btrfs includes read-only superblock metadata, lightweight
  sanity notes, and original-code metadata consistency reports from captured
  probe data; APFS includes read-only container superblock, checkpoint ring
  indexes/windows, object-map OID, visible object-map tree-anchor metadata,
  visible referenced root/non-root B-tree node-header metadata, root
  `btree_info_t` metadata, volume-OID metadata, bounded probe-window
  volume-superblock candidate metadata, referenced-object-header preflight for
  known OIDs found in the probe window, lightweight
  block-geometry/checkpoint-window sanity notes, original-code metadata
  consistency reports, metadata object-checksum validation on APFS object
  blocks, volume browse, selected-file extraction, and bounded recursive
  directory export; HFS+/HFSX includes read-only header metadata,
  catalog consistency checks, attributes B-tree key scans with fork/inline
  metadata reporting, catalog listing, selected data/resource-fork extraction,
  selected attribute-value extraction, bounded recursive directory export with
  `.rsrc` sidecars, catalog/data/resource/attribute extents-overflow
  resolution, image-only data-fork overwrite, data/resource-fork
  allocated-block grow/shrink replacement, bounded initial-extent allocation
  growth, explicit zero-length truncate, constrained empty-file and empty-folder
  create/delete, single-leaf catalog rename/move, bounded file create with data-fork allocation, allocated-file
  delete, optional released-block zeroing for file/folder-tree delete, bounded folder-tree delete, inline-attribute and fork-backed-attribute
  replacement/growth with read-back verification through `sak_hfs_writer_cli.exe`,
  selected raw-partition staged data/resource-fork, single-leaf catalog rename/move, bounded create/delete with optional released-block zeroing, and
  inline/fork-backed-attribute mutation including bounded growth through the HFS File queue/apply path, and
  confirmed sparse-staged `newfs_hfs` format plus `fsck_hfs` repair. HFS File
  Apply must stage the direct or wrapped HFS logical volume span, not the whole
  containing partition, and physical proof is recorded at
  `artifacts/partition-manager-certification/vm-lab/external-evidence/external.hfs-file-apply-physical/report.json`
  with data/resource/inline-attribute raw read-back hashes plus optional secure
  released-block zeroing proof for file/folder-tree delete and final root-listing
  absence. Journaled HFS+
  `fsck_hfs` exit code 8 may be tolerated only on this explicit journaled
  HFS File mutation workflow and only with final read-back proof. APFS has a compiled fail-closed writer certification preflight,
  original Fletcher-64 APFS object checksum calculation/stamping/verification,
  deterministic image-only mutation plan generation, generated-layout
  queue/apply root-file, empty root-directory, root-directory child-file, and
  volume-label mutation proof; the full A1-A8 APFS driver track - inline
  compression, credential-gated encryption, in-place file + directory mutation,
  snapshots, multi-volume, clones/sparse/hard-links, and in-chunk resize - plus
  the H1-H8 HFS+ track (streaming B-trees, underflow merge, hard-links/symlinks,
  attribute overflow records, big-endian journal replay, wrapper write,
  decmpfs read+write) are Apple-certified (see
  `docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Deep XFS/Btrfs tool checks and
  XFS/Btrfs writes remain blocked; APFS Fusion/Tier2, container shrink +
  chunk-adding grow, and arbitrary non-generated Apple-media mutation at the
  Apply layer stay fail-closed by design.
- APFS writer infrastructure is release-allowed only while fail-closed:
  `PartitionApfsWriter` may compute/stamp/verify object checksums, build a
  deterministic image-only mutation plan shape, build a new APFS image-only
  format scratch image for certification, validate structured image-only
  execution evidence, and expose only generated-layout create/format,
  checksum-repair, bounded root-file write/patch/delete, empty
  root-directory create/delete, root-directory child-file write/patch/delete,
  volume-label change, snapshot create/delete/revert, file clone,
  file hard-link, and in-chunk container resize actions through UI/queue/apply.
  (Corrected 2026-08-30: the list ended at volume-label change and was closed
  with "only", but `partition_manager_panel.cpp:10285-10291` reaches snapshot
  create/delete/revert and container resize from the APFS Container dialog, and
  `ApfsCloneRootFile`/`ApfsHardlinkRootFile` are declared operation types. Those
  shipped with the A1-A8 certification, so the code is not exceeding policy --
  but a readiness doc that UNDERSTATES which destructive operations are
  UI-reachable is wrong in the direction that matters, since a reviewer using it
  to decide what needs certifying would never look at snapshot-revert or resize.) The certifier-generated format image must detect as
  APFS, report nonzero spaceman free bytes, round-trip the requested volume name,
  list the generated root directory, and read back the seeded multi-block proof
  file through the read-only APFS browser. The certifier-only write lane may copy
  a generated APFS image to a separate scratch image, add, replace, byte-range
  patch, or delete one bounded root regular file, add/patch/delete one bounded
  regular child file inside a supported generated root directory, preserve
  existing bounded root regular files/directories, zero the previous generated
  file-data region on delete, and prove create/add/replace/patch read-back plus
  delete negative-read-back, empty-directory read-back, and sibling preservation
  for generated images.
  The hidden existing-image format lane may overwrite only after destructive
  wipe confirmation and must prove post-format APFS detection plus empty-root
  browse. The raw-capable target API and read/write raw-device opener must stay
  fail-closed unless the request carries raw-target opt-in, non-image-only writer
  options, destructive evidence, and raw hardware certification evidence. The
  APFS raw-format validation lane must run only through
  `scripts/run_partition_manager_apfs_raw_format_validation.ps1` or its UAC
  launcher on pinned expendable APFS GPT media; passing this lane is required
  before any APFS raw format/write/patch/delete, empty-root-directory create/delete,
  root-directory-child-file write/patch/delete, volume-label change, or repair
  certifier claim can be made. The previous 2026-06-12 PDT / 2026-06-13 UTC
  51 GB JMicron run `run-20260612-192652` is retained as Windows-side evidence
  only because Apple kernel validation later rejected the large generated
  spaceman geometry. Current Windows-side proof passed on JMicron serial
  `DD56419883A5B`, disk 2 partition 2, 134,217,728 bytes, at
  `artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`;
  Apple-native validation has since been completed across the full A1-A8 driver
  track, incl. the A8 physical-USB destructive/crash/rollback gate
  (`external.apfs-a8-physical`), so the 128 MiB JMicron run is retained as the
  I1-era Windows-side evidence the multi-CIB/CAB geometry later superseded. The accepted lane uses bounded APFS detection/root-listing readback, generated-layout-gated
  root-file write/readback, generated-layout raw root-file byte-range patch,
  generated-layout empty root-directory create/delete with empty-listing and
  root-absence proof, root-directory child-file write/read/patch/delete proof with
  SHA-256 `f22535d6b86d4744537a0f58cdfc613162efa764610a9b191b53d5762a63566e`,
  patched child-file SHA-256
  `1d45dc05a258a2e29a7b77cdf5a6763e8f6ac138ee0c4066d6aaf6115eb45d27`,
  non-empty directory delete blocker proof, intentional generated metadata block
  199 checksum corruption, in-place checksum repair across the generated
  metadata block set with
  patched-file read-back, generated-layout raw volume-label change with relabeled
  root-listing proof and patched-file hash preservation, raw root-file delete
  with negative read-back, and an intentional full raw-device SHA-256 skip on a
  one-spaceman-chunk target. The
  certifier-only repair lane may
  restamp recognized APFS metadata object checksums in a separate generated
  scratch image and must prove corrupt-browse failure before repair plus
  post-repair listing/read-back; it is not raw-media APFS repair approval.
- APFS read-only browsing must fail closed on corrupt APFS metadata object
  checksums. File extent payload blocks are not validated as APFS object blocks.
- File Management cross-filesystem proof is destructive and remains outside
  CTest. The latest live lane wrote to pinned expendable physical media through
  `scripts\run_file_management_live_filesystem_certification.ps1`: APFS on
  JMicron serial `DD56419883A5B`, disk 2 partition 2, passed File Explorer
  create-directory, write/read, duplicate finder scan, advanced search,
  delete-file, negative read, and delete-directory at
  `artifacts\file-management-live-certification\disk2-apfs-script-after-fix\file-management-live-certification.json`;
  HFS+ on Best Buy serial `DD564198838A8`, disk 3 partition 3, passed the same
  lane plus rename at
  `artifacts\file-management-live-certification\disk3-hfs-script-after-fix\file-management-live-certification.json`.
  APFS destructive File Management proof is Apple-native certified across the
  A1-A8 driver track (incl. the A8 physical-USB gate, `external.apfs-a8-physical`);
  the earlier 128 MiB Windows-side run is retained as I1-era evidence.
  File Organizer generic moves remain local/mounted-file-API only.
- 2026-07-12 File Explorer command-route certification closed the Files-like
  plan's live-cert lane. The bridge now write-enables any known-size in-range
  APFS raw/image target (size superblock-derived for raw partitions),
  S.A.K.-generated and real Apple-created (foreign) alike. Evidence: foreign-APFS
  full destructive command route on a 7.45 TB real newfs_apfs container with
  Apple-derived `apfsck -cw` EXIT 0 clean
  (`artifacts\file-management-live-certification\disk2-foreign-apfs-destructive\`);
  HFS+ destructive command route (adds rename/read-after-rename) on a 256 MiB
  Apple-HFS partition with bundled `fsck_hfs` and probe `--hfs-check` clean
  before and after
  (`artifacts\file-management-live-certification\disk1-hfs-file-management-cert\`);
  ext4 read-only copy-out and XFS/Btrfs metadata-only blockers on WSL-built
  images
  (`artifacts\file-management-live-certification\ro-ext-xfs-btrfs-20260712\`).
  Unknown-size / out-of-range / snapshot-frozen / Fusion / locked-encrypted APFS
  targets stay read-only with exact blockers. Full local CTest is 257/257
  (corrected 2026-08-30; this said 142/142, the same stale figure the README
  test badge carried).
- Partition Manager ext/HFS+/APFS image browsers must open both normal image
  files and read-only Windows raw partition aliases through
  `openFileOrRawDeviceReadOnly`; physical Apple proof requires HFS+ and APFS
  root listing and can require selected-file read proof, HFS+ selected-attribute
  proof, plus APFS bounded export proof with
  `scripts/run_partition_manager_physical_apple_probe_validation.ps1 -RequireHfsFileProof -RequireHfsAttributeProof -RequireApfsFileProof -RequireApfsExportProof`.
  Current physical proof read `/.fseventsd/0000000000d34037` from the
  expendable APFS partition and recorded SHA-256
  `d5b58364da8b5f9e7996bcc9494db9f93a76484fc137e5aa6153809b8935c0d4`;
  APFS bounded export proof exported `/.fseventsd` with 6 files,
  0 directories, 6 scanned entries, and 67906 bytes.
  (Corrected 2026-08-30: these figures said the APFS proof read `/Fonts/00TT.TTF` at 25056 bytes with sha `d075a134...` and exported `/Fonts` with 63 files / 1 directory / 64 scanned entries / 21161830 bytes. That file and hash are the HFS+ proof's -- `hfs-read-00tt.json` in the same evidence directory records them against HFS+ -- so an HFS+ read was attributed to APFS. `/Fonts/00TT.TTF` does exist on the APFS partition (`apfs-list-file-candidate-directory.json` lists it at 25056 bytes; the operator copied files to both Apple partitions), but no APFS READ of it with a recorded hash exists, and the `/Fonts` export figures appear in no artifact at all. The APFS proof picks its subject by bounded candidate search, so the final run selected `/.fseventsd` instead. Quote the artifact, not the filename: this proof rewrites its own subject every run.) The same report records HFS+
  detection, root metadata, `total_bytes=127724052480`,
  `free_bytes=125197647872`, and selected-file proof for
  `/polyhavenassets_blendermarket_v1.2.0  (Blender 4.5+).zip` with SHA-256
  `72222f9f83d177ea9a2970edb756ab9f3f9f6b00a12f47af63259f215d970709`.
  HFS+ selected-attribute proof also passed by reading inline attribute
  `com.apple.decmpfs` for file ID 1894 at 16 bytes with SHA-256
  `e2aed0d76e90c81c39f9916d56a8f1631c440aec356877ea7dae197b069ea64f`; the
  report records `validation_requirements` showing HFS file, HFS attribute,
  APFS file, and APFS export proofs were required.
  The five-partition APFS physical lane also passes the all-partition APFS
  file/export proof flags using bounded recursive candidate search, including
  hidden Apple metadata directories when no visible regular file is present.
- APFS free-space reporting must stay verified both for probe-window
  space-manager objects and for checkpoint data outside the first 2 MiB probe
  window. The current physical regression command is
  `scripts\run_partition_manager_physical_apple_probe_validation.ps1 -DiskNumber 3 -AllowMissingHfs -ProbeAllApfsPartitions -RequireAllApfsPartitions`;
  it passed against five APFS partitions on a second expendable non-boot USB
  disk and recorded nonzero `free_bytes` for every APFS partition under
  `artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-apfs-multipart-physical\report.json`.
- Partition Manager non-native write payloads must use the exact selected
  `\\?\GLOBALROOT\Device\HarddiskN\PartitionM` raw partition alias. Missing,
  forged, or mismatched paths are blocked by safety validation and script
  generation before Apply, and queued ext repair keeps the destructive target
  field read-only.
- Partition Manager ext browse is read-only and preserves symlink metadata by
  displaying targets and exporting `.symlink.txt` sidecars; it does not follow
  symlinks or create links on Windows.
- QRC resource verification passes.
- Portable package smoke passes from a clean extracted folder.
- Startup E2E smoke passes from the packaged folder.
- All packaged `.exe` and `.dll` files have valid Authenticode signatures.
- `SHA256SUMS.txt` is regenerated after signing.

Use:

```powershell
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"

powershell -NoProfile -File scripts/stage_portable_release.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

powershell -NoProfile -File scripts/create_release_archive.ps1 `
  -BuildDir build\Release `
  -PackageName $packageName

$extract = Join-Path 'build\Release' ('clean-readiness-extract-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $extract) { throw "Extraction target already exists: $extract" }
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract

powershell -NoProfile -File scripts/check_release_readiness.ps1 `
  -PackageRoot $extract `
  -RequireSignedPackage
```

`check_release_readiness.ps1` expects a clean package folder. Startup smoke
creates normal runtime folders such as `data\logs` and `data\temp`, so run the
aggregate readiness gate against a fresh extract when validating the ZIP.

For unsigned local preflight, run the same command without
`-RequireSignedPackage`; signed package validation remains required before
publication.

Partition Manager certification defaults to a non-mutating plan-only harness run
so release readiness can run on ordinary CI and developer shells. When a strict
elevated disposable-VHD report already exists, verify that archived VHD evidence
against that report instead of generating a new plan-only report:

Default readiness also writes `external-evidence.template.json`,
`external-evidence.checklist.md`, an `external-evidence/` per-gate lab package,
`vhd-preflight.json`,
`certification-gap-report.json`, `certification-gap-report.md`, and
`certification-artifact-bundle.json` under the selected certification root, then
verifies that the Markdown checklist contains every external gate, safety
contract, required evidence key, required value hint, and artifact path from the
same certification matrix. The JSON manifest remains the verifier input; the
Markdown checklist is the human lab run sheet generated from the same matrix.
The lab package contains one run-sheet directory per external gate and is
verified by `scripts/check_partition_manager_external_lab_package.ps1`; each
directory includes a `report.template.json` scaffold for the local evidence
report. After lab operators save completed per-gate `report.json` files, run
`scripts/update_partition_manager_external_manifest_from_reports.ps1` to import
them into the external manifest; the importer rejects wrong IDs, stale matrix
contracts, missing evidence keys, blank values, wrong required values, and
missing verification summaries. Pass `-ChecklistPath` and
`-OutputChecklistPath` so the importer writes a paired checklist for the
imported manifest; it also refreshes the per-gate run-sheet README and
`report.template.json` manifest references under the evidence root. The package
does not count as evidence until the imported JSON manifest is filled with
passed, non-empty payload values plus real artifact paths or HTTPS URLs.
The gap report is the current machine-verified list of incomplete VHD and
external gates, including evidence keys, safety contracts, and next commands.
Those next commands are status-aware: once strict VHD evidence is complete, the
report moves operators to external evidence collection, per-gate `report.json`
import, and the strict hardware handoff instead of asking for another VHD run.
The artifact bundle hashes the current status, harness report, VHD preflight,
gap report, external manifest, and lab checklist so release handoff artifacts
cannot silently drift apart.
The VHD preflight report records whether the current host has administrator
rights, required Storage cmdlets/tools, enough temporary drive letters, and
enough workspace for the elevated disposable-VHD certification run. Default
readiness records blockers without failing a non-admin developer shell; use
`scripts/test_partition_manager_vhd_preflight.ps1 -RequireAdministrator
-RequireReady` immediately before starting a strict VHD run.
For the strict elevated VHD handoff, run
`scripts/run_partition_manager_vhd_certification_strict.ps1 -RelaunchElevated`;
it fails unless strict VHD evidence verifies and every disposable-VHD scenario
passes.

```powershell
powershell -NoProfile -File scripts/check_release_readiness.ps1 `
  -PartitionCertificationRoot artifacts\partition-manager-certification\vhd-strict `
  -RequirePartitionVhdEvidence
```

Add `-PartitionExternalEvidenceManifest <path>`,
`-PartitionExternalEvidenceChecklist <path>`, and
`-RequirePartitionExternalEvidence` only after every external VM/hardware/lab
gate is filled with passed, non-empty evidence and the paired checklist verifies
against the same matrix. Strict external evidence fails without the checklist.
For the final strict hardware evidence handoff, run
`scripts/run_partition_manager_hardware_certification_strict.ps1` with the
strict VHD certification root, external manifest, checklist, and evidence
directory. It is non-mutating and requires `HardwareCertified` status before
any release claim uses that wording.
If `certification/PARTITION_MANAGER_CERTIFICATION_MATRIX.json` gains a VHD scenario,
the matrix-integrity guard requires the harness and certification documentation
to include the new scenario before readiness can pass. Rerun the elevated VHD
matrix before using `-RequirePartitionVhdEvidence`.

## Clean VM Manual QA

Run on a fresh Windows 10/11 x64 VM with no repo checkout and no developer
environment variables:

Latest VM smoke evidence: on 2026-06-02 PDT, VirtualBox 7.2.8 VM
`SAK-PM-Lab-Win11` booted Windows 11 25H2 and launched
`\\vboxsvr\sakrepo\build\Release\sak_utility.exe` from a read-only shared
folder. `SAK_STARTUP_SMOKE_OK` passed, and the visible Partition Manager UI
rendered 3 disks, including two RAW 4 GB data disks, in
`artifacts\partition-manager-certification\vm-lab\sak-utility-share-partition-manager-mouse-fixed.png`.
That smoke proved read-only shared-folder startup and non-admin inventory. It
did not prove destructive guest mutation because the guest token was not
administrator.

Follow-up destructive VM smoke passed on 2026-06-02 07:44 PDT in the same VM
after UAC elevation produced `is_admin=true`. The smoke mutated only disposable
4 GB VirtualBox data disks 1 and 2 after boot/system/model/size guards, verified
GPT NTFS create/format/resize/delete/clear on disk 1, verified MBR
FAT32-to-NTFS conversion and clear on disk 2, and left both data disks RAW,
non-boot, and non-system. Evidence:
`artifacts\partition-manager-certification\vm-lab\partition-vm-admin-report.json`.
This does not replace the 18 external VM/hardware/lab gates required for
the hardware-certified claim level.

AI Assistant VM live E2E passed on 2026-06-03 PDT / 2026-06-04 UTC in the
same `SAK-PM-Lab-Win11` Windows 11 VM from shared root `\\vboxsvr\sakrepo`.
The package-only harness read the OpenAI key from the provided key file without
writing it to reports, then passed `test_openai_responses_client.exe` with live
Responses API plain-response and function tool-loop smoke, portable startup
smoke, runtime accessibility audit, and the AI smoke checklist. Evidence:
`artifacts\ai-assistant-vm-smoke\run-20260604-033445\ai-assistant-vm-smoke-report.json`.

AI Assistant current-binary host/live quality pass completed on 2026-06-04 UTC:
Release `sak_utility`, `test_ai_subagent_runner`, `test_ai_chat_title`,
`test_ai_prompt_assembler`, `test_ai_orchestrator`, `test_ai_workflow_evals`, and
`test_openai_responses_client` built successfully; targeted CTest passed 7/7;
live OpenAI plain response and function tool-loop CTest passed using the key
file without printing the key; host package/live smoke passed at
`artifacts\ai-assistant-vm-smoke\run-20260604-060003\ai-assistant-vm-smoke-report.json`.
The latest local follow-up on 2026-06-07 PDT / 2026-06-08 UTC passed the full
Release CTest suite at 136/136, release readiness, `git diff --check`, and the
Release app/helper rebuild after the APFS empty root-directory writer slice,
HFS secure-delete physical proof, docs sync, and lizard refactors.
The follow-up multi-agent quality pass removed the production UI serial-only
cap for workflow delegates: read-only delegate phases can now run up to three
parallel subagents with a fresh OpenAI model client per subagent, while
mutating and conditional phases remain serialized by the orchestrator.
The latest chat-continuity pass now persists assistant response ids into
transcript metadata and restores the last assistant response id when a saved
chat is reopened, preserving Responses API `previous_response_id` continuity
for follow-up turns instead of detaching the reopened chat from server-side
conversation state.
The latest UI polish pass also adds result-bubble copy buttons, Enter-to-send
with Ctrl+Enter newline behavior, New Chat draft reset semantics, visible
background-agent counts, workflow-selection role preview without a role dropdown,
first-prompt role inference with explicit user role-switch support, and an exact composer-side
`Ctx: x/y` context-window meter tied to the selected model through OpenAI
`/v1/responses/input_tokens` so operators can judge when summary/report
compaction is needed.
The current local package proof staged `S.A.K.-Utility-0.9.1.9` (STALE: `VERSION`
is 0.9.2.0 and the recipe below produces `SAK-Utility-v0.9.2.0`; this proof
predates the version bump and needs re-running before release), passed portable
dependency smoke, passed portable startup E2E smoke, passed package-root release
readiness, and rebuilt `S.A.K.-Utility-0.9.1.9-Windows-x64.zip` after the
refactors.
Current staged-package hashes:
- `sak_utility.exe`: `E587E51085199FF18027AF43883DFAA9FD6841FE2129DE0F72A953A7357D9277`
- `sak_elevated_helper.exe`: `EC64933550AC73948F6113E4F7E800EF3BF4EDD084409168F330465DCF53B045`
- `S.A.K.-Utility-0.9.1.9-Windows-x64.zip`: `1DA03D804D0A9EA87DB950BDE49D70482A4BE2D6DB59E532C75F0403D9819261`
The earlier current-binary VM rerun attempted a blank-password guest-control
login and failed before launching the in-guest harness. Do not use blank
guest-control auth for this VM. The fixed host path uses a real guest password
file resolved from the VM unattended metadata or `temp\vm-auth`, then passes it
to `VBoxManage guestcontrol --passwordfile`. For production-grade noninteractive
VM gates, the token must already be a direct administrator token. Keypress-based
UAC acceptance is fallback-only and must be enabled explicitly with
`-AllowUacKeypressFallback`; normal reruns should use the dedicated automation
VM created by `scripts/new_partition_manager_automation_vm.ps1`. This is not a
blocker for non-destructive AI Assistant readiness because the current-binary
local and host package/live smoke passed.

Partition Manager ext filesystem write support has a dedicated destructive VM
gate. Preferred host-driven run:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_vm_gate_host.ps1 `
  -Gate ext-filesystem `
  -DiskNumber 1 `
  -FileSystem ext4
```

The host launcher resolves the real VM guest password file, validates
guest-control auth, requires a direct administrator token, runs the destructive
runner through guestcontrol, and waits for a fresh report. If the legacy lab VM
only provides a filtered token, the launcher fails before deleting stale
evidence unless `-AllowUacKeypressFallback` is supplied explicitly. Manual
in-guest launch is still valid from the `SAK-PM-Lab-Win11` desktop:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  \\vboxsvr\sakrepo\scripts\launch_partition_manager_ext_filesystem_vm_gate_local.ps1 `
  -DiskNumber 1 `
  -FileSystem ext4
```

The elevated runner verifies the bundled e2fsprogs hashes, formats a raw ext4
partition, runs the confirmed repair command path, grows the partition with
`resize2fs`, shrinks the file system before shrinking the partition, rechecks
the filesystem, clears the disposable disk, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
Latest evidence passed on 2026-06-05 against
`\\?\GLOBALROOT\Device\Harddisk1\Partition2` with exit code 0 for format,
repair, read-only checks, grow, shrink, and cleanup. Keep future non-ext writes
blocked until operation-specific proof exists.
An unattended direct-admin rerun also passed in dedicated VM
`SAK-PM-Automation-Win11` on 2026-06-05 PDT / 2026-06-06 UTC. The host report
`artifacts\partition-manager-certification\vm-lab\host-launch\ext-filesystem-20260605224315.json`
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`; the gate report is
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext-filesystem-write\report.json`.
The host launcher now retries Guest Additions desktop readiness before starting
guestcontrol work so a cold-boot VM does not fail on one early `waitrunlevel`
miss.

Linux swap raw-partition automation rerun passed on 2026-06-05 PDT /
2026-06-06 UTC in dedicated VM `SAK-PM-Automation-Win11`. The VM creation report
records `direct_admin_guestcontrol_verified=true`, the host launch report
records `launch_mode=direct-admin`, `direct_admin_token=true`, and
`uac_accept_sent_at=null`, and the gate report records `status=Passed`,
`detected_file_system=Linux swap`, raw target
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`, and cleanup back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\automation-vm\SAK-PM-Automation-Win11-20260605210356.json`,
`artifacts\partition-manager-certification\vm-lab\host-launch\linux-swap-20260605220305.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.

Linux compatibility validation for the same bundle passed on 2026-06-07 UTC with
`scripts/run_partition_manager_ext_linux_validation.ps1 -DistroName archlinux`
for ext2, ext3, and ext4. The harness used Windows-bundled `mke2fs` and
`resize2fs`, then Linux `e2fsck`, `dumpe2fs`, and loop mount accepted each
grown/shrunk image, wrote a fixture file, rechecked clean, and bundled Windows
`e2fsck` rechecked clean after the Linux write. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext2-linux-validation\report.json`,
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext3-linux-validation\report.json`,
and
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.ext4-linux-validation\report.json`.
This validation does not create a runtime WSL dependency.

Linux-created XFS/Btrfs metadata validation passed again on 2026-06-07 UTC with
`scripts/run_partition_manager_linux_metadata_validation.ps1 -DistroName archlinux`.
The harness used Linux `mkfs.xfs` and `mkfs.btrfs` to create disposable images,
then `partition_filesystem_probe_certifier.exe` validated S.A.K. raw detection,
metadata fields, and sanity lines for XFS and Btrfs from Windows. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-metadata-validation\report.json`.
This validation does not create a runtime WSL dependency. It supports the
in-app original read-only metadata consistency check from captured probe data,
but does not approve deep XFS/Btrfs tool checks, format, repair, or write
support.

Filesystem probe helper offset validation is now part of the local release
gate. `partition_filesystem_probe_certifier.exe` accepts
`--input-offset-bytes`, reports `input_offset_bytes` in JSON, and reads from the
offset for regular files and raw Windows device paths. It also accepts
`--hfs-check` for HFS+/HFSX consistency and attribute metadata output and
HFS+ selected-attribute read arguments for file ID/name proof when matching
attributes exist on media. The generated-fixture
self-test `scripts/test_partition_filesystem_probe_certifier.ps1` is wired into
CTest as `test_partition_filesystem_probe_certifier` and into release readiness;
it creates a tiny padded ext2 fixture, verifies deterministic offset report
fields, and confirms invalid offset input fails. A lab-only Sonoma 14 DMG HFS+
fixture also passed at byte offset 134250496 with sane HFS+ header metadata.
The unpacked Tahoe installer `SharedSupport.dmg` is a compressed UDIF with an
HFS+ payload and is useful for future container-extraction validation only; it
is not bundled or required at runtime.

Physical Apple filesystem validation is optional and read-only:

```powershell
powershell -NoProfile -File `
  scripts\run_partition_manager_physical_apple_probe_validation.ps1 `
  -DiskNumber 2
```

The current physical run passed on 2026-06-07 UTC against a
non-boot external USB NVMe GPT disk with one Apple HFS partition and one Apple
APFS partition. The script used `GLOBALROOT` partition aliases, detected HFS+
and APFS with sane metadata, required HFS file, HFS attribute, APFS file, and
APFS export proof, and wrote
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.apple-filesystem-physical\report.json`.
The latest APFS physical probe also verifies free-space reporting from the APFS
space manager: `total_bytes=127992487936`, `free_bytes=127866097664`.
This optional gate is not part of the 18-gate commercial destructive matrix and
does not approve unbounded HFS+ folder-tree delete, complex HFS+ file delete,
HFS+ B-tree split/rebalance, broad HFS+ allocation growth beyond the bounded initial-extent slice,
inline/broad HFS+ attribute growth, compressed-file writes,
arbitrary APFS mutation, non-generated APFS file-write, or APFS
resize behavior. The APFS writer helper exposes generated/minimal APFS
create/format, generated-image root-file create/replace/byte-range patch/delete,
empty root-directory create/delete, root-directory child-file write/patch/delete,
and volume-label change with read-back, directory-empty, old/new-label, or
negative-read-back hashes, generated-layout
raw root-file write/patch/delete, empty root-directory create/delete, and
root-directory child-file write/patch/delete plus volume-label change
queue/apply routes, and generated-layout checksum repair only. Production helper
physical proof is Apple-native certified across the A1-A8 driver track to a
24 TiB cap, incl. the A8 physical-USB destructive/crash/rollback gate
(`external.apfs-a8-physical`). The earlier 128 MiB JMicron Windows-side run
(`artifacts\file-management-live-certification\disk2-apfs-128mb-raw-format\report.json`)
and `run-20260612-192652` are retained as the I1-era Windows-side evidence the
multi-CIB/CAB geometry later superseded.
The production helper does not blanket-enable protected/compressed/snapshot/
multi-volume APFS mutation; each is reached only through its own certified
operation-specific route (A3 snapshots, A4 multi-volume, A5 compression, A6
credential-gated encryption - all Apple-certified), never blanket-on.
Fusion/Tier2 multi-device stays out of scope and unsupported incompatible
feature flags remain fail-closed.
APFS and HFS+/HFSX are now full driver-level write-certified (A1-A8 / H1-H8 -
see `docs/archive/APFS_HFS_FULL_DRIVER_WRITE_PLAN.md`). Release wording must keep
XFS/Btrfs to read-only metadata, and may state full driver-level APFS + HFS+/HFSX
read/write with the standing hard boundaries: APFS Fusion/Tier2 multi-device out
of scope, encryption credential-gated, sealed-system-volume writes behind a typed
seal-invalidation confirmation, APFS writes gated to S.A.K. generated-layout
containers at the Apply layer, and APFS container shrink + chunk-adding grow as
documented follow-ons.

APFS image-offset proof also passed on 2026-06-06 UTC against
`temp\Sonoma 14.dmg` using `--input-offset-bytes 1233310720`; evidence:
`artifacts\partition-manager-certification\tool-tests\sonoma-apfs-offset-probe.json`.
The Tahoe `SharedSupport.dmg` `NXSB` hits were rejected as direct APFS proof
because no 4096-byte-aligned superblock hit with sane block geometry was found.

HFS+/HFSX physical destructive tool proof uses
`scripts/launch_partition_manager_physical_hfsprogs_validation_local.ps1` and
`scripts/run_partition_manager_physical_hfsprogs_validation.ps1`. The launcher
uses Windows `runas` UAC auth, not keypress automation; the runner requires
admin, `-Force`, non-boot/non-system disk guards, USB media by default, and
serial or friendly-name guards for large disks. The proof formats sparse staging
images as HFS+, HFSX, then HFS+ again by default, runs `fsck_hfs`
repair/final read-only checks on those images, copies allocated metadata ranges
to the selected raw partition, verifies S.A.K. HFS detection with
`partition_filesystem_probe_certifier.exe` after each case, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.hfsprogs-physical-destructive\report.json`.

Physical cross-filesystem destructive validation is optional and must target
known expendable external media:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_physical_cross_filesystem_destructive_validation_local.ps1 `
  -DiskNumber 2 `
  -ExpectedSerialNumber DD564198838A8 `
  -ExpectedFriendlyNamePattern "Best Buy|NS-PCNVMEHDE" `
  -Force
```

The launcher uses Windows UAC `runas` auth, not keypress automation. The
elevated runner requires admin, `-Force`, non-boot/non-system checks, USB media
by default, and serial/friendly-name guards for large disks. The current run
passed on 2026-06-05 PDT / 2026-06-06 UTC: ext2, ext3, and ext4 were formatted,
repaired, read-only checked, grown, shrunk, and probe-verified after each stage;
Linux swap SWAPSPACE2 metadata was written, reread, probe-verified, and the disk
was cleared back to RAW. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.cross-filesystem-physical-destructive\report.json`.

Linux swap format compatibility validation passed on 2026-06-05 PDT with
`scripts/run_partition_manager_linux_swap_format_validation.ps1 -DistroName archlinux`.
The harness wrote an original SWAPSPACE2 v1 header to a disposable image using
the same metadata layout as the app formatter, then S.A.K.
`partition_filesystem_probe_certifier.exe`, Linux `blkid`, and Linux
`swaplabel` all recognized the image as swap with matching label, UUID, version,
and page-size metadata. Evidence:
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-validation\report.json`.
This validation does not create a runtime WSL dependency and does not replace a
future raw-disposable-partition VM proof if Linux swap format is raised to a
separate destructive-media certification gate.

Linux swap raw-partition VM proof now has a dedicated destructive runner and
launcher. Preferred host-driven run:

```powershell
powershell -NoProfile -File `
  scripts\launch_partition_manager_vm_gate_host.ps1 `
  -Gate linux-swap `
  -DiskNumber 1 `
  -PageSizeBytes 4096
```

Manual in-guest launch is still valid from the `SAK-PM-Lab-Win11` desktop when
guest UAC is available:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File `
  \\vboxsvr\sakrepo\scripts\launch_partition_manager_linux_swap_vm_gate_local.ps1 `
  -DiskNumber 1 `
  -PageSizeBytes 4096
```

The elevated runner writes original SWAPSPACE2 v1 metadata to an app-style raw
partition target, rereads and verifies the header, runs
`partition_filesystem_probe_certifier.exe` against the raw target, clears the
disposable disk back to RAW, and writes
`artifacts\partition-manager-certification\vm-lab\external-evidence\external.linux-swap-format-vm\report.json`.
Latest evidence passed on 2026-06-05 PDT against
`\\?\GLOBALROOT\Device\Harddisk1\Partition2`; the host launch report is under
`artifacts\partition-manager-certification\vm-lab\host-launch\` and redacts the
password value. This gate is separate from the image-level Linux
`blkid`/`swaplabel` validation above.

Eighteen matrix-backed external gates now pass in VM-lab evidence. The first
imported batch covered:
`external.bitlocker`, `external.bitlocker-mutation`,
`external.file-level-data-recovery`, `external.allocate-free-space`,
`external.cluster-size-change`, `external.hdd-defrag-execution`,
`external.usb-removable`, `external.ssd-retrim`,
`external.ssd-secure-erase`, `external.partition-move`,
`external.primary-logical-conversion`, `external.volume-serial-number`,
`external.dynamic-to-basic`, and `external.hardware-wipe`.
Evidence lives under
`artifacts\partition-manager-certification\vm-lab\external-evidence\`, with
imported manifest
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.json`
and paired imported checklist
`artifacts\partition-manager-certification\vm-lab\external-evidence.imported.checklist.md`.
The imported status was reported as `HardwareCertified` at the June checkpoint: 18/18 external
gates and 12/12 VHD scenarios were recorded as passed. Current R5 verification rejects the old
image-restore proof because it copied VHD containers instead of exercising the application
controller. The replacement controller-driven report and supplementary refusal/offline fixtures
passed host validation at their frozen source checkpoint. After the CTest launch repair changed
the bound build configuration, the rebuilt helper repeated the complete VHD matrix and five
supplementary cases; current-source host validation passed again.
 The September 8 checkpoint supported `VhdDataDiskCertified`, not a permanent current claim.
 The 2026-09-19 `CodeCompleteOnly` (11/12) state was a source-freshness refusal after the
 MCP header changed; the earlier report remains historical. On 2026-09-25 the Windows 11 VM
 strict disposable-VHD run passed 12/12 scenarios with the W68 controller binary and its
 403-file source manifest. The host independently verified the copied report under
 `artifacts/partition-manager-certification/vhd-strict/run-20260925-121253/` against that same
 explicit binary/manifest. The current VHD-only claim is `VhdDataDiskCertified`; no external
 manifest is implicitly selected.
 The historical 18-row external manifest is retained but is not fresh current-application proof.
Release remains incomplete on current external/hardware and whole-application E2E evidence.
The historical external-gate batch completed on 2026-06-03 PDT:

- `external.os-migration-reboot`: target-only cloned OS VM booted with UEFI
  firmware to Guest Additions run level 3.
- `external.boot-repair-uefi`: disposable UEFI BCD/fallback-loader removal
  produced a no-boot symptom, and offline `bcdboot` repair restored boot.
- `external.boot-repair-bios`: disposable BIOS/MBR `\Boot\BCD` removal
  produced Windows Recovery `0xc000000f`, and offline `bcdboot` plus
  `bootsect` restored boot.
- `external.system-mbr2gpt`: disposable BIOS/MBR boot VM validated and
  converted disk 0 with `mbr2gpt` exit code 0, changed MBR to GPT, and the
  converted disk booted under a fresh EFI VM to Guest Additions run level 3
  after VirtualBox UEFI boot-file normalization.

Strict release readiness with
`-RequirePartitionVhdEvidence -RequirePartitionExternalEvidence` passed at that checkpoint;
that historical run does not itself prove the current source or the stronger image-restore contract.
The historical machine-readable release evidence snapshot is tracked under
`artifacts\partition-manager-certification\vhd-strict\run-20260602-194934\`
and `artifacts\partition-manager-certification\vm-lab\external-evidence\` so a
clean checkouts retain the provenance. That snapshot alone no longer supports a current
`HardwareCertified` claim; use the current matrix and R5 evidence requirements.

1. Extract the release ZIP into a clean folder.
2. Run `sak_utility.exe`.
3. Confirm the app starts without Qt/plugin/runtime errors.
4. Confirm `data\logs` is created under the portable folder.
5. Open AI Assistant and confirm provider diagnostics render.
6. Run a package search request and verify no install occurs.
7. Run package install request only after explicit install wording.
8. Run Network Diagnostics ping/DNS/port-scan safe tools.
9. Run Diagnostics hardware scan and generate a report.
10. Run Backup wizard dry path through validation without writing outside the selected target.
11. Run App Management scan/export.
12. Confirm About panel includes third-party attribution.
13. Close and reopen the app; verify settings and sessions remain portable.
14. Verify no files are written under the source repo or developer profile except standard Windows logs.

## Rollback And Versioning

- Release tags must use `v<major>.<minor>.<patch>.<build>` and match `VERSION`.
- A release is rollback-safe only when the previous signed ZIP remains available.
- If a release is pulled, mark the GitHub release as draft or delete the asset,
  publish a replacement patch version, and document the reason in release notes.
- Never overwrite a published ZIP for the same tag. Rebuilds require a new tag.
- Keep at least one previous known-good release artifact available.

## App-Control Rule

App-control workflows ship only when the manifest action is backed by either:

- a documented CLI with tested arguments, or
- a tested Win32 GUI automation workflow with stable selectors and failure
  evidence.

If neither exists, the manifest must expose observation/status only and fail
explicitly for the unsupported action.
