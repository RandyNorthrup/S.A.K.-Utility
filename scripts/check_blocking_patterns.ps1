param(
    [string]$Root = "."
)

$ErrorActionPreference = "Stop"

function Invoke-Rg([string[]]$RgArgs) {
    $rg = Get-Command rg -ErrorAction SilentlyContinue
    if (-not $rg) {
        return Invoke-SelectStringFallback -RgArgs $RgArgs
    }

    $output = & rg @RgArgs 2>$null
    $code = $LASTEXITCODE
    if ($code -gt 1) {
        throw "rg failed with exit code ${code}: rg $($RgArgs -join ' ')"
    }
    return @($output)
}

function Invoke-SelectStringFallback([string[]]$RgArgs) {
    $search_args = @($RgArgs | Where-Object { $_ -ne "--line-number" -and $_ -ne "--no-heading" })
    if ($search_args.Count -lt 2) {
        throw "Invalid fallback search arguments: $($RgArgs -join ' ')"
    }

    $pattern = $search_args[0]
    $roots = @($search_args | Select-Object -Skip 1)
    $files = foreach ($searchRoot in $roots) {
        if (Test-Path -LiteralPath $searchRoot) {
            Get-ChildItem -LiteralPath $searchRoot -Recurse -File -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.Extension -in @(".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx")
                }
        }
    }

    if (-not $files) {
        return @()
    }

    $rootPath = (Get-Location).Path.TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    $matches = Select-String -Path $files.FullName -Pattern $pattern -AllMatches -ErrorAction SilentlyContinue
    return @($matches | ForEach-Object {
            $fullPath = $_.Path
            $relative = $fullPath
            if ($fullPath.StartsWith($rootPath, [System.StringComparison]::OrdinalIgnoreCase)) {
                $relative = $fullPath.Substring($rootPath.Length).TrimStart(
                    [System.IO.Path]::DirectorySeparatorChar,
                    [System.IO.Path]::AltDirectorySeparatorChar)
            }
            "{0}:{1}:{2}" -f ($relative -replace '\\', '/'), $_.LineNumber, $_.Line.Trim()
        })
}

$repo = (Resolve-Path -LiteralPath $Root).Path
Push-Location $repo
try {
    $forbidden = Invoke-Rg -RgArgs @(
        "--line-number",
        "--no-heading",
        "QEventLoop|loop\.exec\(|processEvents",
        "src",
        "include"
    )
    if ($forbidden.Count -gt 0) {
        Write-Error "Forbidden nested event-loop/UI pumping pattern found:`n$($forbidden -join "`n")"
        exit 1
    }

    $waitMatches = Invoke-Rg -RgArgs @(
        "--line-number",
        "--no-heading",
        "waitFor|\bwait\(",
        "src",
        "include"
    )

    $allowed = @(
        '^src[\\/]threading[\\/]worker_base\.cpp:\d+:',
        '^src[\\/]core[\\/]process_runner\.cpp:\d+:',
        '^src[\\/]core[\\/]elevated_pipe_server\.cpp:\d+:',
        '^src[\\/]core[\\/]elevation_broker\.cpp:\d+:',
        '^src[\\/]core[\\/]elevation_manager\.cpp:\d+:',
        '^src[\\/]core[\\/]app_installation_worker\.cpp:\d+:',
        '^src[\\/]core[\\/]diagnostic_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]thermal_monitor\.cpp:\d+:',
        '^src[\\/]core[\\/]offline_deployment_worker\.cpp:\d+:',
        '^src[\\/]core[\\/]advanced_search_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]advanced_uninstall_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]flash_coordinator\.cpp:\d+:',
        '^src[\\/]core[\\/]ost_converter_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]quick_action_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]user_profile_backup_worker\.cpp:\d+:',
        '^src[\\/]core[\\/]user_profile_restore_worker\.cpp:\d+:',
        '^src[\\/]core[\\/]network_transfer_runner\.cpp:\d+:',
        '^src[\\/]core[\\/]imap_uploader\.cpp:\d+:',
        '^src[\\/]core[\\/]network_diagnostic_controller\.cpp:\d+:',
        '^src[\\/]core[\\/]port_scanner\.cpp:\d+:',
        '^src[\\/]ai[\\/]ai_openai_model_client\.cpp:\d+:',
        '^src[\\/]ai[\\/]ai_mcp_http_client\.cpp:\d+:',
        '^src[\\/]ai[\\/]ai_mcp_stdio_client\.cpp:\d+:',
        '^src[\\/]gui[\\/]ai_assistant_panel\.cpp:\d+:',
        '^src[\\/]gui[\\/]organizer_panel\.cpp:\d+:'
    )

    $unexpected = @()
    foreach ($line in $waitMatches) {
        $isAllowed = $false
        foreach ($pattern in $allowed) {
            if ($line -match $pattern) {
                $isAllowed = $true
                break
            }
        }
        if (-not $isAllowed) {
            $unexpected += $line
        }
    }

    if ($unexpected.Count -gt 0) {
        Write-Error "Unexpected blocking wait pattern found:`n$($unexpected -join "`n")"
        exit 1
    }

    Write-Host "Blocking-pattern check passed."
}
finally {
    Pop-Location
}
