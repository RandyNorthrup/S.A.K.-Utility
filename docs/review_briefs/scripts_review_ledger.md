# Review ledger full-file and source-language coverage review

Scope: the 2026-09-04 `UNIT_RULES`, `classify`, and displayed unit-rule delta in
`scripts/review_ledger.py`, plus `tests/unit/test_review_ledger.py`.

- [x] [HIGH] First-party C source, C fixtures, Python and PowerShell tests, and Windows command
  launchers were missing from the R5 review denominator despite being executable source.
  RESOLVED: explicit group-to-extension rules include those languages while preserving existing
  review units. Directory-prefix boundaries still exclude similarly named unrelated roots.
- [x] [MEDIUM] The displayed unit rule was independently hard-coded and could disagree with the
  actual classifier.
  RESOLVED: the inventory's rule description is generated from the same extension mapping used by
  `classify`.

Independent bounded review on 2026-09-04 found no remaining issue in this delta. All seven focused
tests passed, including source/test language additions, existing-language preservation, and
resource/generated-path exclusion. The owner agent separately ran five negative drills against
the original classifier and reported each expected test failure.

This review did not refresh or modify the live ledger inventory or its state.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
The full-file findings below remain INCOMPLETE despite that attained local gate.

## Full-file review -- INCOMPLETE

Reviewed all of `scripts/review_ledger.py` and all seven tests in
`tests/unit/test_review_ledger.py` on 2026-09-04, including path admission, current-byte hashing,
state reconciliation, evidence retention, normalization, persistence, and CLI selection. Exact
reviewed Git blobs:

- Script: `5441699a399913e912a2a8a38ceb4a64413e6ca3`.
- Tests: `e691ba8f2d67122222a8a33f5216b03322bfe871`.

- [ ] [HIGH] Inventory can credit an index blob instead of the current reviewed file bytes.
  INCOMPLETE: `current_units` at script lines 96 and 112-113 hashes only paths reported by
  `git diff-files`. Git's assume-unchanged or skip-worktree flags can hide a changed working file
  from that result. A confirmed in-memory counterexample supplies an empty dirty set, an index
  blob A, and a live hash B: inventory returns A without calling the live hash function. Existing
  driver evidence for A therefore remains reviewed. Direct current-byte verification, or an
  explicit refusal of hidden-file modes, is required before the exact-blob claim is sound.
- [ ] [HIGH] Driver evidence is neither admitted nor revalidated as a durable reviewed artifact.
  INCOMPLETE: `cmd_mark` at lines 300-308 accepts an empty brief, and lines 322-323 record only its
  path. `cmd_inventory` at lines 138-139 preserves the reviewed status without opening or hashing
  that brief. Confirmed counterexamples mark an empty brief as driver-reviewed and preserve an
  existing driver mark whose brief no longer exists. Changing a brief also cannot invalidate the
  mark because no reviewed brief digest is stored. Require a nonempty repository-owned artifact
  and bind/revalidate its bytes as well as the source blob.
- [ ] [MEDIUM] An invalid mark request can rewrite a file before admission, including outside the
  repository.
  INCOMPLETE: `cmd_mark` joins an unrestricted `--brief` path and normalizes it at lines 300-308
  before checking target membership and the expected blob at lines 313-321. A confirmed
  counterexample passes `../outside-review.md` containing a typographic dash and a nonexistent
  target: the outside file is rewritten before the command exits with refusal. Resolve and
  constrain the destination, then finish target/hash validation before any write.
- [ ] [LOW] Invalid selection arguments can report misleading progress instead of refusing.
  INCOMPLETE: lines 228-235 and 338-339 admit negative counts and unknown group names. With three
  pending script units, `next --count -1` succeeds and prints two units under `next -1`;
  `next --group scripst` succeeds with `no pending units`. Validate a positive count and a declared
  group before inventory refresh or selection.

The seven existing tests cover classification only. They do not pin these hashing, evidence,
admission-order, or selection contracts; corresponding positive/negative tests remain INCOMPLETE.
Counterexamples used in-memory mocks and did not write source, tests, or ledger state. No shared
build or CTest was started during this review.

Actual-mark impact was checked separately against the live tree: no review unit had
assume-unchanged/skip-worktree flags, and all 56 current driver marks matched direct working-tree
hashes and had present, nonempty briefs. These findings do not establish that the owner's latest
13 explicit-blob header marks are wrong; they identify admission and future-invalidation gaps.
Concurrent ledger writes were not established as a supported caller and are not claimed as a
confirmed defect here. This whole script must not be marked fully reviewed/closed until the
confirmed findings are resolved and tested.
