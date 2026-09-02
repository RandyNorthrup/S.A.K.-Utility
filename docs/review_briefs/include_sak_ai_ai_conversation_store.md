- [x] [CRITICAL] Session/artifact paths, malformed stores, concurrent readers, partial writes or
  junction/leaf replacement could expose data outside the active session or certify incomplete
  persistence.
  VERIFIED 2026-09-02: bounded strict loaders, blob/session identity, recursive read/write locking,
  retained directory pins, create-new temporary leaves and atomic no-replace publish fail closed.
  Secret redaction occurs at persistence boundaries; no derived-index/copy/merge fallback remains.

Re-review result: zero open findings. Wave 22 closed the final managed-artifact boundary; every
declaration/member has live source/test use and adversarial race coverage.
