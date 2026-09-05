- [x] [MEDIUM] Non-positive candidate_limit silently became one and exposed a candidate.
  RESOLVED 2026-09-03: zero and negative limits fail closed with an explicit error and no candidate
  output.
- [x] [LOW] Candidate source JSON duplicated canonical fields and could preserve stale cleared data.
  RESOLVED 2026-09-03: canonical candidate fields are the single serialization authority; the
  duplicate source member is removed.
- [x] [LOW] Normalization contract overstated ambiguity and did not describe exact raw selection.
  RESOLVED 2026-09-03: lossy keys are private display labels only; selection requires an exact
  case-insensitive raw ID or title, including collision cases.

Exact post-fix re-review 2026-09-03: NO FINDINGS. The focused catalog kills 10/10 mutants.
