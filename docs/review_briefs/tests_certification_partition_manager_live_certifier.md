# W31-CERT-1: controller-driven disposable VHD restore

## Current native evidence checkpoint

Status: INCOMPLETE for the broader controller-certification work. The retained September 8,
2026 checkpoint passed all five supplementary cases: admission-only, offline restore,
offline/read-only restore, offline selected-partition capture and competing-lock refusal.
Independent host validation checked source/helper bindings, cleanup and original VM disk states.
The complete VHD matrix also passed 12/12 at that frozen checkpoint. Evidence is retained under
artifacts/partition-manager-certification/vhd-strict/wave33-controller-current-20260908/;
build/wave33-current-vm-host-audit.log and build/wave33-current-vm-matrix-verify.log record the
host checks. The canonical scope and limits are in docs/PARTITION_MANAGER_CERTIFICATION.md.

These completed cases do not cover native online selected-partition/DASD capture, every clone
or migration controller, current external hardware gates, or whole-application certification.
The September 19 capture/FAT harness changes now have the focused native proof below. Current
local integration passed the complete Release build and all 281 CTest tests; logs are
build/wave36-full-build.log and build/wave36-full-ctest.log. These results do not close the broader
controller-certification gaps above. Compiled MSR guard proof is recorded in the September 6 section below; no broader
compiled-mutation coverage is inferred from the live runs.

### Shared safety-stop fixture integration

The September 19 full Release build passed, but the complete CTest run reported
280/281 passing because an independent routing fixture inherited the simulated
run-level stop from the preceding cleanup fixture. Production correctly refused
the later scenario. The test boundary now snapshots variable presence and value
and restores them in finally, including assertion failures. Presence checks follow
PowerShell's case-insensitive variable semantics; mixed-case prior names are
preserved. The equivalent converter/controller test helpers follow the same rule.
These fixture repairs do not clear a real production run stop or relax any timeout.
The complete Release/CTest rerun passed 281/281. Final routing isolation and production-stop
assertion drills passed on both Windows hosts; canonical R5 records their current bindings.

### Current focused FAT harness proof

The September 19 refresh at guest C:\SC\w34f-5b3321cf passed an observed full producer and a separate
byte-identical pristine producer on two fresh 768 MiB disposable VHDs. Both used system
convert.exe through the current bounded capture helper, completed with exit zero, produced NTFS,
detached their owned disks and retained the VHD files. Observed partition GUID, volume identity,
offset, 256 MiB partition size and label remained unchanged. Original VM disks 0 and 1 retained
their identities and states; no known storage workers remained. No arbitrary-descendant
quiescence claim is inferred from the known-worker check.

All 49 collected files matched before/after guest hashes and sizes. Independent host audit and
both canonical report verifications passed. Evidence: build/wave37-fat-host-audit.log,
build/wave37-fat-observed-report-verify.log, build/wave37-fat-pristine-report-verify.log and
build/wave35-fat-evidence-5b3321cf/collect-ee200f2736884c7b8736c4b14a628cff/collected/.
The source package manifest is
5B06C26918C3017C7A3DBD2E6F637F993649BC67CA2668CE2DD5AEF3EC22B2E9.
This refresh uses capture SHA-256
4287E54D455D9D72753300712E6FE36128B68A44346231261AA1C59A6BC53948.
The prior 9d4ed870 run and its B1DE32 capture binding remain historical evidence; their original
artifacts were not rewritten or relabeled as this refreshed run.

Two earlier attempts stopped before storage and remain preserved: an overly specific policy-error
label, then empty-array evidence serialization. Direct fixes passed original-defect reversals.
The final run verified explicit Restricted refusal and inherited launch-process RemoteSigned;
no Bypass or persistent execution-policy change was used. Temporary credential copies were
removed; original credential source was unchanged.

This proof covers the changed FAT harness, not the application's conversion UI. Each focused
report remains Partial: one selected VHD case passed, eleven were skipped, and eighteen external
gates were unexecuted. It does not replace or upgrade the selected September 8 full-matrix evidence.

## Earlier W31/W32 checkpoint (historical)

Status: INCOMPLETE. Release helper build and non-destructive contracts pass. Fresh disposable-VHD
attempts exposed an attached-VHD access-mask defect, then real CreateImage source-consistency and
locked-volume visibility defects. The corrected frozen helper subsequently passed a focused real
controller create/restore run and the full 12-scenario VHD matrix. Failed runs remain preserved.
Later explicit source-capture modes passed generated-script and named core/UI tests; the current
LockedVolumes whole-disk controller run also passed source-bound validation. Supplementary native
offline/read-only/selected-range and competing-lock proofs remain INCOMPLETE. The new helper mode
implementation passes its Release helper build and non-destructive executable contract; compiled
negative drills for new helper guards and fresh VM mode proof remain INCOMPLETE.
These passes are not whole-app or all-cloning-route certification.
PowerShell routing and strict-wrapper tests have 16 fixture groups and 42 isolated guard drills
passing on Windows PowerShell 5.1. Failed-run artifacts and VHD files are preserved, worker
quiescence was confirmed, and original VM disks were never selected. Root owns integration,
current-source binding, final Release/CTest and the next controlled VM run.

## Why this runner exists

Stored vhd.image-restore proof copied one VHDX container file over another. It never reached
PartitionManagerController, PartitionExecutor or the generated RestoreImage script. It therefore
does not cover Wave31 stable disk binding, fresh preflight, source-content pinning or the locked
restore source handle. This developer-only executable drives those production components without
SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS, a substitute executor, or a second restore implementation.
Defining the production test-hook macro is a compile error for this executable.

## Native command contract

The intended VM invocation, only after safety/build approval:

~~~powershell
$certifierArguments = @(
    "--source-vhd", "C:\SAK-Cert\run\image-restore-source.vhdx",
    "--target-vhd", "C:\SAK-Cert\run\image-restore-target.vhdx",
    "--output-dir", "C:\SAK-Cert\run",
    "--run-id", "<non-null-lowercase-UUID>",
    "--confirm-disposable-vhds"
)
& .\partition_manager_live_certifier.exe @certifierArguments
~~~

The default workflow above has passed on fresh VM fixtures. Both VHDs must already exist as direct children of the same
fresh guest-local output directory. Paths must be literal and canonical; raw device paths,
relative/traversal paths, reparse chains, hard links, aliasing source/target files, unknown VHD
providers/types and absent attachments refuse. Virtual Disk API handles independently resolve
each exact file to PhysicalDrive identity. Authoritative controller inventory must agree on
path, disk number, UniqueId and size. Each VHD must be 512-1024 MiB; boot/system/EFI, dynamic,
Storage Spaces, incomplete and identity-less disks refuse. Target cannot be read-only. Output
volume native extents must exclude both VHDs, and free space must meet the existing conservative
four-times-VHD-size estimate.

No disk number is accepted as input. Existing VM disks 0 and 1 are not disposable targets.

The harness must seed:

- Source: one mounted NTFS volume labelled SAKRESTSRC; file
  sak-partition-manager-restore.txt contains partition-manager-restore-certification:<run-id>.
- Target: one mounted NTFS volume labelled SAKRESTDST; file
  sak-partition-manager-pre-restore.txt contains target-before-restore:<run-id>.

Runner refreshes real inventory, validates both markers and native attachments, then queues
CreateImage, captures the real queue review token and calls applyQueue with real execution.
Full copy verification is requested. It hashes resulting raw controller-source.img, detaches
only its previously validated source Virtual Disk handle, refreshes inventory and proves that
source identity is absent, then queues RestoreImage against the revalidated target. Restore
carries the exact image digest and known sizes and requests full byte verification. Completion
also checks target native VHD attachment, fresh inventory, restored sentinel hash, absent old
target marker and unchanged raw-image digest.

Helper writes controller-restore-report.json only after supervisor outcome checks. Existing
raw-image/report outputs refuse instead of being overwritten. Helper deletes no fixture VHD;
cleanup remains the owning PowerShell harness's responsibility.

## Containment and provenance

Supervisor starts its worker suspended, assigns a new kill-on-close Windows Job, and only then
resumes the single initial thread. Worker verifies exact named-job membership before opening
VHDs. An already-elevated token is mandatory; Apply launches no separate elevated broker.
Controller stages have a 180-second deadline and a 30-second cooperative-cancel grace. Outer
worker deadline is 900 seconds, with bounded output and an explicit descendant-completion check.
Timeout, malformed/missing evidence, output overflow, failed/skipped execution, unknown
termination or remaining descendants cannot produce Passed.

CMake must embed SAK_PARTITION_CERTIFIER_SOURCE_SHA256 from the complete linked runtime source
and header input set. Provenance includes that build-bound digest, executable SHA-256, Qt version,
native VHD identifiers, pre/post inventory identity, queue revision/fingerprint, planned script
digest and real controller execution reports. A planned script digest does not claim that a
separate script copy ran; only the production controller owns execution.

## Implemented old-harness integration

run_partition_manager_destructive_certification.ps1 now routes vhd.image-restore as follows:

1. Keep New-DisposableVhdDisk, Get-AttachedVhdDisk, NTFS fixture creation and independent final
   sentinel checks. Reuse partition_manager_storage.ps1 cleanup helpers.
2. Generate one run UUID and append it to the two marker contents above.
3. Invoke the reviewed helper with exact source.path/target.path and a fresh UUID scenario
   subdirectory under Script:RunRoot, using
   process_capture.ps1 for bounded, non-interactive capture. Pass no guessed disk numbers.
4. Remove the Copy-Item restore surrogate. Helper creates raw image and restores through the
   real controller. Do not reattach source before target verification: restored target
   deliberately inherits its partition-table identity.
5. Require a complete typed Passed report, controller_executed true, the two expected real
   operation results, matching run UUID, executable/source digests and
   worker_descendants_finished true.
6. Resolve target again from its exact VHD path for independent OS-level sentinel/hash and
   overwrite checks. Map source_image from helper raw image, target_image from target.path,
   target_disk_number from fresh attachment, and retain marker/verification evidence.
7. Gate cleanup. If complete correlated evidence is absent or
   worker_descendants_finished is false, preserve both VHD files/attachments and surface
   failure with a preservation reason. Never detach/delete media underneath possible remaining
   writers. Try cleanup of both known fixtures independently and retain both cleanup errors.

ControllerCertifierPath is explicit and mandatory for this scenario; there is no container-copy
fallback. The single optional VhdScenarioId selects one matrix row and leaves all others Skipped,
so a focused run cannot satisfy full strict evidence. The default still selects every scenario.
Shared Remove-DisposableVhdDisk now deletes only after successful detach, never from finally.
New-DisposableVhdDisk refuses an existing destination instead of overwriting it.
The scenario's controller_report field is controller-restore-<UUID>/controller-restore-report.json,
relative to the directory holding the enclosing partition-manager-certification-report.json.
Copying the complete run subtree preserves that reference. Raw helper guest paths remain unchanged.

Matrix/verifier must require controller execution and source/binary provenance fields.
Old Copy-Item evidence must not satisfy the updated contract.

## Non-destructive contract script

scripts/test_partition_manager_live_certifier.ps1 accepts -CertifierPath and -OutputRoot. It
invokes pure --self-test, rejects malformed CLI/direct-worker requests, and runs --validate-only
against two ordinary text files named .vhdx. It creates no VHD and performs no raw write.
Helper must refuse both files; their hashes must remain unchanged. Validate-only is not
live certification.

Static checks: clang-format, lizard, PowerShell syntax and cppcheck (183/186) pass for the helper.
Producer and routing-test PowerShell syntax and ASCII checks pass. Release helper build and
registered non-destructive contract passed; complete supervisor termination proof and real
disposable-VHD execution remain INCOMPLETE until scheduled by root.

The first non-destructive contract report, under
build/test_results/partition_manager_live_certifier/fc5eed78-a700-439e-a76c-df876931d104,
reported a failed worker with controller_executed false and mutations_may_have_started false,
but its one-shot job-accounting check reported remaining processes and hid the worker's original
error. The updated check polls native ActiveProcesses for at most five seconds and succeeds only
after observing zero. Timeout still refuses and requests job termination; it never turns an
uncertain stop into success. Reports retain initial/last counts, elapsed wait and separate worker
and containment errors. The rebuilt report confirms initial_active_processes 1,
last_active_processes 0 and wait_ms 46; worker_descendants_finished is true only after that
observation. Recorded CTest evidence is build/wave32-live-certifier-quiescence.xml and
build/wave32-live-certifier-quiescence-test.log.
The script also hashes the raw sibling partition_manager_live_certifier.sources.txt bytes and
requires equality with the binary's embedded source_digest; no newline normalization is allowed.

scripts/test_partition_manager_controller_restore_routing.ps1 imports only named production
functions through the AST and mocks every storage/process boundary. Seven groups cover cleanup,
typed correlated reports, operation evidence, VHD binding, exact native launch, focused/default
selection and independent target verification. All pass in Windows PowerShell 5.1. Sixteen
separate in-memory mutations were killed: unsafe finally cleanup, typed quiescence, run identity,
source/target path binding, binary/raw-manifest hash binding, boot/system/size/single attachment
guards, launch preservation, uncertain cleanup, dry-run refusal, focused selection and independent
overwrite verification. The exact unchanged HEAD cleanup function was also substituted in memory:
its failed-detach path went RED for deleting the VHD, while current cleanup went GREEN. Production
source hashes stayed unchanged during these drills; no mutation marker or raw-disk action exists.

Independent review:

- [x] Invalid-VHD report omitted operations while PowerShell expected an empty array.
  reportBase now emits operations: [] and the contract requires array shape before counting.
  No other concrete defect was found in the frozen VHD/controller/job/provenance safety pass.
  Final post-fix review is clean at helper SHA-256
  7F67A748B35AA9FCC93BA1F9C23520E1AC9F1A400A120246BCA3682F2CD4EF3A and contract-script SHA-256
  B48AFD8ED52990F0B2DE165A3C15E28B566B35E13A060C286186A7E93A06F43D.
  This read-only review and pure PowerShell shape proof do not certify executable or VM behavior.
- [x] Independent bounded quiescence/manifest delta review found no additional issue at helper
  SHA-256 62A364EBDC16E8D720DF69366603640C0B05DC9DF08A2FF3F5B33CB12ABE10E6 and native
  contract-script SHA-256 7508AD4678BEFEE366EE047AF487A8EF5E844390CE59E92B3C80F57CD4BC8A9A.
  The subsequent native CTest supplies the executable evidence described above.

Routing handoff checkpoint: producer SHA-256
201CA89F70F292127D14C34E239E05D9C6182A05AFB4A92887FFE91180DAA96A and routing-fixture SHA-256
DE4FE6A87219DB429DF878D108842E1CBD5C8A38218F52C7FC053795181DD634.
Root integration review, full Release/CTest and disposable-VM proof remain open.

## Strict-wrapper integration and mirror-gap repairs

Both strict wrappers now pass independently selected ControllerCertifierPath and
ControllerSourceManifestPath to verification and status generation. The VHD producer receives
the selected helper; elevated relaunch preserves both paths. Defaults name the current Release
helper and raw source sidecar explicitly. Relaunch reuses the existing process_capture Windows
argument encoder, preserving a trailing backslash before a closing quote, and stays hidden.

partition_manager_certification_steps.ps1 owns the shared checked-step and artifact lifecycle.
The VHD wrapper previously checked LASTEXITCODE alone. The hardware wrapper also checked $?,
but a directly reproduced nonterminating child error can leave both apparent success and zero
exit code after scriptblock completion. The shared seam now rejects emitted ErrorRecord values,
preserves normal output and still checks command status/native exit code. No error is silenced.

Generated status, gap JSON/Markdown and bundle steps clear their exact previous output files,
run through that checked-step seam and require newly produced files. VHD preflight uses the same
contract. Hardware and VHD wrappers no longer carry separate guard implementations. A directory
or reparse output refuses before deletion; a failed clear remains an error. The VHD final claim
also now requires a string, matching the hardware acceptance rule.

Eight additional fixture groups exercise the real shared helpers and AST-selected wrapper calls
without executing either certification workflow. Only harmless native exit-code commands,
in-process failing scripts and ordinary temporary artifact files are used. Twenty-four additional
isolated in-memory mutants were killed, bringing this script to 15 groups and 40 guard drills.
These cover emitted PowerShell errors, native exit status, clear/require-regeneration guards,
each of seven wrapper artifact routes, all eight verifier/getter path forwards, producer helper
forwarding, both elevated path forwards, trailing-slash quoting and array-valued claim refusal.

Exact unchanged HEAD VHD functions/call sites also went RED separately for error-without-exit,
stale no-op status generation, array-valued claim and trailing-slash quoting. Current helpers and
wrapper went GREEN afterward; all four edited source hashes stayed unchanged during those
isolated drills. Scoped PowerShell syntax, ASCII and wrapper diff checks pass.

Strict-wrapper handoff SHA-256 values:

- Shared step module: 0883A56CD2E16EDBEA89910DF295B44565C083B63FB5975B3913CA8F11630CBD.
- VHD wrapper: 716B90E11457C1B7EAE153A4D654A44476832BA592DD143B4BD9119239B5787A.
- Hardware wrapper: 19CD4F1E61ED5A36E6BDAA3A91161FCDAF9DE65BAF988C28D1EC23BBFD7EA799.
- Routing fixture: E79E4382131D0085EAFC2D61F19E696F96F5424820018D010836D50D1076EEE6.

## Optional Git provenance return-contract proof, 2026-09-06

The producer now uses Get-CertificationGitProvenance to capture optional Git probe stdout,
stderr, exit code and timeout state through process_capture.ps1. An unavailable repository
retains its diagnostic in git_probes and git_error without emitting a PowerShell ErrorRecord
or changing scenario outcomes. The producer explicitly exits zero after successful report
generation; missing required VHD evidence still exits two, and failed scenarios still exit one.
No stderr suppression was added.

A fresh Windows PowerShell 5.1 run of test_partition_manager_controller_restore_routing.ps1
with DryProducerOnly and RunGuardDrills passed. Its disposable non-Git source package ran only
metadata mode: run_vhd_data_disk_matrix=false, status=NotRun, zero passed, zero failed and twelve
skipped scenarios. The report retained Git exit 128 and the actual "not a git repository"
stderr. Both direct invocation and the shared checked-step caller accepted successful completion;
the required-evidence variant preserved exit two. No VHD file or storage mutation was created.

Two isolated mutations of disposable producer copies went red independently: removing the
explicit success exit leaked an earlier native exit code, and clearing captured stderr lost
the diagnostic required by the fixture. Production source bytes were never mutated. Retained
evidence is build/wave32-producer-provenance-verified.log. Earlier
build/non-git-producer-before.log records the original exit=-1 false failure, while
build/non-git-producer-after-ps51.log records its corrected positive result.

The complete safe routing fixture was then rerun in Windows PowerShell 5.1 with RunGuardDrills:
exit zero, 16 groups passed and 42 isolated guard drills killed. Evidence is
build/wave32-controller-routing-verified.log. The earlier 15-group/40-drill handoff above is a
historical checkpoint; this additional metadata-return group and its two drills explain the
increase. Neither run executes the real VHD matrix, builds the application, or establishes a
VM, hardware or complete Release/CTest certification claim.

Producer and fixture hashes were unchanged before and after both runs:

- Producer SHA-256: B86DB364A651D593D3EF76617DBB6E23B1A376F46CEF9B017FEF07462A326D99.
- Producer Git blob: 408500c6d4e0a1e4b7dd3942b6ce23730de18b5e.
- Routing fixture SHA-256: 47480A02E5B73F3D06F1723166E37D758D60DA19796D7D0B9188797CE2CB8EC3.
- Routing fixture Git blob: 436720e2d3bfb4cb746242e7c2764b1d82ec4702.

## Live VHD admission failure and direct repair, 2026-09-06

The first fresh two-VHD run failed before controller execution. Source admission succeeded;
target OpenVirtualDisk returned Windows error 5 because its access mask omitted DETACH.
Microsoft requires DETACH when reopening a permanently attached VHD, including information-only
use. Both source and target now use the same production request builder with GET_INFO | DETACH.
The backing-store RWDepth is zero; the handle is not used to write backing-file bytes. Actual
DetachVirtualDisk remains source-only after a verified CreateImage result, never in validate-only.
The existing self-test independently checks the access mask and read-only depth, and its contract
prints individual check results on failure. A fresh Release rebuild and native contract passed.
Two isolated compiled negative drills then passed through the real mutation runner: omitting
DETACH failed only permanent-attachment-access, and restoring RWDepth's default failed only
attachment-backing-store-read-only. Both mutants built successfully before their CTest failures.
The runner restored production bytes and rebuilt successfully; no mutation marker/snapshot
remains. Logs: build/mutation-runs/1788714653816777900-2356 and
build/wave32-vhd-open-negative-drills.log. The live rerun remains open at this checkpoint.

API contracts: [OpenVirtualDisk](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/nf-virtdisk-openvirtualdisk)
and [OPEN_VIRTUAL_DISK_PARAMETERS](https://learn.microsoft.com/en-us/windows/win32/api/virtdisk/ns-virtdisk-open_virtual_disk_parameters).

The failed run confirmed worker quiescence and detached both newly created fixture disks.
Pre/post inventory matched the original two VM disks. The two 768 MiB VHD files remain preserved;
no original VM disk was selected. Evidence is retained under
build/wave32-live-restore-20260906-1004/evidence-focused and build/wave32-vm-focused-restore.log.
This failure is regression evidence, not restore certification.

## Supplementary proof cannot satisfy default restore evidence, 2026-09-06

The existing tests/unit/test_partition_manager_controller_evidence.ps1 now pins all four
supplementary scenario identities: vhd.image-restore-offline,
vhd.image-restore-offline-readonly, vhd.partition-image-create-offline, and
vhd.image-create-busy-refusal. Four otherwise-valid default reports isolate the scenario guard;
four schema-2 reports also model the supplementary operation counts and outcomes. The image-only
report contains one successful Create Image, while the busy report contains one failed,
non-skipped Create Image and RefusalVerified. A separate otherwise-valid default report proves
RefusalVerified itself cannot satisfy the required Passed status. Report filename and proof_mode
labels are not substituted for the validator's schema, scenario and execution requirements.

The existing schema-2, single-operation, failed-execution and failed-step fixture rows provide
independent coverage of those remaining guards without duplicating a validator or test harness.
The production restore validator was not changed or widened. Both complete ordinary-data suites
pass: 154 cases in Windows PowerShell 5.1 and 154 in PowerShell 7. Logs are
build/wave32-supplementary-evidence-ps51.log and build/wave32-supplementary-evidence-ps7.log.

Nine parse-valid isolated validator copies were tested on each host, with one selected fixture
per copy. Four copies relaxed the scenario identity for one supplementary mode each; the other
five relaxed schema, status, operation count, execution success and step success separately.
All eighteen runs went assertion-red. Eight mutants per host admitted the invalid synthetic
evidence. The operation-count mutant still encountered the downstream absent-RestoreImage guard;
its regression failed for the wrong refusal path, not for accepting a one-operation report.
These are nine mutations across six guard locations, not nine distinct guard locations.
Retained copies, exact outputs and result JSON are under
build/wave32-supplementary-evidence-drills. The initial raw Windows PowerShell child inherited an
incompatible PowerShell 7 module path and could not load Get-FileHash; that infrastructure failure
was not counted. Reusing the existing CMake env launcher with PSModulePath unset gave Windows
PowerShell its own module environment and produced the required assertion failures.

Validator SHA-256 remained
25489D7EF2B0AD5D6354561481553B9097C252D00400193846254FE7280DA3E0.
Fixture SHA-256 is F707E806E1E110BEE3BA1F63C002C7DB02C6278A5C6484E2E9B0BA8D04B7856B;
its Git blob is 50b628c7aa4eb6678139d8020700f5d0c025ad8a.
No binary, VHD, device or VM was executed by these fixtures. This proves the default consumer
boundary only; supplementary helper execution, its dedicated acceptance and live proof remain
separate integration gates. No complete Release/CTest or certification claim is made here.

## Explicit source-capture proof modes, 2026-09-06

The helper now accepts one exact --proof-mode value. Missing option retains restore; an empty,
unknown or repeated option refuses. No CLI disk or partition number becomes storage authority.

| Mode | Controller scope | Required outcome | Scenario identity |
| --- | --- | --- | --- |
| restore | LockedVolumes whole source, then restore | Two successful operations | vhd.image-restore |
| offline-restore | OfflineDisk whole source, then restore | Two successful operations | vhd.image-restore-offline |
| offline-readonly-restore | OfflineDisk read-only whole source, then restore | Two successful operations | vhd.image-restore-offline-readonly |
| offline-partition-image | OfflineDisk admitted source data partition | One successful CreateImage and exact raw-range hash | vhd.partition-image-create-offline |
| busy-source-refusal | LockedVolumes source held independently | One real failed CreateImage at owned source acquisition, no image | vhd.image-create-busy-refusal |

Default restore retains schema_version 1 and controller-restore-report.json. Supplementary modes
use schema_version 2, an exact proof_mode, their distinct scenario and
controller-image-proof-report.json. Busy proof status is RefusalVerified; its failed operation
is never relabeled Passed. The existing strict restore consumer remains closed to every
supplementary report. A separate supplementary evidence consumer and fresh native mode proofs
remain INCOMPLETE; a distinct label alone cannot establish certification.

Every mode shares the original VHD/nonce admission, real inventory, queue/apply path, containment,
timeouts and supervisor publication. Validation-only returns before source preparation. Only
after admission may offline test modes prepare the disposable source attributes; production
CreateImage still receives an already-offline source and never changes source state. The helper
pins a native physical control handle, records complete attributes before preparation, before
capture and after capture, and requires exact preservation across CreateImage. Offline modes
independently hash the full selected physical range before and after capture and require both
digests to equal the complete image. Selected range comes from the uniquely admitted nonce
volume partition, not caller input, and binds number, GUID, offset and size.

Offline fixture preparation first requires only that uniquely admitted source partition and exact
known MSR partitions. It acquires the existing owned source-volume lock, dismounts that locked
volume, then sets the nonpersistent offline/read-only attributes and verifies the full result.
Its lock handle closes before app execution. The offline bit alone does not prove a mounted
filesystem was quiesced: Microsoft's [WSL disk implementation](https://github.com/microsoft/WSL/blob/master/src/windows/common/disk.cpp)
performs the same lock/dismount prerequisite before setting disk attributes. This is fixture
preparation, not production source-state mutation or permission to force-close unrelated holders.

Busy mode holds the admitted source volume in the contained worker while the controller executes
its real PowerShell child. Inventory refresh happens while held. Acceptance requires one failed,
non-skipped, non-dry-run, non-cancelled, non-timeout CreateImage step containing the owned native
open/lock refusal; unrelated failures and verification output refuse. No image may exist. The
helper verifies source native binding again, unlocks and closes its holder, and records release
before supervisor job quiescence. Failure handles remain RAII-owned through worker exit.

Recorded operation evidence now contains the actual accepted payload and target kind, partition
number/GUID, offset and size. The helper compares requested fields with accepted queue fields
before Apply. Supervisor frame identity also binds the exact mode, schema, scenario and typed
validation-only flag. These controls prevent a mode label from substituting for the app request.

The duplicate-option loop now uses std::ranges::find_if, directly addressing the newly revealed
STL diagnostic without adding a suppression. Scoped formatting, lizard, PowerShell syntax and
cppcheck (183/186 active checkers) passed before compilation. The expanded executable contracts
use pure value/in-memory tests and ordinary invalid VHD files only. They do not access a host raw
device, attach a VHD or establish native VM proof. NTFS source proof does not certify foreign or
opaque filesystem byte-range capture; that remains a separate explicit native row.

Final local helper checkpoint: build and registered test_partition_manager_live_certifier both
captured exit zero. CTest ran one registered test (2.39 seconds), with --no-tests=error. The pure
--self-test independently reports 90 passed checks and storage_accessed=false. Five invalid-file
mode rows require no controller attempt, no preparation, no image, unchanged ordinary-file hashes,
typed mode/schema/scenario and durable isolated report files. Retained logs are
build/wave32-helper-proof-modes-frozen-build.log,
build/wave32-helper-proof-modes-frozen-contract.log and
build/wave32-helper-proof-modes-self-test.json.

This checkpoint is not native mode certification. Successful validation-only admission on real
disposable VHDs, new helper guard red drills and fresh supplementary mode executions remain open.
Offline selected-partition capture uses a physical source handle; it does not exercise the
LockedVolumes selected-volume FSCTL_ALLOW_EXTENDED_DASD_IO path. That native selected-volume proof
also remains open; managed script/native-boundary checks cannot substitute for it.

Frozen SHA-256 bindings:

- Helper source: 4E4022D9284C3A3ED86492CA64B135CDDCBE7F40A86D492AADB10179DC620E2A.
- Contract script: CE2C1CD9D5B397BE18163305297BE59E0EDC50C7E2BB816CD3E978BDC23DDBF2.
- Executable: FEB07F380B7E4AD7AE09611BB468EA72A79C3042AEC336E4D56A40F146577799.
- Raw source sidecar: F9D35A0F57EEA96826CFF8485B38BED0CE506150586D329A76C204D8890744E5.

## Schema 2 strict consumer -- 2026-09-06

The supplementary consumer is implemented in the existing
scripts/partition_manager_controller_evidence.ps1 library. Local assertion-drill verification
and the complete registered fixture passed as described below. This section does not certify
native operation, the full Release gate, foreign filesystems, or the selected-volume DASD path;
those broader gates remain INCOMPLETE until independently completed.

Assert-PartitionManagerImageProofEvidence requires caller-selected ExpectedProofMode,
ExpectedValidationOnly, ExpectedRunId, ExpectedFixture and the existing explicit report,
executable, raw source-manifest and project paths. It shares the bounded report loader, source
and executable binding, typed values, attachment parsing, job-quiescence and operation binding
with the default consumer. The original schema-1 restore entry still accepts only its original
scenario and two successful operations; it has no supplementary dispatch or fallback.

ExpectedFixture contains independently captured source and target_before attachment facts,
guest_directory, before_preparation_attributes, sentinel_sha256, and selected_partition
(number, guid, offset_bytes, size_bytes, volume_path). These are caller authority, not values
copied from the report. Whole-disk and partition operation targets, physical paths and exact
ranges must agree with those facts. The CreateImage payload has exactly its six requested keys;
RestoreImage also binds the controller-added target disk UniqueId, serial and size, for ten
keys total. Full verification and both actual per-operation cancellation/timeout Booleans are
checked, with no string/number coercion accepted as a Boolean.

Offline source preparation must record an owned lock, dismount and release. The complete native
attribute value must match the initial observation plus exactly the requested offline/read-only
bits, and remain unchanged during capture. All other bits are preserved. Independent raw-range
hashes before and after must match the image hash. Positive modes require LocalImagePath from
the caller, with its real retained file length and SHA-256 checked; guest report paths are only
compared with expected guest paths and are never opened as local artifact authority.

Busy proof requires one actual non-skipped failed CreateImage step with the owned acquisition
error and no full/sample verification witness, the independently pinned source lock path and
partition, unchanged source attributes, released holder, and image absence. Busy and validation-
only callers supply ImageAbsenceEvidence (run_id, report_sha256, image_path, absent), recorded
independently on the guest before cleanup and bound to the exact report bytes. Host absence of
a guest C: path is not evidence. Admission-only reports require zero operations and no preparation,
capture, lock or restore claims; their receipt says AdmissionOnly and RuntimeEvidence=false.
Capture-only and busy reports cannot carry restore-success fields. Independent post-run marker
checks remain the invoking fixture's responsibility; report labels do not invent those checks.

Shared canonical hash/revision/VHD-size/job-count parsing now uses the strict end-of-string
anchor, not the regex dollar anchor that accepts a final newline. Separate old-regex reversals
cover each shared guard and the new native-attribute/volume-path parsing. No suppression was
added. All earlier helper/source hashes and fixture counts above remain historical checkpoints.

The current fixture preserves all 154 earlier cases and adds 198 cases, for 352 passing on both
Windows PowerShell 5.1 and PowerShell 7. Captured process exits were zero. The final runs took
108.318 seconds and 94.095 seconds respectively; the existing registered 120-second test limit
was not increased. Ordinary generated whole-disk image data is shared read-only within each
fixture run, while report/source-mutation cases remain isolated. No VHD, raw device or VM is
accessed by this fixture. Logs: build/wave32-schema2-frozen-ps51.log and
build/wave32-schema2-frozen-ps7.log.

The source-bound assertion-removal catalog produced 191 case-specific assertion-red results:
144 weakened cases admitted invalid evidence; 47 reached another refusal and failed the test's
required first-refusal diagnostic. These are not 191 security findings. Each mutation had a
confirmed false-guard bypass: 189 had one activation; two directory cases intentionally had two
activations because the same guard validates both VHD parents or both equivalent-path operands.
Those two cases were rerun separately with exact expected_hits=2 after an overly strict private
harness initially classified their real assertion-red results as invalid. Zero-hit drills never
count. The final classified receipt preserves the 189 single-hit results and two fresh two-hit
results against identical frozen library/fixture hashes:
build/wave32-schema2-drills-classified/results.json. Original per-case logs and the full traced
baseline remain under build/wave32-schema2-drills-activated-final.

Earlier diagnostic runs also exposed a test whose broad text match accepted a later disk-number
refusal, and a private selector tied to a randomly generated report digest. The test now requires
its exact first-refusal prefix; the digest drill selects its uniquely named field comparison and
requires actual activation. Their initial runs are retained as diagnostics, not credited proof.
Separately, six exact production regex endpoints were independently restored to their old dollar
anchor and each run on both shells. All 12 runs admitted malformed evidence and therefore failed
the fixture assertion; results are retained under build/wave32-schema2-regex-final. Shared source
files were never mutated by these drills. Syntax, scoped ASCII (three files) and diff checks pass.

Frozen SHA-256 bindings for this local consumer checkpoint:

- Validator: 658532A4AB1C3354020D4BB887E18C0C9681421DCCFF96DA6F8EF44817FA9171.
- Fixture: E97E3CE371B59F3227F3BD922BEAA7B35D7749EFEC417DF980D7EA366E5E8F61.

Fresh supplementary native runs must call this same strict entry with independent fixture facts
and a build-bound source tree; a second weaker report validator is not a substitute. Evidence
collected while compiled mutation drills run cannot be declared current-source certification
until the source tree is restored and the retained evidence is revalidated against it.

## Empty VHD serial and fixture mechanics correction -- 2026-09-06

Fresh native admission-only evidence exposed an invalid consumer assumption: both Get-Disk
serial fields were present JSON strings containing no characters. The helper correctly returned
Validated with validation_only=true, no source preparation, no controller attempt and zero image
operations. The consumer rejected that valid unknown-serial observation as a missing serial.
Evidence: build/w32s-evidence-4a60d3f1/collected/evidence/supplementals/
controller-restore-8671e295-1bd7-4219-bf7d-69c28a8a5098. Original disks and allowed cleanup were
verified separately by the invoking owner; this admission-only case is not image-operation proof.

Assert-PmImagePinnedAttachment now requires a present string serial on both sides and exact
equality, while allowing an empty string. It does not invent a serial or treat a missing/null/
numeric/array field as unknown. VHD identifier, disk UniqueId, path, disk number, size and other
strong bindings remain unchanged. Restore payload serial binding also remains present, typed
and exact. The old actual validator fails the new empty-serial admission positive; the fix
passes it. Logs: build/wave32-serial-old-red.log and build/wave32-serial-fixed-green.log.

The fixture now preserves all earlier 352 cases and adds 37, for 389 total. Eight positive rows
cover empty source/target serials in all four supplementary modes, both runtime and admission-
only; restore payloads carry the same empty observed target serial. Missing/null/type/mismatch
negatives remain strict. Final targeted reversals run on both shells: 16 restored old-nonempty
checks refuse valid empty observations, 12 weakened equality checks admit invalid evidence, and
16 removed expected-type checks reach later refusals and fail their required diagnostic. All
44 are assertion-red, not compilation/infrastructure failures. Retained results:
build/wave32-serial-guard-drills-final/results.json.

The first 389-case direct runs exit zero in 135.501 seconds (Windows PowerShell 5.1) and 101.620
seconds (PowerShell 7), but the complete CTest attempt times out at its unchanged 120-second
limit after printing 389 passes. That is not a green registered gate. Its leftover synthetic
directory shows cleanup was interrupted. The direct mechanics fix retains canonical temporary-
root and junction safeguards, uses Directory.Delete for generated ordinary files, and publishes
the final success summary only after cleanup. Report-only image cases share one immutable
synthetic source tree and copied expected records; source/header mutation cases remain isolated.
The actual validator still rehashes current source files on every call. No validation-result or
current-file hash cache was introduced.

Serialized final direct runs pass all 389 cases in 65.389 seconds (PS5.1, cleanup 2.107 seconds)
and 65.863 seconds (PS7, cleanup 3.577 seconds), with captured exits zero. Logs:
build/wave32-serial-shared-inputs-ps51.log and build/wave32-serial-serialized-ps7.log. A concurrent
PS7 run took 155.715 seconds while independent drills/build work also ran; that diagnostic is
retained in build/wave32-serial-shared-inputs-ps7.log and receives no budget credit. Separate
injected cleanup failures on both shells prove the old fixture prints its final pass summary
too early and the fixed fixture does not: build/wave32-fixture-cleanup-report-proof/results.json.
The exact mechanics diff is build/wave32-serial-fixture-mechanics.diff; its reconstructed before
file matches SHA-256 3B2921601F76967F591966C5A45052DA762511FB4FD19138623C75288C1B68A7.

Current serial/mechanics checkpoint hashes supersede the earlier local consumer checkpoint:

- Validator: 21C57BA755E7F9D5C592A7A8F820EDF92A6420B3076D0C4253F41503F6115130.
- Fixture: CF650CEC558B6636CC8199E6B4EA00EF4FDA7B9495F3138B2EDECD6B3AF3C429.

At that historical checkpoint, the registered CTest rerun awaited release of the shared
test/build tree after compiled-mutation restoration. The later complete integration gate
cited at the top now covers current fixtures. Native supplementary evidence and its remaining
limits are stated in the current checkpoint, not inferred from these earlier local results.

## Parser-backed MSR fixture admission, 2026-09-06

The fresh supplementary VM's admission-only receipt passes the shared schema-2 validator and
independent postchecks. Offline preparation then refuses before any controller operation because
the inventory parser gives MSR partitions an InferredProtected display annotation. The original
helper assumed every present PartitionVolumeInfo represented an actual Windows volume. Its
hand-built pure fixture omitted the annotation, so that test did not reproduce the live input.

The new test uses StorageInventoryWorker::parseInventoryJson. Against the original helper guard,
exactly offline-fixture-inventory-msr-positive fails; the other 116 checks pass. The corrected
helper accepts only the exact parser-produced MSR annotation, with all 16 volume fields compared
against that display-only representation. Actual volume identity, geometry, protection metadata,
probe details and errors still refuse. Both the failed-lookup Boolean and error text independently
refuse. Exact MSR GUID classification and the single admitted source-partition check remain.
No filesystem is inferred for capture and no mount/lock guard is removed.

Fixed helper build, all 117 pure checks and the registered contract pass. Format, complexity and
scoped cppcheck pass (183/186 active checkers). All eighteen independent compiled field/lookup-guard
reversals are killed, with zero survivors or invalid mutations. An independent log audit confirms
each reversal fails exactly its intended assertion while all other 116 checks pass. Normal
restoration rebuild succeeds, the source hash matches its original snapshot, and the active
marker/snapshot are gone. Evidence:
build/wave32-msr-parser-old-guard-self.json, build/wave32-msr-parser-fixed-self.json and
build/wave32-msr-annotation-compiled-mutations.log and build/wave32-msr-independent-guard-audit.json.
Restored helper source SHA-256 is 4a5eb83b872410f66ce471ff27c0b427c939e0c32685b3707b4a3b5dc8665390;
the raw source sidecar is 188d345f6799e86b7d4af20a187a68cdeda2f928fa78d5d1e56a96a9d1e6b584.
At this September 6 checkpoint, fresh native offline execution remained open while the owner
restarted the VM; no storage operation was running and the earlier fixture pairs were detached.
The September 8 native evidence checkpoint above supersedes that temporary execution status.
