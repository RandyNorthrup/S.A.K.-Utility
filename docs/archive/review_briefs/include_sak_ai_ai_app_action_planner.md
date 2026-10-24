- [x] [HIGH] AiAppActionPlan::ok accepted a powershell/cli plan whose only body was GUI steps, and
  a win32_gui plan whose only body was a shell command.
  RESOLVED 2026-09-02: the structural predicate now binds the required body to the exact supported
  method. Separate old-code drills for the shell and GUI mismatch both failed before the fix.
- [x] [HIGH] Custom output-budget Options with nonpositive, inverted, over-broker or out-of-range
  default values were silently reordered/clamped into a plan, or produced a plan the broker later
  refused.
  RESOLVED 2026-09-02: invalid planner configuration fails closed with one explicit error before
  plan construction. Four isolated old-code drills failed and the valid clamp contract stays green.
- [x] [CRITICAL] A destructive/obfuscated manifest command or malformed GUI recipe could reach an
  executor without the required human/policy treatment.
  VERIFIED 2026-09-02: guard block/approval reasons, catastrophic/risky classification, exact GUI
  step structure and required evidence are covered by focused negative and positive tests.

Re-review result: zero open header findings. Every field is consumed by planner/executor/tests and
the default Options invariants are compile-time checked.
