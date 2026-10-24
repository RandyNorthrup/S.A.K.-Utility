- [x] [MEDIUM] Restore-image preflight comment claimed every refusal was handled in one helper.
  VERIFIED 2026-09-03: missing, non-file, and empty sources were already rejected together; the
  mandatory digest stage separately rejects unreadable content. The helper contract names file
  shape, source placement, and capacity; local source placement must be provable before approval.
- [x] [LOW] `m_headerWidgets` was never assigned or read and retained a header dependency.
  RESOLVED 2026-09-03: removed the dead member and header include. Production accessibility helpers
  now include `widget_helpers.h` at their real source use site.
- [x] [LOW] The header used QVector and uint64_t without direct includes.
  RESOLVED 2026-09-03: added direct QVector and cstdint ownership.
- [x] [LOW] Restore-image validation/hash helpers were declared as Qt slots despite having no signal
  role.
  RESOLVED 2026-09-03: moved both ordinary helpers into the private method section.
- [x] [LOW] Test-seam parameter names violated snake_case.
  RESOLVED 2026-09-03: renamed root_path and usable_bytes in declarations and definitions.
- [x] [LOW] Fourteen panel members still used the pre-gate camelCase style.
  RESOLVED 2026-09-03: renamed every declaration and source use to the enforced m_snake_case form;
  corrected the stale private-member example in CODING_STANDARDS at the same time.
- [x] [HIGH] Restore-image approval occurred before hashing, so a same-size content replacement
  between approval and hashing could become the pinned payload.
  RESOLVED 2026-09-03: the image is now hashed before approval, the digest is displayed in the
  confirmation, and the execution script still re-hashes and refuses any later replacement. The
  old ordering turns the permanent structural test red and was restored.
- [x] [MEDIUM] Self-disk preflight treated an unmappable local source as another disk, allowing a
  mounted-folder or volume-GUID source on the target disk to fail open.
  RESOLVED 2026-09-03: source placement is tri-state. QStorageInfo device identity maps volume GUID
  and mounted-folder sources; drive-letter roots without authoritative volume identity refuse.
  UNC and extended UNC also refuse because local administrative shares may alias the target disk.
  Ambiguous or unknown local mappings refuse. Catalog entries pin each guard separately.
- [x] [LOW] QWidget was forward-declared after its direct header was included.
  RESOLVED 2026-09-03: removed the redundant declaration.
- [x] [LOW] The digest helper comment still described hashing after operator approval.
  RESOLVED 2026-09-04: the header now states that hashing precedes approval, matching the caller.
- [x] [LOW] ActionLinkOptions comments still described APFS actions and label changes as limited
  to generated containers. RESOLVED 2026-09-04: both comments now name certified APFS COW operations,
  matching admission for supported generated and foreign containers.

- [x] [HIGH] Panel regression failures could leave Change Cluster Size or another modal waiting
  for a person. RESOLVED 2026-09-04: all 32 modal inspection callbacks now own a
  QPointer-backed scope guard that rejects their exact dialog when an assertion returns early.
  A watchdog records a QtTest failure before closing an overdue interaction. The panel CTest now
  explicitly uses Qt's offscreen backend and a 120-second limit. Scoped inspection timers also
  cancel pending callbacks before their captured stack locals expire. Three APFS callers now check
  operation count directly before dereferencing after a helper assertion. Success fixtures click
  the enabled Add to Queue button, so a disabled action cannot enter an unintended warning modal.
  Focused Release build and the full panel baseline pass (69 QtTest cases, zero failures or skips).
  The reversed backup-volume guard compiles and fails the exact cluster Add to Queue assertion
  unattended, with no watchdog rescue. Independent drills kill the unowned callback timer and
  disabled modal-rejection guard; the latter reports a bounded test failure instead of waiting.
  Every current panel catalog entry now compiles and is killed: 46/46, zero survivors and zero
  product-mutant watchdog deadlines. The runner refused two malformed mutation builds instead of
  counting them as kills: one dropped a parameter's last use, one referenced a renamed variable.
  Both catalog entries were fixed. Target-only drills retain the independent layout guard, and the
  false-success drill consumes the nodiscard batch result. Only unfinished suffixes were resumed;
  completed entries were not repeatedly run. Final restore builds passed and the marker cleared.

2026-09-04 retained proof under `build/mutation-runs/`:

- `1788569578952472800-13628`: catalog entries 1-3 compiled and killed.
- `1788569764144122900-24184`: corrected entries 4-13 compiled and killed.
- `1788570156616720300-28000`: corrected entries 14-46 compiled and killed, 33/33 PASS.
- `1788569293061550400-8660`: exact cluster and expired-callback assertion failures. Its original
  third drill caused a test crash and was refused as an infrastructure error, never a valid kill.
- `1788569480178237000-25720`: corrected isolated cleanup drill, 1/1 killed by the bounded watchdog
  failure. Each successful product-mutant run retained QtTest assertion text and CTest XML.

Unattended workflow validation, 2026-09-04: Release panel build exit 0; panel, mutation-runner, and
review-ledger CTests passed 3/3 in 11.66 seconds. The panel reported 69 passed, zero failed, zero
skipped. Mutation marker absent; complete catalog integrity passed for 61 catalogs. This is focused
regression evidence; the attained complete local Release/CTest gate is recorded below.

- [x] [MEDIUM] Windows clang naming analysis could not declare the MSVC-only _udiv128 intrinsic,
  preventing analysis of the panel. RESOLVED 2026-09-04: the existing saturatingMultiplyDivide
  keeps supported Windows _umul128 multiplication and divides the high/low product with one exact,
  bounded 64-step unsigned long-division implementation. The high-limb overflow precheck preserves
  saturation; carrying the 65th remainder bit preserves divisors above 2^63. No compiler branch,
  declaration shim, fallback, or suppression was added. Existing checkedMul helpers do not provide
  a wide quotient, so no compatible existing implementation was duplicated.

Arithmetic follow-up evidence: 15 independent boundary controls include zero operands/divisor,
MAX/MAX identities, exact and truncated quotients, divisors above 2^63, overflowing results, and a
product whose high limb equals the divisor. Expected floors were computed independently using
arbitrary-precision integers. Five isolated guard mutants plus the two affected scaling mutants
compile and are killed (7/7), with no crash, timeout, skip, or build-only kill. The clone-wrap mutant
preserves independent zero-divisor policy. Evidence: build/mutation-runs/1788575471485299500-28752/.
The catalog now has 51 panel entries; unrelated completed entries were not rerun.

Restored source SHA-256 is 710E30C0DEA8D8E0E05C2D4FDCD568D0D4C3864B9D044A5BE928204B7D58998E,
matching pre-drill bytes. Marker/snapshot absent; Release build exit 0; final panel CTest passed in
9.53 seconds with 70 QtTest cases, no failures or skips. Scoped format/lizard/magic and cppcheck pass
(183/186 active checkers). Windows naming reran through the existing vcvars wrapper and passed
1/1 translation unit with exit 0 and no diagnostics; log: build/naming-panel-check.log.

Post-fix review result, 2026-09-04: NO FINDINGS in the final bounded header and panel regression
convergence pass. Header blob: `f473c61d1827646374fe85df681334ae18baa9bf`. Regression test blob:
`210bc12c4359849c55da0ac77f3c2585a7422ea7`. Panel source blob:
`12c05575281518673e309ffdd251d5bba19a16bc`. No new callback lifetime or unattended interaction gap
was found; controller and validator review scope was not reopened.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
