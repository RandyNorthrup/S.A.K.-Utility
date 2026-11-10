- [x] [HIGH] A GUI search could inherit an unsafe link-following default and open a planted file
  symlink whose target is a UNC share.
  RESOLVED 2026-09-02: SearchConfig now skips file links by default, so buildSearchConfig inherits
  the same QDir::NoSymLinks guard already required by headless callers. The exact default-contract
  test failed against the old false value before the fix; the production filter helper test pins
  that the default reaches QDirIterator while an explicit opt-out remains possible.
- [x] [MEDIUM] A stale or partial result could be presented as a complete search.
  VERIFIED 2026-09-02: the panel receives the controller's complete flag, distinguishes INCOMPLETE
  in its terminal log, clears prior result/preview state at run start, and keeps Stop available
  while the worker is active.

Re-review result: zero open header findings. Every widget/state member has live source use; target,
result, preview and cancellation declarations match the current three-pane implementation.
