<#
.SYNOPSIS
    Deploys Qt's validated runtime plan without copying Windows system DLLs.
.DESCRIPTION
    Uses the selected windeployqt as the dependency/plugin planner. Its JSON target
    values name directories, not filenames. Validate all source/destination paths
    before copying; retain the tool's debug/release detection and plugin layout.
    Only relocatable Qt builds are supported because a dry-run plan cannot encode
    the separate QtCore patching step required by a non-relocatable build.
    No existing build files are removed. Existing app-local OS shims are refused.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$WindeployqtPath,
    [Parameter(Mandatory = $true)] [string]$ApplicationPath,
    [switch]$PlanOnly
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows_runtime_dlls.ps1")
. (Join-Path $PSScriptRoot "process_capture.ps1")
$MaximumQtPlanBytes = 4MB
$MaximumQtPlanEntries = 4096
$QtPlannerTimeoutSeconds = 60

function Assert-QtDeployment {
    param([bool]$Condition, [string]$Message)
    if (-not $Condition) { throw "Qt deployment refused: $Message" }
}

function Get-QtDeploymentFullPath {
    param($Path)
    Assert-QtDeployment ($Path -is [string] -and $Path -match '^[A-Za-z]:[\\/]' -and
        $Path.Substring(2) -notmatch ':' -and $Path -notmatch '[\x00-\x1f]') "Expected a literal local drive-qualified path"
    $native = $Path.Replace('/', '\')
    $parts = $native.Substring(3).Split('\')
    Assert-QtDeployment ($parts -notcontains "." -and $parts -notcontains "..") "Path traversal is not allowed"
    foreach ($part in $parts) {
        Assert-QtDeployment (-not $part.EndsWith(".") -and -not $part.EndsWith(" ") -and
            $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -lt 0) "Ambiguous or invalid Windows path component"
    }
    $full = [IO.Path]::GetFullPath($native)
    Assert-QtDeployment ($native.TrimEnd('\') -ieq $full.TrimEnd('\')) "Noncanonical path is not allowed"
    if ($full.Length -eq 3) { return $full }
    return $full.TrimEnd('\')
}

function Test-QtDeploymentChildPath {
    param([string]$Path, [string]$Root, [switch]$AllowRoot)
    if ($AllowRoot -and $Path.Equals($Root, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    return $Path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}

function Assert-QtDeploymentDirectPath {
    param([string]$Path)
    $current = $Path
    for ($depth = 0; $depth -lt 128; $depth++) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            Assert-QtDeployment (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Reparse path is not allowed: $current"
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { return }
        $current = $parent.FullName
    }
    throw "Qt deployment refused: path ancestor bound exceeded"
}

function Get-QtDeploymentContext {
    param([string]$ToolPath, [string]$ExePath)
    $tool = Get-QtDeploymentFullPath $ToolPath
    $application = Get-QtDeploymentFullPath $ExePath
    Assert-QtDeployment (Test-Path -LiteralPath $tool -PathType Leaf) "windeployqt executable is missing"
    Assert-QtDeployment ([IO.Path]::GetFileName($tool) -ieq "windeployqt.exe") "Expected the explicitly selected windeployqt executable"
    Assert-QtDeployment (Test-Path -LiteralPath $application -PathType Leaf) "Application executable is missing"
    Assert-QtDeployment ([IO.Path]::GetExtension($application) -ieq ".exe") "Application must be a Windows executable"
    Assert-QtDeploymentDirectPath $tool
    Assert-QtDeploymentDirectPath $application
    $qtRoot = Split-Path -Parent (Split-Path -Parent $tool)
    $destination = Split-Path -Parent $application
    Assert-QtDeployment ($destination.Length -gt 3) "Refusing a drive-root deployment"
    Assert-QtDeployment (-not (Test-QtDeploymentChildPath $destination $qtRoot -AllowRoot)) "Deployment must not overwrite the Qt installation"
    $systemDirectory = Get-QtDeploymentFullPath ([Environment]::SystemDirectory)
    $windowsDirectory = Split-Path -Parent $systemDirectory
    Assert-QtDeployment (-not (Test-QtDeploymentChildPath $destination $windowsDirectory -AllowRoot)) "Deployment must not target the Windows directory"
    $featuresPath = Join-Path $qtRoot "mkspecs\qmodule.pri"
    Assert-QtDeployment (Test-Path -LiteralPath $featuresPath -PathType Leaf) "Qt feature metadata is missing"
    $features = Get-Content -LiteralPath $featuresPath -Raw -ErrorAction Stop
    Assert-QtDeployment ($features -match '(?m)^QT\.global_private\.enabled_features\s*=.*\brelocatable\b') "A relocatable Qt build is required; raw plans do not apply QtCore patches"
    return @{ tool = $tool; application = $application; qt_root = $qtRoot; destination = $destination; system_directory = $systemDirectory }
}

function Get-ValidatedQtDeploymentPlan {
    param([object]$Plan, [hashtable]$Context)
    Assert-QtDeployment ($Plan -is [pscustomobject] -and $Plan.files -is [Array]) "Expected windeployqt JSON with a files array"
    Assert-QtDeployment ($Plan.files.Count -gt 0 -and $Plan.files.Count -le $MaximumQtPlanEntries) "Qt plan entry count is invalid"
    $copies = [Collections.Generic.List[object]]::new()
    $system = [Collections.Generic.List[string]]::new()
    $targets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $Plan.files) {
        Assert-QtDeployment ($entry -is [pscustomobject] -and @($entry.PSObject.Properties).Count -eq 2) "Qt plan record must contain source and target only"
        $source = Get-QtDeploymentFullPath $entry.source
        $targetDirectory = Get-QtDeploymentFullPath $entry.target
        Assert-QtDeployment (Test-Path -LiteralPath $source -PathType Leaf) "Qt plan source is missing: $source"
        Assert-QtDeployment (Test-QtDeploymentChildPath $targetDirectory $Context.destination -AllowRoot) "Qt plan target escaped application directory"
        Assert-QtDeploymentDirectPath $source
        Assert-QtDeploymentDirectPath $targetDirectory
        if (Test-Path -LiteralPath $targetDirectory) {
            Assert-QtDeployment (Test-Path -LiteralPath $targetDirectory -PathType Container) "Qt plan target directory is a file"
        }
        $name = [IO.Path]::GetFileName($source)
        if (Test-IsSystemDll $name) {
            Assert-QtDeployment ((Split-Path -Parent $source).Equals($Context.system_directory, [StringComparison]::OrdinalIgnoreCase)) "OS-named Qt dependency did not resolve from the Windows system directory"
            $system.Add($name)
            continue
        }
        Assert-QtDeployment (Test-QtDeploymentChildPath $source $Context.qt_root) "Qt plan source escaped selected Qt installation"
        $target = Join-Path $targetDirectory $name
        Assert-QtDeploymentDirectPath $target
        Assert-QtDeployment ($targets.Add($target)) "Qt plan has duplicate destination files"
        Assert-QtDeployment (-not (Test-Path -LiteralPath $target -PathType Container)) "Qt target file is an existing directory"
        $copies.Add([pscustomobject]@{ source = $source; target_directory = $targetDirectory; target = $target })
    }
    $cores = @($copies | Where-Object { [IO.Path]::GetFileName($_.source) -imatch '^Qt6Core(d)?\.dll$' })
    Assert-QtDeployment ($cores.Count -eq 1) "Qt plan must deploy exactly one matching Core runtime"
    Assert-QtDeployment ($cores[0].target_directory.Equals($Context.destination, [StringComparison]::OrdinalIgnoreCase)) "Qt Core runtime must be deployed next to the application"
    return [pscustomobject]@{ files = @($copies.ToArray()); windows_provided = @($system.ToArray()) }
}

function Get-QtRuntimePlan {
    param([hashtable]$Context)
    $options = @{
        FilePath = $Context.tool
        Arguments = @("--dry-run", "--json", "--no-translations", "--no-compiler-runtime", $Context.application)
        WorkingDirectory = $Context.destination
        TimeoutSeconds = $QtPlannerTimeoutSeconds
    }
    $captured = Invoke-CapturedProcess @options
    if (-not [string]::IsNullOrWhiteSpace($captured.stderr)) { [Console]::Error.WriteLine($captured.stderr) }
    Assert-QtDeployment (-not $captured.timed_out -and $captured.exit_code -eq 0) "windeployqt planning failed or exceeded its deadline"
    Assert-QtDeployment ([Text.Encoding]::UTF8.GetByteCount($captured.stdout) -le $MaximumQtPlanBytes) "Qt plan exceeds its byte limit"
    $plan = $captured.stdout | ConvertFrom-Json -ErrorAction Stop
    return Get-ValidatedQtDeploymentPlan $plan $Context
}

function Copy-QtRuntimePlan {
    param([object]$Plan, [hashtable]$Context)
    Assert-NoPackageSystemDlls $Context.destination
    foreach ($directory in @($Plan.files.target_directory | Sort-Object -Unique)) {
        if (Test-Path -LiteralPath $directory -PathType Container) {
            Assert-NoPackageSystemDlls $directory
        }
    }
    foreach ($entry in $Plan.files) {
        Assert-QtDeploymentDirectPath $entry.source
        Assert-QtDeploymentDirectPath $entry.target_directory
        Assert-QtDeploymentDirectPath $entry.target
        New-Item -ItemType Directory -Path $entry.target_directory -Force -ErrorAction Stop | Out-Null
        Copy-Item -LiteralPath $entry.source -Destination $entry.target -Force -ErrorAction Stop
        $sourceHash = (Get-FileHash -LiteralPath $entry.source -Algorithm SHA256).Hash
        $targetHash = (Get-FileHash -LiteralPath $entry.target -Algorithm SHA256).Hash
        Assert-QtDeployment ($sourceHash -ceq $targetHash) "Copied Qt runtime hash mismatch"
        Write-Host "Deployed Qt runtime: $($entry.target)"
    }
    foreach ($directory in @($Plan.files.target_directory | Sort-Object -Unique)) {
        Assert-NoPackageSystemDlls $directory
    }
    foreach ($name in $Plan.windows_provided) {
        Write-Host "Windows provides $name; host OS file was not packaged."
    }
}

$context = Get-QtDeploymentContext $WindeployqtPath $ApplicationPath
$plan = Get-QtRuntimePlan $context
if ($PlanOnly) {
    $plan | ConvertTo-Json -Depth 6
    exit 0
}
Copy-QtRuntimePlan $plan $context
