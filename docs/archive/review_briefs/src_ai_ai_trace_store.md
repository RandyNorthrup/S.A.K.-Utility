- [x] [HIGH] Appends checked only whether the current file was already full, so the next record
  could cross the stream cap; a single oversized record was also accepted.
  RESOLVED 2026-09-01: payloads serialize before mutation, line/file ceilings are enforced, and
  rotation occurs before projected size crosses the cap.
- [x] [HIGH] Malformed, truncated, oversized and unreadable audit input was skipped or returned as
  partial history.
  RESOLVED 2026-09-01: every load error clears the result and returns one exact diagnostic;
  aggregate, line, completion and read integrity are all checked.
- [x] [HIGH] Any JSON object, including an unsupported schema, was accepted as an event.
  RESOLVED 2026-09-01: every trace/activity record must carry the exact supported schema version.
- [x] [MEDIUM] Windows text-mode newline expansion could invalidate projected byte caps.
  RESOLVED 2026-09-01: JSONL writes and reads use binary mode, so measured and stored bytes agree.
- [x] [MEDIUM] Public path getters could race setSessionDirectory while append/load paths were
  protected by the store mutex.
  RESOLVED 2026-09-01: public getters now lock; already-locked I/O uses an internal path builder.

Re-review result: zero open findings. Twelve isolated guards failed against the prior behavior.
The trace-store mutation catalog kills 7/7 boundary mutants.
