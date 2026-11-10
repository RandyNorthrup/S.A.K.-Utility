- [x] [HIGH] VM password staging and VBox native-capture security had no registered cross-host
  regression suite.
  RESOLVED 2026-09-02: the hermetic harness is registered under PowerShell 7 and Windows
  PowerShell 5.1. It drives real ACL application, fake failing native tools, before/after-write
  checkpoints, cleanup failure, reuse bounds, lossless-ASCII refusal, overwrite/create races and
  junction refusal.
- [x] [HIGH] Host-specific PowerShell ACL behavior could make one runner green and the other fail.
  RESOLVED 2026-09-02: ACL inspection uses the direct .NET FileSecurity API and the identical suite
  passes under both registered hosts.

Re-review result: zero open findings. Scratch paths are PID-scoped, environment cleanup is
terminating, and no plaintext test sentinel survives a successful run.
