- [x] [HIGH] Workflow and skill stores risked maintaining separate native file-security code.
  RESOLVED 2026-09-01: one Windows handle-pinned loader now owns bounded enumeration, no-follow
  opens, parent identity checks, stable exact reads and resource-directory handling.
- [x] [HIGH] Native directory enumeration could materialize an attacker-sized listing before its
  cap was checked.
  RESOLVED 2026-09-01: FindFirstFileExW/FindNextFileW stops on item 513 and refuses the whole
  directory before sorting or parsing.
- [x] [HIGH] Path checks before QFile open could race a file or directory redirect.
  RESOLVED 2026-09-01: opened directory/file handles reject reparse points, and each file's
  realized parent object must equal the pinned directory handle.
- [x] [HIGH] Pre-read size checks did not protect against growth, truncation or short reads.
  RESOLVED 2026-09-01: reads are capped at policy max plus one byte and require matching before,
  bytes-read and after sizes.
- [x] [HIGH] Managed artifact leaf writers previously crossed a writable QString path window.
  RESOLVED 2026-09-02: the shared loader creates a no-follow, no-replace temporary leaf under a
  retained verified directory, writes or streams with hard caps, flushes, and publishes by handle
  without replacing an existing destination.
- [x] [HIGH] Multi-file managed outputs previously wrote leaves directly under session paths.
  RESOLVED 2026-09-02: offline and app-action producers use private staging; imports reject
  redirected entries, stream through the atomic publisher, and clean staging bottom-up without
  following junctions. A regression proved outside junction targets remain intact.

Re-review result: zero open findings. Skill and workflow race/cap suites cover both read consumers;
ConversationStore and app-action suites cover managed leaf writes/imports. The relevant catalogs
kill 16 ConversationStore mutants and four AppAction output mutants.
