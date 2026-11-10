- [x] [HIGH] Persistence read path was check-then-open and reparse-racy.
  RESOLVED 2026-09-01: the directory/file are opened without following reparse points; the opened
  file parent identity must match the pinned directory handle. A pre-open directory swap to a
  junction is refused.
- [x] [HIGH] Persistence save path was check-then-QSaveFile and reparse-racy.
  RESOLVED 2026-09-01: the destination directory is pinned without delete sharing for the whole
  temp write, flush and handle rename, so rename/junction replacement is blocked.
- [x] [HIGH] Pre-read size plus readAll allowed growth and short/truncated reads.
  RESOLVED 2026-09-01: handle reads are capped at 8 MiB + 1 and before size, bytes read and after
  size must agree exactly.
- [x] [HIGH] Save could emit more than its own 8 MiB load ceiling.
  RESOLVED 2026-09-01: the serialized payload is rejected before any file is created when it
  exceeds the loader ceiling.
- [x] [MEDIUM] Missing and unreadable ledgers were conflated through QFileInfo::exists.
  RESOLVED 2026-09-01: only FILE_NOT_FOUND/PATH_NOT_FOUND mean first run; inspect/open/read errors
  are distinct failures.
- [x] [MEDIUM] A forged disabled_until could suppress without causal failure history.
  RESOLVED 2026-09-01: a persisted disable requires a last failure, a threshold-reaching
  consecutive count, and a window between failure time and failure time + max backoff.
- [x] [LOW] Valid non-object JSON reported a parser no-error message.
  RESOLVED 2026-09-01: valid non-object JSON has an explicit expected-object diagnostic.
- [x] [LOW] QHash iteration made serialized record order nondeterministic.
  RESOLVED 2026-09-01: snapshot records are emitted in sorted normalized-key order.

Re-review result: zero open findings. Eight focused tests cover size races, both path-swap
boundaries, oversized save, forged disable, root shape and order. The ai_tool_health_ledger
mutation catalog kills 7/7 behavioral mutants.
