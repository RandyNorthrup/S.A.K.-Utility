# S.A.K. Utility Security Threat Model

## Scope

This threat model covers the portable Windows application, elevated helper,
AI assistant, MCP provider gateway, bundled tools, package manager flows, and
release packaging.

## Assets

- Local machine integrity, including elevated operations.
- User files, backups, exported reports, and diagnostic artifacts.
- OpenAI API keys and any future provider credentials.
- Tool results, command transcripts, screenshots, and replay traces.
- Release artifacts and bundled third-party binaries.

## Trust Boundaries

- Standard-user UI process to elevated helper.
- AI model output to local tool execution.
- MCP provider boundary to local Win32 automation.
- Package metadata/downloads to local installer execution.
- Remote documentation providers to prompt context.
- Build system to portable release artifact.

## Required Controls

- Elevated operations must go through explicit brokered tasks or approval gates.
- AI tools must enforce policy before execution, including installed-before-install.
- Package checksum mismatch must never be bypassed by the assistant.
- MCP providers must be manifest-defined and tool allowlisted at runtime.
- App-control actions must be manifest-gated and only enabled for tested workflows.
- Secrets must be redacted before logs, traces, reports, and replay artifacts.
- Runtime health/backoff must be visible so failed tools do not retry endlessly.
- Portable packages must not contain local provider overrides or mutable runtime data.
- Release binaries must be Authenticode signed before publication.

## High-Risk Flows

| Flow | Primary Risk | Required Mitigation |
|---|---|---|
| PowerShell/cmd execution | Arbitrary local modification | command guard, access mode, approval gates, trace |
| Package install | Unwanted install or checksum bypass | explicit install intent, installed check, checksum hard fail |
| Elevated helper | Privilege escalation | narrow task protocol, input validation, signature verification |
| MCP Win32 automation | UI control outside intended app | provider allowlist, app manifests, availability preflight |
| Remote docs MCP | Prompt injection from docs | read-only provider, no API key, citations/context separation |
| Replay trace | Secret leakage | strict redaction and max-size caps |
| Portable packaging | Dev path or local state leak | release smoke, secret scan, package mutable-data guard |

## Release Gate

Before release, run:

```powershell
cmake --build build --config Release --target sak_utility --parallel
ctest --test-dir build -C Release --output-on-failure
$version = (Get-Content VERSION -Raw).Trim()
$packageName = "SAK-Utility-v$version"
powershell -ExecutionPolicy Bypass -File scripts/stage_portable_release.ps1 -BuildDir build\Release -PackageName $packageName
powershell -ExecutionPolicy Bypass -File scripts/create_release_archive.ps1 -BuildDir build\Release -PackageName $packageName
$extract = "build\Release\clean-readiness-extract"
Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $extract | Out-Null
Expand-Archive -LiteralPath "build\Release\$packageName-Windows-x64.zip" -DestinationPath $extract -Force
powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 -PackageRoot $extract
```

For signed release candidates, use:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/check_release_readiness.ps1 `
  -PackageRoot $extract `
  -RequireSignedPackage
```
