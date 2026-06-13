param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath,

    [Parameter(Mandatory = $true)]
    [string]$CertifierPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    throw "sak_hfs_writer_cli self-test failed: $Message"
}

function Read-Report([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail "missing report: $Path"
    }
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

if (-not (Test-Path -LiteralPath $CliPath -PathType Leaf)) {
    Fail "CLI not found: $CliPath"
}
if (-not (Test-Path -LiteralPath $CertifierPath -PathType Leaf)) {
    Fail "certifier not found: $CertifierPath"
}

$runRoot = Join-Path $OutputRoot ("run-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$targetPath = Join-Path $runRoot "not-hfs.img"
$payloadPath = Join-Path $runRoot "payload.bin"
$blockedReportPath = Join-Path $runRoot "blocked-not-hfs-basic.json"
$invalidReportPath = Join-Path $runRoot "blocked-not-hfs.json"
$fixturePath = Join-Path $runRoot "hfs-writer-fixture.img"
$fixtureReportPath = Join-Path $runRoot "hfs-fixture.json"
$noConfirmReportPath = Join-Path $runRoot "blocked-no-confirm.json"
$writeReportPath = Join-Path $runRoot "hfs-overwrite.json"
$readBackReportPath = Join-Path $runRoot "hfs-readback.json"
$resizeFixturePath = Join-Path $runRoot "hfs-resize-fixture.img"
$resizeFixtureReportPath = Join-Path $runRoot "hfs-resize-fixture.json"
$allocationPayloadPath = Join-Path $runRoot "allocation-growth-payload.bin"
$allocationReportPath = Join-Path $runRoot "hfs-allocation-growth.json"
$allocationReadBackReportPath = Join-Path $runRoot "hfs-allocation-growth-readback.json"
$resourceAllocationPayloadPath = Join-Path $runRoot "resource-allocation-growth-payload.bin"
$resourceAllocationReportPath = Join-Path $runRoot "hfs-resource-allocation-growth.json"
$resourceAllocationReadBackReportPath = Join-Path $runRoot "hfs-resource-allocation-growth-readback.json"
$growPayloadPath = Join-Path $runRoot "grow-payload.bin"
$growReportPath = Join-Path $runRoot "hfs-grow.json"
$growReadBackReportPath = Join-Path $runRoot "hfs-grow-readback.json"
$shrinkPayloadPath = Join-Path $runRoot "shrink-payload.bin"
$shrinkReportPath = Join-Path $runRoot "hfs-shrink.json"
$shrinkReadBackReportPath = Join-Path $runRoot "hfs-shrink-readback.json"
$truncateReportPath = Join-Path $runRoot "hfs-truncate.json"
$truncateReadBackReportPath = Join-Path $runRoot "hfs-truncate-readback.json"
$resourcePayloadPath = Join-Path $runRoot "resource-payload.bin"
$resourceReportPath = Join-Path $runRoot "hfs-resource-replace.json"
$resourceReadBackReportPath = Join-Path $runRoot "hfs-resource-readback.json"
$resourceTruncateReportPath = Join-Path $runRoot "hfs-resource-truncate.json"
$resourceTruncateReadBackReportPath = Join-Path $runRoot "hfs-resource-truncate-readback.json"
$attributePayloadPath = Join-Path $runRoot "attribute-payload.bin"
$attributeReportPath = Join-Path $runRoot "hfs-inline-attribute.json"
$attributeReadBackReportPath = Join-Path $runRoot "hfs-inline-attribute-readback.json"
$forkAttributePayloadPath = Join-Path $runRoot "fork-attribute-payload.bin"
$forkAttributeReportPath = Join-Path $runRoot "hfs-fork-attribute.json"
$forkAttributeReadBackReportPath = Join-Path $runRoot "hfs-fork-attribute-readback.json"
$forkAttributeGrowthPayloadPath = Join-Path $runRoot "fork-attribute-growth-payload.bin"
$forkAttributeGrowthReportPath = Join-Path $runRoot "hfs-fork-attribute-growth.json"
$forkAttributeGrowthReadBackReportPath = Join-Path $runRoot "hfs-fork-attribute-growth-readback.json"
$createEmptyReportPath = Join-Path $runRoot "hfs-create-empty-file.json"
$createEmptyReadBackReportPath = Join-Path $runRoot "hfs-create-empty-file-readback.json"
$createEmptyListReportPath = Join-Path $runRoot "hfs-create-empty-file-list.json"
$createFilePayloadPath = Join-Path $runRoot "create-file-payload.bin"
$createFileReportPath = Join-Path $runRoot "hfs-create-file.json"
$createFileReadBackReportPath = Join-Path $runRoot "hfs-create-file-readback.json"
$createFileListReportPath = Join-Path $runRoot "hfs-create-file-list.json"
$renameCatalogReportPath = Join-Path $runRoot "hfs-rename-catalog-entry.json"
$renameCatalogReadBackReportPath = Join-Path $runRoot "hfs-rename-catalog-entry-readback.json"
$renameCatalogListReportPath = Join-Path $runRoot "hfs-rename-catalog-entry-list.json"
$deleteCreatedFileReportPath = Join-Path $runRoot "hfs-delete-created-file.json"
$deleteEmptyReportPath = Join-Path $runRoot "hfs-delete-empty-file.json"
$deleteEmptyListReportPath = Join-Path $runRoot "hfs-delete-empty-file-list.json"
$createEmptyFolderReportPath = Join-Path $runRoot "hfs-create-empty-folder.json"
$createEmptyFolderListReportPath = Join-Path $runRoot "hfs-create-empty-folder-list.json"
$createEmptyFolderChildListReportPath = Join-Path $runRoot "hfs-create-empty-folder-child-list.json"
$deleteEmptyFolderReportPath = Join-Path $runRoot "hfs-delete-empty-folder.json"
$deleteEmptyFolderListReportPath = Join-Path $runRoot "hfs-delete-empty-folder-list.json"
$deleteAllocatedReportPath = Join-Path $runRoot "hfs-delete-allocated-file.json"
$deleteAllocatedListReportPath = Join-Path $runRoot "hfs-delete-allocated-file-list.json"
$folderTreeFixturePath = Join-Path $runRoot "hfs-folder-tree-fixture.img"
$folderTreeFixtureReportPath = Join-Path $runRoot "hfs-folder-tree-fixture.json"
$deleteFolderTreeReportPath = Join-Path $runRoot "hfs-delete-folder-tree.json"
$deleteFolderTreeListReportPath = Join-Path $runRoot "hfs-delete-folder-tree-list.json"

[System.IO.File]::WriteAllBytes($targetPath, [byte[]](0, 1, 2, 3))
$replacementBytes = [System.Text.Encoding]::ASCII.GetBytes("patched hfs ok`n")
[System.IO.File]::WriteAllBytes($payloadPath, $replacementBytes)
$expectedReplacementHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
$createFileBytes = [System.Text.Encoding]::ASCII.GetBytes("created hfs file payload`n")
[System.IO.File]::WriteAllBytes($createFilePayloadPath, $createFileBytes)
$expectedCreateFileHash = (Get-FileHash -LiteralPath $createFilePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath overwrite-image `
    --target $targetPath `
    --hfs-path "/hello.txt" `
    --payload-file $payloadPath `
    --output-json $blockedReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "overwrite-image accepted non-HFS media"
}
$blockedReport = Read-Report $blockedReportPath
if ($blockedReport.ok) {
    Fail "basic non-HFS report marked ok"
}
$blockerText = [string]::Join(" ", @($blockedReport.blockers))
if (-not $blockerText.Contains("Unable to open HFS+ filesystem")) {
    Fail "basic non-HFS run did not return structured HFS blocker"
}

& $CliPath overwrite-image `
    --target $targetPath `
    --hfs-path "/hello.txt" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --output-json $invalidReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "overwrite-image accepted non-HFS media"
}
$invalidReport = Read-Report $invalidReportPath
if ($invalidReport.ok) {
    Fail "non-HFS report marked ok"
}
$invalidBlockers = [string]::Join(" ", @($invalidReport.blockers))
if (-not $invalidBlockers.Contains("Unable to open HFS+ filesystem")) {
    Fail "non-HFS run did not explain media rejection"
}

& $CertifierPath `
    --hfs-build-writer-fixture $fixturePath `
    --output $fixtureReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "fixture builder exited $LASTEXITCODE"
}
$fixtureReport = Read-Report $fixtureReportPath
if ($fixtureReport.status -ne "Passed") {
    Fail "fixture build failed: $($fixtureReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $fixturePath -PathType Leaf)) {
    Fail "fixture image missing"
}

& $CliPath overwrite-image `
    --target $fixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $payloadPath `
    --output-json $noConfirmReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "overwrite-image accepted valid HFS media without confirmation"
}
$noConfirmReport = Read-Report $noConfirmReportPath
if ($noConfirmReport.ok) {
    Fail "missing-confirmation report marked ok"
}
$noConfirmBlockers = [string]::Join(" ", @($noConfirmReport.blockers))
if (-not $noConfirmBlockers.Contains("explicit target confirmation")) {
    Fail "missing-confirmation run did not explain confirmation guard"
}

& $CliPath overwrite-image `
    --target $fixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-overwrite" `
    --output-json $writeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "overwrite-image exited $LASTEXITCODE"
}
$writeReport = Read-Report $writeReportPath
if (-not $writeReport.ok) {
    Fail "overwrite-image report not ok: $($writeReport.blockers -join '; ')"
}
if ($writeReport.bytes_written -ne "$($replacementBytes.Length)") {
    Fail "write byte count mismatch"
}
if ($writeReport.catalog_id -ne "17") {
    Fail "unexpected HFS catalog id: $($writeReport.catalog_id)"
}
if ([string]::IsNullOrWhiteSpace([string]$writeReport.before_sha256) -or
    [string]::IsNullOrWhiteSpace([string]$writeReport.after_sha256) -or
    $writeReport.before_sha256 -eq $writeReport.after_sha256) {
    Fail "write report did not include distinct before/after hashes"
}
if ($writeReport.after_sha256 -ne $expectedReplacementHash) {
    Fail "write after hash mismatch"
}

& $CertifierPath `
    --input $fixturePath `
    --expect "HFS+" `
    --hfs-read-file "/hello.txt" `
    --hfs-read-max-bytes 1024 `
    --output $readBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "read-back certifier exited $LASTEXITCODE"
}
$readBackReport = Read-Report $readBackReportPath
if ($readBackReport.status -ne "Passed") {
    Fail "read-back report failed: $($readBackReport.blockers -join '; ')"
}
if ($readBackReport.hfs_read_file.sha256 -ne $expectedReplacementHash) {
    Fail "read-back hash mismatch"
}

$growBytes = [System.Text.Encoding]::ASCII.GetBytes("grown hfs payload inside allocation`n")
[System.IO.File]::WriteAllBytes($growPayloadPath, $growBytes)
$expectedGrowHash = (Get-FileHash -LiteralPath $growPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
$shrinkBytes = [System.Text.Encoding]::ASCII.GetBytes("tiny hfs`n")
[System.IO.File]::WriteAllBytes($shrinkPayloadPath, $shrinkBytes)
$expectedShrinkHash = (Get-FileHash -LiteralPath $shrinkPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CertifierPath `
    --hfs-build-writer-fixture $resizeFixturePath `
    --output $resizeFixtureReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "resize fixture builder exited $LASTEXITCODE"
}
$resizeFixtureReport = Read-Report $resizeFixtureReportPath
if ($resizeFixtureReport.status -ne "Passed") {
    Fail "resize fixture build failed: $($resizeFixtureReport.blockers -join '; ')"
}

$allocationBytes = [System.Text.Encoding]::ASCII.GetBytes("allocation-growth-data`n" + ("A" * 4200))
[System.IO.File]::WriteAllBytes($allocationPayloadPath, $allocationBytes)
$expectedAllocationHash = (Get-FileHash -LiteralPath $allocationPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath grow-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $allocationPayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-allocation-growth" `
    --output-json $allocationReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "grow-image exited $LASTEXITCODE"
}
$allocationReport = Read-Report $allocationReportPath
if (-not $allocationReport.ok) {
    Fail "grow-image report not ok: $($allocationReport.blockers -join '; ')"
}
if ($allocationReport.bytes_written -ne "$($allocationBytes.Length)") {
    Fail "allocation-growth byte count mismatch"
}
if ($allocationReport.after_sha256 -ne $expectedAllocationHash) {
    Fail "allocation-growth after hash mismatch"
}
$allocationWarnings = [string]::Join(" ", @($allocationReport.warnings))
if (-not $allocationWarnings.Contains("allocating 1 new block")) {
    Fail "allocation-growth report did not include allocation warning"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/hello.txt" `
    --hfs-read-max-bytes 8192 `
    --output $allocationReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "allocation-growth read-back certifier exited $LASTEXITCODE"
}
$allocationReadBackReport = Read-Report $allocationReadBackReportPath
if ($allocationReadBackReport.status -ne "Passed" -or
    $allocationReadBackReport.hfs_read_file.sha256 -ne $expectedAllocationHash) {
    Fail "allocation-growth read-back hash mismatch"
}

$resourceAllocationBytes = [System.Text.Encoding]::ASCII.GetBytes("resource-allocation-growth`n" + ("R" * 9000))
[System.IO.File]::WriteAllBytes($resourceAllocationPayloadPath, $resourceAllocationBytes)
$expectedResourceAllocationHash = (Get-FileHash -LiteralPath $resourceAllocationPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath grow-resource-fork-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $resourceAllocationPayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-resource-allocation-growth" `
    --output-json $resourceAllocationReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "grow-resource-fork-image exited $LASTEXITCODE"
}
$resourceAllocationReport = Read-Report $resourceAllocationReportPath
if (-not $resourceAllocationReport.ok) {
    Fail "grow-resource-fork-image report not ok: $($resourceAllocationReport.blockers -join '; ')"
}
if ($resourceAllocationReport.after_sha256 -ne $expectedResourceAllocationHash) {
    Fail "resource allocation-growth after hash mismatch"
}
$resourceAllocationWarnings = [string]::Join(" ", @($resourceAllocationReport.warnings))
if (-not $resourceAllocationWarnings.Contains("resource fork")) {
    Fail "resource allocation-growth report did not identify selected fork"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-resource-fork "/hello.txt" `
    --hfs-read-max-bytes 12288 `
    --output $resourceAllocationReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "resource allocation-growth read-back certifier exited $LASTEXITCODE"
}
$resourceAllocationReadBackReport = Read-Report $resourceAllocationReadBackReportPath
if ($resourceAllocationReadBackReport.status -ne "Passed" -or
    $resourceAllocationReadBackReport.hfs_read_resource_fork.sha256 -ne $expectedResourceAllocationHash) {
    Fail "resource allocation-growth read-back hash mismatch"
}

& $CliPath replace-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $growPayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-grow-within-allocation" `
    --output-json $growReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace-image grow exited $LASTEXITCODE"
}
$growReport = Read-Report $growReportPath
if (-not $growReport.ok) {
    Fail "replace-image grow report not ok: $($growReport.blockers -join '; ')"
}
if ($growReport.bytes_written -ne "$($growBytes.Length)") {
    Fail "grow byte count mismatch"
}
if ($growReport.after_sha256 -ne $expectedGrowHash) {
    Fail "grow after hash mismatch"
}
$growWarnings = [string]::Join(" ", @($growReport.warnings))
if (-not $growWarnings.Contains("already allocated")) {
    Fail "grow report did not include allocated-block warning"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/hello.txt" `
    --hfs-read-max-bytes 4096 `
    --output $growReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "grow read-back certifier exited $LASTEXITCODE"
}
$growReadBackReport = Read-Report $growReadBackReportPath
if ($growReadBackReport.status -ne "Passed" -or
    $growReadBackReport.hfs_read_file.sha256 -ne $expectedGrowHash) {
    Fail "grow read-back hash mismatch"
}

& $CliPath replace-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $shrinkPayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-shrink-within-allocation" `
    --output-json $shrinkReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace-image shrink exited $LASTEXITCODE"
}
$shrinkReport = Read-Report $shrinkReportPath
if (-not $shrinkReport.ok) {
    Fail "replace-image shrink report not ok: $($shrinkReport.blockers -join '; ')"
}
if ($shrinkReport.bytes_written -ne "$($shrinkBytes.Length)") {
    Fail "shrink byte count mismatch"
}
if ($shrinkReport.after_sha256 -ne $expectedShrinkHash) {
    Fail "shrink after hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/hello.txt" `
    --hfs-read-max-bytes 4096 `
    --output $shrinkReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "shrink read-back certifier exited $LASTEXITCODE"
}
$shrinkReadBackReport = Read-Report $shrinkReadBackReportPath
if ($shrinkReadBackReport.status -ne "Passed" -or
    $shrinkReadBackReport.hfs_read_file.sha256 -ne $expectedShrinkHash) {
    Fail "shrink read-back hash mismatch"
}

& $CliPath truncate-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-truncate-within-allocation" `
    --output-json $truncateReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "truncate-image exited $LASTEXITCODE"
}
$truncateReport = Read-Report $truncateReportPath
if (-not $truncateReport.ok) {
    Fail "truncate-image report not ok: $($truncateReport.blockers -join '; ')"
}
if ($truncateReport.bytes_written -ne "0") {
    Fail "truncate byte count mismatch"
}
if ($truncateReport.after_sha256 -ne "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    Fail "truncate after hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/hello.txt" `
    --hfs-read-max-bytes 4096 `
    --output $truncateReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "truncate read-back certifier exited $LASTEXITCODE"
}
$truncateReadBackReport = Read-Report $truncateReadBackReportPath
if ($truncateReadBackReport.status -ne "Passed" -or
    $truncateReadBackReport.hfs_read_file.sha256 -ne "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    Fail "truncate read-back hash mismatch"
}

$resourceBytes = [System.Text.Encoding]::ASCII.GetBytes("patched hfs resource fork`n")
[System.IO.File]::WriteAllBytes($resourcePayloadPath, $resourceBytes)
$expectedResourceHash = (Get-FileHash -LiteralPath $resourcePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath replace-resource-fork-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --payload-file $resourcePayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-resource-fork-replace" `
    --output-json $resourceReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace-resource-fork-image exited $LASTEXITCODE"
}
$resourceReport = Read-Report $resourceReportPath
if (-not $resourceReport.ok) {
    Fail "resource-fork replace report not ok: $($resourceReport.blockers -join '; ')"
}
if ($resourceReport.bytes_written -ne "$($resourceBytes.Length)") {
    Fail "resource-fork byte count mismatch"
}
if ($resourceReport.after_sha256 -ne $expectedResourceHash) {
    Fail "resource-fork after hash mismatch"
}
$resourceWarnings = [string]::Join(" ", @($resourceReport.warnings))
if (-not $resourceWarnings.Contains("resource fork")) {
    Fail "resource-fork report did not identify selected fork"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-resource-fork "/hello.txt" `
    --hfs-read-max-bytes 4096 `
    --output $resourceReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "resource-fork read-back certifier exited $LASTEXITCODE"
}
$resourceReadBackReport = Read-Report $resourceReadBackReportPath
if ($resourceReadBackReport.status -ne "Passed" -or
    $resourceReadBackReport.hfs_read_resource_fork.sha256 -ne $expectedResourceHash) {
    Fail "resource-fork read-back hash mismatch"
}

& $CliPath truncate-resource-fork-image `
    --target $resizeFixturePath `
    --hfs-path "/hello.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-resource-fork-truncate" `
    --output-json $resourceTruncateReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "truncate-resource-fork-image exited $LASTEXITCODE"
}
$resourceTruncateReport = Read-Report $resourceTruncateReportPath
if (-not $resourceTruncateReport.ok) {
    Fail "resource-fork truncate report not ok: $($resourceTruncateReport.blockers -join '; ')"
}
if ($resourceTruncateReport.after_sha256 -ne "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    Fail "resource-fork truncate after hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-resource-fork "/hello.txt" `
    --hfs-read-max-bytes 4096 `
    --output $resourceTruncateReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "resource-fork truncate read-back certifier exited $LASTEXITCODE"
}
$resourceTruncateReadBackReport = Read-Report $resourceTruncateReadBackReportPath
if ($resourceTruncateReadBackReport.status -ne "Passed" -or
    $resourceTruncateReadBackReport.hfs_read_resource_fork.sha256 -ne "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    Fail "resource-fork truncate read-back hash mismatch"
}

$attributeBytes = [System.Text.Encoding]::ASCII.GetBytes("finder-patched")
[System.IO.File]::WriteAllBytes($attributePayloadPath, $attributeBytes)
$expectedAttributeHash = (Get-FileHash -LiteralPath $attributePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath replace-inline-attribute-image `
    --target $resizeFixturePath `
    --file-id 17 `
    --attribute-name "com.apple.FinderInfo" `
    --payload-file $attributePayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-inline-attribute" `
    --output-json $attributeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace-inline-attribute-image exited $LASTEXITCODE"
}
$attributeReport = Read-Report $attributeReportPath
if (-not $attributeReport.ok) {
    Fail "inline attribute report not ok: $($attributeReport.blockers -join '; ')"
}
if ($attributeReport.bytes_written -ne "$($attributeBytes.Length)") {
    Fail "inline attribute byte count mismatch"
}
if ($attributeReport.after_sha256 -ne $expectedAttributeHash) {
    Fail "inline attribute after hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-attribute-file-id 17 `
    --hfs-read-attribute-name "com.apple.FinderInfo" `
    --hfs-read-max-bytes 1024 `
    --output $attributeReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "inline attribute read-back certifier exited $LASTEXITCODE"
}
$attributeReadBackReport = Read-Report $attributeReadBackReportPath
if ($attributeReadBackReport.status -ne "Passed" -or
    $attributeReadBackReport.hfs_read_attribute.sha256 -ne $expectedAttributeHash) {
    Fail "inline attribute read-back hash mismatch"
}

$forkAttributeBytes = [System.Text.Encoding]::ASCII.GetBytes("fork-attribute-patched")
[System.IO.File]::WriteAllBytes($forkAttributePayloadPath, $forkAttributeBytes)
$expectedForkAttributeHash = (Get-FileHash -LiteralPath $forkAttributePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath replace-fork-attribute-image `
    --target $resizeFixturePath `
    --file-id 17 `
    --attribute-name "com.apple.ResourceFork" `
    --payload-file $forkAttributePayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-fork-attribute" `
    --output-json $forkAttributeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace-fork-attribute-image exited $LASTEXITCODE"
}
$forkAttributeReport = Read-Report $forkAttributeReportPath
if (-not $forkAttributeReport.ok) {
    Fail "fork attribute report not ok: $($forkAttributeReport.blockers -join '; ')"
}
if ($forkAttributeReport.bytes_written -ne "$($forkAttributeBytes.Length)") {
    Fail "fork attribute byte count mismatch"
}
if ($forkAttributeReport.after_sha256 -ne $expectedForkAttributeHash) {
    Fail "fork attribute after hash mismatch"
}
$forkAttributeWarnings = [string]::Join(" ", @($forkAttributeReport.warnings))
if (-not $forkAttributeWarnings.Contains("allocated blocks")) {
    Fail "fork attribute report did not include allocated-block warning"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-attribute-file-id 17 `
    --hfs-read-attribute-name "com.apple.ResourceFork" `
    --hfs-read-max-bytes 1024 `
    --output $forkAttributeReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "fork attribute read-back certifier exited $LASTEXITCODE"
}
$forkAttributeReadBackReport = Read-Report $forkAttributeReadBackReportPath
if ($forkAttributeReadBackReport.status -ne "Passed" -or
    $forkAttributeReadBackReport.hfs_read_attribute.sha256 -ne $expectedForkAttributeHash) {
    Fail "fork attribute read-back hash mismatch"
}

$forkAttributeGrowthBytes = [System.Text.Encoding]::ASCII.GetBytes("fork-attribute-growth`n" + ("F" * 4300))
[System.IO.File]::WriteAllBytes($forkAttributeGrowthPayloadPath, $forkAttributeGrowthBytes)
$expectedForkAttributeGrowthHash = (Get-FileHash -LiteralPath $forkAttributeGrowthPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()

& $CliPath grow-fork-attribute-image `
    --target $resizeFixturePath `
    --file-id 17 `
    --attribute-name "com.apple.ResourceFork" `
    --payload-file $forkAttributeGrowthPayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-fork-attribute-growth" `
    --output-json $forkAttributeGrowthReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "grow-fork-attribute-image exited $LASTEXITCODE"
}
$forkAttributeGrowthReport = Read-Report $forkAttributeGrowthReportPath
if (-not $forkAttributeGrowthReport.ok) {
    Fail "fork attribute growth report not ok: $($forkAttributeGrowthReport.blockers -join '; ')"
}
if ($forkAttributeGrowthReport.bytes_written -ne "$($forkAttributeGrowthBytes.Length)") {
    Fail "fork attribute growth byte count mismatch"
}
if ($forkAttributeGrowthReport.after_sha256 -ne $expectedForkAttributeGrowthHash) {
    Fail "fork attribute growth after hash mismatch"
}
$forkAttributeGrowthWarnings = [string]::Join(" ", @($forkAttributeGrowthReport.warnings))
if (-not $forkAttributeGrowthWarnings.Contains("allocating")) {
    Fail "fork attribute growth report did not include allocation warning"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-attribute-file-id 17 `
    --hfs-read-attribute-name "com.apple.ResourceFork" `
    --hfs-read-max-bytes 8192 `
    --output $forkAttributeGrowthReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "fork attribute growth read-back certifier exited $LASTEXITCODE"
}
$forkAttributeGrowthReadBackReport = Read-Report $forkAttributeGrowthReadBackReportPath
if ($forkAttributeGrowthReadBackReport.status -ne "Passed" -or
    $forkAttributeGrowthReadBackReport.hfs_read_attribute.sha256 -ne $expectedForkAttributeGrowthHash) {
    Fail "fork attribute growth read-back hash mismatch"
}

& $CliPath create-empty-file-image `
    --target $resizeFixturePath `
    --hfs-path "/created.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-create-empty-file" `
    --output-json $createEmptyReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "create-empty-file-image exited $LASTEXITCODE"
}
$createEmptyReport = Read-Report $createEmptyReportPath
if (-not $createEmptyReport.ok) {
    Fail "create-empty-file-image report not ok: $($createEmptyReport.blockers -join '; ')"
}
if ($createEmptyReport.bytes_written -ne "0") {
    Fail "create-empty-file-image byte count mismatch"
}
$createWarnings = [string]::Join(" ", @($createEmptyReport.warnings))
if (-not $createWarnings.Contains("empty file created")) {
    Fail "create-empty-file-image did not report catalog create"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/created.txt" `
    --hfs-read-max-bytes 1 `
    --output $createEmptyReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created empty file read-back certifier exited $LASTEXITCODE"
}
$createReadBackReport = Read-Report $createEmptyReadBackReportPath
if ($createReadBackReport.status -ne "Passed" -or
    $createReadBackReport.hfs_read_file.sha256 -ne "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
    Fail "created empty file read-back mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $createEmptyListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created empty file listing certifier exited $LASTEXITCODE"
}
$createListReport = Read-Report $createEmptyListReportPath
$createdEntry = @($createListReport.hfs_listing.entries | Where-Object { $_.name -eq "created.txt" })
if ($createListReport.status -ne "Passed" -or $createdEntry.Count -ne 1 -or
    $createdEntry[0].size_bytes -ne "0") {
    Fail "created empty file missing from listing"
}

& $CliPath delete-empty-file-image `
    --target $resizeFixturePath `
    --hfs-path "/created.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-delete-empty-file" `
    --output-json $deleteEmptyReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-empty-file-image exited $LASTEXITCODE"
}
$deleteEmptyReport = Read-Report $deleteEmptyReportPath
if (-not $deleteEmptyReport.ok) {
    Fail "delete-empty-file-image report not ok: $($deleteEmptyReport.blockers -join '; ')"
}
$deleteWarnings = [string]::Join(" ", @($deleteEmptyReport.warnings))
if (-not $deleteWarnings.Contains("empty file deleted")) {
    Fail "delete-empty-file-image did not report catalog delete"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $deleteEmptyListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "deleted empty file listing certifier exited $LASTEXITCODE"
}
$deleteListReport = Read-Report $deleteEmptyListReportPath
$deletedEntry = @($deleteListReport.hfs_listing.entries | Where-Object { $_.name -eq "created.txt" })
if ($deleteListReport.status -ne "Passed" -or $deletedEntry.Count -ne 0) {
    Fail "deleted empty file still present in listing"
}

& $CliPath create-file-image `
    --target $resizeFixturePath `
    --hfs-path "/created-data.txt" `
    --payload-file $createFilePayloadPath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-create-file" `
    --output-json $createFileReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "create-file-image exited $LASTEXITCODE"
}
$createFileReport = Read-Report $createFileReportPath
if (-not $createFileReport.ok) {
    Fail "create-file-image report not ok: $($createFileReport.blockers -join '; ')"
}
if ($createFileReport.after_sha256 -ne $expectedCreateFileHash) {
    Fail "create-file-image after hash mismatch"
}
$createFileWarnings = [string]::Join(" ", @($createFileReport.warnings))
if (-not $createFileWarnings.Contains("file created")) {
    Fail "create-file-image did not report file create"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/created-data.txt" `
    --hfs-read-max-bytes 1024 `
    --output $createFileReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created file read-back certifier exited $LASTEXITCODE"
}
$createFileReadBackReport = Read-Report $createFileReadBackReportPath
if ($createFileReadBackReport.status -ne "Passed" -or
    $createFileReadBackReport.hfs_read_file.sha256 -ne $expectedCreateFileHash) {
    Fail "created file read-back hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $createFileListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created file listing certifier exited $LASTEXITCODE"
}
$createFileListReport = Read-Report $createFileListReportPath
$createdFileEntry = @($createFileListReport.hfs_listing.entries | Where-Object { $_.name -eq "created-data.txt" })
if ($createFileListReport.status -ne "Passed" -or $createdFileEntry.Count -ne 1 -or
    $createdFileEntry[0].size_bytes -ne "$($createFileBytes.Length)") {
    Fail "created data file missing from listing"
}

& $CliPath rename-catalog-entry-image `
    --target $resizeFixturePath `
    --hfs-path "/created-data.txt" `
    --destination-hfs-path "/created-renamed.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-rename-catalog-entry" `
    --output-json $renameCatalogReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "rename-catalog-entry-image exited $LASTEXITCODE"
}
$renameCatalogReport = Read-Report $renameCatalogReportPath
if (-not $renameCatalogReport.ok) {
    Fail "rename-catalog-entry-image report not ok: $($renameCatalogReport.blockers -join '; ')"
}
if ($renameCatalogReport.destination_hfs_path -ne "/created-renamed.txt") {
    Fail "rename-catalog-entry-image did not report destination path"
}
$renameCatalogWarnings = [string]::Join(" ", @($renameCatalogReport.warnings))
if (-not $renameCatalogWarnings.Contains("renamed")) {
    Fail "rename-catalog-entry-image did not report catalog rename"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-read-file "/created-renamed.txt" `
    --hfs-read-max-bytes 1024 `
    --output $renameCatalogReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "renamed file read-back certifier exited $LASTEXITCODE"
}
$renameCatalogReadBackReport = Read-Report $renameCatalogReadBackReportPath
if ($renameCatalogReadBackReport.status -ne "Passed" -or
    $renameCatalogReadBackReport.hfs_read_file.sha256 -ne $expectedCreateFileHash) {
    Fail "renamed file read-back hash mismatch"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $renameCatalogListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "renamed file listing certifier exited $LASTEXITCODE"
}
$renameCatalogListReport = Read-Report $renameCatalogListReportPath
$oldFileEntry = @($renameCatalogListReport.hfs_listing.entries | Where-Object { $_.name -eq "created-data.txt" })
$renamedFileEntry = @($renameCatalogListReport.hfs_listing.entries | Where-Object { $_.name -eq "created-renamed.txt" })
if ($renameCatalogListReport.status -ne "Passed" -or
    $oldFileEntry.Count -ne 0 -or
    $renamedFileEntry.Count -ne 1 -or
    $renamedFileEntry[0].size_bytes -ne "$($createFileBytes.Length)") {
    Fail "renamed file listing mismatch"
}

& $CliPath delete-file-image `
    --target $resizeFixturePath `
    --hfs-path "/created-renamed.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --secure-wipe-released-blocks `
    --evidence-id "ctest.hfs-cli-delete-created-file" `
    --output-json $deleteCreatedFileReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete created file exited $LASTEXITCODE"
}
$deleteCreatedFileReport = Read-Report $deleteCreatedFileReportPath
if (-not $deleteCreatedFileReport.ok) {
    Fail "delete created file report not ok: $($deleteCreatedFileReport.blockers -join '; ')"
}
if (-not (($deleteCreatedFileReport.warnings -join ' ') -like "*zeroing released allocated blocks*")) {
    Fail "delete created file did not report secure released-block wipe"
}

& $CliPath create-empty-folder-image `
    --target $resizeFixturePath `
    --hfs-path "/Created Folder" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-create-empty-folder" `
    --output-json $createEmptyFolderReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "create-empty-folder-image exited $LASTEXITCODE"
}
$createEmptyFolderReport = Read-Report $createEmptyFolderReportPath
if (-not $createEmptyFolderReport.ok) {
    Fail "create-empty-folder-image report not ok: $($createEmptyFolderReport.blockers -join '; ')"
}
$createFolderWarnings = [string]::Join(" ", @($createEmptyFolderReport.warnings))
if (-not $createFolderWarnings.Contains("empty folder created")) {
    Fail "create-empty-folder-image did not report catalog create"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $createEmptyFolderListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created empty folder root listing certifier exited $LASTEXITCODE"
}
$createFolderListReport = Read-Report $createEmptyFolderListReportPath
$createdFolderEntry = @($createFolderListReport.hfs_listing.entries | Where-Object { $_.name -eq "Created Folder" })
if ($createFolderListReport.status -ne "Passed" -or $createdFolderEntry.Count -ne 1) {
    Fail "created empty folder missing from root listing"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/Created Folder" `
    --output $createEmptyFolderChildListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "created empty folder child listing certifier exited $LASTEXITCODE"
}
$createFolderChildListReport = Read-Report $createEmptyFolderChildListReportPath
if ($createFolderChildListReport.status -ne "Passed" -or
    @($createFolderChildListReport.hfs_listing.entries).Count -ne 0) {
    Fail "created empty folder was not empty"
}

& $CliPath delete-empty-folder-image `
    --target $resizeFixturePath `
    --hfs-path "/Created Folder" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-delete-empty-folder" `
    --output-json $deleteEmptyFolderReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-empty-folder-image exited $LASTEXITCODE"
}
$deleteEmptyFolderReport = Read-Report $deleteEmptyFolderReportPath
if (-not $deleteEmptyFolderReport.ok) {
    Fail "delete-empty-folder-image report not ok: $($deleteEmptyFolderReport.blockers -join '; ')"
}
$deleteFolderWarnings = [string]::Join(" ", @($deleteEmptyFolderReport.warnings))
if (-not $deleteFolderWarnings.Contains("empty folder deleted")) {
    Fail "delete-empty-folder-image did not report catalog delete"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $deleteEmptyFolderListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "deleted empty folder listing certifier exited $LASTEXITCODE"
}
$deleteFolderListReport = Read-Report $deleteEmptyFolderListReportPath
$deletedFolderEntry = @($deleteFolderListReport.hfs_listing.entries | Where-Object { $_.name -eq "Created Folder" })
if ($deleteFolderListReport.status -ne "Passed" -or $deletedFolderEntry.Count -ne 0) {
    Fail "deleted empty folder still present in listing"
}

& $CliPath delete-file-image `
    --target $resizeFixturePath `
    --hfs-path "/Docs/note.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-delete-allocated-file" `
    --output-json $deleteAllocatedReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-file-image exited $LASTEXITCODE"
}
$deleteAllocatedReport = Read-Report $deleteAllocatedReportPath
if (-not $deleteAllocatedReport.ok) {
    Fail "delete-file-image report not ok: $($deleteAllocatedReport.blockers -join '; ')"
}
if ($deleteAllocatedReport.bytes_written -ne "4096") {
    Fail "delete-file-image released byte count mismatch"
}
$deleteAllocatedWarnings = [string]::Join(" ", @($deleteAllocatedReport.warnings))
if (-not $deleteAllocatedWarnings.Contains("allocated blocks released")) {
    Fail "delete-file-image did not report allocation release"
}

# --- Real newfs_hfs volume proof: catalog root-leaf split plus extents-overflow
# --- growth/delete validated by bundled fsck_hfs after every mutation phase.
$hfsprogsRoot = Join-Path (Split-Path -Parent $PSScriptRoot) "tools\filesystem\hfsprogs"
$newfsHfs = Join-Path $hfsprogsRoot "newfs_hfs.exe"
$fsckHfs = Join-Path $hfsprogsRoot "fsck_hfs.exe"
if (-not (Test-Path -LiteralPath $newfsHfs -PathType Leaf)) {
    Fail "bundled newfs_hfs not found: $newfsHfs"
}
if (-not (Test-Path -LiteralPath $fsckHfs -PathType Leaf)) {
    Fail "bundled fsck_hfs not found: $fsckHfs"
}

$realImagePath = Join-Path $runRoot "hfs-real-volume.img"
$realStream = [System.IO.File]::Create($realImagePath)
$realStream.SetLength(64MB)
$realStream.Close()

& $newfsHfs -v "SAKSPLIT" $realImagePath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "newfs_hfs format exited $LASTEXITCODE"
}

function Invoke-RealVolumeFsck([string]$Phase) {
    & $script:fsckHfs -n -f $script:realImagePath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "fsck_hfs -n -f failed after $Phase (exit $LASTEXITCODE)"
    }
}
Invoke-RealVolumeFsck "newfs_hfs format"

$fragPayloadPath = Join-Path $runRoot "real-frag-payload.bin"
[System.IO.File]::WriteAllBytes($fragPayloadPath, [byte[]](,[byte]65 * 4096))

function Invoke-RealCreateFile([string]$HfsPath, [string]$PayloadPath, [string]$ReportPath) {
    & $script:CliPath create-file-image `
        --target $script:realImagePath `
        --hfs-path $HfsPath `
        --payload-file $PayloadPath `
        --confirm-target `
        --evidence-id "ctest.hfs-cli-real-volume" `
        --output-json $ReportPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "real-volume create-file-image $HfsPath exited $LASTEXITCODE"
    }
    return Read-Report $ReportPath
}

function Invoke-RealDeleteFile([string]$HfsPath, [string]$ReportPath) {
    & $script:CliPath delete-file-image `
        --target $script:realImagePath `
        --hfs-path $HfsPath `
        --confirm-target `
        --evidence-id "ctest.hfs-cli-real-volume" `
        --output-json $ReportPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Fail "real-volume delete-file-image $HfsPath exited $LASTEXITCODE"
    }
    return Read-Report $ReportPath
}

foreach ($index in 1..8) {
    $report = Invoke-RealCreateFile "/frag-$index.bin" $fragPayloadPath `
        (Join-Path $runRoot "real-create-frag-$index.json")
    if (-not $report.ok) {
        Fail "real-volume frag create $index not ok: $($report.blockers -join '; ')"
    }
}
Invoke-RealVolumeFsck "fragmentation creates"

foreach ($index in @(1, 3, 5, 7)) {
    $report = Invoke-RealDeleteFile "/frag-$index.bin" `
        (Join-Path $runRoot "real-delete-frag-$index.json")
    if (-not $report.ok) {
        Fail "real-volume frag delete $index not ok: $($report.blockers -join '; ')"
    }
}
Invoke-RealVolumeFsck "fragmentation deletes"

$growStage1Path = Join-Path $runRoot "real-grow-stage1.bin"
$stage1Bytes = [byte[]](,[byte]66 * (5 * 4096 + 10))
[System.IO.File]::WriteAllBytes($growStage1Path, $stage1Bytes)
$growStage1ReportPath = Join-Path $runRoot "real-grow-stage1.json"
& $CliPath grow-image `
    --target $realImagePath `
    --hfs-path "/frag-2.bin" `
    --payload-file $growStage1Path `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-growth" `
    --output-json $growStage1ReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume stage-1 grow exited $LASTEXITCODE"
}
$growStage1Report = Read-Report $growStage1ReportPath
if (-not $growStage1Report.ok) {
    Fail "real-volume stage-1 grow not ok: $($growStage1Report.blockers -join '; ')"
}
Invoke-RealVolumeFsck "stage-1 fragmented growth"

foreach ($index in @(4, 6)) {
    $report = Invoke-RealDeleteFile "/frag-$index.bin" `
        (Join-Path $runRoot "real-delete2-frag-$index.json")
    if (-not $report.ok) {
        Fail "real-volume second-round delete $index not ok: $($report.blockers -join '; ')"
    }
}
Invoke-RealVolumeFsck "second-round deletes"

$growStage2Path = Join-Path $runRoot "real-grow-stage2.bin"
$stage2Bytes = [byte[]]::new(11 * 4096 + 50)
for ($i = 0; $i -lt $stage2Bytes.Length; $i++) {
    $stage2Bytes[$i] = [byte](($i * 7 + 13) % 251)
}
[System.IO.File]::WriteAllBytes($growStage2Path, $stage2Bytes)
$expectedStage2Hash = (Get-FileHash -LiteralPath $growStage2Path -Algorithm SHA256).Hash.ToLowerInvariant()
$growStage2ReportPath = Join-Path $runRoot "real-grow-stage2.json"
& $CliPath grow-image `
    --target $realImagePath `
    --hfs-path "/frag-2.bin" `
    --payload-file $growStage2Path `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-overflow-growth" `
    --output-json $growStage2ReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume overflow grow exited $LASTEXITCODE"
}
$growStage2Report = Read-Report $growStage2ReportPath
if (-not $growStage2Report.ok) {
    Fail "real-volume overflow grow not ok: $($growStage2Report.blockers -join '; ')"
}
$growStage2Warnings = [string]::Join(" ", @($growStage2Report.warnings))
if (-not $growStage2Warnings.Contains("extents-overflow record")) {
    Fail "real-volume overflow grow did not report extents-overflow records"
}
Invoke-RealVolumeFsck "extents-overflow growth"

$overflowReadBackReportPath = Join-Path $runRoot "real-overflow-readback.json"
& $CertifierPath `
    --input $realImagePath `
    --expect "HFS+" `
    --hfs-read-file "/frag-2.bin" `
    --hfs-read-max-bytes (16 * 1024 * 1024) `
    --output $overflowReadBackReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume overflow read-back certifier exited $LASTEXITCODE"
}
$overflowReadBackReport = Read-Report $overflowReadBackReportPath
if ($overflowReadBackReport.status -ne "Passed" -or
    $overflowReadBackReport.hfs_read_file.sha256 -ne $expectedStage2Hash) {
    Fail "real-volume overflow read-back hash mismatch"
}

$overflowDeleteReport = Invoke-RealDeleteFile "/frag-2.bin" `
    (Join-Path $runRoot "real-delete-overflow.json")
if (-not $overflowDeleteReport.ok) {
    Fail "real-volume overflow delete not ok: $($overflowDeleteReport.blockers -join '; ')"
}
$overflowDeleteWarnings = [string]::Join(" ", @($overflowDeleteReport.warnings))
if (-not $overflowDeleteWarnings.Contains("extents-overflow records for the deleted file were removed")) {
    Fail "real-volume overflow delete did not remove extents-overflow records"
}
Invoke-RealVolumeFsck "extents-overflow delete"

$splitSeen = $false
$lastSplitName = ""
foreach ($index in 1..24) {
    $splitName = "/split-{0:d2}.txt" -f $index
    $splitReportPath = Join-Path $runRoot ("real-split-create-{0:d2}.json" -f $index)
    & $CliPath create-empty-file-image `
        --target $realImagePath `
        --hfs-path $splitName `
        --confirm-target `
        --evidence-id "ctest.hfs-cli-real-volume-split" `
        --output-json $splitReportPath
    if ($LASTEXITCODE -ne 0) {
        Fail "real-volume split create $splitName exited $LASTEXITCODE"
    }
    $splitReport = Read-Report $splitReportPath
    if (-not $splitReport.ok) {
        Fail "real-volume split create $splitName not ok: $($splitReport.blockers -join '; ')"
    }
    $lastSplitName = $splitName
    if (([string]::Join(" ", @($splitReport.warnings))).Contains("split into two leaf nodes")) {
        $splitSeen = $true
        break
    }
}
if (-not $splitSeen) {
    Fail "catalog root-leaf split did not trigger within 24 creates"
}
Invoke-RealVolumeFsck "catalog root-leaf split"

$splitListReportPath = Join-Path $runRoot "real-split-list.json"
& $CertifierPath `
    --input $realImagePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $splitListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume split listing certifier exited $LASTEXITCODE"
}
$splitListReport = Read-Report $splitListReportPath
$lastSplitEntry = @($splitListReport.hfs_listing.entries |
    Where-Object { $_.name -eq $lastSplitName.TrimStart('/') })
if ($splitListReport.status -ne "Passed" -or $lastSplitEntry.Count -ne 1) {
    Fail "real-volume split listing missing $lastSplitName"
}

# --- Depth-2 catalog mutation proof on the real split volume: create, delete,
# --- and rename after the split, each phase validated by fsck_hfs.
$postSplitCreateReportPath = Join-Path $runRoot "real-post-split-create.json"
& $CliPath create-empty-file-image `
    --target $realImagePath `
    --hfs-path "/post-split-created.txt" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-post-split-create" `
    --output-json $postSplitCreateReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "post-split create failed on a depth-2 catalog"
}
$postSplitCreateReport = Read-Report $postSplitCreateReportPath
if (-not $postSplitCreateReport.ok) {
    Fail "post-split create not ok: $($postSplitCreateReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "post-split create"

$postSplitRenameReportPath = Join-Path $runRoot "real-post-split-rename.json"
& $CliPath rename-catalog-entry-image `
    --target $realImagePath `
    --hfs-path "/post-split-created.txt" `
    --destination-hfs-path "/aa-post-split-renamed.txt" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-post-split-rename" `
    --output-json $postSplitRenameReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "post-split rename failed on a depth-2 catalog"
}
$postSplitRenameReport = Read-Report $postSplitRenameReportPath
if (-not $postSplitRenameReport.ok) {
    Fail "post-split rename not ok: $($postSplitRenameReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "post-split rename"

$postSplitDeleteReportPath = Join-Path $runRoot "real-post-split-delete.json"
& $CliPath delete-empty-file-image `
    --target $realImagePath `
    --hfs-path "/aa-post-split-renamed.txt" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-post-split-delete" `
    --output-json $postSplitDeleteReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "post-split delete failed on a depth-2 catalog"
}
$postSplitDeleteReport = Read-Report $postSplitDeleteReportPath
if (-not $postSplitDeleteReport.ok) {
    Fail "post-split delete not ok: $($postSplitDeleteReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "post-split delete"

# --- New-attribute create on the real volume: materializes the first
# --- attributes B-tree leaf and must pass fsck_hfs afterwards.
$frag8Report = Read-Report (Join-Path $runRoot "real-create-frag-8.json")
$attrCreatePayloadPath = Join-Path $runRoot "real-attr-create-payload.bin"
[System.IO.File]::WriteAllBytes($attrCreatePayloadPath,
    [System.Text.Encoding]::ASCII.GetBytes("sak created xattr"))
$attrCreateReportPath = Join-Path $runRoot "real-attr-create.json"
& $CliPath create-inline-attribute-image `
    --target $realImagePath `
    --file-id $frag8Report.catalog_id `
    --attribute-name "org.sak.proof" `
    --payload-file $attrCreatePayloadPath `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-attr-create" `
    --output-json $attrCreateReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume attribute create exited $LASTEXITCODE"
}
$attrCreateReport = Read-Report $attrCreateReportPath
if (-not $attrCreateReport.ok) {
    Fail "real-volume attribute create not ok: $($attrCreateReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "attribute create"

$attrReadBackPath = Join-Path $runRoot "real-attr-readback.json"
& $CertifierPath `
    --input $realImagePath `
    --expect "HFS+" `
    --hfs-read-attribute-file-id $frag8Report.catalog_id `
    --hfs-read-attribute-name "org.sak.proof" `
    --hfs-read-max-bytes 1024 `
    --output $attrReadBackPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume attribute read-back certifier exited $LASTEXITCODE"
}
$attrReadBack = Read-Report $attrReadBackPath
$expectedAttrHash = (Get-FileHash -LiteralPath $attrCreatePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($attrReadBack.status -ne "Passed" -or
    $attrReadBack.hfs_read_attribute.sha256 -ne $expectedAttrHash) {
    Fail "real-volume created attribute hash mismatch"
}

# --- Depth-3 catalog proof: bulk creates with long names force two root
# --- index splits (depth 1 -> 2 -> 3); fsck_hfs validates the full tree.
$depth3ReportPath = Join-Path $runRoot "real-depth3-create.json"
& $CliPath create-empty-files-image `
    --target $realImagePath `
    --hfs-path "/d3" `
    --file-count 60 `
    --name-pad 220 `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-depth3" `
    --output-json $depth3ReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "depth-3 bulk create exited $LASTEXITCODE"
}
$depth3Report = Read-Report $depth3ReportPath
if (-not $depth3Report.ok) {
    Fail "depth-3 bulk create not ok: $($depth3Report.blockers -join '; ')"
}
Invoke-RealVolumeFsck "depth-3 bulk create"

$depth3DeleteReportPath = Join-Path $runRoot "real-depth3-delete.json"
$depth3Pad = "x" * 220
& $CliPath delete-empty-file-image `
    --target $realImagePath `
    --hfs-path "/d3-0030-$depth3Pad.txt" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-depth3-delete" `
    --output-json $depth3DeleteReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "depth-3 delete exited $LASTEXITCODE"
}
$depth3DeleteReport = Read-Report $depth3DeleteReportPath
if (-not $depth3DeleteReport.ok) {
    Fail "depth-3 delete not ok: $($depth3DeleteReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "depth-3 delete"

# --- Fork-backed attribute create on the real volume (allocates blocks),
# --- then delete both attributes; fsck_hfs must pass after every step.
$forkAttrPayloadPath = Join-Path $runRoot "real-fork-attr-payload.bin"
$forkAttrBytes = New-Object byte[] (12000)
for ($i = 0; $i -lt $forkAttrBytes.Length; $i++) { $forkAttrBytes[$i] = [byte](($i * 11 + 5) % 251) }
[System.IO.File]::WriteAllBytes($forkAttrPayloadPath, $forkAttrBytes)
$forkAttrReportPath = Join-Path $runRoot "real-fork-attr-create.json"
& $CliPath create-fork-attribute-image `
    --target $realImagePath `
    --file-id $frag8Report.catalog_id `
    --attribute-name "org.sak.fork-proof" `
    --payload-file $forkAttrPayloadPath `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-fork-attr-create" `
    --output-json $forkAttrReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume fork attribute create exited $LASTEXITCODE"
}
$forkAttrReport = Read-Report $forkAttrReportPath
if (-not $forkAttrReport.ok) {
    Fail "real-volume fork attribute create not ok: $($forkAttrReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "fork attribute create"

$forkAttrReadBackPath = Join-Path $runRoot "real-fork-attr-readback.json"
& $CertifierPath `
    --input $realImagePath `
    --expect "HFS+" `
    --hfs-read-attribute-file-id $frag8Report.catalog_id `
    --hfs-read-attribute-name "org.sak.fork-proof" `
    --hfs-read-max-bytes 65536 `
    --output $forkAttrReadBackPath
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume fork attribute read-back certifier exited $LASTEXITCODE"
}
$forkAttrReadBack = Read-Report $forkAttrReadBackPath
$expectedForkAttrHash = (Get-FileHash -LiteralPath $forkAttrPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($forkAttrReadBack.status -ne "Passed" -or
    $forkAttrReadBack.hfs_read_attribute.sha256 -ne $expectedForkAttrHash) {
    Fail "real-volume fork attribute hash mismatch"
}

$attrDeleteForkReportPath = Join-Path $runRoot "real-attr-delete-fork.json"
& $CliPath delete-attribute-image `
    --target $realImagePath `
    --file-id $frag8Report.catalog_id `
    --attribute-name "org.sak.fork-proof" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-attr-delete-fork" `
    --output-json $attrDeleteForkReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume fork attribute delete exited $LASTEXITCODE"
}
$attrDeleteForkReport = Read-Report $attrDeleteForkReportPath
if (-not $attrDeleteForkReport.ok) {
    Fail "real-volume fork attribute delete not ok: $($attrDeleteForkReport.blockers -join '; ')"
}
Invoke-RealVolumeFsck "fork attribute delete"

$attrDeleteInlineReportPath = Join-Path $runRoot "real-attr-delete-inline.json"
& $CliPath delete-attribute-image `
    --target $realImagePath `
    --file-id $frag8Report.catalog_id `
    --attribute-name "org.sak.proof" `
    --confirm-target `
    --evidence-id "ctest.hfs-cli-real-volume-attr-delete-inline" `
    --output-json $attrDeleteInlineReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "real-volume inline attribute delete exited $LASTEXITCODE"
}
$attrDeleteInlineReport = Read-Report $attrDeleteInlineReportPath
if (-not $attrDeleteInlineReport.ok) {
    Fail "real-volume inline attribute delete not ok: $($attrDeleteInlineReport.blockers -join '; ')"
}
if (-not (([string]::Join(" ", @($attrDeleteInlineReport.warnings))).Contains("empty attributes B-tree leaf was freed"))) {
    Fail "real-volume final attribute delete did not free the attributes leaf"
}
Invoke-RealVolumeFsck "inline attribute delete"

# --- Journaled volume: replay command handles a fresh (needs-init) journal,
# --- mutations still pass fsck, and replay on the non-journaled volume blocks.
$journalImagePath = Join-Path $runRoot "hfs-journaled-volume.img"
$journalStream = [System.IO.File]::Create($journalImagePath)
$journalStream.SetLength(32MB)
$journalStream.Close()
& $newfsHfs -J -v SAKJNL $journalImagePath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "newfs_hfs -J failed"
}
$journalReplayReportPath = Join-Path $runRoot "journal-replay.json"
& $CliPath replay-journal-image `
    --target $journalImagePath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-journal-replay" `
    --output-json $journalReplayReportPath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "journal replay exited $LASTEXITCODE"
}
$journalReplayReport = Read-Report $journalReplayReportPath
$journalWarnings = [string]::Join(" ", @($journalReplayReport.warnings))
if (-not $journalReplayReport.ok -or
    -not ($journalWarnings.Contains("nothing to replay"))) {
    Fail "journal replay on fresh journal did not no-op cleanly"
}
& $CliPath create-empty-file-image `
    --target $journalImagePath `
    --hfs-path "/journaled-create.txt" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-journaled-create" `
    --output-json (Join-Path $runRoot "journal-create.json") | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "create on journaled volume exited $LASTEXITCODE"
}
& $fsckHfs -n -f $journalImagePath | Out-Null
if ($LASTEXITCODE -ne 0) {
    Fail "fsck_hfs failed on journaled volume after create"
}
$journalBlockedReportPath = Join-Path $runRoot "journal-replay-blocked.json"
& $CliPath replay-journal-image `
    --target $realImagePath `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-journal-replay-blocked" `
    --output-json $journalBlockedReportPath | Out-Null
if ($LASTEXITCODE -eq 0) {
    Fail "journal replay unexpectedly succeeded on non-journaled volume"
}
$journalBlockedReport = Read-Report $journalBlockedReportPath
if (-not (([string]::Join(" ", @($journalBlockedReport.blockers))).Contains("not journaled"))) {
    Fail "journal replay on non-journaled volume did not report the blocker"
}

# --- decmpfs compressed replacement: the CLI command must fail closed on a
# --- non-compressed file; payload round trips are proven by unit tests.
$compressedBlockedReportPath = Join-Path $runRoot "real-compressed-blocked.json"
& $CliPath replace-compressed-image `
    --target $realImagePath `
    --hfs-path "/frag-8.bin" `
    --payload-file $fragPayloadPath `
    --confirm-target `
    --allow-compressed-file-mutation `
    --evidence-id "ctest.hfs-cli-compressed-fail-closed" `
    --output-json $compressedBlockedReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "replace-compressed-image accepted a non-compressed file"
}
$compressedBlockedReport = Read-Report $compressedBlockedReportPath
if (-not (([string]::Join(" ", @($compressedBlockedReport.blockers))).Contains("decmpfs-compressed file"))) {
    Fail "replace-compressed-image did not report the decmpfs boundary"
}

& $CertifierPath `
    --input $resizeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/Docs" `
    --output $deleteAllocatedListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "allocated-file delete listing certifier exited $LASTEXITCODE"
}
$deleteAllocatedListReport = Read-Report $deleteAllocatedListReportPath
if ($deleteAllocatedListReport.status -ne "Passed" -or
    @($deleteAllocatedListReport.hfs_listing.entries).Count -ne 0) {
    Fail "allocated deleted file still present in Docs listing"
}

& $CertifierPath `
    --hfs-build-writer-fixture $folderTreeFixturePath `
    --output $folderTreeFixtureReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "folder-tree fixture builder exited $LASTEXITCODE"
}
$folderTreeFixtureReport = Read-Report $folderTreeFixtureReportPath
if ($folderTreeFixtureReport.status -ne "Passed") {
    Fail "folder-tree fixture build failed: $($folderTreeFixtureReport.blockers -join '; ')"
}

& $CliPath delete-folder-tree-image `
    --target $folderTreeFixturePath `
    --hfs-path "/Docs" `
    --confirm-target `
    --allow-journaled-volume `
    --evidence-id "ctest.hfs-cli-delete-folder-tree" `
    --output-json $deleteFolderTreeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-folder-tree-image exited $LASTEXITCODE"
}
$deleteFolderTreeReport = Read-Report $deleteFolderTreeReportPath
if (-not $deleteFolderTreeReport.ok) {
    Fail "delete-folder-tree-image report not ok: $($deleteFolderTreeReport.blockers -join '; ')"
}
if ($deleteFolderTreeReport.bytes_written -ne "4096") {
    Fail "delete-folder-tree-image released byte count mismatch"
}
$deleteFolderTreeWarnings = [string]::Join(" ", @($deleteFolderTreeReport.warnings))
if (-not $deleteFolderTreeWarnings.Contains("folder tree deleted")) {
    Fail "delete-folder-tree-image did not report tree delete"
}

& $CertifierPath `
    --input $folderTreeFixturePath `
    --expect "HFS+" `
    --hfs-list-path "/" `
    --output $deleteFolderTreeListReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "folder-tree delete root listing certifier exited $LASTEXITCODE"
}
$deleteFolderTreeListReport = Read-Report $deleteFolderTreeListReportPath
$deletedFolderEntry = @($deleteFolderTreeListReport.hfs_listing.entries | Where-Object { $_.name -eq "Docs" })
if ($deleteFolderTreeListReport.status -ne "Passed" -or $deletedFolderEntry.Count -ne 0) {
    Fail "folder-tree deleted folder still present in root listing"
}

Write-Host "sak_hfs_writer_cli self-test passed: $runRoot"
