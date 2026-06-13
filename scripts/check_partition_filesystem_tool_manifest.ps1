<#
.SYNOPSIS
    Verifies the portable Partition Manager filesystem tool manifest.

.DESCRIPTION
    Static/non-mutating release gate. Non-native filesystem tools must remain
    blocked unless every bundled executable and companion runtime artifact has
    required metadata, a safe path, and a matching SHA-256 hash. Extra files
    under tools/filesystem are rejected unless listed in the manifest. The
    top-level _build directory is scratch-only and ignored by this gate.
#>

[CmdletBinding()]
param(
    [string]$ProjectRoot = "",
    [string]$ManifestPath = "tools\filesystem\manifest.json"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

function Assert-Condition {
    param(
        [Parameter(Mandatory = $true)]
        [bool]$Condition,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Resolve-ProjectPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return $Path
    }
    return Join-Path $ProjectRoot $Path
}

function Test-Sha256Hex {
    param(
        [string]$Value
    )

    return -not [string]::IsNullOrWhiteSpace($Value) -and $Value -match "^[a-fA-F0-9]{64}$"
}

function Assert-StringField {
    param(
        [object]$Object,
        [string]$Field,
        [string]$ToolId
    )

    $property = $Object.PSObject.Properties[$Field]
    Assert-Condition -Condition ($null -ne $property) -Message "Tool '$ToolId' missing '$Field'"
    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace([string]$property.Value)) `
        -Message "Tool '$ToolId' has blank '$Field'"
}

function Assert-StringArrayField {
    param(
        [object]$Object,
        [string]$Field,
        [string]$ToolId
    )

    $property = $Object.PSObject.Properties[$Field]
    Assert-Condition -Condition ($null -ne $property) -Message "Tool '$ToolId' missing '$Field'"
    $values = @($property.Value) | ForEach-Object { [string]$_ } | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_)
    }
    Assert-Condition -Condition ($values.Count -gt 0) `
        -Message "Tool '$ToolId' must list at least one '$Field' value"
}

function Assert-SafeRelativeToolPath {
    param(
        [string]$ToolsRoot,
        [string]$RelativePath,
        [string]$ToolId,
        [string]$Description = "binary"
    )

    Assert-Condition -Condition (-not [string]::IsNullOrWhiteSpace($RelativePath)) `
        -Message "Tool '$ToolId' has blank $Description relative_path"
    Assert-Condition -Condition (-not [System.IO.Path]::IsPathRooted($RelativePath)) `
        -Message "Tool '$ToolId' $Description relative_path must not be absolute"
    $parts = $RelativePath -split "[/\\]+" | Where-Object { $_ -ne "" }
    Assert-Condition -Condition (-not ($parts -contains "..")) `
        -Message "Tool '$ToolId' $Description relative_path must stay under tools root"

    $rootFull = [System.IO.Path]::GetFullPath($ToolsRoot)
    $binaryFull = [System.IO.Path]::GetFullPath((Join-Path $ToolsRoot $RelativePath))
    $rootPrefix = $rootFull.TrimEnd([System.IO.Path]::DirectorySeparatorChar) +
        [System.IO.Path]::DirectorySeparatorChar
    Assert-Condition -Condition ($binaryFull.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) `
        -Message "Tool '$ToolId' $Description path escapes tools root"
    Assert-Condition -Condition (Test-Path -LiteralPath $binaryFull -PathType Leaf) `
        -Message "Tool '$ToolId' $Description is missing: $RelativePath"
    return $binaryFull
}

$resolvedManifest = Resolve-ProjectPath $ManifestPath
Assert-Condition -Condition (Test-Path -LiteralPath $resolvedManifest -PathType Leaf) `
    -Message "Filesystem tool manifest missing: $ManifestPath"

$toolsRoot = Split-Path -Parent $resolvedManifest
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
Assert-Condition -Condition ($manifest.schema_version -eq 1) `
    -Message "Filesystem tool manifest schema_version must be 1"
Assert-Condition -Condition ($null -ne $manifest.PSObject.Properties["tools"]) `
    -Message "Filesystem tool manifest must contain a tools array"

$seenIds = @{}
$manifestedFilePaths = New-Object "System.Collections.Generic.HashSet[string]" -ArgumentList @(
    [StringComparer]::OrdinalIgnoreCase
)
$tools = @($manifest.tools)
foreach ($tool in $tools) {
    Assert-StringField -Object $tool -Field "id" -ToolId "<unknown>"
    $toolId = [string]$tool.id
    Assert-Condition -Condition (-not $seenIds.ContainsKey($toolId)) `
        -Message "Duplicate filesystem tool id: $toolId"
    $seenIds[$toolId] = $true

    foreach ($field in @(
            "display_name",
            "version",
            "upstream_url",
            "license",
            "source_archive_sha256",
            "relative_path",
            "binary_sha256")) {
        Assert-StringField -Object $tool -Field $field -ToolId $toolId
    }
    Assert-Condition -Condition (Test-Sha256Hex ([string]$tool.source_archive_sha256)) `
        -Message "Tool '$toolId' source_archive_sha256 is not a SHA-256 hex digest"
    Assert-Condition -Condition (Test-Sha256Hex ([string]$tool.binary_sha256)) `
        -Message "Tool '$toolId' binary_sha256 is not a SHA-256 hex digest"
    Assert-StringArrayField -Object $tool -Field "file_systems" -ToolId $toolId
    Assert-StringArrayField -Object $tool -Field "operations" -ToolId $toolId

    $binaryPath = Assert-SafeRelativeToolPath -ToolsRoot $toolsRoot `
        -RelativePath ([string]$tool.relative_path) -ToolId $toolId
    $actualHash = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash
    Assert-Condition -Condition ($actualHash.Equals([string]$tool.binary_sha256,
            [StringComparison]::OrdinalIgnoreCase)) `
        -Message "Tool '$toolId' binary hash mismatch"
    [void]$manifestedFilePaths.Add([System.IO.Path]::GetFullPath($binaryPath))

    if ($tool.PSObject.Properties["runtime_files"]) {
        foreach ($runtimeFile in @($tool.runtime_files)) {
            Assert-StringField -Object $runtimeFile -Field "relative_path" -ToolId $toolId
            Assert-StringField -Object $runtimeFile -Field "sha256" -ToolId $toolId
            Assert-Condition -Condition (Test-Sha256Hex ([string]$runtimeFile.sha256)) `
                -Message "Tool '$toolId' runtime file sha256 is not a SHA-256 hex digest: $($runtimeFile.relative_path)"
            $runtimePath = Assert-SafeRelativeToolPath -ToolsRoot $toolsRoot `
                -RelativePath ([string]$runtimeFile.relative_path) `
                -ToolId $toolId `
                -Description "runtime file"
            $runtimeHash = (Get-FileHash -LiteralPath $runtimePath -Algorithm SHA256).Hash
            Assert-Condition -Condition ($runtimeHash.Equals([string]$runtimeFile.sha256,
                    [StringComparison]::OrdinalIgnoreCase)) `
                -Message "Tool '$toolId' runtime file hash mismatch: $($runtimeFile.relative_path)"
            [void]$manifestedFilePaths.Add([System.IO.Path]::GetFullPath($runtimePath))
        }
    }
}

$allowedRootFiles = @("manifest.json", "README.md")
$unmanifestedFiles = @()
$toolsRootFull = [System.IO.Path]::GetFullPath($toolsRoot).TrimEnd(
    [System.IO.Path]::DirectorySeparatorChar,
    [System.IO.Path]::AltDirectorySeparatorChar
) + [System.IO.Path]::DirectorySeparatorChar
Get-ChildItem -LiteralPath $toolsRoot -Recurse -File -ErrorAction SilentlyContinue | ForEach-Object {
    $fullPath = [System.IO.Path]::GetFullPath($_.FullName)
    $relativePath = $fullPath.Substring($toolsRootFull.Length).Replace("\", "/")
    $isAllowedRootFile = ($relativePath -notmatch "/") -and ($allowedRootFiles -contains $_.Name)
    $isBuildScratchFile = $relativePath.StartsWith("_build/", [StringComparison]::OrdinalIgnoreCase)
    if ((-not $isAllowedRootFile) -and
        (-not $isBuildScratchFile) -and
        (-not $manifestedFilePaths.Contains($fullPath))) {
        $unmanifestedFiles += $relativePath
    }
}
Assert-Condition -Condition ($unmanifestedFiles.Count -eq 0) `
    -Message "Unmanifested filesystem tool bundle files found: $($unmanifestedFiles -join ', ')"

Write-Host "Partition filesystem tool manifest passed: $($tools.Count) tool(s)."
