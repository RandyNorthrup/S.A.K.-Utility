param(
    [Parameter(Mandatory = $true)]
    [string]$CliPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Fail([string]$Message) {
    throw "sak_apfs_writer_cli self-test failed: $Message"
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

$runRoot = Join-Path $OutputRoot ("run-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null

$sizeBytes = 67108864
$imagePath = Join-Path $runRoot "generated.apfs"
$formatReportPath = Join-Path $runRoot "format-image.json"
$repairImagePath = Join-Path $runRoot "repaired.apfs"
$repairReportPath = Join-Path $runRoot "repair-image.json"
$relabeledImagePath = Join-Path $runRoot "relabeled.apfs"
$relabeledReportPath = Join-Path $runRoot "change-image-volume-label.json"
$payloadPath = Join-Path $runRoot "payload.bin"
$replacementPayloadPath = Join-Path $runRoot "replacement-payload.bin"
$patchPayloadPath = Join-Path $runRoot "patch-payload.bin"
$patchedExpectedPayloadPath = Join-Path $runRoot "patched-expected.bin"
$writtenImagePath = Join-Path $runRoot "written.apfs"
$writeReportPath = Join-Path $runRoot "write-image-root-file.json"
$replacedImagePath = Join-Path $runRoot "replaced.apfs"
$replaceReportPath = Join-Path $runRoot "replace-image-root-file.json"
$patchedImagePath = Join-Path $runRoot "patched.apfs"
$patchReportPath = Join-Path $runRoot "patch-image-root-file.json"
$deletedImagePath = Join-Path $runRoot "deleted.apfs"
$deleteReportPath = Join-Path $runRoot "delete-image-root-file.json"
$directoryImagePath = Join-Path $runRoot "directory-created.apfs"
$directoryCreateReportPath = Join-Path $runRoot "create-image-root-directory.json"
$directoryFilePayloadPath = Join-Path $runRoot "directory-file-payload.bin"
$directoryFilePatchPayloadPath = Join-Path $runRoot "directory-file-patch-payload.bin"
$directoryFilePatchedExpectedPayloadPath = Join-Path $runRoot "directory-file-patched-expected.bin"
$directoryFileImagePath = Join-Path $runRoot "directory-file-written.apfs"
$directoryFileWriteReportPath = Join-Path $runRoot "write-image-root-directory-file.json"
$directoryFilePatchedImagePath = Join-Path $runRoot "directory-file-patched.apfs"
$directoryFilePatchReportPath = Join-Path $runRoot "patch-image-root-directory-file.json"
$nonEmptyDirectoryDeleteReportPath = Join-Path $runRoot "non-empty-delete-image-root-directory.json"
$directoryFileDeletedImagePath = Join-Path $runRoot "directory-file-deleted.apfs"
$directoryFileDeleteReportPath = Join-Path $runRoot "delete-image-root-directory-file.json"
$directoryDeletedImagePath = Join-Path $runRoot "directory-deleted.apfs"
$directoryDeleteReportPath = Join-Path $runRoot "delete-image-root-directory.json"
$missingDirectoryDeleteReportPath = Join-Path $runRoot "missing-delete-image-root-directory.json"
$missingDeleteReportPath = Join-Path $runRoot "missing-delete-image-root-file.json"
$blockedRawWriteReportPath = Join-Path $runRoot "blocked-raw-write.json"
$blockedRawPatchReportPath = Join-Path $runRoot "blocked-raw-patch.json"
$blockedRawDeleteReportPath = Join-Path $runRoot "blocked-raw-delete.json"
$blockedRawDirectoryFileWriteReportPath = Join-Path $runRoot "blocked-raw-directory-file-write.json"
$blockedRawDirectoryFilePatchReportPath = Join-Path $runRoot "blocked-raw-directory-file-patch.json"
$blockedRawDirectoryFileDeleteReportPath = Join-Path $runRoot "blocked-raw-directory-file-delete.json"
$blockedRawDirectoryCreateReportPath = Join-Path $runRoot "blocked-raw-directory-create.json"
$blockedRawDirectoryDeleteReportPath = Join-Path $runRoot "blocked-raw-directory-delete.json"
$blockedRawLabelReportPath = Join-Path $runRoot "blocked-raw-volume-label.json"
$blockedReportPath = Join-Path $runRoot "blocked-raw-repair.json"

& $CliPath format-image `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --volume-name "SAK CLI APFS" `
    --output-json $formatReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "format-image exited $LASTEXITCODE"
}
$formatReport = Read-Report $formatReportPath
if (-not $formatReport.ok) {
    Fail "format-image report not ok: $($formatReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
    Fail "format-image did not create image"
}
if ((Get-Item -LiteralPath $imagePath).Length -ne $sizeBytes) {
    Fail "format-image size mismatch"
}

& $CliPath change-image-volume-label `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --output-image $relabeledImagePath `
    --volume-name "SAK CLI Relabeled" `
    --output-json $relabeledReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "change-image-volume-label exited $LASTEXITCODE"
}
$relabeledReport = Read-Report $relabeledReportPath
if (-not $relabeledReport.ok) {
    Fail "change-image-volume-label report not ok: $($relabeledReport.blockers -join '; ')"
}
if ($relabeledReport.old_volume_name -ne "SAK CLI APFS" -or $relabeledReport.new_volume_name -ne "SAK CLI Relabeled") {
    Fail "change-image-volume-label name report mismatch"
}
if (-not (Test-Path -LiteralPath $relabeledImagePath -PathType Leaf)) {
    Fail "change-image-volume-label did not create output"
}
$imagePath = $relabeledImagePath

[byte[]]$payloadBytes = for ($i = 0; $i -lt 9000; $i++) { [byte]($i % 251) }
[System.IO.File]::WriteAllBytes($payloadPath, $payloadBytes)
$payloadHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
& $CliPath write-image-root-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --output-image $writtenImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $payloadPath `
    --output-json $writeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "write-image-root-file exited $LASTEXITCODE"
}
$writeReport = Read-Report $writeReportPath
if (-not $writeReport.ok) {
    Fail "write-image-root-file report not ok: $($writeReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $writtenImagePath -PathType Leaf)) {
    Fail "write-image-root-file did not create output"
}
if ($writeReport.payload_sha256 -ne $payloadHash -or $writeReport.readback_sha256 -ne $payloadHash) {
    Fail "write-image-root-file hash/read-back mismatch"
}
if ([int64]$writeReport.written_data_blocks -lt 2) {
    Fail "write-image-root-file did not span expected multiple data blocks"
}

[byte[]]$replacementBytes = for ($i = 0; $i -lt 5123; $i++) { [byte](255 - ($i % 251)) }
[System.IO.File]::WriteAllBytes($replacementPayloadPath, $replacementBytes)
$replacementHash = (Get-FileHash -LiteralPath $replacementPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
& $CliPath write-image-root-file `
    --target $writtenImagePath `
    --size-bytes $sizeBytes `
    --output-image $replacedImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $replacementPayloadPath `
    --output-json $replaceReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace write-image-root-file exited $LASTEXITCODE"
}
$replaceReport = Read-Report $replaceReportPath
if (-not $replaceReport.ok) {
    Fail "replace write-image-root-file report not ok: $($replaceReport.blockers -join '; ')"
}
if ($replaceReport.payload_sha256 -ne $replacementHash -or $replaceReport.readback_sha256 -ne $replacementHash) {
    Fail "replace write-image-root-file hash/read-back mismatch"
}

[byte[]]$patchBytes = for ($i = 0; $i -lt 257; $i++) { [byte](65 + ($i % 26)) }
[System.IO.File]::WriteAllBytes($patchPayloadPath, $patchBytes)
$patchHash = (Get-FileHash -LiteralPath $patchPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
[byte[]]$patchedBytes = [byte[]]::new($replacementBytes.Length)
[System.Array]::Copy($replacementBytes, $patchedBytes, $replacementBytes.Length)
$patchOffset = 321
[System.Array]::Copy($patchBytes, 0, $patchedBytes, $patchOffset, $patchBytes.Length)
[System.IO.File]::WriteAllBytes($patchedExpectedPayloadPath, $patchedBytes)
$patchedHash = (Get-FileHash -LiteralPath $patchedExpectedPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
& $CliPath patch-image-root-file `
    --target $replacedImagePath `
    --size-bytes $sizeBytes `
    --output-image $patchedImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes $patchOffset `
    --output-json $patchReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "patch-image-root-file exited $LASTEXITCODE"
}
$patchReport = Read-Report $patchReportPath
if (-not $patchReport.ok) {
    Fail "patch-image-root-file report not ok: $($patchReport.blockers -join '; ')"
}
if ($patchReport.patch_sha256 -ne $patchHash -or $patchReport.readback_sha256 -ne $patchedHash) {
    Fail "patch-image-root-file hash/read-back mismatch"
}
if ([int64]$patchReport.patch_offset_bytes -ne $patchOffset -or [int64]$patchReport.patch_bytes -ne $patchBytes.Length) {
    Fail "patch-image-root-file range report mismatch"
}

& $CliPath delete-image-root-file `
    --target $patchedImagePath `
    --size-bytes $sizeBytes `
    --output-image $deletedImagePath `
    --file-name "cli-proof.bin" `
    --output-json $deleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-image-root-file exited $LASTEXITCODE"
}
$deleteReport = Read-Report $deleteReportPath
if (-not $deleteReport.ok) {
    Fail "delete-image-root-file report not ok: $($deleteReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $deletedImagePath -PathType Leaf)) {
    Fail "delete-image-root-file did not create output"
}
if ($deleteReport.deleted_file_sha256 -ne $patchedHash) {
    Fail "delete-image-root-file deleted hash mismatch"
}
if ([int64]$deleteReport.freed_data_blocks -lt 1) {
    Fail "delete-image-root-file did not report freed blocks"
}

& $CliPath create-image-root-directory `
    --target $deletedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryImagePath `
    --directory-name "Cli Proof Folder" `
    --output-json $directoryCreateReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "create-image-root-directory exited $LASTEXITCODE"
}
$directoryCreateReport = Read-Report $directoryCreateReportPath
if (-not $directoryCreateReport.ok) {
    Fail "create-image-root-directory report not ok: $($directoryCreateReport.blockers -join '; ')"
}
if ($directoryCreateReport.directory_name -ne "Cli Proof Folder") {
    Fail "create-image-root-directory did not report target directory"
}
if (-not (Test-Path -LiteralPath $directoryImagePath -PathType Leaf)) {
    Fail "create-image-root-directory did not create output"
}

[byte[]]$directoryFileBytes = for ($i = 0; $i -lt 1234; $i++) { [byte](33 + ($i % 90)) }
[System.IO.File]::WriteAllBytes($directoryFilePayloadPath, $directoryFileBytes)
$directoryFileHash = (Get-FileHash -LiteralPath $directoryFilePayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
& $CliPath write-image-root-directory-file `
    --target $directoryImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFileImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --payload-file $directoryFilePayloadPath `
    --output-json $directoryFileWriteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "write-image-root-directory-file exited $LASTEXITCODE"
}
$directoryFileWriteReport = Read-Report $directoryFileWriteReportPath
if (-not $directoryFileWriteReport.ok) {
    Fail "write-image-root-directory-file report not ok: $($directoryFileWriteReport.blockers -join '; ')"
}
if ($directoryFileWriteReport.directory_name -ne "Cli Proof Folder" -or $directoryFileWriteReport.file_name -ne "child-proof.bin") {
    Fail "write-image-root-directory-file target names mismatch"
}
if ($directoryFileWriteReport.payload_sha256 -ne $directoryFileHash -or $directoryFileWriteReport.readback_sha256 -ne $directoryFileHash) {
    Fail "write-image-root-directory-file hash/read-back mismatch"
}

[byte[]]$directoryFilePatchBytes = [System.Text.Encoding]::UTF8.GetBytes("CHILD-PATCH")
[System.IO.File]::WriteAllBytes($directoryFilePatchPayloadPath, $directoryFilePatchBytes)
$directoryFilePatchHash = (Get-FileHash -LiteralPath $directoryFilePatchPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
$directoryFilePatchOffset = 128
[byte[]]$directoryFilePatchedBytes = [byte[]]::new($directoryFileBytes.Length)
[System.Array]::Copy($directoryFileBytes, $directoryFilePatchedBytes, $directoryFileBytes.Length)
[System.Array]::Copy($directoryFilePatchBytes, 0, $directoryFilePatchedBytes, $directoryFilePatchOffset, $directoryFilePatchBytes.Length)
[System.IO.File]::WriteAllBytes($directoryFilePatchedExpectedPayloadPath, $directoryFilePatchedBytes)
$directoryFilePatchedHash = (Get-FileHash -LiteralPath $directoryFilePatchedExpectedPayloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
& $CliPath patch-image-root-directory-file `
    --target $directoryFileImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFilePatchedImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --payload-file $directoryFilePatchPayloadPath `
    --patch-offset-bytes $directoryFilePatchOffset `
    --output-json $directoryFilePatchReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "patch-image-root-directory-file exited $LASTEXITCODE"
}
$directoryFilePatchReport = Read-Report $directoryFilePatchReportPath
if (-not $directoryFilePatchReport.ok) {
    Fail "patch-image-root-directory-file report not ok: $($directoryFilePatchReport.blockers -join '; ')"
}
if ($directoryFilePatchReport.directory_name -ne "Cli Proof Folder" -or $directoryFilePatchReport.file_name -ne "child-proof.bin") {
    Fail "patch-image-root-directory-file target names mismatch"
}
if ($directoryFilePatchReport.patch_sha256 -ne $directoryFilePatchHash -or $directoryFilePatchReport.readback_sha256 -ne $directoryFilePatchedHash) {
    Fail "patch-image-root-directory-file hash/read-back mismatch"
}
if ([int64]$directoryFilePatchReport.patch_offset_bytes -ne $directoryFilePatchOffset -or [int64]$directoryFilePatchReport.patch_bytes -ne $directoryFilePatchBytes.Length) {
    Fail "patch-image-root-directory-file range report mismatch"
}

& $CliPath delete-image-root-directory `
    --target $directoryFilePatchedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "non-empty-directory-delete.apfs") `
    --directory-name "Cli Proof Folder" `
    --output-json $nonEmptyDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-image-root-directory accepted non-empty root directory"
}
$nonEmptyDirectoryDeleteReport = Read-Report $nonEmptyDirectoryDeleteReportPath
if ($nonEmptyDirectoryDeleteReport.ok) {
    Fail "non-empty delete-image-root-directory reported ok"
}
if (-not ([string]::Join(" ", @($nonEmptyDirectoryDeleteReport.blockers))).Contains("empty")) {
    Fail "non-empty delete-image-root-directory did not explain empty-directory guard"
}

& $CliPath delete-image-root-directory-file `
    --target $directoryFilePatchedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFileDeletedImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --output-json $directoryFileDeleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-image-root-directory-file exited $LASTEXITCODE"
}
$directoryFileDeleteReport = Read-Report $directoryFileDeleteReportPath
if (-not $directoryFileDeleteReport.ok) {
    Fail "delete-image-root-directory-file report not ok: $($directoryFileDeleteReport.blockers -join '; ')"
}
if ($directoryFileDeleteReport.deleted_file_sha256 -ne $directoryFilePatchedHash) {
    Fail "delete-image-root-directory-file deleted hash mismatch"
}

& $CliPath delete-image-root-directory `
    --target $directoryFileDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryDeletedImagePath `
    --directory-name "Cli Proof Folder" `
    --output-json $directoryDeleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "delete-image-root-directory exited $LASTEXITCODE"
}
$directoryDeleteReport = Read-Report $directoryDeleteReportPath
if (-not $directoryDeleteReport.ok) {
    Fail "delete-image-root-directory report not ok: $($directoryDeleteReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $directoryDeletedImagePath -PathType Leaf)) {
    Fail "delete-image-root-directory did not create output"
}

& $CliPath delete-image-root-directory `
    --target $directoryDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "missing-directory-delete.apfs") `
    --directory-name "Cli Proof Folder" `
    --output-json $missingDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-image-root-directory accepted missing root directory"
}
$missingDirectoryDeleteReport = Read-Report $missingDirectoryDeleteReportPath
if ($missingDirectoryDeleteReport.ok) {
    Fail "missing delete-image-root-directory reported ok"
}
if (-not ([string]::Join(" ", @($missingDirectoryDeleteReport.blockers))).Contains("not found")) {
    Fail "missing delete-image-root-directory did not explain missing target"
}

& $CliPath delete-image-root-file `
    --target $directoryDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "missing-delete.apfs") `
    --file-name "cli-proof.bin" `
    --output-json $missingDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-image-root-file accepted missing root file"
}
$missingDeleteReport = Read-Report $missingDeleteReportPath
if ($missingDeleteReport.ok) {
    Fail "missing delete-image-root-file reported ok"
}
if (-not ([string]::Join(" ", @($missingDeleteReport.blockers))).Contains("not found")) {
    Fail "missing delete-image-root-file did not explain missing target"
}

$stream = [System.IO.File]::Open($imagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $stream.Seek(9 * 4096, [System.IO.SeekOrigin]::Begin) | Out-Null
    $stream.Write(([byte[]](0, 0, 0, 0, 0, 0, 0, 0)), 0, 8)
    $stream.Flush()
} finally {
    $stream.Dispose()
}

& $CliPath repair-image `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --output-image $repairImagePath `
    --output-json $repairReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "repair-image exited $LASTEXITCODE"
}
$repairReport = Read-Report $repairReportPath
if (-not $repairReport.ok) {
    Fail "repair-image report not ok: $($repairReport.blockers -join '; ')"
}
if (-not (Test-Path -LiteralPath $repairImagePath -PathType Leaf)) {
    Fail "repair-image did not create output"
}

& $CliPath write-raw-root-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --file-name "raw-blocked.bin" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawWriteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "write-raw-root-file accepted a normal file path"
}
$blockedRawWriteReport = Read-Report $blockedRawWriteReportPath
if ($blockedRawWriteReport.ok) {
    Fail "blocked raw write reported ok"
}
$blockedRawWriteText = [string]::Join(" ", @($blockedRawWriteReport.blockers))
if (-not $blockedRawWriteText.Contains("Windows raw-device path")) {
    Fail "blocked raw write did not explain raw-device guard"
}

& $CliPath patch-raw-root-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --file-name "raw-blocked.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 0 `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawPatchReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "patch-raw-root-file accepted a normal file path"
}
$blockedRawPatchReport = Read-Report $blockedRawPatchReportPath
if ($blockedRawPatchReport.ok) {
    Fail "blocked raw patch reported ok"
}
$blockedRawPatchText = [string]::Join(" ", @($blockedRawPatchReport.blockers))
if (-not $blockedRawPatchText.Contains("Windows raw-device path")) {
    Fail "blocked raw patch did not explain raw-device guard"
}

& $CliPath delete-raw-root-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --file-name "raw-blocked.bin" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-raw-root-file accepted a normal file path"
}
$blockedRawDeleteReport = Read-Report $blockedRawDeleteReportPath
if ($blockedRawDeleteReport.ok) {
    Fail "blocked raw delete reported ok"
}
$blockedRawDeleteText = [string]::Join(" ", @($blockedRawDeleteReport.blockers))
if (-not $blockedRawDeleteText.Contains("Windows raw-device path")) {
    Fail "blocked raw delete did not explain raw-device guard"
}

& $CliPath write-raw-root-directory-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --file-name "raw-child-blocked.bin" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFileWriteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "write-raw-root-directory-file accepted a normal file path"
}
$blockedRawDirectoryFileWriteReport = Read-Report $blockedRawDirectoryFileWriteReportPath
if ($blockedRawDirectoryFileWriteReport.ok) {
    Fail "blocked raw directory-file write reported ok"
}
$blockedRawDirectoryFileWriteText = [string]::Join(" ", @($blockedRawDirectoryFileWriteReport.blockers))
if (-not $blockedRawDirectoryFileWriteText.Contains("Windows raw-device path")) {
    Fail "blocked raw directory-file write did not explain raw-device guard"
}

& $CliPath patch-raw-root-directory-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --file-name "raw-child-blocked.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 0 `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFilePatchReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "patch-raw-root-directory-file accepted a normal file path"
}
$blockedRawDirectoryFilePatchReport = Read-Report $blockedRawDirectoryFilePatchReportPath
if ($blockedRawDirectoryFilePatchReport.ok) {
    Fail "blocked raw directory-file patch reported ok"
}
$blockedRawDirectoryFilePatchText = [string]::Join(" ", @($blockedRawDirectoryFilePatchReport.blockers))
if (-not $blockedRawDirectoryFilePatchText.Contains("Windows raw-device path")) {
    Fail "blocked raw directory-file patch did not explain raw-device guard"
}

& $CliPath delete-raw-root-directory-file `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --file-name "raw-child-blocked.bin" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFileDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-raw-root-directory-file accepted a normal file path"
}
$blockedRawDirectoryFileDeleteReport = Read-Report $blockedRawDirectoryFileDeleteReportPath
if ($blockedRawDirectoryFileDeleteReport.ok) {
    Fail "blocked raw directory-file delete reported ok"
}
$blockedRawDirectoryFileDeleteText = [string]::Join(" ", @($blockedRawDirectoryFileDeleteReport.blockers))
if (-not $blockedRawDirectoryFileDeleteText.Contains("Windows raw-device path")) {
    Fail "blocked raw directory-file delete did not explain raw-device guard"
}

& $CliPath create-raw-root-directory `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryCreateReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "create-raw-root-directory accepted a normal file path"
}
$blockedRawDirectoryCreateReport = Read-Report $blockedRawDirectoryCreateReportPath
if ($blockedRawDirectoryCreateReport.ok) {
    Fail "blocked raw directory create reported ok"
}
$blockedRawDirectoryCreateText = [string]::Join(" ", @($blockedRawDirectoryCreateReport.blockers))
if (-not $blockedRawDirectoryCreateText.Contains("Windows raw-device path")) {
    Fail "blocked raw directory create did not explain raw-device guard"
}

& $CliPath delete-raw-root-directory `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "delete-raw-root-directory accepted a normal file path"
}
$blockedRawDirectoryDeleteReport = Read-Report $blockedRawDirectoryDeleteReportPath
if ($blockedRawDirectoryDeleteReport.ok) {
    Fail "blocked raw directory delete reported ok"
}
$blockedRawDirectoryDeleteText = [string]::Join(" ", @($blockedRawDirectoryDeleteReport.blockers))
if (-not $blockedRawDirectoryDeleteText.Contains("Windows raw-device path")) {
    Fail "blocked raw directory delete did not explain raw-device guard"
}

& $CliPath change-raw-volume-label `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --volume-name "Raw Blocked Label" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawLabelReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "change-raw-volume-label accepted a normal file path"
}
$blockedRawLabelReport = Read-Report $blockedRawLabelReportPath
if ($blockedRawLabelReport.ok) {
    Fail "blocked raw volume label reported ok"
}
$blockedRawLabelText = [string]::Join(" ", @($blockedRawLabelReport.blockers))
if (-not $blockedRawLabelText.Contains("Windows raw-device path")) {
    Fail "blocked raw volume label did not explain raw-device guard"
}

& $CliPath repair-raw `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "repair-raw accepted a normal file path"
}
$blockedReport = Read-Report $blockedReportPath
if ($blockedReport.ok) {
    Fail "blocked raw repair reported ok"
}
$blockerText = [string]::Join(" ", @($blockedReport.blockers))
if (-not $blockerText.Contains("Windows raw-device path")) {
    Fail "blocked raw repair did not explain raw-device guard"
}

Write-Host "sak_apfs_writer_cli self-test passed: $runRoot"
