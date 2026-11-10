# Document-accuracy diagnostic scope

Status: COMPLETE for this bounded diagnostic fix. Archived after the September 19, 2026
full Release build and 281/281 CTest gate passed on the current code. Evidence:
build/wave36-full-build.log and build/wave36-full-ctest.log.

Scope: the failure-footer delta in `scripts/check_doc_accuracy.ps1` and the four executable
cases in `tests/unit/test_doc_accuracy.py`. This is not a full-file parser review.

- [x] [LOW] An R5-only tally mismatch told the operator to fix `tests/README.md`.
  RESOLVED locally: the gate records which document added violations and names exactly that set
  in its correction footer. Detailed violations, truthful counts, and nonzero refusal remain.

Proof on 2026-09-04: before the production fix, the new R5-only and combined assertions failed on
both Windows PowerShell 5.1 and PowerShell 7. After the fix, all four cases passed on both hosts.
Two separate mutations of the README and R5 scope guards, applied only to disposable fixture
copies, each failed its expected footer assertion. No live project source was mutated by those
drills. The registered `test_doc_accuracy` CTest passed in 5.80 seconds.

The later full integration gate satisfies the former Wave 32 gate requirement. This brief
carries no application or VM certification claim and does not close the broader R5 campaign.
