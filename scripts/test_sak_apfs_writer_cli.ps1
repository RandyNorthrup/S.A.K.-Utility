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

function Test-HasProperty {
    param([object]$Object, [string]$Name)
    return @($Object.PSObject.Properties.Name) -contains $Name
}

# Shared assertion for a successful copy-on-write (COW) commit report.
# The COW commit-* reports expose: ok, operation, source_image/target,
# output_image, previous_xid, new_xid, blockers. They intentionally do NOT
# expose payload/read-back hashes, data-block counts or patch ranges (byte
# correctness is proven by the C++ unit tests, and the CLI has no export verb).
function Assert-CommitOk {
    param(
        [object]$Report,
        [string]$Label,
        [string]$OutputImage = ""
    )
    if (-not $Report.ok) {
        Fail "$Label report not ok: $($Report.blockers -join '; ')"
    }
    if ($OutputImage -ne "" -and -not (Test-Path -LiteralPath $OutputImage -PathType Leaf)) {
        Fail "$Label did not create output image"
    }
    if ((Test-HasProperty $Report "previous_xid") -and (Test-HasProperty $Report "new_xid")) {
        if ([int64]$Report.new_xid -le [int64]$Report.previous_xid) {
            Fail "$Label new_xid ($($Report.new_xid)) did not advance past previous_xid ($($Report.previous_xid))"
        }
    }
}

# Shared assertion for a fail-closed report (negative test).
function Assert-Blocked {
    param(
        [object]$Report,
        [string]$Label,
        [string]$ExpectSubstring
    )
    if ($Report.ok) {
        Fail "$Label reported ok"
    }
    $text = [string]::Join(" ", @($Report.blockers))
    if ($ExpectSubstring -ne "" -and -not $text.Contains($ExpectSubstring)) {
        Fail "$Label did not explain guard (blockers: $text)"
    }
    if (@($Report.blockers).Count -lt 1) {
        Fail "$Label did not report a blocker"
    }
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
$corruptImagePath = Join-Path $runRoot "corrupt.apfs"
$relabeledImagePath = Join-Path $runRoot "relabeled.apfs"
$relabeledReportPath = Join-Path $runRoot "change-image-volume-label.json"
$payloadPath = Join-Path $runRoot "payload.bin"
$replacementPayloadPath = Join-Path $runRoot "replacement-payload.bin"
$patchPayloadPath = Join-Path $runRoot "patch-payload.bin"
$writtenImagePath = Join-Path $runRoot "written.apfs"
$writeReportPath = Join-Path $runRoot "commit-image-file-write.json"
$replacedImagePath = Join-Path $runRoot "replaced.apfs"
$replaceReportPath = Join-Path $runRoot "commit-image-file-write-replace.json"
$patchedImagePath = Join-Path $runRoot "patched.apfs"
$patchReportPath = Join-Path $runRoot "commit-image-file-patch.json"
$deletedImagePath = Join-Path $runRoot "deleted.apfs"
$deleteReportPath = Join-Path $runRoot "commit-image-file-delete.json"
$directoryImagePath = Join-Path $runRoot "directory-created.apfs"
$directoryCreateReportPath = Join-Path $runRoot "commit-image-directory-create.json"
$directoryFilePayloadPath = Join-Path $runRoot "directory-file-payload.bin"
$directoryFilePatchPayloadPath = Join-Path $runRoot "directory-file-patch-payload.bin"
$directoryFileImagePath = Join-Path $runRoot "directory-file-written.apfs"
$directoryFileWriteReportPath = Join-Path $runRoot "commit-image-directory-child-write.json"
$directoryFilePatchedImagePath = Join-Path $runRoot "directory-file-patched.apfs"
$directoryFilePatchReportPath = Join-Path $runRoot "commit-image-directory-child-patch.json"
$nonEmptyDirectoryDeleteReportPath = Join-Path $runRoot "non-empty-delete-image-directory.json"
$directoryFileDeletedImagePath = Join-Path $runRoot "directory-file-deleted.apfs"
$directoryFileDeleteReportPath = Join-Path $runRoot "commit-image-directory-child-delete.json"
$directoryDeletedImagePath = Join-Path $runRoot "directory-deleted.apfs"
$directoryDeleteReportPath = Join-Path $runRoot "commit-image-directory-delete.json"
$missingDirectoryDeleteReportPath = Join-Path $runRoot "missing-delete-image-directory.json"
$missingDeleteReportPath = Join-Path $runRoot "missing-delete-image-file.json"
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

$rawGuardSubstring = "Windows raw-device path"

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

# The plain generated container (repairable minimal layout) is retained for the
# repair-image proof below; the write chain runs on a relabeled copy.
$formatImagePath = $imagePath

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

# --- COW root-file write (create) ---
[byte[]]$payloadBytes = for ($i = 0; $i -lt 9000; $i++) { [byte]($i % 251) }
[System.IO.File]::WriteAllBytes($payloadPath, $payloadBytes)
& $CliPath commit-image-file-write `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --output-image $writtenImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $payloadPath `
    --output-json $writeReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-file-write exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $writeReportPath) -Label "commit-image-file-write" -OutputImage $writtenImagePath

# --- COW root-file write (create-or-replace: second write replaces) ---
[byte[]]$replacementBytes = for ($i = 0; $i -lt 5123; $i++) { [byte](255 - ($i % 251)) }
[System.IO.File]::WriteAllBytes($replacementPayloadPath, $replacementBytes)
& $CliPath commit-image-file-write `
    --target $writtenImagePath `
    --size-bytes $sizeBytes `
    --output-image $replacedImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $replacementPayloadPath `
    --output-json $replaceReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "replace commit-image-file-write exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $replaceReportPath) -Label "commit-image-file-write (replace)" -OutputImage $replacedImagePath

# --- Negative: case-insensitive name collision is detected (P01-11) ---
# The writer's volume is APFS_INCOMPAT_CASE_INSENSITIVE, so 'Foo' and 'foo' hash
# to the same j_drec key. Inserting 'CaseFold' then 'casefold' must fail closed
# rather than emit a second drec with an identical key (fsroot b-tree corruption).
$caseInsertImagePath = Join-Path $runRoot "case-insert.apfs"
$caseInsertReportPath = Join-Path $runRoot "commit-image-file-insert-case.json"
& $CliPath commit-image-file-insert `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --output-image $caseInsertImagePath `
    --file-name "CaseFold" `
    --payload-file $payloadPath `
    --output-json $caseInsertReportPath
Assert-CommitOk -Report (Read-Report $caseInsertReportPath) -Label "commit-image-file-insert (CaseFold)" -OutputImage $caseInsertImagePath

$caseCollisionReportPath = Join-Path $runRoot "commit-image-file-insert-case-collision.json"
& $CliPath commit-image-file-insert `
    --target $caseInsertImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "case-collision.apfs") `
    --file-name "casefold" `
    --payload-file $payloadPath `
    --output-json $caseCollisionReportPath
Assert-Blocked -Report (Read-Report $caseCollisionReportPath) -Label "case-insensitive commit-image-file-insert collision" -ExpectSubstring "already exists"

# --- COW root-file patch (in range) ---
[byte[]]$patchBytes = for ($i = 0; $i -lt 257; $i++) { [byte](65 + ($i % 26)) }
[System.IO.File]::WriteAllBytes($patchPayloadPath, $patchBytes)
& $CliPath commit-image-file-patch `
    --target $replacedImagePath `
    --size-bytes $sizeBytes `
    --output-image $patchedImagePath `
    --file-name "cli-proof.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 321 `
    --output-json $patchReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-file-patch exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $patchReportPath) -Label "commit-image-file-patch" -OutputImage $patchedImagePath

# --- Negative: an out-of-range patch offset fails closed (P01-08) ---
# An offset near UINT64_MAX casts to a negative qsizetype and would drive an
# out-of-bounds heap write in applyBytePatch; it must be rejected up front.
$overflowPatchReportPath = Join-Path $runRoot "commit-image-file-patch-overflow.json"
& $CliPath commit-image-file-patch `
    --target $patchedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "patch-overflow.apfs") `
    --file-name "cli-proof.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 18446744073709551615 `
    --output-json $overflowPatchReportPath
Assert-Blocked -Report (Read-Report $overflowPatchReportPath) -Label "overflow commit-image-file-patch" -ExpectSubstring "maximum supported"

# --- COW root-file delete ---
& $CliPath commit-image-file-delete `
    --target $patchedImagePath `
    --size-bytes $sizeBytes `
    --output-image $deletedImagePath `
    --file-name "cli-proof.bin" `
    --output-json $deleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-file-delete exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $deleteReportPath) -Label "commit-image-file-delete" -OutputImage $deletedImagePath

# --- COW root-directory create ---
& $CliPath commit-image-directory-create `
    --target $deletedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryImagePath `
    --directory-name "Cli Proof Folder" `
    --output-json $directoryCreateReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-directory-create exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $directoryCreateReportPath) -Label "commit-image-directory-create" -OutputImage $directoryImagePath

# --- COW directory-child write ---
[byte[]]$directoryFileBytes = for ($i = 0; $i -lt 1234; $i++) { [byte](33 + ($i % 90)) }
[System.IO.File]::WriteAllBytes($directoryFilePayloadPath, $directoryFileBytes)
& $CliPath commit-image-directory-child-write `
    --target $directoryImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFileImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --payload-file $directoryFilePayloadPath `
    --output-json $directoryFileWriteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-directory-child-write exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $directoryFileWriteReportPath) -Label "commit-image-directory-child-write" -OutputImage $directoryFileImagePath

# --- COW directory-child patch (commit-image-file-patch with --directory-name) ---
[byte[]]$directoryFilePatchBytes = [System.Text.Encoding]::UTF8.GetBytes("CHILD-PATCH")
[System.IO.File]::WriteAllBytes($directoryFilePatchPayloadPath, $directoryFilePatchBytes)
& $CliPath commit-image-file-patch `
    --target $directoryFileImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFilePatchedImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --payload-file $directoryFilePatchPayloadPath `
    --patch-offset-bytes 128 `
    --output-json $directoryFilePatchReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-file-patch (directory child) exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $directoryFilePatchReportPath) -Label "commit-image-file-patch (directory child)" -OutputImage $directoryFilePatchedImagePath

# --- Negative: delete of a NON-empty directory still fails closed ---
& $CliPath commit-image-directory-delete `
    --target $directoryFilePatchedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "non-empty-directory-delete.apfs") `
    --directory-name "Cli Proof Folder" `
    --output-json $nonEmptyDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-image-directory-delete accepted non-empty directory"
}
Assert-Blocked -Report (Read-Report $nonEmptyDirectoryDeleteReportPath) -Label "non-empty commit-image-directory-delete" -ExpectSubstring "empty"

# --- COW directory-child delete ---
& $CliPath commit-image-directory-child-delete `
    --target $directoryFilePatchedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryFileDeletedImagePath `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --output-json $directoryFileDeleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-directory-child-delete exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $directoryFileDeleteReportPath) -Label "commit-image-directory-child-delete" -OutputImage $directoryFileDeletedImagePath

# --- COW directory delete (now empty) ---
& $CliPath commit-image-directory-delete `
    --target $directoryFileDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image $directoryDeletedImagePath `
    --directory-name "Cli Proof Folder" `
    --output-json $directoryDeleteReportPath
if ($LASTEXITCODE -ne 0) {
    Fail "commit-image-directory-delete exited $LASTEXITCODE"
}
Assert-CommitOk -Report (Read-Report $directoryDeleteReportPath) -Label "commit-image-directory-delete" -OutputImage $directoryDeletedImagePath

# --- Negative: delete of a missing directory fails closed ---
& $CliPath commit-image-directory-delete `
    --target $directoryDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "missing-directory-delete.apfs") `
    --directory-name "Cli Proof Folder" `
    --output-json $missingDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-image-directory-delete accepted missing directory"
}
Assert-Blocked -Report (Read-Report $missingDirectoryDeleteReportPath) -Label "missing commit-image-directory-delete" -ExpectSubstring "not found"

# --- Negative: delete of a missing file fails closed ---
& $CliPath commit-image-file-delete `
    --target $directoryDeletedImagePath `
    --size-bytes $sizeBytes `
    --output-image (Join-Path $runRoot "missing-delete.apfs") `
    --file-name "cli-proof.bin" `
    --output-json $missingDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-image-file-delete accepted missing file"
}
Assert-Blocked -Report (Read-Report $missingDeleteReportPath) -Label "missing commit-image-file-delete" -ExpectSubstring "not found"

# --- repair-image: corrupt an object checksum in the plain generated container and repair it ---
Copy-Item -LiteralPath $formatImagePath -Destination $corruptImagePath -Force
$stream = [System.IO.File]::Open($corruptImagePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
try {
    $stream.Seek(9 * 4096, [System.IO.SeekOrigin]::Begin) | Out-Null
    $stream.Write(([byte[]](0, 0, 0, 0, 0, 0, 0, 0)), 0, 8)
    $stream.Flush()
} finally {
    $stream.Dispose()
}

& $CliPath repair-image `
    --target $corruptImagePath `
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

# --- Negative: every commit-raw-* mutation refuses a normal (non raw-device) file path ---
# patch / child-write / child-delete validate target existence before the raw
# guard, so those negatives run against images that already contain the target
# file/directory; all raw commits then fail closed with the raw-device guard.
& $CliPath commit-raw-file-write `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --file-name "raw-blocked.bin" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawWriteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-file-write accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawWriteReportPath) -Label "blocked commit-raw-file-write" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-file-patch `
    --target $writtenImagePath `
    --size-bytes $sizeBytes `
    --file-name "cli-proof.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 0 `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawPatchReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-file-patch accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawPatchReportPath) -Label "blocked commit-raw-file-patch" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-file-delete `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --file-name "raw-blocked.bin" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-file-delete accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDeleteReportPath) -Label "blocked commit-raw-file-delete" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-directory-child-write `
    --target $directoryImagePath `
    --size-bytes $sizeBytes `
    --directory-name "Cli Proof Folder" `
    --file-name "raw-child-blocked.bin" `
    --payload-file $payloadPath `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFileWriteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-directory-child-write accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDirectoryFileWriteReportPath) -Label "blocked commit-raw-directory-child-write" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-file-patch `
    --target $directoryFileImagePath `
    --size-bytes $sizeBytes `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --payload-file $patchPayloadPath `
    --patch-offset-bytes 0 `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFilePatchReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-file-patch (directory child) accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDirectoryFilePatchReportPath) -Label "blocked commit-raw-file-patch (directory child)" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-directory-child-delete `
    --target $directoryFileImagePath `
    --size-bytes $sizeBytes `
    --directory-name "Cli Proof Folder" `
    --file-name "child-proof.bin" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryFileDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-directory-child-delete accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDirectoryFileDeleteReportPath) -Label "blocked commit-raw-directory-child-delete" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-directory-create `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryCreateReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-directory-create accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDirectoryCreateReportPath) -Label "blocked commit-raw-directory-create" -ExpectSubstring $rawGuardSubstring

& $CliPath commit-raw-directory-delete `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --directory-name "Raw Blocked Folder" `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedRawDirectoryDeleteReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "commit-raw-directory-delete accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedRawDirectoryDeleteReportPath) -Label "blocked commit-raw-directory-delete" -ExpectSubstring $rawGuardSubstring

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
Assert-Blocked -Report (Read-Report $blockedRawLabelReportPath) -Label "blocked change-raw-volume-label" -ExpectSubstring $rawGuardSubstring

& $CliPath repair-raw `
    --target $imagePath `
    --size-bytes $sizeBytes `
    --confirm-target `
    --allow-raw-target `
    --output-json $blockedReportPath
if ($LASTEXITCODE -eq 0) {
    Fail "repair-raw accepted a normal file path"
}
Assert-Blocked -Report (Read-Report $blockedReportPath) -Label "blocked repair-raw" -ExpectSubstring $rawGuardSubstring

Write-Host "sak_apfs_writer_cli self-test passed: $runRoot"
