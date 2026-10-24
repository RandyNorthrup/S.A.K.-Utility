# Pure path checks shared by portable staging and its regression test.

$MaximumStageTreeEntries = 100000

function Get-StagePathItem([string]$Path) {
    try {
        return Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    }
    catch {
        if ($_.Exception -isnot [System.Management.Automation.ItemNotFoundException] -and
            $_.Exception -isnot [System.IO.FileNotFoundException] -and
            $_.Exception -isnot [System.IO.DirectoryNotFoundException]) {
            throw
        }
    }
    # Some providers report a dangling link as missing. Inspect its parent entry
    # before accepting absence; enumeration itself never follows the child.
    $fullPath = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetDirectoryName($fullPath.TrimEnd('\', '/'))
    if ([string]::IsNullOrEmpty($parent)) { return $null }
    try {
        $name = [IO.Path]::GetFileName($fullPath.TrimEnd('\', '/'))
        $candidateItems = @(Get-ChildItem -LiteralPath $parent -Force -ErrorAction Stop |
            Where-Object { $_.Name -ieq $name })
    }
    catch {
        if ($_.Exception -is [System.Management.Automation.ItemNotFoundException] -or
            $_.Exception -is [System.IO.DirectoryNotFoundException]) {
            return $null
        }
        throw
    }
    if ($candidateItems.Count -gt 1) { throw "Ambiguous stage path entry: $fullPath" }
    if ($candidateItems.Count -eq 1) { return $candidateItems[0] }
    return $null
}

function Assert-StageDirectDirectoryPath([string]$Path, [string]$Description, [bool]$RequireExisting) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $current = $fullPath
    for ($depth = 0; $depth -lt 128; $depth++) {
        $item = Get-StagePathItem $current
        if ($null -eq $item) {
            if ($depth -eq 0 -and $RequireExisting) {
                throw "$Description does not exist: $current"
            }
        } else {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description crosses a reparse point: $current"
            }
            if (-not $item.PSIsContainer) {
                throw "$Description has a non-directory ancestor: $current"
            }
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent) { return $fullPath }
        $current = $parent.FullName
    }
    throw "$Description path ancestor bound exceeded: $Path"
}

function Get-StageDirectTreeItem([string]$Root, [string]$Description) {
    $rootPath = Assert-StageDirectDirectoryPath -Path $Root -Description $Description -RequireExisting $true
    $rootPrefix = $rootPath.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $pending = [Collections.Generic.Stack[string]]::new()
    $items = [Collections.Generic.List[object]]::new()
    $pending.Push($rootPath)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($child in (Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)) {
            if ($items.Count -ge $MaximumStageTreeEntries) {
                throw "$Description tree entry bound exceeded: $rootPath"
            }
            $childPath = [IO.Path]::GetFullPath($child.FullName)
            if (-not $childPath.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
                throw "$Description child escaped its root: $childPath"
            }
            if (($child.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Description contains a reparse point: $childPath"
            }
            $items.Add($child)
            if ($child.PSIsContainer) { $pending.Push($childPath) }
        }
    }
    return $items.ToArray()
}

function Assert-StageSafeDeleteTarget([string]$Path, [string]$ExpectedType) {
    if (@('Leaf', 'Container') -notcontains $ExpectedType) {
        throw "Stage delete needs ExpectedType 'Leaf' or 'Container': $ExpectedType"
    }
    $fullPath = [IO.Path]::GetFullPath($Path)
    [void](Assert-StageDirectDirectoryPath -Path ([IO.Path]::GetDirectoryName($fullPath)) `
        -Description 'Stage delete parent' -RequireExisting $true)
    $item = Get-StagePathItem $fullPath
    if ($null -eq $item) { return $false }
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to remove reparse point: $fullPath"
    }
    if (($ExpectedType -eq 'Container') -ne [bool]$item.PSIsContainer) {
        throw "Refusing to remove '$fullPath': expected a $ExpectedType"
    }
    if ($ExpectedType -eq 'Container') {
        [void](Get-StageDirectTreeItem -Root $fullPath -Description 'Stage delete target')
    }
    return $true
}
