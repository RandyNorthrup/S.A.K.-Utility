# AI agent runtimes

This folder is filled only when `scripts/bundle_ai_agents.ps1` is run by hand
for a build; release CI does not bundle runtimes (one-click in-app downloads
are the planned delivery). Only this README is committed.

| Folder    | Runtime                                   | Used by                          |
|-----------|-------------------------------------------|----------------------------------|
| `codex/`  | OpenAI Codex app-server package           | GPT with a ChatGPT plan          |
| `claude/` | Claude Code native executable             | Claude with a Claude plan        |
| `gemini/` | Gemini CLI bundle + private Node.js       | Gemini (Workspace / Code Assist) |

Every file is an unmodified vendor release, verified against the vendor's
published checksum and Authenticode signature; `manifest.json` records the
SHA-256 of each bundled file.

Muse Code is not bundled (no published redistribution grant). S.A.K. uses a
copy installed with Meta's official per-user installer, found under
`%LOCALAPPDATA%\Programs\muse`, or a `muse.exe` placed in `muse/` here.

Each runtime keeps its sign-in state in its own folder under the S.A.K. data
root (`data/ai_agents/<runtime>`), never in the customer's user profile.
