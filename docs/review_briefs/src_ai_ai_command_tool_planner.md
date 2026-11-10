- [x] [HIGH] Policy-only risky classification was dropped from the plan's human-gate risk bit.
  RESOLVED 2026-09-02: planner risk is the OR of command guard and policy classification.
- [x] [HIGH] Approval previews missed Unicode format classes and could collide with literal escape
  tokens or combining/private-use characters.
  RESOLVED 2026-09-02: category-based escaping covers controls, format/bidi, separators,
  combining marks, private/unassigned code points and surrogates; generated-token text is
  injectively disambiguated.
- [x] [HIGH] Direct-process command lines were bounded per field but not after Windows argv
  quoting and aggregation.
  RESOLVED 2026-09-02: the shared broker refuses a quoted aggregate over 30,000 characters before
  policy, lease or approval.
- [x] [HIGH] Fractional timeout_seconds values were truncated despite the integer schema.
  RESOLVED 2026-09-02: non-finite or fractional values are refused before conversion.
- [x] [MEDIUM] Process previews were sanitized twice and visible shell quotes were escaped.
  RESOLVED 2026-09-02: process argv is rendered once; whole shell commands preserve visible
  quotes while display-affecting characters remain explicit.
- [x] [HIGH] Approval-dialog credential redaction could hide executable shell substitution inside
  a quoted assignment.
  RESOLVED 2026-09-02: the accessible, left-to-right approval box shows the exact display-safe
  command; persisted/log sinks retain independent credential redaction.

Re-review result: zero open findings. Eight independent old-code-red drills passed after restore.
The command-planner mutation catalog kills 6/6 mutants; focused planner and real-modal tests pass.
