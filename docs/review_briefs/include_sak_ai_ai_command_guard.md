- [x] [CRITICAL] A default/unrun guard result or an approval-required command could be read as an
  allow decision.
  VERIFIED 2026-09-02: allowed requires evaluated plus no block; approval is mutually exclusive
  with a block; the combined evaluator is the only path that marks evaluated. Focused tests pin
  package-integrity bypasses, obfuscation, risky approvals and benign controls.

Re-review result: zero open findings. Every field/function has live planner/test consumers and the
header matches the fail-closed command-guard implementation.
