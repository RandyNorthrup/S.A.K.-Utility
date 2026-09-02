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
- [x] [HIGH] Manifest reads and session-root enumeration were name-based and unbounded.
  RESOLVED 2026-09-02: the shared AI store loader now bounds direct session children and manifest
  bytes, opens root/session/manifest without following reparses, verifies parent identity, requires
  stable exact reads, binds manifest id to directory name, and returns zero partial sessions with
  surfaced errors.
- [x] [HIGH] Artifact-root/subdirectory creation was name-based and junction-racy.
  RESOLVED 2026-09-02: the shared loader walks one safe child component at a time, verifies reparse
  and parent identity, and ConversationStore retains no-share-delete directory pins for the active
  session. commandLogPath and artifactSubdir use that single path.
- [ ] [HIGH] Artifact leaf files are still returned as QString paths to external writers
  (screenshots, downloads, reports, offline/app-action output). Directory pins prevent ancestor
  junction swaps, but cannot prevent an attacker replacing the final filename with a file reparse
  point after artifactPath returns. Closing this requires a handle/callback artifact-write API and
  migration of those external writer contracts; no ConversationStore-owned artifact write exists
  to migrate locally.

Re-review remains INCOMPLETE because the artifact-leaf API campaign above is open. The two original
handle-pinning findings are closed; fourteen conversation-store mutants are killed and focused
tests pass. This source is not ledger-credited.
