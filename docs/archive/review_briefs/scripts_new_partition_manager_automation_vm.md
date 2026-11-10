- [x] [CRITICAL] The generated guest password was written before a best-effort icacls call whose
  exit was ignored; an insecure plaintext credential could be reused indefinitely.
  RESOLVED 2026-09-02: generation routes through the owner-only staging helper and existing files
  must pass exact ACL and size verification before VBoxManage sees the path.
- [x] [HIGH] VBoxManage launch/exit handling used a scoped native capture block and could lose
  reliable stdout/stderr/exit evidence.
  RESOLVED 2026-09-02: every VBox invocation uses process_capture, redacts registered secret values
  before diagnostics, and fails on nonzero exit unless the caller explicitly handles AllowFailure.

Re-review result: zero open findings. Reported password path/value stay redacted and focused
PS7/PS5.1 tests pin native failure, success and credential staging contracts.
