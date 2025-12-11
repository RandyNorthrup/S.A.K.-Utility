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

# Validate version format
if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    Write-Error "Invalid version format. Expected: X.Y.Z (e.g., 0.5.0)"
    exit 1
}

$tag = "v$Version"

Write-Host "Creating release tag: $tag" -ForegroundColor Cyan
Write-Host "Message: $Message" -ForegroundColor Cyan
Write-Host ""

# Check if tag already exists
$existingTag = git tag -l $tag
if ($existingTag) {
    Write-Error "Tag $tag already exists!"
    exit 1
}

# Check for uncommitted changes
$status = git status --porcelain
if ($status) {
    Write-Warning "You have uncommitted changes:"
    git status --short
    Write-Host ""
    $response = Read-Host "Continue anyway? (y/n)"
    if ($response -ne 'y') {
        Write-Host "Aborted." -ForegroundColor Yellow
        exit 0
    }
}

# Create annotated tag
Write-Host "Creating annotated tag..." -ForegroundColor Green
git tag -a $tag -m "$Message"

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
