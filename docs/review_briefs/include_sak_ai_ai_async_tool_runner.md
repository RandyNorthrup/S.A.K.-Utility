- [x] [LOW] The cancellation-token comment called the shared flag per-job and claimed later jobs
  did not disturb it, while start() deliberately lowers the same runner-owned token for reuse.
  RESOLVED 2026-09-02: documented the actual reusable-token lifecycle and rejected-start behavior.
- [x] [HIGH] Detached/throwing work could be mistaken for idle, deliver a stale result, or wedge
  busy state forever.
  VERIFIED 2026-09-02: one-at-a-time start, attached state, cooperative cancellation, exception
  backstop, finished-before-drained ordering and destructor join behavior have focused async tests.

Re-review result: zero open findings. Every member has live owning-thread use; cancellation and
late-result contracts match the panel integration.
