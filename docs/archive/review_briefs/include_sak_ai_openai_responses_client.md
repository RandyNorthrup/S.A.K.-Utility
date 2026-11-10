- [x] [HIGH] The parser body cap ran only after `QNetworkReply::readAll`, allowing an oversized live
  response to be buffered and copied before refusal.
  RESOLVED 2026-09-03: every Responses, model-list, and token-count reply gets a cap-plus-one read
  buffer, `readyRead` and `downloadProgress` overflow guards, declared-length admission, immediate
  abort, and pre-read plus post-read final size checks.
- [x] [HIGH] An overflow-triggered abort could be reported as a generic network cancellation, while
  a deliberate user stop could run the normal failure handler.
  RESOLVED 2026-09-03: overflow carries an explicit size-limit classification; user cancellation
  disconnects completion handlers before abort and emits one clean completion.
- [x] [LOW] Network-reply internals needed a deterministic test seam without widening production
  API.
  RESOLVED 2026-09-03: bounded-reply helpers remain private and are reachable only by the friend
  test driver with injected `QNetworkReply` implementations.

Mutation evidence: all eight `openai_response_body_cap` mutants compiled and were killed for
production watcher wiring, buffer sizing, both streaming guards, overflow classification, both
final caps, and user cancel. The first four passed under
`build/mutation-runs/1788571352230388900-28276/`; the final four passed under
`build/mutation-runs/1788571484498476100-9856/`. Both retain exact command, CTest and Qt assertion
logs. The initial fifth mutation caused an unused-parameter build failure; that attempt was
rejected, the mutation was corrected to retain operand use, and its compiled assertion failure
was then proved. No build failure counted as a killed mutant.

Bounded review of the latest live-reply delta found no remaining issue on 2026-09-04. Scoped
format, lizard, magic-number and exact catalog checks pass; cppcheck passed with 183/186 active
checkers. Baseline and restored Release target builds passed. Final three-target CTest passed
3/3 in 168.06 seconds; this suite reported 31 passed, zero failed and three opt-in live-model
skips. Source hashes matched their restored values and the mutation marker cleared.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
