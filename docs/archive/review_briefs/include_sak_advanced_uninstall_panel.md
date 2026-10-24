- [x] [LOW] m_leftover_header_label stored the builder's widget pointer but was never read after
  construction.
  RESOLVED 2026-09-02: removed the write-only member and assignment; Qt parent ownership retains
  the visible label.
- [x] [CRITICAL] A destructive batch or cleanup could silently act on a reduced set when a checked
  row no longer resolved to its backing object.
  VERIFIED 2026-09-02: selectedPrograms and selectedLeftovers expose an all_resolved result and the
  destructive handlers refuse ambiguous selection. Cleanup failures keep their rows visible and
  retryable.
- [x] [HIGH] Long enumeration/uninstall/cleanup work could leave the responsive UI without a stop
  path or accessible status.
  VERIFIED 2026-09-02: the cancel control remains enabled for every active state, routes to the
  cooperative controller cancellation seam, and every terminal path restores controls and emits
  status/progress. Table empty/loading states and named controls are retained.
- [x] [LOW] Dead/misplaced declarations retained unused QMenu and QSplitter includes and a
  QHBoxLayout forward declaration.
  RESOLVED 2026-09-03: removed all three from the header. The first app rebuild exposed that the
  source had relied transitively on QMenu, so its real use site now includes QMenu directly.
- [x] [LOW] Private state mixed snake_case and camelCase names within one class.
  RESOLVED 2026-09-03: all seven camelCase state members and every use now follow the repository's
  snake_case member convention.

Re-review result: zero open header findings. Every remaining widget/state member has live source
use and the declarations match the split dialog/table implementation.
