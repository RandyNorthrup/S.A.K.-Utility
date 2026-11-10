- [x] [HIGH] Workflow recovery needed an authenticated current-user storage seam rather than a
  plaintext or caller-authenticated snapshot contract.
  RESOLVED 2026-09-03: `protectCurrentUserData` and `unprotectCurrentUserData` require nonempty
  purpose entropy, enforce hard payload limits, and return typed errors on every DPAPI failure.
- [x] [MEDIUM] DPAPI test hooks were exposed as normal production API surface.
  RESOLVED 2026-09-03: wipe observers and the credential-decrypt driver are private friend-only
  certification seams; production callers retain only the two bounded DPAPI operations.
- [x] [MEDIUM] The existing API-key decrypt path wiped ciphertext and entropy only after success.
  RESOLVED 2026-09-03: one scope guard now wipes both mutable inputs after every
  `CryptUnprotectData` outcome, including authentication failure.

Mutation evidence: the original `ai_dpapi_state` catalog killed 4/4 mutants. The two new scoped
failure-wipe mutants both compiled and failed the expected zero-byte assertion on 2026-09-04.
Exact command, CTest and Qt assertion logs are retained under
`build/mutation-runs/1788571529506020700-17736/`.

Bounded review of the latest failure-wipe delta found no remaining issue on 2026-09-04. Scoped
format, lizard, magic-number and exact catalog checks pass; cppcheck passed with 183/186 active
checkers. Baseline and restored Release target builds passed. Final three-target CTest passed
3/3 in 168.06 seconds; the credential suite reported 9 passed, zero failed or skipped. Sources
were restored and the mutation marker cleared.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
