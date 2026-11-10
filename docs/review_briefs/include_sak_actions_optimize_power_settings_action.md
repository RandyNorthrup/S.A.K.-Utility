- [x] [HIGH] Power-plan discovery/parsing could mutate a guessed plan or fail on localized Windows.
  VERIFIED 2026-09-02: discovery failure/empty results refuse activation; list, active-plan and
  timeout parsing use GUID/structure rather than English labels. Built-in High/Ultimate decisions
  use canonical GUIDs, not display names.
- [x] [MEDIUM] A successful activation report could assert timeouts that were never measured.
  VERIFIED 2026-09-02: queryPowerPlan is wired, effective timeouts carry an explicit found flag,
  missing/partial blocks never borrow defaults, and zero seconds renders as Never. Focused tests
  cover real query output, translated labels, truncated blocks and exact formatting.

Re-review result: zero open findings. Every declaration has a definition/caller and the public pure
seams cover fail-closed discovery, identity and read-back behavior.
