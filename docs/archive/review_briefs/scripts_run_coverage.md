- [x] [HIGH] A failed OpenCppCoverage/CTest run could leave or reuse coverage.xml/HTML and the
  script never checked the native exit, so stale evidence could be reported as current.
  RESOLVED 2026-09-02: known outputs are removed before the run; captured parent/child failure is
  fatal; both fresh Cobertura XML and HTML index are required; zero measured lines are refused; any
  failure removes partial evidence.
- [x] [CRITICAL] Recursive stale-output cleanup accepted an arbitrary output directory.
  RESOLVED 2026-09-02: OutDir must be a strict descendant of the selected direct, non-reparse
  BuildDir and every existing parent component must be a real directory. Root, outside, file-parent
  and junction paths fail before cleanup.

Re-review result: zero open findings. Hermetic PS7/PS5.1 tests pin stale, tool, child-test, empty,
missing-HTML, safe-path and positive-report behavior.
