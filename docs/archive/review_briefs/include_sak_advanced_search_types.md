- [x] [HIGH] skip_symlinks defaulted false, so ordinary GUI/file-explorer callers followed
  symlinked file entries even though headless callers explicitly enabled the UNC/NTLM-leak guard.
  RESOLVED 2026-09-02: the safe default is true for every caller; an exceptional verified caller
  must opt out explicitly. The default test was changed first and failed against the old value;
  the worker's production filter helper now proves QDir::NoSymLinks is actually applied.
- [x] [LOW] SearchMatch documented context vectors as limited to 0-10 even though injected configs
  are clamped at kAdvancedSearchMaxContextLines (1,000).
  RESOLVED 2026-09-02: the field contract now cites the clamped configured bound instead of a false
  GUI-only ceiling.

Re-review result: zero open findings. Default, copy/move, nested target, exclusion set, extension
set and preference contracts have deterministic unit coverage.
