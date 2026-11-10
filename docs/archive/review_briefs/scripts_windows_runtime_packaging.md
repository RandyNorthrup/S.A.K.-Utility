# Windows runtime packaging: ICU, install routing and required artifacts

Status: COMPLETE for the bounded runtime, capture, archive and startup-wrapper repairs described
here. The September 19 full Release build and 281/281 CTest gate passed on their final code;
build/wave36-full-build.log and build/wave36-full-ctest.log retain those results. Fresh canonical
install/stage/archive and separate native PowerShell 5.1/7 startup runs passed under
build/pk36-458da9ecf137. Both restored all 196 path/type/attribute/length/hash entries exactly,
passed pristine static checks and left no app process. The app deadline stayed 30 seconds.
Wrapper times of 6.823/6.641 seconds are not cold-start measurements. Browser-extension
installation, whole-application UI coverage and hostile filesystem races are not certified here.
Failed rehearsals remain intact; no existing build/system DLL was deleted.

## Archive crash-evidence boundary (2026-09-19)

The standalone archiver omitted data/crashes even though portable static validation rejected it.
It could therefore archive a failed run's crash evidence. Its existing output replacement also
preceded package validation. The new dedicated crash check runs before any removal/compression
and refuses a crash path regardless of CleanMutableRuntimeData. Adding it to the ordinary mutable
cleanup list would be wrong: that switch must not delete crash evidence to manufacture success.

The existing registered portable-stage suite now runs the real child archiver against fresh
ordinary fixtures. A clean fixture creates a real ZIP and verifies required member contents.
Populated directory, empty directory and file crash paths are checked with cleanup both off and
on. Refusal must preserve every path/type/file hash, including previous ZIP, checksum and signing
catalog bytes. The -ArchiveOnly selector runs these checks without launching the app or staging
the full portable package; -ArchiveScriptPath supports isolated production-copy red drills.
Canonical R5 owns current results, independent guard reversals and final full-gate status.

## Child-only startup environment (2026-09-19)

Real isolated process checks found that smoke's old parent-environment setup/restoration was not
exact: PowerShell7 turned absent variables into present-empty entries, while Windows PowerShell5.1
lost genuinely present-empty entries. Value-only hashtable mocks hid both outcomes. A null-delete
helper alone could not preserve all states on both hosts.

The canonical capture seam now accepts optional EnvironmentOverrides and updates only its child's
ProcessStartInfo.EnvironmentVariables. Null removes the child's key; an empty string remains
present-empty; other strings replace values; unspecified entries stay inherited. Caller state is
never changed. Names, string-or-null values, duplicate Windows-case names, NULs and finite input
bounds are validated before process creation. Diagnostics never print override values. Existing
capture deadline, output, stdin and cleanup contracts remain independently tested.

Portable smoke passes its existing five settings through that seam and no longer needs a parent
environment restoration path. Canonical tests now exercise real child inheritance and caller
presence/value on both Windows hosts, including nonzero exits and failed starts. The final full
gate and fresh package execution cited above cover this code. The separate current-capture FAT
refresh at C:\SC\w34f-5b3321cf passed observed/pristine runs and independent host/report checks;
earlier native package and FAT artifacts remain historical, not relabeled changed-source proof.

## Earlier ICU packaging defect

The pinned Qt 6.10.3 windeployqt selects the host's System32/icuuc.dll when inspecting Qt6Core.
Its dry-run mapping reproduced that source exactly. The file in build/Release is byte-identical
to the Windows copy, SHA-256
59B48000C6A0F0E7FE9D705C92BC31E7DFD1F00895FBCFB87B312CB161F24E3E.
Its exports forward into icu.dll rather than implementing ICU locally.

Microsoft documents icuuc.dll and icuin.dll as Windows components since Windows 10 1703;
the combined icu.dll appeared in 1903. Shipping this host's forwarder can therefore introduce
a missing combined-DLL dependency on the application's supported Windows 10 1809 floor.
That compatibility consequence is an inference from the documented OS timeline and observed
forwarder exports, not a claimed 1809 execution test.

Primary references:

- [Microsoft Windows ICU history](https://learn.microsoft.com/en-us/windows/win32/intl/international-components-for-unicode--icu-)
- [Qt 6.10 supported Windows versions](https://doc.qt.io/qt-6.10/supported-platforms.html)
- [Pinned windeployqt ICU selection](https://github.com/qt/qtbase/blob/v6.10.3/src/tools/windeployqt/main.cpp#L1522)

There is no selective ICU exclusion option in the pinned tool. Its generic ICU search uses PATH.
The no-libraries option also removes required Qt libraries and is not a complete deployment fix.
The existing portable stager already copies Qt/CRT files selectively and excludes bare ICU names;
that working selection was preserved and pinned by tests, not rewritten.

## Implemented seams

windows_runtime_dlls.ps1 now owns the existing exact Windows DLL classifier and package-local
OS-shim refusal. The verifier preserves the Authz, Mpr, Ntdll and VirtDisk classifications. Bare
icuuc.dll, icuin.dll and icu.dll are OS components; versioned private ICU remains a required
non-system dependency. Classification does not claim that every named OS component exists on
every historic Windows release.

verify_windows_runtime_dependencies.ps1 has explicit RequireNoSystemDlls package mode. It refuses
OS-named DLLs in the package root and primary executable's search directory, including hidden
files, without deleting anything. Normal build-tree dependency checks remain available. The
portable stager now requires strict package mode in its final dependency verification.

deploy_qt_runtime.ps1 uses the selected windeployqt only as a dry-run JSON planner, retaining
debug/release auto-detection and plugin paths. It validates all records before any copy. The
actual pinned plan has source-file and target-directory fields; the helper does not mistake
target for a complete filename. It rejects traversal, sibling-prefix escapes, outside-Qt sources,
UNC/ADS and ambiguous Windows paths, reparse paths, target type/collision errors and missing or
misplaced Core. OS-named files are omitted only when resolved from the real system directory;
an OS-named file under a private Qt directory is refused. Private versioned ICU is copied.
Existing OS shims are refused rather than deleted, and every copied file is hash-checked.

The helper requires Qt's declared relocatable feature because a raw plan cannot represent the
separate QtCore patching needed by a non-relocatable build. Pinned installed Qt declares both
relocatable and winsdkicu. Planner execution has a finite timeout and validates status, JSON size,
array shape and record count. Diagnostics remain visible, including the upstream missing-DXC
warning observed during the current app plan.

The CMake install hook calls this helper with exact tool/application paths. Its child clears only
the inherited PSModulePath so the selected Windows PowerShell host reconstructs its own standard
module search path. The existing build POST_BUILD deployment route remains unchanged this wave.

## Real-install launcher and scratch-exclusion proof

The 2026-09-06 rehearsal used only the fresh ignored directory
build/wave32-package-rehearsal-20260906-095557-f2c8b75a/install. The current generated install hook
was inspected first; no configure or build was run by the packaging reviewer. The complete
install output remains in that rehearsal's install.log. CMake exited 1 after the Qt helper's
Get-FileHash command could not resolve; the partial install was retained rather than accepted.

The exact cause was the caller shape: PowerShell 7 -> CMake -> Windows PowerShell 5.1 retains the
PowerShell 7 module search path, unlike a direct PowerShell-to-PowerShell launch. The child selected
the incompatible Core Utility module and lost Get-FileHash. The production install command now
uses cmake -E env --unset=PSModulePath around its already selected Windows PowerShell executable.
This is a direct environment fix, not a missing-command fallback or a diagnostic suppression.

The existing packaging regression extracts the production execute_process block and runs it
through real CMake against an ordinary hash-probe fixture under the actual PowerShell 7 module
path. It neither configures nor mutates the shared build. The unchanged launch went RED on the
same Get-FileHash CommandNotFoundException; the fixed launch went GREEN with the hash-completion
marker. Both logs are retained as cmake-launch-red.log and cmake-launch-green.log in the rehearsal
directory. This caller proof is not a successful full-install claim.

The raw CMake install(DIRECTORY tools/) also copied developer scratch
under tools/filesystem/_build, including the MSYS toolchain, headers and source
trees. The source tools inventory contained 39,572 files totaling 2,559,560,781 bytes, no
*.local.json files and no reparse entries. The portable stager already excluded _build directories;
the raw install route now uses the same exact PATTERN "_build" EXCLUDE rule as the existing
cmake/copy_tools_bundle.cmake. No broad name pattern or source cleanup was introduced.

The regression configures an isolated no-language fixture using the actual production tools-install
block, then performs a real install. The unchanged block went RED on leaked _build/direct.txt.
The fixed block excludes root/nested _build while preserving vendor EXE/DLL/license/manifest bytes
and the _builder/build near matches. Source scratch remains untouched. Logs: cmake-tools-red.log
and cmake-tools-green.log in the rehearsal directory.

After root regenerated the configured build, a fresh install into install-fixed-ac898ff3 passed
in 4.9 seconds; install-fixed.log retains the output, including the upstream missing-DXC warning.
It deployed 21 Qt/runtime/plugin files and left icuuc.dll to Windows. Installed tools contain
111 files totaling 189,211,968 bytes rather than the developer scratch tree.

The integrated Windows PowerShell 5.1 ContractOnly fixture run passed after the launcher fix,
including the existing 18 deployment/staging guard drills and the new real-CMake caller proof.
Its output is contracts-with-cmake-launch.log in the same rehearsal directory. The original
build/system ICU hashes and deployment source hashes remained unchanged across that run.

## Exact Windows dependency ownership

Full app/runtime/plugin inspection exposed ten Windows imports that a main-executable-only pass
did not exercise. Every exact System32 file had a Valid Microsoft Windows Authenticode signature;
paths, signatures and SHA-256 values are retained in windows-system-dll-evidence.json in the
rehearsal directory. These exact names are classified as Windows dependencies and refused as
app-local OS copies. Private near matches, versioned CRT/ICU and D3Dcompiler_47.dll remain package
dependencies. No new wildcard classification, suppression or missing-file fallback was added.

| Exact Windows DLL | Primary ownership contract |
| --- | --- |
| d3d9.dll | [Direct3DCreate9](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-direct3dcreate9) |
| d3d11.dll | [D3D11CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-d3d11createdevice) |
| d3d12.dll | [D3D12CreateDevice](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-d3d12createdevice) |
| dwrite.dll | [DWriteCreateFactory](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-dwritecreatefactory) |
| ncrypt.dll | [NCryptOpenStorageProvider](https://learn.microsoft.com/en-us/windows/win32/api/ncrypt/nf-ncrypt-ncryptopenstorageprovider) |
| uiautomationcore.dll | [UiaReturnRawElementProvider](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationcoreapi/nf-uiautomationcoreapi-uiareturnrawelementprovider) |
| wtsapi32.dll | [WTSQuerySessionInformationW](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsquerysessioninformationw) |
| imagehlp.dll | [MapFileAndCheckSumW](https://learn.microsoft.com/en-us/windows/win32/api/imagehlp/nf-imagehlp-mapfileandchecksumw) |
| msvcrt.dll | [Windows-protected CRT](https://learn.microsoft.com/en-us/windows/win32/sbscs/fixing-an-existing-application-using-a-private-assembly) |
| rpcrt4.dll | [RpcStringFreeW](https://learn.microsoft.com/en-us/windows/win32/api/rpcdce/nf-rpcdce-rpcstringfreew) |

## Signed browser-extension artifact

The real install/stage comparison found sak_browser_control.crx present in the install tree but
missing from the portable package. The browser installer names that exact app-directory artifact;
startup alone never exercises extension installation. The stager now requires the existing CRX,
copies it unchanged, and includes it in both staging and portable-smoke required-artifact checks.

Three independent in-memory drills killed omission of the copy, staging check and smoke check.
The old retained real package also fails the updated smoke with the exact missing-CRX diagnostic
(crx-old-package-smoke-red.log). The final staged CRX is byte-identical to browser/dist input:
SHA-256 F4D881F56BA1FA7E3B9F87BEBB27F2AB64DD804EC120F8400B39823BE48214F1.
This proves packaging integrity, not a live browser installation or browser-control E2E session.

## PE architecture defect

The old reader shifted a byte by eight bits. PowerShell retained byte width and discarded the
high byte, so x64 0x8664 and ARM64 0xAA64 both became 0x64. The exact unchanged HEAD reader and
dependency loop accepted an ARM64 fixture beside the real x64 certifier. Casting header bytes
to int before shifting preserves the full machine word; the same loop now rejects that fixture
as 0xAA64 versus 0x8664.

## Completed scoped proof

- Real app, live-certifier and Qt6Core dependency checks pass on Windows PowerShell 5.1.
- Nineteen independent in-memory verifier/classifier mutants were killed: three ICU classifications,
  private-ICU preservation, package refusal, hidden-file refusal, package call-site enforcement,
  PE high-byte decoding, same-low-byte architecture comparison and ten separate OS-name removals.
- Twenty-one independent portable/deployment mutants were killed: Qt/CRT selection, strict package
  mode, target/source/OS-source bounds, private ICU, duplicate destinations, Core placement,
  relocatable requirement, reparse and target-type refusal, record shape, copy integrity,
  preexisting OS shim refusal, auto-detection, planner exit/timeout handling and three CRX guards.
- Actual pinned read-only JSON planning selected 21 non-OS app runtime/plugin files and reported
  icuuc.dll separately as Windows-provided. Release Core and platform-plugin destinations matched.
- Fake-plan copies used only ordinary temporary fixture files. Debug Core, private ICU and plugin
  layout positive controls passed; no Windows DLL was copied during these tests.
- Final fixture logs: runtime-contracts-closure-final.log and stage-contracts-crx-final.log, both
  exit 0. The latter includes both real CMake caller/install regressions. Fixture-only runs did
  not modify shared build output; separate real install/staging followed in fresh destinations.
- Final package: install-fixed-ac898ff3/bin/SAK-Rehearsal-Package-CRX under the rehearsal directory,
  159 files / 253.8 MB. portable-stage-crx-final.log records successful staging. Static smoke,
  real startup E2E and post-startup pristine smoke each exited 0; logs are portable-crx-static-smoke.log,
  portable-crx-startup-e2e.log and portable-crx-post-startup-smoke.log.
- Each of the 36 root app/runtime and Qt-plugin PE files passed strict dependency verification;
  per-file logs are in closure-crx-final-logs. All 36 were independently checked as x64. This is
  not a dependency-closure claim for every third-party executable under tools/. The original
  two-file closure failure is preserved in closure-logs; no failed package was relabeled green.

Handoff SHA-256:

- Verifier: F2FB44696D6A9F530AEDDECF4A655500CA1ABE45183FB43579ACB5ED0E0945AA.
- Verifier tests: 0D3CC4E6F3B3D78A6FA4906518372DEB1EC9CB6B2BA5465A130576E4A73A23E8.
- Shared DLL rules: 4491BDE2EDEE29DFD3FFFF7D1387C754EA0D3F0202F925916588BA4C962FC8DE.
- Qt deploy helper: 2204DEB619DCDB5B02BC5608DF5B405E2772A6178CEA40D59FC3E6B4E9D77C18.
- Portable stager: 0D648F2D0398F6388788835DBD40725F542F8E06DFAE122F5EDB47438715F759.
- Stage/deploy tests: 0D847CBF3AB7CD3AEFCA0C66BADFDCD00449CD1A8F6AF63BDC4973AEBA09D94C.
- Portable smoke: E1FC8DB909C476779459B4EA03CE0560BF7725E3B0DA64D83FBE90CC50CA2284.
- CMake install launch: 6D6F560C1A1D71C62099AAC0729922B3C6D693E58D8D6B65DC659F47C24527AE.

## Portable startup wrapper guarantee audit (2026-09-06)

The existing wrapper had four reproduced false-green paths: an inherited alternate
CI-headless flag/offscreen platform could weaken a local native run; stdout success
was not required; an unchanged log from before launch could satisfy the timestamp
window; and a null Get-Item result still reached recursive cleanup with no error.
An unmodified native-success mock passed while each bad case was accepted by the old
script. Evidence and byte-preserved old scripts: build/smoke32-59d4a4b4/old-smoke-red.log.

The wrapper now uses the existing process_capture helper, with its additive
HiddenWindow switch preserving the prior hidden startup style. The switch defaults
false, so existing capture callers are unchanged. Pure evaluation of the actual
ProcessStartInfo setup proves requested Hidden and default Normal without creating
a process. The complete preexisting capture tests pass on Windows PowerShell 5.1 and
PowerShell 7, including exit, stdin/pipe, timeout and bounded live-child-tree cases.

Native Windows is pinned through QT_QPA_PLATFORM=windows; both inherited headless
aliases and plugin-path overrides are cleared. Only explicit CI=true or
GITHUB_ACTIONS=true selects the existing constructed-window mode, which is reported
separately. Current code configures only the child's environment; parent presence/value never
changes. The earlier setup/restoration approach was superseded by the child-only repair above.
Success requires typed integer exit zero, no timeout, one exact stdout smoke marker,
exactly one newly named ordinary sak_*.log absent from the prelaunch filename snapshot,
its clean-shutdown record and native/CI-consistent window markers. Timestamps are no
longer accepted as freshness evidence. Invalid timeouts refuse before child launch;
the normal 30-second deadline and existing caller-supplied deadlines are unchanged.

The shared Assert-DirectStagingPath ancestor guard is reused without invoking any
ACL operation. Item metadata and directory/file types are required. Every newly
created runtime cleanup tree is preflighted completely, including descendants,
before any deletion. Bottom-up nonrecursive CLR deletion cannot prompt or recurse
into an unexpected new child; failures and remaining paths are reported. Preexisting
runtime paths are retained. No unsupported-OS termination branch or silent temporary
capture cleanup remains. Primary errors, failed log diagnostics and runtime cleanup errors are
retained together. Child-environment setup errors also fail visibly. Failure evidence is not
deleted to force a pass.

Scoped proof covers 33 filesystem/process/environment-mocked cases in the existing
false-green fixture. Twenty-two independent in-memory guard reversals were killed
with zero drill errors; tracked wrapper bytes were unchanged. Per-guard logs and
guard-drills-summary.json are under build/smoke32-59d4a4b4. No real app or junction was
used by these mocks. An initial PowerShell 5.1 fixture failure exposed property-name
sorting of hashtable AST offsets; explicit key sorting fixed the fixture without
changing production policy or silencing an error. Its failed full-suite log is retained.

The full existing false-green suite passed under Windows PowerShell 5.1 (33.084 s)
and PowerShell 7 (40.479 s), retaining all existing controls plus the new smoke cases.
Both remain within the unchanged registered 60-second test limit. Final logs are
full-false-green-final-ps51-private.log and full-false-green-final-ps7-private.log
under the smoke audit directory.

Fresh native packages under build/pk32-8870b6e2 passed both PowerShell hosts with
the unchanged 30-second deadline, but final inspection found empty data/crashes
left behind. Those retained packages are evidence of that gap, not pristine-tree
proof. The startup wrapper now includes crash storage in its prelaunch inventory
and cleanup plan; only a newly created EMPTY crashes directory is removable.
New crash evidence refuses the run before any runtime deletion, and preexisting
crash storage is preserved. The static verifier also rejects data/crashes.

The expanded fixture passes 36 mocked startup contracts and the static crash-state
check on both hosts. Four independent in-memory reversals prove new-empty-directory
cleanup, crash-evidence refusal, preexisting-crash preservation, and static refusal.
Logs: build/capture32-portable-*-powershell.log and
build/capture32-portable-*-pwsh.log. Tracked wrapper/verifier bytes were unchanged
during the drills. No app or real junction was launched by these mocked checks.

The final fresh package cited at the top now passes native PowerShell 5.1/7 startup and exact
pre/post file-tree comparison after the crash-storage and child-environment repairs. Earlier
package passes remain historical, exact-binary scoped evidence.
Pathname rechecks are not proof against hostile concurrent
rename races or handle-bound deletion, and the shared capture helper is not being
claimed to prove job-object quiescence after arbitrary parent-only exit.

## Capture fixture identity and readiness follow-up (2026-09-06)

The historical full CTest failure did not record the descendant PID, so its exact
cause cannot be reconstructed. A separate controlled reproduction demonstrates a
real test defect: delaying the old fixture launch by 1500 milliseconds lets the
unchanged one-second timeout expire before a child exists. Empty stdout converts
to integer zero, GetProcessById(0) observes Idle, and the old test falsely reports
"Timed-out descendant process remained alive". Both-shell reproductions are in
build/capture32-old-readiness-powershell-red.log and
build/capture32-old-readiness-pwsh-red.log.

The repaired test waits for an atomic parent/child PID and creation-time receipt,
binds both to held process handles, and requires the child to be alive before
starting the same one-second execution wait. Setup has its own 30-second bound;
production execution and five-second termination grace are unchanged. The actual
capture body runs through an instrumented real Process construction boundary;
Start and WaitForExit call their original CLR methods. Before pipe draining, the
held child must exit within the remaining original execution/termination budget.
This catches a parent-only-kill mutation without waiting for the leaked child's
natural exit to make a later liveness check look successful.

Independent fixture cleanup preserves the on-disk termination function's header
parameters when extracting its AST body. An earlier negative readiness drill
exposed the missing-parameter defect; fixed invalid-receipt drills report zero
cleanup errors. Parent-only killing, empty captured output, child PID zero, wrong
parent generation, and wrong child generation each produce their intended RED
on both hosts, with unchanged tracked source bytes. Controls and per-guard logs
are build/capture32-*-ps51-final.log and build/capture32-*-ps7-final.log. Complete
capture-suite runs are build/capture32-complete-powershell-final.log and
build/capture32-complete-pwsh-final.log; success records exact process identities
only after fixture cleanup completes.

This historical fixture repair alone did not establish inherited-pipe or blocked-stdin deadlines.
The later finite-deadline implementation and independently exercised controls below close those
specific boundaries without broadening the claim to arbitrary descendant quiescence.

Frozen source hashes for the capture32 scoped proof above (SHA-256):

- Capture fixture: E43D0B59D069213177606787FD1AF73575BB72986771F0B97B89DF6D2B68EDC7.
- Capture production: B05DD9D7357EB71D322EDED98E171D412264EC0987DB5C7E84DA910FA713EE87.
- Startup wrapper: 62CD6754B457E72B7FBEF03874BD289A061F86772E51CA21F1F028E2A0DADA50.
- Static smoke: 2E94F7FDC4DD1D860947C78B2BDAC1F5C9A7882106C1113CC982FC64F23338D5.
- False-green fixture: E7597371D7020AA3ABB0D8355B3B86D472A29C87223573001402CA7C86E2D18D.

## Finite capture deadlines -- Wave 33

The canonical process_capture.ps1 now assigns one post-launch monotonic deadline to stdin,
process exit, stdout and stderr. Input writes/flush/EOF and both readers run on owned cancellable
I/O threads. This is not a second process-launch backend: ProcessStartInfo, argument quoting,
working-directory selection, hidden-window behavior and result fields remain in the same helper.
Explicit zero timeout retains its existing unlimited policy; this does not certify every caller's
choice of budget. No job-kill-on-close policy is added for intentional background VM/service children.

The Windows I/O seam uses only a handle for its own thread and CancelSynchronousIo. Microsoft's
[cancellation contract](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelsynchronousio)
does not equate requesting cancellation with completion. The helper therefore repeats cancellation
across the documented no-pending-request race until a bounded Join proves termination. Reads and
writes also check cancellation between chunks, so continuously available data cannot defeat it.
No Thread.Abort or TerminateThread is used. Native lookup is restricted to System32; the exact
embedded source hash forms the loaded type's identity, preventing stale helper reuse after edits.

Cleanup shares one additional five-second budget, including the taskkill utility and stream workers.
Primary and cleanup failures are retained together. Cancelled or failed writers close their owned
base stream without subsequently asking StreamWriter to flush it; normal disposal is unchanged. Actual native-handle and stream state,
not only a Boolean flag, is exposed to the fixture's ownership check. Failed process startup also
disposes its owned Process object without replacing the original Win32 error.

The maintained test_process_capture.ps1 suite now drives the bounded Windows-only Python fixture
under tests/support/process_capture_deadline_fixture.py. It independently pins parent/child PID and
creation time and uses a separate outer watchdog. Blocked stdin, stdout-only inheritance and
stderr-only inheritance are separate cases. The background child must remain alive after the
normal-exit parent's capture fails; only fixture-owned cleanup stops it. I/O completion/disposal
is observed before Process.Dispose can incidentally hide missing worker cleanup.

Original-source counterexamples fail all three deadline assertions on both PowerShell hosts.
Normal output, exit-code, empty-input, duplex-I/O, zero-timeout and launch-error contracts are
retained. A UTF-8 memory-stream case tests a surrogate pair split at the real chunk boundary;
it does not assume the process console's default encoding is UTF-8. A controlled write barrier
tests disposal without relying on a broken pipe, which can close itself and mask that omission.
Earlier failed setup and surviving-disposal drills remain diagnostic history, not credited coverage.
Final guard, aggregate and release-gate receipts are recorded in R5-CAP-DEADLINE in the canonical R5
document. These local checks do not establish arbitrary descendant quiescence or full-app certification.

## Cleanup refusal and native writer follow-up

Independent real-process reproductions in build/capture-review-190cc9f1 confirmed two separate
timeout races on both PowerShell hosts. A parent can exit after the real timeout but before tree
cleanup starts, or after cleanup admission but before actual System32 taskkill launches. In the
second case taskkill returned 128. Both old paths returned a timeout result without an exception
while an independently bound, separately redirected descendant remained alive.

Requested cleanup now refuses explicitly when the root has already exited and descendant
termination cannot be verified. Nonzero taskkill is never accepted merely because the root exited.
This is fail-closed refusal, not a launch-time ownership or process-tree quiescence guarantee.
Normal-completion background children remain preserved. The canonical Python/PowerShell fixtures
now exercise each race independently, recording actual timeout admission, parent state at cleanup,
the real taskkill exit, retained child liveness and owned I/O disposal before Process.Dispose.

A separate review claim that Process.Dispose would block after I/O cleanup exhaustion was
disproved and withdrawn. With a genuinely live native stdin worker and a separate cancellation
fault, actual Process.Dispose returned in 6 ms on Windows PowerShell 5.1 and 3 ms on PowerShell 7.
Intact controls returned in 2 and 4 ms. No change is justified by that disproved hang claim.

The reproduction did expose a different intact error: after native stdin cancellation, the old
worker closed its base stream and then StreamWriter.Dispose attempted to flush that closed stream.
Both hosts reported ObjectDisposedException with the original full five-second cleanup allowance.
The direct fix closes the owned base stream only on failed/cancelled writes; ordinary writer
disposal remains unchanged. No ObjectDisposedException catch or silence was added. The new
inherited-stdin case observes actual worker-disposal exceptions separately from the genuine timeout
and cancellation errors, so resource-state flags alone cannot make this regression pass.

The existing capture suite retains its previous cases and adds inherited stdin plus both parent
exit races. Its shared AST extraction preserves function-header parameters for observations and
for independent on-disk cleanup. Detailed original errors and the withdrawn finding remain in
build/capture-review-190cc9f1/review-results.md; old evidence was not relabeled as a clean run.
Final scoped proof and full-gate integration remain owned by the canonical R5 ledger.

Scoped repair proof: complete capture suites passed on Windows PowerShell 5.1 and PowerShell 7,
including all prior migration cases and six independent native deadline/race scenarios. Final
logs are fix-full-final-ps51.log and fix-full-final-ps7.log under build/capture-review-190cc9f1.
Three separate production reversals per host restore the early-parent return, the conditional
nonzero-taskkill acceptance, and the closed-stream writer flush. Each goes green, intended red,
exact copied-source restoration, green. Bound source/test/fixture hashes and per-phase logs are
in cleanup-drills-ps51-2f824bfe/results.json and cleanup-drills-ps7-ccf2ec7f/results.json under that
same evidence directory. No tracked source was mutated by a drill.

The first full PS7 repair run failed a new fixture assertion because ConvertFrom-Json represented
the real taskkill exit 128 as Int64 rather than Int32. Its observation proves the actual refusal
and zero cleanup errors. The fixture now accepts bounded Int32 or Int64, matching existing PID
receipt checks. fix-full-ps7.log and fix-taskkill-int64-initial-observation.json preserve that
failed setup; neither is credited as passing full-suite proof. Scoped syntax and four-file ASCII
checks passed. The September 19 complete Release/CTest gate cited above now covers these repairs.

## Bounded output and prompt worker failures -- Wave 34

Invoke-CapturedProcess now has a MaxOutputCharacters argument. Each stdout/stderr stream permits
at most that many decoded UTF-16 characters, independently; the default and hard maximum are
16 * 1024 * 1024. Smaller positive limits are accepted. Zero, negative and above-maximum values
refuse before process launch. This is a character budget, not an encoded-byte or total-process
memory claim. The existing chunk reader checks remaining capacity before appending and throws
an explicit stream-named overflow error. It never returns a truncated success result.

Process, stdin and output completion waits inspect all completed I/O workers every bounded
50-millisecond slice. A completed zero-duration Join publishes the worker exception before it
is read. A failure interrupts both finite and explicit zero-timeout execution waits; the latter
still permits unlimited successful execution, but not unlimited output or hidden I/O failures.
The original post-launch deadline and one five-second cleanup allowance remain unchanged.
The taskkill subprocess uses the same capped readers and failure-aware waits; failure also
stops and observes that owned utility before disposing it. No new launch backend, platform
fallback, blanket descendant-kill policy or suppression is introduced.

The canonical fixture contains 24 output cases alongside every prior capture case. Native
Windows Python probes exercise stdout/stderr exact and one-over limits, both streams, minimum
and default limits, prelaunch invalid values, and overflow during process, blocked-stdin and
inherited-output waits under finite and zero timeouts. Small limits exercise most cases; only
the default contract uses a bounded 16-Mi-character stream. An independent eight-second native
self-expiry prevents a defective unlimited wait from hanging the fixture. Real process handles
are pinned to PID and creation time; independent post-Start clocks require prompt refusal, and
owned worker disposal is observed before Process.Dispose. A separately bound background child
remains alive after its normal-exit parent, then is stopped by fixture-owned cleanup only.

Two utility cases replace only the actual termination ProcessStartInfo target with a bound fake
tool. The canonical termination launch, readers and waits still execute; each output stream
exceeds the hard maximum while the fake utility stays alive. The test requires explicit overflow,
stopped utility workers and utility process, and independently cleans the unchanged target.
An initial fixture override leaked through Function: lookup into the following case; replacing
it with an explicitly local function fixed the fixture. The failed expanded run is retained as
build/wave34-output-expanded-ps7.log, not credited as passing evidence.

The first process-wait reversal survived because overflow could arrive during the earlier
empty-stdin wait, and the observation also counted the cleanup utility's process wait. That
was a fixture gap, not a passing drill. Native release barriers now emit overflow only after
the named parent-process or stdin wait is entered; inherited-output overflow has its own
drain barrier. The repaired process-wait reversal produces its intended prompt-propagation
failure on both hosts, with exact restoration and fixture cleanup complete. The failed first
batch remains in build/output-guard-drills-bf890fe4; no old success was relabeled.

Passing focused and aggregate logs, independent guard reversals and final integration status are
owned by R5-CAP-TASK-OUTPUT in the canonical R5 ledger. Output-budget proof does not certify all
caller deadlines, arbitrary descendant quiescence, or whole-application release readiness.

## Explicit stdin EOF policy

The string-typed InputText parameter already normalized omitted, explicit null and empty input
to an empty string. Its nullable-looking default and hasInput predicate therefore did not create
an inherited-console-input mode: the predicate was always true. All 33 current production callsites
were inspected. Only review prompts and FAT conversion labels provide text; none requires inherited
console stdin. The helper now states that existing contract directly: always redirect input, write
the supplied text, then close with EOF. Omitted/null/empty input sends immediate EOF. AllowNull
remains accepted and normalized, and the writer's deadline/ownership path is unchanged.

The canonical capture fixture executes the actual parameter binding and ProcessStartInfo setup
for omitted, null, empty and nonempty input. A real child then reads to EOF and independently
reports redirection and exact payload/length. The setup assertion prevents an already-closed test
host pipe from disguising accidental console inheritance. Existing descendant-held stdin and
blocked-input cases remain intact. Final both-host runs and guard drills belong to
R5-CAP-TASK-STDIN; this cleanup changes no caller policy or background-child ownership.

The stdin selector follows PowerShell's case-insensitive ValidateSet and refuses an empty
selection explicitly. A copied-fixture reversal restores the original case-sensitive loop;
both hosts return child exit zero without running a case for EMPTY, and the independent
discovery check correctly rejects that false green. Restored controls each execute exactly
one named native empty-input case. Evidence: build/stdin-selector-drill-1cf75f1d/results.json.

## Final current-source evidence

The canonical R5 receipts bind the final capture source 4287E54D, capture fixture 652C4E26,
startup wrapper CA1CF6CA and archive script 64383DB4. Current capture proof includes 84 separate
green/red/restored-green triples under capture-guard-drills-8c00ef99, stdin-guard-drills-5a7239fa,
stdin-selector-drill-43cfec0a, output-guard-drills-00a344c0 and the two current cleanup-drill roots.
Twelve child-environment triples and six archive crash-boundary triples remain independently
bound in R5. Complete suites retain normal output, exit, stdin, timeout and cleanup behavior.

The final package pins app B2D5EFD4 and signed CRX F4D881F5. Its source-pinned rehearsal first
exposed two proof-harness defects before app creation: PowerShell 5.1 nested JSON arrays, then
host-dependent dot/underscore sorting. Both original preparations were retained. The corrected
reader and explicit ordinal ordering passed the real 196-entry tree on both Windows shells,
plus independent member/type/hash/attribute refusal controls, before fresh package execution.
No failed native run was hidden by a warm retry. See the exact receipts and remaining broader
work in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
