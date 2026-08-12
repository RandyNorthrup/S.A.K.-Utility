# Line-Coverage Baseline (R5-G14-16)

CODEX_REVIEW_5 MEASURED GAP 3 was that the suite had no coverage measurement of any
kind, so "N tests pass" carried an unknown denominator. This is the first real
measurement, produced with OpenCppCoverage 0.9.9.0 over a RelWithDebInfo build.

Reproduce it with:

```
cmake --build build --config RelWithDebInfo --target <core test targets>
scripts/run_coverage.ps1 -TestRegex '<regex>' -OutDir build/coverage
```

The script needs OpenCppCoverage (choco install opencppcoverage) and a RelWithDebInfo
build, because the default Release build strips the PDBs coverage maps against. It is
NOT a pre-commit hook: an instrumented run is far too slow for every commit. The CI
workflow carries an opt-in "coverage" job (workflow_dispatch) that installs the tool,
builds the core tests, runs this script, and uploads the HTML report.

## Scope of this baseline

Measured over a CURATED CORE SET of tests, not the full 235-test suite: the parsers and
the security boundary (PST, MBOX, ISO, the AI tool policy, the MIME/HTML/framing seams,
encryption, input validation, path utilities, crash reporter). The per-subsystem
aggregate below is therefore pessimistic for src/core: these exes statically link many
core files (via logger, error_codes, email_types) that this set does not exercise, and
every such line counts against the total. The per-file table is the sharper signal.

Measured 2026-08-12.

## Per subsystem (curated core set)

| Subsystem      | Covered / Total | Line rate |
|----------------|-----------------|-----------|
| src\ai         | 624 / 702       | 88.89%    |
| include\sak    | 448 / 515       | 86.99%    |
| src\win32mcp   | 34 / 34         | 100.00%   |
| src\core       | 2657 / 6536     | 40.65%    |
| TOTAL measured | 3763 / 7787     | 48.32%    |

## Per file (the directly-tested files)

| File                      | Covered / Total | Rate   |
|---------------------------|-----------------|--------|
| email_html_sanitizer.h    | 19 / 19         | 100.0% |
| mbox_header_parser.h      | 32 / 32         | 100.0% |
| mbox_transfer_decoder.h   | 34 / 34         | 100.0% |
| native_messaging.cpp      | 34 / 34         | 100.0% |
| mbox_parser.cpp           | 387 / 431       | 89.8%  |
| ai_mcp_jsonrpc.h          | 25 / 28         | 89.3%  |
| ai_tool_policy.cpp        | 312 / 351       | 88.9%  |
| iso_analyzer.cpp          | 209 / 257       | 81.3%  |
| input_validator.cpp       | 262 / 337       | 77.7%  |
| encryption.cpp            | 288 / 385       | 74.8%  |
| path_utils.cpp            | 107 / 157       | 68.2%  |
| pst_parser.cpp            | 470 / 1534      | 30.6%  |
| crash_reporter.cpp        | 15 / 60         | 25.0%  |

## What the low numbers tell us (and the follow-ups they name)

- **pst_parser.cpp 30.6%.** The PST fuzz seeds fail closed at the header / CRC layer, so
  the deep BTree, LTP, and messaging code is never reached. This is the exact gap already
  noted for R5-G14-5: adding page-trailer-valid store seeds so accepted files exercise
  those layers is the next increment, and this number will move when it lands.
- **crash_reporter.cpp 25.0%.** Only the pure helpers are unit-tested; the actual
  SetUnhandledExceptionFilter / MiniDumpWriteDump path needs a real fault to exercise and
  is certified manually (see R5-G23-2). Coverage cannot reach it from a normal test.
- **The newly-fuzzed seams (sanitizer, mbox header, mbox transfer decoder, native
  messaging) are at 100%.** Extracting a parser into a pure seam and fuzzing it drives its
  lines fully, which is the coverage case for continuing the seam program.

## Not yet done (tracked, not claimed)

This is a baseline, not a target. R5-G14-16a/b (enforce 100% line AND branch coverage on
all testable code) remain a multi-week program: it needs the full suite measured, the
exclusion inventory built (R5-G14-16c: every excluded file/function named with a reason),
and a gate wired (R5-G14-16d). Branch coverage is not reported here; OpenCppCoverage
measures line coverage only, so branch enforcement needs an additional tool decision.
