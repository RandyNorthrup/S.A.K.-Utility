- [x] [HIGH] `allowed()` returned true while approval was still required.
  RESOLVED 2026-09-02: allowed now requires evaluated, unblocked, and no pending approval. The
  command-guard mutation catalog pins all result-state combinations.
- [x] [MEDIUM] Public mutable fields allowed callers to fabricate contradictory guard results.
  RESOLVED 2026-09-02: result state is private and construction-bound to guard evaluation; callers
  receive accessors only.

Exact current-blob re-review 2026-09-03: NO FINDINGS.
