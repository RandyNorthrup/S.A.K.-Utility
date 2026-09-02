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

Re-review result: zero open findings. Skill and workflow race/cap suites cover both consumers.
The shared loader's four boundary mutants are killed by the skill-store suite.
