- [x] [HIGH] Metadata callers issued separate GET requests without one shared enforcement point.
  IMPLEMENTED 2026-09-04: NetworkMetadataRequest and startNetworkMetadataGet centralize the ISO
  metadata/checksum, distribution-catalog, and UUP metadata paths. A caller-owned manager remains
  the reply owner; no fallback manager exists. Requests require HTTPS, TLS 1.2 or later,
  non-downgrading redirects, a positive total deadline, and a positive response cap.
- [x] [HIGH] Metadata completion needed consistent HTTP, timeout, cancellation, and size authority.
  IMPLEMENTED 2026-09-04: takeNetworkMetadataResult requires HTTP 2xx and no reply error, rejects
  oversized streamed or final bodies, clears rejected body data, and distinguishes a deadline or
  size abort from user cancellation. The reply read buffer applies backpressure at cap plus one.
- [x] [MEDIUM] Caller failure tests depended on external services or could miss asynchronous faults.
  IMPLEMENTED 2026-09-04: a scoped thread-local injector returns manager-owned replies that finish
  asynchronously. Caller tests cover offline, malformed, timeout, and positive metadata results.
  Direct tests cover invalid scheme/timeout/cap, HTTP boundaries, final and streamed overflow, and
  cancellation classification. Their wait is bounded; invalid-input tests inject a success result
  so a broken validation guard cannot escape into a real network request.
- [x] [MEDIUM] Native QNetworkReply::TimeoutError did not set timed_out unless the wrapper's own
  deadline property had also fired. RESOLVED 2026-09-04: classification now recognizes both native
  timeout errors and the wrapper deadline. A manager-owned fake transport reply tests the native
  error without the private property.
- [x] [HIGH] A reply could fail while readAll decoded its body, after the existing error check,
  and still publish partial metadata as success. RESOLVED 2026-09-04: completion checks reply
  errors again after reading, clears rejected body data, and leaves bytes_received zero. The direct
  transport test starts finished and error-free, then raises UnknownContentError from readData.
- [x] [MEDIUM] A negative HTTP status bypassed both the positive non-2xx and missing-status paths.
  RESOLVED 2026-09-04: every nonzero status outside 2xx now fails; zero retains its missing-status
  error. The direct boundary test requires status -1 to fail and return no body.

Current scoped static verification: clang-format and lizard pass; cppcheck reports no findings
with 183/186 checkers active across the six translation units checked in this finishing slice.
Initial focused verification 2026-09-04: the four-target Release build and CTest selection both pass
before and after mutation testing. test_uup_dump_api reports 39 passed, zero failed or skipped.
The registered network_metadata catalog kills all nine compile-valid mutants. Every mutant has
retained Qt assertion failures; there are no build-only, timeout, or skipped-test kills. Evidence:
build/mutation-runs/1788572986766676600-31568/. The source SHA-256 matches the pre-run bytes and the
mutation marker/snapshot are absent. The restored four-test CTest selection passes in 1.94 seconds.

Final metadata-contract verification 2026-09-04: all three new isolated mutations compile and fail
their own assertions, with 40 passed, one failed, and zero skipped for each run. Native timeout
fails the timed_out assertion; late read failure and negative status each fail their specific
success-refusal assertion. Retained evidence: build/mutation-runs/1788574106705851300-4368/.
The catalog now registers 12 mutants: the earlier nine plus these three new contract guards.
Existing anchors remained unchanged, so only the three added guards were rerun.

The final restored four-target Release build exits 0 and CTest passes 4/4 in 0.38 seconds. UUP has
41 passed with no failures or skips; the distribution catalog has 28 passed; network diagnostics
has 12 passed. ISO has 47 passed and its two existing opt-in live-download skips. No live-download
or whole-application certification is claimed by this focused proof. Source SHA-256 before and
after mutation matches 4FF5CD3E411B6E02A7CEC1438F6C6DBE93895EFE1553CB227B942A8F530F2458; marker and
snapshot are absent. The changed runner and direct-test translation units pass cppcheck with
183/186 active checkers; format, lizard, and magic-number checks pass without added suppressions.

Final scoped header/implementation contract review: NO FINDINGS after the three fixes. Reviewed
header blob: `9f1dd0c375cdf1e6fefc26630c96664565e99f70`; runner blob:
`98f95ff8625b1b130e38fee5a864bf2e8d7bc7b1`; direct-test blob:
`b0ba6ced6ac396a93c0e95f781e0e3cf3614edbb`.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
