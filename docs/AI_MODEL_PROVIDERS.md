# AI Assistant model providers

The AI Assistant can use four frontier model families, each either with an API
key or with the technician's own subscription.

| Provider | API key mode (vendor HTTP API) | Subscription sign-in mode (vendor runtime) |
|---|---|---|
| GPT (OpenAI) | OpenAI Responses API | Codex app-server, ChatGPT sign-in |
| Claude (Anthropic) | Anthropic Messages API | Claude Code, Claude sign-in (scoped, see below) |
| Gemini (Google) | Gemini API `generateContent` | Gemini CLI over ACP, Google sign-in (Workspace / Code Assist only) |
| Muse (Meta) | Meta Model API (Responses-compatible) | Muse Code `muse serve` (MSP), Meta sign-in |

The panel's **Provider** and **Sign-in** selectors choose the pair. The choice
is remembered (`QSettings` keys `ai/provider`, `ai/auth_mode`). Switching
provider starts a new conversation chain because response ids are vendor
specific.

## Design

```
AiAssistantPanel --> AiModelRouter (AiChatBackend) --> one backend per provider x mode
                                                     +- OpenAIApiBackend (OpenAI, Muse)
                                                     +- AnthropicApiBackend
                                                     +- GeminiApiBackend
                                                     +- AiAgentBackend
                                                         +- CodexAppServerBackend
                                                         +- ClaudeCodeBackend
                                                         +- GeminiCliBackend
                                                         +- MuseCodeBackend
```

* `AiChatBackend` keeps the exact signal contract of the original
  `OpenAIResponsesClient` (`responseReady`, function calls, function outputs,
  `previous_response_id` chaining), so the panel's tool loop, access modes,
  human gates, tracing and workflow runner are unchanged.
* Stateless APIs (Anthropic, Gemini) emulate `previous_response_id` with a
  bounded in-memory transcript cache (`ConversationHistoryCache`). Thinking
  blocks and Gemini thought signatures are echoed back verbatim as the vendors
  require. After an app restart the first message starts a fresh vendor-side
  context.
* Agent runtimes run their own agent loop with their own system prompt. S.A.K.
  adds only what the product is: its **tools** and **knowledge of how to use
  the app**, through the extension points every runtime supports:
  * Tools: a standard stdio MCP server named `sak`. The runtime launches
    `sak_ai_tool_bridge.exe`, which connects back to the running S.A.K. over a
    per-session local socket (user-only access, random token passed in the
    environment). A `tools/call` is handed to the panel as an ordinary function
    call, so access mode, human gates, elevation and the tool health ledger
    apply exactly as with API keys; the result goes back over MCP.
  * Knowledge: the panel's existing tools guide (`AiPromptAssembler`) is written
    to the runtime's project instructions file in the session workspace
    (`CLAUDE.md`, `AGENTS.md`, `GEMINI.md`), minus the tone line. S.A.K. does
    not change the runtime's own system prompt.
  * The runtime's own permission prompts (its shell, file edits) are shown in
    S.A.K. as Allow once / Allow for this chat / Deny. Access mode maps to a
    policy: Research denies, Assisted asks, Unattended allows.
* The session's artifacts folder is the runtime's working directory.
* Multi-agent workflows route every subagent to the selected provider; agent
  runtimes get a private scratch workspace per subagent and never prompt.

## Credentials and state

* API keys: DPAPI (current user) files `data/credentials/<provider>_api_key.dpapi.json`.
  The OpenAI file name and DPAPI entropy are unchanged from earlier releases.
* Subscription sign-in: each runtime keeps its own state in an isolated folder
  under the S.A.K. data root (`data/ai_agents/codex`, `claude`, `gemini`), set
  via `CODEX_HOME`, `CLAUDE_CONFIG_DIR` and `GEMINI_CLI_HOME`. Nothing is written
  to the customer's user profile. In portable mode that folder travels with the
  S.A.K. drive, so treat the drive like the technician's credentials. Signing
  out removes the runtime's login.
* Muse Code uses the technician's own installation and its own profile
  (`~/.config/muse`).
* API-key environment variables (`OPENAI_API_KEY`, `ANTHROPIC_*`,
  `GEMINI_API_KEY`, `META_API_KEY`, and others) are removed from runtime
  processes so the subscription sign-in is what gets billed.
* S.A.K. only checks sign-in state through each runtime's own status command or
  the presence of its credential file. It never reads, copies or relays tokens
  or sign-in codes.

## Vendor conditions

* **Claude (Anthropic).** Preinstalling Claude Code is allowed under Anthropic's
  published conditions: the binary is shipped unmodified, none of its
  authentication methods are restricted, each user signs in with their own
  plan or key through Claude Code's own flow, and S.A.K. does not pay for,
  resell or intermediate usage. Because Anthropic separately restricts
  third-party apps built on the Agent SDK from offering claude.ai login, the
  Claude subscription mode is kept to hosting Claude Code:
  * sign-in runs `claude auth login` in its own console window, and S.A.K. only
    reads `claude auth status`;
  * Claude Code keeps its own system prompt (no `--system-prompt` or
    `--append-system-prompt`; knowledge is a `CLAUDE.md` project file);
  * S.A.K. tools are a regular MCP server, with no SDK in-process tools or hook
    callbacks;
  * multi-agent workflows are not offered on a Claude subscription.

  Get written confirmation from Anthropic before a release enables Claude
  subscription mode. Claude API-key mode has none of these limits.
* **Gemini (Google).** Since 2026-06-18 Google serves Gemini CLI only for
  Workspace / Gemini Code Assist Standard and Enterprise accounts. Consumer
  Google AI plans are sent to the Antigravity CLI, which has no embeddable
  protocol yet. Google's terms forbid third-party reuse of Gemini CLI's OAuth,
  so S.A.K. drives the unmodified CLI over the Agent Client Protocol, as editor
  integrations do. Bundling is opt-in (`-IncludeGemini`; about 100 MB plus
  Node.js).
* **Muse (Meta).** The `muse` binary has no published redistribution grant, so
  it is not bundled. The Sign-in button offers Meta's official per-user
  installer. Meta states that a Muse subscription only works through the Muse
  Code CLI, which is exactly how S.A.K. uses it. `META_API_KEY` is removed so
  subscription billing is used.
* **GPT (OpenAI).** Codex is Apache-2.0. The app-server is marked experimental
  upstream, so the bundle pins one release (`scripts/bundle_ai_agents.ps1`).
  Re-test on every upgrade.

## Packaging

`scripts/bundle_ai_agents.ps1` (run in CI after the MCP bundle step) fills
`tools/ai_agents/` with pinned releases:

| Runtime | Source | Verification |
|---|---|---|
| Codex app-server package | GitHub release `rust-v<ver>` | release asset SHA-256 digest + Authenticode (OpenAI) |
| Claude Code `claude.exe` | `downloads.claude.ai/claude-code-releases/<ver>` | release `manifest.json` SHA-256 + Authenticode (Anthropic) |
| Node.js (Gemini only) | nodejs.org | `SHASUMS256.txt` + Authenticode |
| Gemini CLI bundle (optional) | npm `@google/gemini-cli` | npm sha512 integrity |

`tools/ai_agents/manifest.json` records the SHA-256 of every bundled file. The
folder is git-ignored except for its README. `sak_ai_tool_bridge.exe` is built
with the app and staged next to `sak_utility.exe`.

## Verification

* Unit tests: `test_ai_model_providers` (catalog, key store paths, history
  cache, Anthropic and Gemini request/response mapping, router) and
  `test_ai_agent_backends` (stdio framing, JSON-RPC routing, MCP bridge,
  runtime locator and environment scrubbing, per-runtime mapping, tool-call
  round trip, approval policy).
* Live protocol checks during development:
  * codex app-server 0.157: handshake, `model/list`, `account/read`, ChatGPT
    sign-in URL, thread start with the `sak` MCP server connected and all
    S.A.K. tools listed;
  * Gemini CLI 0.61 over ACP: handshake, `session/new` authentication gate;
  * Claude Code stream-json: `auth status`, launch flags, control handshake, a
    full turn.
* Muse Code is implemented from Meta's published MSP schema and the conventions
  of the Muse Spark Code VS Code extension. It still needs a live check on a
  machine with Muse Code installed.

## Manual QA checklist (Windows)

1. For each provider, API key mode: load a key, send a message, run a local
   tool in Assisted mode, clear the key.
2. GPT subscription: Sign In opens the ChatGPT page, the status shows the
   account email and plan, a message and a S.A.K. tool call work, Sign Out works.
3. Claude subscription: Sign In opens a Claude Code console window, the status
   updates after login, Claude Code's own shell prompt appears as an approval
   dialog, and a workflow start is refused with the Claude message.
4. Gemini subscription (build with `-IncludeGemini`): Workspace account
   sign-in; a consumer account shows the not-served error.
5. Muse subscription: Get Muse Code installs it per user, then Sign In runs
   `muse login`.
6. Switch access mode to Research: agent runtime permission prompts are denied
   automatically.
