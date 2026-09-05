- [x] [CRITICAL] Resume state could accept incomplete identity, unordered or contradictory phase
  history, caller-derived flags/results, and unsafe skipped or tool outcomes.
  RESOLVED 2026-09-03: resume requires exact workflow/run identity and canonical ordered history;
  phase type, agent, outcome, unconditional-phase and tool-result invariants are validated; flags
  and phase results are derived only from that history.
- [x] [HIGH] Parallel delegates could share one model client, unsafe tool executor state, or
  unsynchronized factory/callback paths.
  RESOLVED 2026-09-03: parallel execution requires isolated factory-created clients and an
  explicitly concurrent-safe executor; factory invocation is serialized; lifecycle callbacks run
  sequentially on the `run()` thread; the independent hard ceiling is eight subagents.
- [x] [HIGH] Missing/empty overseer, guidance, or required-software resolution could silently
  continue, and unsupported phase/model policy input could reach execution.
  RESOLVED 2026-09-03: typed resolvers, full public template validation, concrete overseer success,
  every declared guidance reference, and every required software kind all fail closed.
- [x] [HIGH] User request/input/tool/prior-result text could become trusted instructions, while a
  global string cut could split JSON or remove a safety tail.
  RESOLVED 2026-09-03: workflow data uses bounded structured user-role messages. Trusted guidance
  is included completely within its field cap or refused; no mid-JSON or guidance-tail truncation
  remains.
- [x] [MEDIUM] Cancellation and lifecycle accounting could misclassify a child tool stop or leave
  started/completed callbacks unbalanced after retry, reassignment, recovery, or exceptions.
  RESOLVED 2026-09-03: child cancellation is `Cancelled`; each declared phase has one balanced
  lifecycle pair; callback exceptions become visible terminal errors, including recovery.

Mutation evidence: `ai_orchestrator` killed 34/34 compile-valid mutants with zero survivors.
Its restored focused Release build and seven focused CTest targets passed.

Exact-header convergence 2026-09-04: NO FINDINGS for live Git blob
`0b6a60a4ddcc8d4f57c4ac60b774c1f1ec85bfb0`.
The later four-finding delta also passed its 13/13 compile-valid assertion drills: eight live
response-body guards, two DPAPI failure-wipe guards, and three resume-plaintext/context-notice
guards. Restored focused Release builds and 3/3 CTests passed; evidence and runtime-skip boundaries
are recorded in the OpenAI Responses client, credential store, and assistant panel briefs.
The final CTest record is `build/artifacts/wave31-ai-four-fix-final-ctest.xml`.
Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
