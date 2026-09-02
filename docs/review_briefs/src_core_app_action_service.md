- [x] [HIGH] Reset Network Settings and BitLocker Key Backup sent display names that did not match
  the elevated helper allowlist; BitLocker also lost its required destination payload.
  RESOLVED 2026-09-01: shared display/task constants and an exact fail-closed mapping now drive
  both controller and helper; unmapped admin actions are refused before helper launch.
- [x] [HIGH] File-writing actions were permanently constructed against an AppData fallback before
  any AI session existed.
  RESOLVED 2026-09-01: no-session output stays empty, every session transition retargets the live
  service to that session's artifact root, and a new workspace clears the destination.
- [x] [HIGH] Missing BitLocker provenance silently became C:/SAK_Backups.
  RESOLVED 2026-09-01: empty output produces no payload; the helper's existing required-field
  guard refuses the request.
- [x] [LOW] m_backup_dir was write-only and controller() had no caller.
  RESOLVED 2026-09-01: the member now owns the live retargetable output state; the dead accessor
  is removed. File-writing actions expose only their output retarget setters.

Re-review result: zero open findings. Three focused regressions failed against the old routing,
fallback and stale-root behavior. Elevation/output mutation catalogs kill 6/6 mutants.
