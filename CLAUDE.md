# CLAUDE.md

**Read [AGENTS.md](AGENTS.md) now, before starting work.** It is the agent-facing entry point for
this repository and applies to Claude Code exactly as it does to any other agent: the
non-negotiable owner rulings, the gate every code commit must pass, break-every-fix drill
discipline, the ways a green run has meant nothing here, where authoritative status lives, and
the pre-push secret scan.

Then, if `AGENTS.local.md` exists at the repo root, read it too. It is gitignored and holds this
machine's certification rigs and the owner's working preferences. Its absence elsewhere is normal.

Everything an agent needs is in the repository. Nothing here depends on a per-tool memory or
scratchpad -- those are not versioned, not reviewable, not shared between agents, and do not
survive the machine. If you learn something durable, put it in `AGENTS.md` (project convention),
`AGENTS.local.md` (this machine), or the relevant file under `docs/` -- never in a private note.

Nothing else is restated here on purpose: two copies of one rule means one of them goes stale,
and whichever an agent happens to read becomes the rule it follows.
