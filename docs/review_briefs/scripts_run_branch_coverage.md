- [x] [HIGH] Instrumented CTest, llvm-profdata and llvm-cov failures did not stop final branch
  reports from being written, and old reports survived a failed rerun.
  RESOLVED 2026-09-02: every native process is captured with an exact exit; stale/final/temporary
  outputs are removed first and on failure; summary and missed-branch reports publish only after
  CTest, merge, report and show all succeed with nonempty output.
- [x] [CRITICAL] Profile/report cleanup was not confined to a verified output tree.
  RESOLVED 2026-09-02: OutDir is a strict descendant of the direct non-reparse BuildDir; existing
  components cannot be files or reparse points. The first-party ignore filter is retained exactly.

Re-review result: zero open findings. Registered PS7/PS5.1 fake-native-tool tests cover every
failure stage, stale evidence, safe path policy, atomic publication and a positive branch report.
