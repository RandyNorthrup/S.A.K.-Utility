- [x] [HIGH] A directly injected model client could be used concurrently by parallel delegates.
  RESOLVED 2026-09-03: `canRunConcurrently` rejects direct-client sharing and requires one
  factory-created client per call; factory invocation itself is mutex-serialized.
- [x] [HIGH] Runner concurrency had no contract for tool executors or configuration setters.
  RESOLVED 2026-09-03: concurrent calls require explicit executor opt-in; configuration must finish
  before `run()` and cannot race it; non-owning dependency lifetime is documented.
- [x] [HIGH] Structured workflow data could be promoted into trusted model instructions.
  RESOLVED 2026-09-03: `AiSubagentTask::untrusted_context_messages` maps only to
  `IAiModelClient::Request::untrusted_user_messages` and survives JSON round-trip separately.

Mutation evidence: runner isolation, executor opt-in, factory serialization, shared-client
rejection, and role separation are all killed within `ai_orchestrator` 34/34. Focused runner,
orchestrator, and workflow-eval tests passed.

Exact-header convergence 2026-09-04: NO FINDINGS for live Git blob
`668b57dd851860ecf8a5457a9b6d15b60a01ccf3`.
The later four-finding delta also passed its 13/13 compile-valid assertion drills: eight live
response-body guards, two DPAPI failure-wipe guards, and three resume-plaintext/context-notice
guards. Restored focused Release builds and 3/3 CTests passed; evidence and runtime-skip boundaries
are recorded in the OpenAI Responses client, credential store, and assistant panel briefs.
The final CTest record is `build/artifacts/wave31-ai-four-fix-final-ctest.xml`.
Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
