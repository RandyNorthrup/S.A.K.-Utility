- [x] [HIGH] Transcript/command search depended on a derived index plus raw-log fallback, with
  best-effort writes and an incomplete marker that could itself fail silently.
  RESOLVED 2026-09-02: the duplicate index/marker implementation is deleted; bounded raw logs are
  the single authoritative search source.
- [x] [HIGH] Malformed, truncated, oversized or changed JSONL could be skipped and returned as
  partial transcript, response-id or search data.
  RESOLVED 2026-09-02: bounded binary readers require complete newline-terminated schema-v1
  objects and return zero partial data with an exact error.
- [x] [HIGH] Transcript/command/context files and externally modified memory had unbounded reads
  or growth.
  RESOLVED 2026-09-02: hard line/file/search-aggregate limits, a 4 MiB memory recovery-read cap,
  stable exact reads and projected append caps now fail closed.
- [x] [HIGH] Artifact rename merged into an existing directory and used rename-copy-delete as a
  fallback, allowing irreversible partial moves or an orphaned copy after failure.
  RESOLVED 2026-09-02: only an exact same-volume rename is allowed; existing destinations and
  failed moves are refused before copying anything.
- [x] [MEDIUM] writeUsage silently created a new session when none was active.
  RESOLVED 2026-09-02: it returns the exact No active AI session refusal.
- [ ] [HIGH] Manifest reads and session-root enumeration remain name-based and need bounded,
  handle-pinned identity/reparse protection before this source can be marked reviewed.
- [ ] [HIGH] Artifact-root/subdirectory creation remains name-based and needs pinned parent
  identity enforcement against junction swaps.

Re-review remains INCOMPLETE. Seven conversation-store mutants are killed and focused tests pass,
but the two handle-pinning findings above remain open; this source is not ledger-credited.
