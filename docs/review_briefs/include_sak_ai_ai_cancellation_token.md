- [x] [CRITICAL] Root cancellation could miss a live grandchild after the intermediate token died,
  or race a concurrent child creation.
  VERIFIED 2026-09-02: child-to-parent ownership retains the ancestor chain; parent-to-child links
  are weak/pruned; one mutex guards child/id/reason/time state; cancellation publishes before the
  atomic flag and recurses without holding two locks. Stress and lifetime tests cover both races.
- [x] [MEDIUM] Empty IDs, repeated cancellation and children born after cancellation could yield
  ambiguous or inconsistent state.
  VERIFIED 2026-09-02: root/child defaults are deterministic and monotonic, the first cancellation
  wins, and late children inherit the exact reason/timestamp.

Re-review result: zero open findings. All state fields and public operations have live callers and
deterministic tests; invalid tokens remain explicit and inert.
