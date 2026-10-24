- [x] [MEDIUM] The public class documentation claimed HTML/msinfo32 and driver collection, while
  production writes an atomic UTF-8 text report from PowerShell and Qt OS/hardware/storage/network/
  volume collectors.
  RESOLVED 2026-09-02: corrected the class and scan descriptions to the actual shipped contract.
- [x] [LOW] m_report_path was never written or read.
  RESOLVED 2026-09-02: removed the dead member instead of retaining legacy state.
- [x] [HIGH] A saved report could be called complete after collector failure.
  VERIFIED 2026-09-02: timeout, nonzero/negative exit, blank output and truncated output record an
  incomplete collector; final success requires both atomic save and every collector. Focused tests
  pin collector truth tables, explicit unavailable-query text and on-disk UTF-8 byte counts.

Re-review result: zero open findings. Header declarations match the implementation and all members
have live callers.
