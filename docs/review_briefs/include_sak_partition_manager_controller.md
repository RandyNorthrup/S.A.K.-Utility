- [x] [HIGH] Apply could rely on planning-time inventory and report success after failed verification.
  IMPLEMENTED 2026-09-04: explicit asynchronous Refresh, ApplyPreflight, and ApplyVerification scan
  purposes carry cancellation outcomes. Apply rechecks the reviewed queue revision, fingerprint,
  and layout hash, then replans and validates every operation against fresh inventory. An empty,
  incomplete, warning-bearing, or identity-less verification scan downgrades success and retains
  the queue. Reports are generated from the resulting verified or failed state.
- [x] [HIGH] Disk numbers and raw payload paths did not prove stable device identity across apply.
  IMPLEMENTED 2026-09-04: planned operations bind disk identity, raw payload target identity, and
  backup volume identity; fresh preflight refuses changed or unavailable bindings. Inventory
  includes the stable unique disk identity used by these checks.
- [x] [HIGH] A partially accepted multi-operation request could leave a misleading Quick Partition
  queue, and a reviewed queue could change before apply.
  IMPLEMENTED 2026-09-04: queueOperations plans the whole request against sequentially projected
  inventory, then atomically adds the batch within the bounded queue. applyQueue requires the
  captured PartitionQueueReviewToken and verifies it again after preflight.
- [x] [MEDIUM] Cancellation, progress, and terminal signal ordering could contradict completed work.
  IMPLEMENTED 2026-09-04: canCancel rejects already-finished futures; scan completion uses its own
  recorded cancellation outcome. Cancellation reaches inventory, executor, and filesystem-check
  workers. Terminal progress completes, completion signals remain interaction-blocked, watcher
  ownership is released, then the terminal state is published. Destruction requests cooperative
  cancellation and joins every owned future before releasing resources.
- [x] [MEDIUM] Empty queues and partial inventories could report the wrong stable state.
  IMPLEMENTED 2026-09-04: stable state derives from live queue and inventory authority; diagnostics
  distinguish missing disks, missing layout identity, failed partition enumeration, and warnings.
- [x] [HIGH] Fresh preflight discarded the sequential layout projection used when accepting a
  Quick Partition batch, so RAW -> Initialize GPT -> Create APFS was queued successfully but
  refused during Apply.
  RESOLVED 2026-09-04: preflight starts from fresh inventory and advances the same projection seam
  after each operation. Disk, raw target and backup identity checks remain bound to real fresh
  inventory. The existing batch regression now follows the accepted queue through injected Apply
  and requires both operations to complete successfully in dry-run mode.

The dedicated test_partition_manager_controller target and registered partition_manager_controller
mutation catalog cover these contracts. The earlier 19 controller guards passed; the new projection
guard and both adjusted existing projection/metadata anchors compiled and were killed on
2026-09-04, with no survivors. Removing the fix specifically failed the Apply regression with
`APFS create requires a GPT disk`. Exact build, CTest and Qt assertion logs are retained under
`build/mutation-runs/1788573827432297700-2880/`.

Scoped format, lizard, magic-number, catalog integrity and cppcheck passed; cppcheck reported
183/186 active checkers. Baseline and restored Release target builds passed. Final focused CTest
passed 1/1 in 0.79 seconds; XML is `artifacts/wave31-controller-projection-final-ctest.xml`. The
controller source hash matched its pre-mutation value and the mutation marker cleared.

The bounded exact-header review found no remaining issue after the projection fix.

Integrated local gate 2026-09-04: Release build exit 0; CTest 276/276 passed in 1052.17 seconds
(`build/wave31-full-ctest-final.xml`). Runtime skip audit passed with 27 reviewed skips. Remaining
work and certification boundaries are tracked in [R5](../CODEX_REVIEW_5_REMEDIATION.md).
This brief makes no VM, raw-disk execution, or hardware certification claim.
