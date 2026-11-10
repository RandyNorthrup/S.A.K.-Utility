# Release claims: current evidence sets, not blanket caveats

Status: COMPLETE for this scoped claim/evidence-authority implementation. The September 19
full Release build and 281/281 CTest gate passed on current code; evidence is retained in
build/wave36-full-build.log and build/wave36-full-ctest.log. Both-host selector refreshes in
build/claim-authority-d765de1b/drills-ps51-wave36-current and drills-ps7-wave36-current each passed
16 independent guard triples. No certification result was upgraded or requirement suppressed.

The old caveat gate treated every non-HardwareCertified status as if the full external matrix
were missing. At the 2026-09-06 checkpoint, status accepted 11 of 12 retained VHD contracts and
all 18 external contracts while refusing vhd.image-restore. The old gate required a false paragraph saying every
listed external category still needed evidence, and let historical text satisfy current caveats.

The gate now binds five unique canonical lines in docs/PARTITION_MANAGER_CERTIFICATION.md under
Current Certification State, ending at the next Markdown heading:

- Current claim level.
- Accepted VHD evidence count.
- Accepted external evidence count.
- Exact incomplete VHD scenario IDs.
- Exact incomplete external gate IDs.

Accepted counts are not new live-proof claims. The status/matrix schema, nonnegative integer
counts, matrix denominators, known unique IDs and passed-plus-incomplete invariants are checked
before document comparison. Higher claim levels cannot coexist with incomplete lower-level
evidence. Unknown, duplicated, missing or wrong-type data is refused, not coerced into completion.

Partial certification still requires current partial/incomplete wording and explicit refusal to
mark destructive paths fully certified before the required reports exist. FailedEvidence also
requires evidence-failure guidance. Current stale blocker text is rejected for completed groups;
historical sections cannot supply required current values or caveats. The broad unsupported-claim
scan still covers all release-facing files and was not weakened by adding historical exemptions.

The existing certification-tool suite now contains 28 synthetic release-claim cases. These cover
one VHD gap with all external evidence accepted, an external-only gap, both gap groups, complete
hardware evidence, failed evidence, wrong/duplicate/missing counters and IDs, forged status,
historical-correct/current-wrong text, missing current caveats, stale current blockers and an
unsupported claim outside the current-state block. ReleaseClaimsOnly runs these cases without
storage or controller preparation; the normal registered suite includes them all.

Proof on 2026-09-06:

- The unchanged HEAD checker rejects the truthful one-VHD-gap/all-external-accepted fixture.
- Twelve independent in-memory reversals are killed: current-block boundary, accepted-count
  comparison, incomplete-set count/membership, status ID membership, matrix denominator,
  passed-plus-incomplete sum, partial/refusal caveats, completed-external/hardware stale prose,
  and hardware-with-incomplete-evidence refusal. Tracked checker bytes remain unchanged.
- All 28 fixtures pass on Windows PowerShell 5.1 and PowerShell 7. Evidence is retained under
  build/wave32-package-rehearsal-20260906-095557-f2c8b75a in claims-complete-ps51.log,
  claims-complete-ps7.log and claims-guard-drills-complete.log.
- The live checker accepts the current CodeCompleteOnly document block against the actual
  claim-check-status.json. This is documentation/evidence-contract verification, not a new
  disposable-VHD restore certificate or a full application certification claim.

Source: scripts/check_partition_manager_release_claims.ps1. Regression:
scripts/test_partition_manager_certification_tools.ps1, Test-ReleaseClaimCurrentState.

## Explicit evidence authority (2026-09-08)

The standalone claim checker now defaults directly to the strict VHD evidence root. Historical
external manifest existence no longer selects that manifest. An external manifest is accepted
only when explicitly supplied; explicit status still takes precedence over generation, and
explicit roots are preserved. This changes evidence selection, not the status verifier's source
binding or evidence contracts. Current counts remain authoritative only in the certification
document and freshly generated status, not in this historical review brief.

Aggregate release readiness retains its separate readiness scratch root and caller-specified
root, manifest, checklist and requirement switches. Its old auto-detection branch also selected
historical external evidence and silently enabled both strict switches; that branch is removed.
Default scratch planning still writes a plan-only report only inside the scratch root. A caller
consuming current VHD evidence must explicitly pass the strict root and RequirePartitionVhdEvidence;
the default aggregate scratch report cannot satisfy a higher current document claim.
An explicit strict-root selection without either evidence requirement now fails before any child
check or generated write. Absolute and relative paths, case, slash, trailing separator and dot
variants resolve to the same refusal. The caller's root is never silently redirected.

The canonical test suite retains all 28 current-state cases and adds 22 executable selector and
fixture-isolation cases. Production parameter blocks, functions and contiguous aggregate selector
statements execute unchanged inside isolated roots. Only external child scripts are instrumented:
they record actual bound inputs and perform observable fixture writes. These tests prove selection,
precedence, explicit-input preservation, missing-input refusal, strict-switch propagation and
plan-only routing; they do not execute storage certification or prove external evidence valid.

Both PowerShell hosts passed all 50 cases. Evidence is under build/claim-authority-d765de1b in
final-green-ps51.log and final-green-ps7.log. Windows PowerShell is launched with PSModulePath unset so it loads
its own installed modules; no execution-policy bypass or suppressed test failure is added.

Sixteen independent production selector drills passed on each host: both original implicit-import
implementations, default and explicit roots, explicit external input, explicit status precedence,
missing status and manifest refusal, both requirement switches, checklist selection and refusal,
required checklist refusal, strict-root plan-only routing, and pre-write unsafe-mode refusal.
Five path-alias refusal cases each retain a strict-report canary and produce no child receipts.
Every drill ran green, produced its
named assertion failure against the mutated copy, restored exact bytes in finally, and ran green
again. Results and source/log hashes are in drills-ps51-final/results.json and drills-ps7-final/results.json.
Two further drills reversed the fixture write-containment guard itself, one per host; the owned
outside canary made the missing guard observable. Both went red and restored green, with exact
test-source restoration recorded in isolation-drill-final/results.json. No live source was mutated.
Initial 45-case/15-drill results remain as earlier evidence, not as final-source proof.

### Fixture isolation incident and repair boundary

The first PS7 selector run failed because fixture children used relative .NET output paths.
Push-Location does not change .NET's process working directory. Two untracked scratch files were
overwritten with fixture text:

- artifacts/partition-manager-certification/readiness/external-evidence.template.json
- artifacts/partition-manager-certification/readiness/external-evidence.checklist.md

Original bytes are unknown; exact historical restoration is not claimed. The resulting bytes
are retained as affected-scratch-external-evidence.template.json and
affected-scratch-external-evidence.checklist.md beside first-ps7.log in the evidence directory.
No disk command ran and no strict certification evidence was targeted.

Fixture children now validate every output inside their unique root before any write, including
their call receipt, and use absolute paths for every .NET write. An executable outside-root canary
case proves rejection before the canary or receipt changes. The corrected second-ps7.log is
retained alongside the failed setup, not substituted for it.

The canonical manifest generator produced independently inspected replacement templates under
scratch-repair-staging: 18 Pending rows, blank evidence references and no evidence directories.
Regenerating the two exact scratch targets with OutputPath, ChecklistPath and Force, without
CreateEvidenceDirectories, repairs current scratch templates without touching evidence reports.
That operation is regeneration, not recovery of unknown original bytes; staging alone does not
claim the actual scratch targets were repaired.

The root worker subsequently regenerated exactly those two scratch targets with the canonical
generator, without CreateEvidenceDirectories. Before writing, it verified their resolved paths,
non-reparse status and hashes against the retained incident copies. Regeneration exited zero;
independent inspection found 18 Pending rows and blank evidence references, and the canonical
checklist gate passed all 18 contracts. Evidence: build/wave33-scratch-template-regeneration.log.
Current scratch templates are repaired; recovery of the unknown original bytes is not claimed.

## Commercial mapping must not require a false promotion

The full CTest run exposed another stale-claim dependency: the commercial feature gate required
the exact sentence fragment saying the full historical external set now upgrades the handoff.
Removing that false current claim correctly broke the gate. The static mapping now requires the
hardware-wipe scope description and its canonical external.hardware-wipe ID. Current acceptance
levels remain the status/release-claim verifier's responsibility, not a hard-coded prose result.

The existing Apple capability/commercial fixture now has a positive commercial-mapping case and
separate scope/ID removal cases. Each removal asserts that its input exists before mutation.
The replaced hardware negative had become vacuous: its old promotion phrase no longer existed,
so the test passed for the unchanged gate failure instead of proving the intended guard.
Error assertions retain the full expected wording while allowing PowerShell's line wrapping.

All 11 capability/commercial cases passed in build/wave33-commercial-final-tests.log. Six
independent drills passed under both PowerShell hosts: old promotion requirement, missing scope
guard and missing evidence-ID guard. Each ran the existing fixture against an exact copied
production script, observed the intended red, restored the original bytes and passed again.
Evidence: build/commercial-drills-c766f671/results.json. Initial formatting-related assertion
failures remain retained; they are not counted as behavioral guard proof.
