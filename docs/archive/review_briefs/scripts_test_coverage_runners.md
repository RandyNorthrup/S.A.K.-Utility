- [x] [HIGH] Coverage-runner safety and evidence behavior previously had no registered executable
  regression suite.
  RESOLVED 2026-09-02: one hermetic fake-native-tool harness is registered under PowerShell 7 and
  Windows PowerShell 5.1. It launches a real fake child process and checks exact exits, evidence
  cleanup/publication, empty and missing outputs, filter retention, and BuildDir/output confinement.
- [x] [HIGH] Cross-host path parsing could pass one shell while missing a file/reparse parent in
  the other.
  RESOLVED 2026-09-02: both hosts run the same cases; the suite caught and drove the parent walk to
  Directory.GetParent. Eighteen independent old-guard mutants went red, including missing HTML.

Re-review result: zero open findings. Scratch paths are PID-scoped, cleanup is terminating, and no
suppression or silence is introduced.
