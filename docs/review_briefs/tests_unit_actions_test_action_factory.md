- [x] [MEDIUM] The exact Check Disk Errors description still pinned the retired CHKDSK claim after
  the action contract was corrected to the PowerShell Repair-Volume online scan it actually runs.
  RESOLVED 2026-09-02: updated the exact metadata expectation. The old value failed this test
  before the test change; the corrected value passes with the rebuilt action.
- [x] [HIGH] A new, missing, duplicated, miscategorized or wrongly elevated quick action could
  escape a shallow metadata smoke test.
  VERIFIED 2026-09-02: the suite pins the exact seven-action catalog, unique names, full
  descriptions, category distribution, elevation policy, clean initial state and all cancel-state
  outcomes.

Re-review result: zero open findings. Every assertion has a deterministic local fixture and no
skip or host dependency.
