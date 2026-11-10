param(
    [Parameter(Mandatory = $true)]
    [string]$CertifierPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

# Self-test for the live certifier's destructive-APFS size gate: the gate must
# refuse a destructive APFS target without an in-band --target-size, refuse an
# out-of-band size, and accept either an in-band size or the explicit
# --allow-foreign-apfs-destructive opt-in (which defers range-gating to the
# bridge's superblock-derived bounds). The two ACCEPTED cases really do run the
# destructive path past the gate -- they are safe only because the target path is
# proven not to exist below, so the run fails per-target instead of writing; the
# JSON reports under $OutputRoot are the only files this test produces.

function Fail([string]$Message) {
    throw "file_management_live_certifier gate self-test failed: $Message"
}

function Read-Report([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "missing report: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Test-HasProperty {
    param([object]$Object, [string]$Name)
    return @($Object.PSObject.Properties.Name) -contains $Name
}

# Clear a report path BEFORE the run that must produce it: otherwise a stale report
# from an earlier run (or a pre-seeded file) satisfies the assertions even when this
# invocation wrote nothing at all.
function Clear-Report([string]$Path) {
    Remove-Item -LiteralPath $Path -Force -Recurse -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $Path) {
        Fail "could not clear a stale report before the run: $Path"
    }
}

$script:LastCertifierOutput = ""

function Invoke-Certifier {
    param([string[]]$Arguments)
    $script:LastCertifierOutput = (& $CertifierPath @Arguments 2>&1 | Out-String).Trim()
    return $LASTEXITCODE
}

if (-not (Test-Path -LiteralPath $CertifierPath -PathType Leaf)) {
    Fail "certifier not found: $CertifierPath"
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$missingTarget = Join-Path $OutputRoot "certifier-gate-missing-target.img"
Remove-Item -LiteralPath $missingTarget -Force -Recurse -ErrorAction SilentlyContinue
# Fail closed on a deletion that did not take: the accepted cases below hand this path
# to --destructive, and every one of them is safe ONLY while the path does not exist.
if (Test-Path -LiteralPath $missingTarget) {
    Fail "destructive-gate target still exists and must not be handed to --destructive: $missingTarget"
}

# 1. Destructive APFS without --target-size and without the opt-in: parse
#    refusal (exit 2) naming the required argument.
$report1 = Join-Path $OutputRoot "gate-no-size.json"
Clear-Report $report1
$code = Invoke-Certifier @(
    "--output", $report1,
    "--target", "APFS=$missingTarget",
    "--destructive")
if ($code -ne 2) { Fail "no-size case expected exit 2, got $code. Output: $script:LastCertifierOutput" }
$json = Read-Report $report1
if (-not (Test-HasProperty $json "errors")) { Fail "no-size case report has no errors" }
if (-not (($json.errors -join "`n") -match "--target-size APFS=bytes is required")) {
    Fail "no-size case error text missing: $($json.errors -join '; ')"
}

# 2. Destructive APFS with an OUT-OF-BAND size (1 GiB, past the generated
#    one-spaceman-chunk 128 MiB bound) and no opt-in: parse refusal.
$report2 = Join-Path $OutputRoot "gate-oversize.json"
Clear-Report $report2
$code = Invoke-Certifier @(
    "--output", $report2,
    "--target", "APFS=$missingTarget",
    "--target-size", "APFS=1073741824",
    "--destructive")
if ($code -ne 2) { Fail "oversize case expected exit 2, got $code. Output: $script:LastCertifierOutput" }
$json = Read-Report $report2
if (-not (Test-HasProperty $json "errors")) { Fail "oversize case report has no errors" }
if (-not (($json.errors -join "`n") -match "one-spaceman-chunk")) {
    Fail "oversize case error text missing: $($json.errors -join '; ')"
}

# 3. Destructive APFS with an IN-BAND size (64 MiB): the gate passes; the run
#    then fails per-target on the nonexistent path (exit 1, targets report,
#    no parse errors).
$report3 = Join-Path $OutputRoot "gate-inband.json"
Clear-Report $report3
$code = Invoke-Certifier @(
    "--output", $report3,
    "--target", "APFS=$missingTarget",
    "--target-size", "APFS=67108864",
    "--destructive",
    "--worker-timeout-ms", "5000",
    "--max-depth", "1")
if ($code -ne 1) {
    Fail "in-band case expected exit 1 (run failure past the gate), got $code. Output: $script:LastCertifierOutput"
}
$json = Read-Report $report3
if (Test-HasProperty $json "errors") { Fail "in-band case unexpectedly reported parse errors" }
if (-not (Test-HasProperty $json "targets")) { Fail "in-band case has no per-target report" }
if (@($json.targets).Count -lt 1) { Fail "in-band case per-target report is empty" }

# 4. Destructive APFS with NO size but the explicit foreign opt-in: the gate
#    passes (the bridge range-gates from the superblock); run fails per-target.
$report4 = Join-Path $OutputRoot "gate-foreign-optin.json"
Clear-Report $report4
$code = Invoke-Certifier @(
    "--output", $report4,
    "--target", "APFS=$missingTarget",
    "--destructive",
    "--allow-foreign-apfs-destructive",
    "--worker-timeout-ms", "5000",
    "--max-depth", "1")
if ($code -ne 1) {
    Fail "opt-in case expected exit 1 (run failure past the gate), got $code. Output: $script:LastCertifierOutput"
}
$json = Read-Report $report4
if (Test-HasProperty $json "errors") { Fail "opt-in case unexpectedly reported parse errors" }
if (-not (Test-HasProperty $json "targets")) { Fail "opt-in case has no per-target report" }
if (@($json.targets).Count -lt 1) { Fail "opt-in case per-target report is empty" }

# 5. NON-destructive APFS without a size: the gate does not apply, and the run still
#    fails per-target on the nonexistent path (exit 1). Accepting anything-but-2 here
#    would pass on a crash, an internal error, or a bogus success.
$report5 = Join-Path $OutputRoot "gate-readonly.json"
Clear-Report $report5
$code = Invoke-Certifier @(
    "--output", $report5,
    "--target", "APFS=$missingTarget",
    "--worker-timeout-ms", "5000",
    "--max-depth", "1")
if ($code -eq 2) { Fail "read-only case must not hit the destructive gate" }
if ($code -ne 1) {
    Fail "read-only case expected exit 1 (missing target fails per-target), got $code. Output: $script:LastCertifierOutput"
}
$json = Read-Report $report5
if (Test-HasProperty $json "errors") { Fail "read-only case unexpectedly reported parse errors" }
if (-not (Test-HasProperty $json "targets")) { Fail "read-only case has no per-target report" }
if (@($json.targets).Count -lt 1) { Fail "read-only case per-target report is empty" }

Write-Output "file_management_live_certifier gate self-test passed."
exit 0
