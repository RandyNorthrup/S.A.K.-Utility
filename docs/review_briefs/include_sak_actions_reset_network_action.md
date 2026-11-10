- [x] [HIGH] Destructive reset steps could run without rollback evidence or report partial work as
  success.
  VERIFIED 2026-09-02: Winsock backup failure aborts before mutation; firewall reset runs only after
  verified export; per-step timeout/nonzero exit is retained; final verification must be nonempty,
  untruncated and successful. Reboot-required is set only after a successful Winsock reset.
- [x] [HIGH] Adapter restart could exit zero after a per-adapter nonterminating failure.
  VERIFIED 2026-09-02: the generated PowerShell uses ErrorAction Stop inside each adapter try/catch,
  aggregates failures and exits nonzero. Focused tests pin the exact ordering and failure truth
  table.

Re-review result: zero open findings. Header descriptions include every mutated subsystem, private
backup paths feed the final report, and every declaration has a live definition/caller.
