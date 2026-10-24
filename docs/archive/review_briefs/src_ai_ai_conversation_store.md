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
- [x] [HIGH] Managed screenshot, download, default-report, and command-log leaves no longer cross a
  writable QString-path window: the shared loader writes and flushes a no-follow CREATE_NEW temp,
  then publishes by handle without replacement under the retained directory pin. Managed
  OfflineDeploymentWorker and AppActionService runs now write only into private temporary staging;
  every resulting file is streamed through the same pinned atomic publisher before its managed path
  is reported, and redirected staged entries are refused. Explicit user-selected output directories
  remain an approved external-output contract, not ConversationStore artifacts.

Re-review result: zero open findings. The original handle-pinning findings and every managed
artifact leaf writer are closed; sixteen conversation-store mutants are killed and focused tests
pass. Ledger credit remains a separate campaign step.
