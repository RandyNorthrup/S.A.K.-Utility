- [x] [HIGH] Default summary generation persisted recognized credentials when callers omitted the
  optional redactor.
  RESOLVED 2026-09-01: credential redaction is unconditional; an optional caller redactor now
  layers on top instead of replacing the security boundary.
- [x] [HIGH] Transcript metadata bypassed the conversation persistence redactor.
  RESOLVED 2026-09-01: ConversationStore redacts transcript role, text and nested metadata before
  writing either the transcript or search-index record.
- [x] [MEDIUM] Positive output caps below the truncation marker length could return more text than
  the caller allowed.
  RESOLVED 2026-09-01: the marker itself is clipped for tiny caps; every positive limit is now a
  hard result-length ceiling.
- [x] [MEDIUM] Wrong-typed, fractional or out-of-range status fields were coerced to plausible
  success values.
  RESOLVED 2026-09-01: exit codes require a finite in-range integer and success requires a JSON
  boolean; invalid fields are shown as invalid.

Re-review result: zero open findings. Four focused regression tests failed against the old
boundaries. The ai_tool_result_recorder mutation catalog kills 6/6 mutants, and the conversation
persistence catalog kills 3/3 mutants.
