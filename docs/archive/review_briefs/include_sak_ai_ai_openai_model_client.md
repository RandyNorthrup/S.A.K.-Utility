- [x] [HIGH] Generic workflow context could be flattened into model system instructions.
  RESOLVED 2026-09-03: `openAiRequestFromModelRequest` preserves every declared untrusted context
  item as an explicit user-role input message; system instructions remain trusted-only.
- [x] [MEDIUM] Thread-launch failure could wait for the normal long timeout and report a false
  model timeout after no worker started.
  RESOLVED 2026-09-03: launch failure is terminal immediately, with balanced cleanup and the real
  launch error. Cancellation remains distinguishable from launch failure.

Mutation evidence: `ai_openai_model_client` killed 2/2 mutants, including role promotion and
thread-launch/cancellation misclassification. Its focused Release proof passed.

Exact-header convergence 2026-09-04: NO FINDINGS for live Git blob
`06c957bbcab36c37ba8a1b9d07fec71582f19558`.
The later four-finding delta also passed its 13/13 compile-valid assertion drills: eight live
response-body guards, two DPAPI failure-wipe guards, and three resume-plaintext/context-notice
guards. Restored focused Release builds and 3/3 CTests passed; evidence and runtime-skip boundaries
are recorded in the OpenAI Responses client, credential store, and assistant panel briefs.
The final CTest record is `build/artifacts/wave31-ai-four-fix-final-ctest.xml`.
Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
