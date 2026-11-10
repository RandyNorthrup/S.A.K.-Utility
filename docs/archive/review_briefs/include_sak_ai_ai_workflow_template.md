- [x] [HIGH] Public workflow execution could accept unsupported phase types and unknown agent
  model policies after bypassing file-load validation.
  RESOLVED 2026-09-03: `WorkflowTemplate::isValid` rejects both, and public orchestrator `run()`
  invokes the complete validation contract before any resolver, callback, or phase.
- [x] [LOW] `WorkflowCancelPolicy` carried four all-true fields with no live behavior-changing
  caller, while every bundled workflow repeated the same inert object.
  RESOLVED 2026-09-03: the dead knobs and all 25 redundant JSON blocks are removed. Live root and
  child cancellation semantics stay in the cancellation token and runner contracts.

Mutation evidence: runtime-template admission and unknown model policy are killed in
`ai_orchestrator` 34/34. Workflow-store, orchestrator, and workflow-eval focused tests passed.

Exact-header convergence 2026-09-04: NO FINDINGS for live Git blob
`6c2f666989b4981d6e7138cdba4719c7bf6b7335`.
The later four-finding delta also passed its 13/13 compile-valid assertion drills: eight live
response-body guards, two DPAPI failure-wipe guards, and three resume-plaintext/context-notice
guards. Restored focused Release builds and 3/3 CTests passed; evidence and runtime-skip boundaries
are recorded in the OpenAI Responses client, credential store, and assistant panel briefs.
The final CTest record is `build/artifacts/wave31-ai-four-fix-final-ctest.xml`.
Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
