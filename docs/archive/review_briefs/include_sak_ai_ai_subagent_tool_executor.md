- [x] [HIGH] Tool-executor instances had no explicit concurrent-call safety contract.
  RESOLVED 2026-09-03: the default is serial/fail-closed; construction must explicitly set
  `concurrent_safe` before `supportsConcurrentCalls()` can admit parallel delegate tool use.
- [x] [HIGH] Cancellation needed pre-dispatch refusal and correct terminal status ownership.
  RESOLVED 2026-09-03: the executor refuses pre-dispatch cancellation with structured error text
  `Cancelled before tool dispatch`. It does not serialize a canonical `Cancelled` JSON status.
  The runner and orchestrator use the cancellation token to assign the terminal cancelled
  subagent/phase status, including child-only cancellation detected after dispatch.
- [x] [MEDIUM] Mutation-intent checks lacked the originating user request.
  RESOLVED 2026-09-03: the executor propagates `AiSubagentTask::user_message` into the guarded tool
  request while keeping delegated model text untrusted.

Mutation evidence: concurrent-safety and child-cancellation guards are killed in
`ai_orchestrator` 34/34; executor argument/dispatch guards remain covered by the existing focused
executor catalog and tests.

Exact-header convergence 2026-09-04: NO FINDINGS for live Git blob
`50a19c8965540c1c236f866ec891b8efaa7194be`.
The later four-finding delta also passed its 13/13 compile-valid assertion drills: eight live
response-body guards, two DPAPI failure-wipe guards, and three resume-plaintext/context-notice
guards. Restored focused Release builds and 3/3 CTests passed; evidence and runtime-skip boundaries
are recorded in the OpenAI Responses client, credential store, and assistant panel briefs.
The final CTest record is `build/artifacts/wave31-ai-four-fix-final-ctest.xml`.
Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
