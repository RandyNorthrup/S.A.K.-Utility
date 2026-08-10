# Create Git Tag for Release
#
# Usage: .\scripts\create-release-tag.ps1 -Version "0.5.0" -Message "Initial beta release"

param(
    [Parameter(Mandatory=$true)]
    [string]$Version,

    [Parameter(Mandatory=$false)]
    [string]$Message = "Release version $Version"
)

$ErrorActionPreference = "Stop"

# Anchor every git call to the repository this script ships in, not the caller's
# current directory, so an absolute invocation from another checkout cannot tag
# the wrong repository.
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)

# Validate version format (supports X.Y.Z and X.Y.Z.W)
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    Write-Error "Invalid version format. Expected: X.Y.Z or X.Y.Z.W (e.g., 0.9.0 or 0.9.0.3)"
    exit 1
}

$tag = "v$Version"

Write-Host "Creating release tag: $tag" -ForegroundColor Cyan
Write-Host "Message: $Message" -ForegroundColor Cyan
Write-Host ""

# Check if tag already exists
$existingTag = git -C $projectRoot tag -l $tag
if ($LASTEXITCODE -ne 0) {
    Write-Error "git tag -l failed (exit $LASTEXITCODE); cannot confirm whether $tag already exists."
    exit 1
}
if ($existingTag) {
    Write-Error "Tag $tag already exists!"
    exit 1
}

# Check for uncommitted changes
$status = git -C $projectRoot status --porcelain
if ($LASTEXITCODE -ne 0) {
    Write-Error "git status failed (exit $LASTEXITCODE); cannot confirm the working tree is clean."
    exit 1
}
if ($status) {
    Write-Warning "You have uncommitted changes:"
    git -C $projectRoot status --short
    Write-Host ""
    $response = Read-Host "Continue anyway? (y/n)"
    if ($response -ne 'y') {
        Write-Host "Aborted." -ForegroundColor Yellow
        exit 1
    }
}

# Create annotated tag
Write-Host "Creating annotated tag..." -ForegroundColor Green
git -C $projectRoot tag -a $tag -m "$Message"

if ($LASTEXITCODE -eq 0) {
    Write-Host "Tag created successfully!" -ForegroundColor Green
    Write-Host ""
    Write-Host "To push the tag to GitHub, run:" -ForegroundColor Cyan
    Write-Host "  git push origin $tag" -ForegroundColor White
    Write-Host ""
    Write-Host "Or to push all tags:" -ForegroundColor Cyan
    Write-Host "  git push origin --tags" -ForegroundColor White
} else {
    Write-Error "Failed to create tag!"
    exit 1
}
