- [x] [CRITICAL] Persisted recovery state was reusable without authenticated gate, workflow
  revision, next-phase, or one-time generation binding.
  RESOLVED 2026-09-03: the panel protects snapshots with current-user DPAPI before gate persistence,
  binds the pre-generated gate/run/workflow/phase/revision/history-derived next index, and atomically
  consumes a monotonic per-run generation before prompting. Unsigned, old, stale, and replayed
  records are refused.
- [x] [HIGH] Resume serialization/decryption left caller-owned plaintext `QByteArray` buffers alive
  after both success and error exits.
  RESOLVED 2026-09-03: scope guards securely wipe the serialized caller buffer and authenticated
  decrypted bytes; a private friend observer proves successful and failed-JSON paths.
- [x] [HIGH] Workflow guidance, software probes, and overseer work could degrade through missing
  resolution or non-model fallbacks.
  RESOLVED 2026-09-03: the panel supplies typed full-body guidance resolution, probes every declared
  requirement kind, and routes overseer phases through the concrete model runner. All failures are
  visible and closed.
- [x] [HIGH] User/input/tool/prior-result content could enter trusted instructions.
  RESOLVED 2026-09-03: the panel and runner preserve explicit structured user/tool-role boundaries;
  restored results never become system or developer instructions.
- [x] [MEDIUM] Context-pressure UI claimed server auto-truncation even though requests disable it.
  RESOLVED 2026-09-03: request policy refuses a known-full context locally, keeps server truncation
  disabled, and the critical notice now states both facts.

Mutation evidence: the first 15 `ai_workflow_resume_auth` guards were compile-valid and killed with
zero survivors. The three new resume-plaintext/UI mutants compiled and failed their expected
cleanup-count or context-notice assertion on 2026-09-04. Exact command, CTest and Qt assertion
logs are retained under `build/mutation-runs/1788571564795389400-27784/`.

Bounded review of the latest plaintext-wipe and context-notice deltas found no remaining issue on
2026-09-04. Scoped format, lizard, magic-number and exact catalog checks pass; cppcheck passed with
183/186 active checkers. Baseline and restored Release target builds passed. Final three-target
CTest passed 3/3 in 168.06 seconds. This suite reported 297 passed, zero failed, and four existing
skips: one host symlink-privilege case and three opt-in raw-storage certification cases. The panel
source hash matched its pre-mutation value and the marker cleared. Final CTest XML is
`build/artifacts/wave31-ai-four-fix-final-ctest.xml`.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).

- [ ] [LOW] The Windows separator-aware naming reveal finds 115 private-field naming violations
  in this header. The existing header filter concealed them from the configured gate. Functional
  convergence above is not a clean naming result. Semantic replacement preparation is read-only;
  the header stays pending in the review ledger until direct cleanup and fresh proof complete.
  The sole friend-test translation unit also exposes ten incomplete aggregate initializers that
  must be repaired before its semantic rename export is usable. No warning suppression is added.
