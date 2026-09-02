- [x] [CRITICAL] Plaintext VM credentials could be written before ACL hardening, inherit broad
  access, cross a reparse parent, or survive an ACL/write/cleanup failure.
  RESOLVED 2026-09-02: the helper creates a no-share file with CreateNew, applies and independently
  verifies one current-owner FullControl rule before writing, flushes, verifies again, and removes
  only a file this invocation created on failure. Reparse, overwrite and create-race paths refuse.
- [x] [HIGH] icacls launch/exit failure or a stale reusable file could be accepted as secure.
  RESOLVED 2026-09-02: System32 icacls runs through reliable capture; every exit is checked;
  reusable files must already have exact owner-only ACL and a bounded nonempty size. New content
  must be 7-bit ASCII; lossy character replacement is refused before file creation.

Re-review result: zero open findings. PS7/PS5.1 tests exercise positive and every failure/race path
with 14 independent old-guard mutants killed.
