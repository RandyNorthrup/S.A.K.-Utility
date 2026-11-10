- [x] [CRITICAL] Generated titles are persisted, so secrets, credentials, user paths, URLs or code
  copied from the first prompt could become durable metadata.
  VERIFIED 2026-09-02: title generation routes through the shared redactor and tests realistic
  credential families, spaced Windows paths, arbitrary URL schemes and unclosed code fences.
- [x] [MEDIUM] Substring domain rules, punctuation-only input, non-Latin input or low-signal prompts
  could produce a misleading/generic title and defeat the workflow-title fallback.
  VERIFIED 2026-09-02: whole-word domain matching, Unicode word handling, multi-word product names,
  bounded title length and exact default-title detection have focused tests.

Re-review result: zero open findings. Both exported functions and the maximum-length constant have
live production/test consumers.
