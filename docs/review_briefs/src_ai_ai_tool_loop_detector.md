- [x] [HIGH] Oversized arguments were folded for the key but retained in full as worst arguments.
  RESOLVED 2026-09-01: retained state is capped at the 80-character preview plus one truncation
  sentinel character; loopingSummary renders the ellipsis.
- [x] [HIGH] Saturation cleared the whole signature map and erased repeat progress.
  RESOLVED 2026-09-01: a stale-aware singleton queue evicts one singleton first; a repeating key
  at 3/4 survives the 10001st distinct signature and trips on call four.
- [x] [MEDIUM] Tool names were unbounded model input.
  RESOLVED 2026-09-01: blank or over-256-character normalized names are refused and retain no
  detector state.
- [x] [MEDIUM] Unit-separator concatenation allowed two distinct field pairs to alias.
  RESOLVED 2026-09-01: signatures length-prefix the normalized tool name before the argument field.
- [x] [MEDIUM] Oversized arguments used qHash as their only identity.
  RESOLVED 2026-09-01: oversized arguments use a length plus full SHA-256 hex digest.
- [x] [LOW] Repeat counters could overflow signed int in the standalone class.
  RESOLVED 2026-09-01: increments saturate at INT_MAX.

Re-review result: zero open findings. Three regression tests failed against the old code. The
suite also pins the SHA-256 digest, bounded retained preview and distinct oversized arguments.
The ai_tool_loop_detector mutation catalog kills 5/5 mutants.
