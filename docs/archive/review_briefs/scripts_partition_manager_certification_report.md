# Recorded certification-report selection review

Status: COMPLETE for the report-selection implementation. Archived after the September 19,
2026 full Release build and 281/281 CTest gate passed on the current code. Evidence:
build/wave36-full-build.log and build/wave36-full-ctest.log.

Scope: the complete new `scripts/partition_manager_certification_report.ps1` helper; only the
selector/import/call sites in the verifier, status tool, strict VHD wrapper, and strict hardware
wrapper; and the new fixture coverage in the existing certification-tool self-test. No whole-file
review claim is made for the four callers or the rest of the self-test script.

- [x] [HIGH] Status and both strict wrappers ordered reports by LastWriteTimeUtc while the verifier
  used recorded run time. Reformatting or copying an old report could therefore select stale
  evidence for a claim. RESOLVED: all four import one selector and retain their own validation.
  The shared selector orders completed_utc, or started_utc when completion is absent/null/empty.
  Missing, unreadable, or malformed time refuses selection; there is no mtime selection path.
- [x] [MEDIUM] PowerShell 7 converts ISO JSON timestamps to DateTime. Parsing that value's display
  string again lost its local/UTC kind and shifted offset-bearing records. RESOLVED: parsed
  DateTime/DateTimeOffset values preserve their zone during UTC conversion; strings use explicit
  invariant UTC parsing. The old parser turned the fixture's 02:00 UTC into the previous day's
  18:00 UTC on this Windows host; the zone-preserving comparison now selects the right run.
- [x] [MEDIUM] A zero/false completed_utc value was treated as absent and hidden by started_utc.
  RESOLVED: only absent/null/empty completion permits the start-time contract; a present value
  with an invalid type or unreadable content refuses. A dedicated regression went red before
  this correction and green after it.
- [x] [MEDIUM] Verifier/status prepended the project root to an already absolute CertificationRoot.
  RESOLVED: both pass the requested root directly while already operating from the project root,
  preserving relative roots and accepting absolute roots. Strict-wrapper workflow behavior is
  otherwise unchanged.

Verification on 2026-09-04: original status-tool behavior failed two independent fixture cases:
reversed mtimes chose the old run, and missing recorded time was accepted. Final focused fixture
tests pass under Windows PowerShell 5.1 and PowerShell 7. They exercise both real report-consuming
entrypoints and the exact source-derived selector calls from all four callers, including completed
precedence, start-only reports, UTC offsets, relative/absolute roots, invalid JSON/types/times,
and an exclusively locked unreadable report. Strict VHD/hardware workflows are never executed.

Eight separate parse-valid mutants in a disposable script fixture failed their expected policy
checks; baseline and restored fixture runs passed. Production source was never mutated by those
drills. Exact failure logs and the reproducible fixture driver are under
`build/cert-report-selector-drill-ec78f857c5f848d1bb2943071448b9f5/`.
The registered `test_partition_manager_certification_tools` includes these cases by default;
`-ReportSelectionOnly` is the explicit focused route used here. Temporary report trees are cleaned
within their validated, newly created output directories. No real certification reports, matrix,
VM, disks, or live ledger state were changed.

Bounded final review found no further issue in this selection seam. Shared helper blob:
`d39d700db4087a1036574fb2742b6b66cbf6699e`; fixture-test blob:
`e532a88a90914f558cd75c2fbe5361460c155218`.
The later full integration gate satisfies the former integration requirement. This fixture proof
does not certify the application's disk operations or external hardware. Current boundaries remain
in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
