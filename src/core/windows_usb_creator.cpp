// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_usb_creator.cpp
/// @brief Implements Windows USB installation media creation

#include "sak/windows_usb_creator.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>
#include <QTemporaryFile>
#include <QThread>

namespace {

constexpr int kFormatVerificationProgress = 5;
constexpr int kDriveRecognitionWaitIterations = 30;
constexpr int kDriveRecognitionProgressSpan = 5;
constexpr int kNtfsVerificationProgress = 10;
constexpr int kExtractionPreparationProgress = 13;
constexpr int kExtractionVerifiedProgress = 60;
constexpr int kBootConfigurationStartProgress = 62;
constexpr int kBootConfigurationCompleteProgress = 70;
constexpr int kBootFlagVerifiedProgress = 85;

// Require the target disk to be at least this multiple of the ISO size BEFORE any
// destructive clean, matching the post-format workspace check so an undersized
// disk is rejected up front rather than erased and only then failed.
constexpr qint64 kWorkspaceSizeMultiplier = 2;

}  // namespace

WindowsUSBCreator::WindowsUSBCreator(QObject* parent) : QObject(parent) {}

WindowsUSBCreator::~WindowsUSBCreator() {}

bool WindowsUSBCreator::createBootableUSB(const QString& isoPath, const QString& diskNumber) {
    // Both arguments are validated by validateUSBInputs below, which reports the
    // failure through setError/failed(). They are not asserted: an assert would
    // abort a Debug build on input that Release rejects cleanly.

    // Single-flight guard: this method mutates per-run instance state (m_diskNumber,
    // m_targetDiskUniqueId, m_isoSizeBytes, m_isoModifiedMs, m_volumeLabel) that is
    // neither atomic nor mutex-guarded, so a second concurrent call on the same instance
    // would race and could cross-wire destructive targets. Reject re-entry, fail closed.
    bool notRunning = false;
    if (!m_running.compare_exchange_strong(notRunning, true)) {
        setError("A USB creation is already in progress");
        Q_EMIT failed(lastError());
        return false;
    }
    struct RunningGuard {
        std::atomic<bool>& flag;
        ~RunningGuard() { flag.store(false); }
    } runningGuard{m_running};

    resetPerRunState(diskNumber);

    if (!validateUSBInputs(isoPath, diskNumber)) {
        return false;
    }

    // ==================== STEP 1: FORMAT ====================
    QString driveLetter = formatAndVerifyDrive(diskNumber);
    if (driveLetter.isEmpty()) {
        return false;
    }

    if (cancellationRequested()) {
        return false;
    }

    // ==================== STEP 2: EXTRACT ====================
    if (!extractAndVerifyFiles(isoPath, driveLetter)) {
        return false;
    }

    if (cancellationRequested()) {
        return false;
    }

    // ==================== STEPS 3-4: BOOT CONFIGURATION ====================
    if (!configureBootAndVerify(diskNumber, driveLetter)) {
        return false;
    }

    if (cancellationRequested()) {
        return false;
    }

    // ==================== STEP 5: FINAL VERIFICATION ====================
    if (!runFinalVerification(driveLetter)) {
        return false;
    }

    // SUCCESS - finalVerification() emits completed() signal
    sak::logInfo("========================================");
    sak::logInfo("ALL STEPS COMPLETED AND VERIFIED");
    sak::logInfo("========================================");
    return true;
}

void WindowsUSBCreator::resetPerRunState(const QString& diskNumber) {
    m_cancelled = false;
    setError({});
    m_diskNumber = diskNumber;
    // Reset per-run state so a prior run's volume label / pinned identity can
    // never leak into this run if extraction/probing fails early.
    m_volumeLabel.clear();
    m_targetDiskUniqueId.clear();
    m_isoSizeBytes = -1;
    m_isoModifiedMs = -1;
}

bool WindowsUSBCreator::cancellationRequested() {
    // Cancellation checkpoint between destructive steps. Report a cancel exactly as
    // the inline checks did -- setError + failed() -- and return true so the caller
    // bails and fails closed.
    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return true;
    }
    return false;
}

bool WindowsUSBCreator::runFinalVerification(const QString& driveLetter) {
    // STEP 5, the sole path to a verified success. finalVerification() emits
    // completed() on success; on failure report through failed() and fail closed.
    Q_EMIT statusChanged("Step 5/5: Running final comprehensive verification...");
    sak::logInfo("STEP 5: Final comprehensive verification...");

    if (!finalVerification(driveLetter)) {
        sak::logError("STEP 5 VERIFICATION FAILED");
        Q_EMIT failed(lastError());
        return false;
    }
    return true;
}

bool WindowsUSBCreator::validateUSBInputs(const QString& isoPath, const QString& diskNumber) {
    // This IS the validation, so it asserts nothing: an empty diskNumber fails the
    // integer regex below and an empty isoPath fails the existence check, both
    // fail-closed with an error the caller can see.
    // Validate diskNumber is a pure integer to prevent command injection.
    static const QRegularExpression diskNumRegex(QStringLiteral("^\\d{1,3}$"));
    if (!diskNumRegex.match(diskNumber).hasMatch()) {
        setError(QString("Invalid disk number format: %1").arg(diskNumber));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo(QString("========================================").toStdString());
    sak::logInfo(QString("Creating Windows bootable USB: %1 -> Disk %2")
                     .arg(isoPath, diskNumber)
                     .toStdString());
    sak::logInfo(QString("========================================").toStdString());

    // Verify ISO file exists
    if (!QFile::exists(isoPath)) {
        setError(QString("ISO file not found: %1").arg(isoPath));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    // Require a real regular file before wiping the target: a directory or special file must not
    // pass the existence check above and only fail after the disk has been cleaned/formatted.
    if (!QFileInfo(isoPath).isFile()) {
        setError(QString("ISO path is not a regular file: %1").arg(isoPath));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    // Engine-level safety gate: never DiskPart-clean the OS boot/system disk. This
    // also captures the target capacity into m_targetDiskSizeBytes.
    if (!guardTargetDiskSafe(diskNumber)) {
        return false;
    }

    // Capacity gate BEFORE any destructive DiskPart clean: an undersized disk must
    // be rejected here, not cleaned/formatted and only then failed at copy time.
    // Require the SAME 2x-ISO workspace the post-format check enforces, using an
    // overflow-safe division so the multiply can never wrap qint64. Fail closed if
    // capacity is unknown.
    const QFileInfo isoInfo(isoPath);
    const qint64 isoBytes = isoInfo.size();
    if (m_targetDiskSizeBytes <= 0 || isoBytes <= 0 ||
        isoBytes > m_targetDiskSizeBytes / kWorkspaceSizeMultiplier) {
        setError(QString("Target disk %1 (%2 bytes) is too small for the ISO (%3 bytes, needs %4x)")
                     .arg(diskNumber)
                     .arg(m_targetDiskSizeBytes)
                     .arg(isoBytes)
                     .arg(kWorkspaceSizeMultiplier));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    // Pin ISO size + mtime so a mid-operation swap of the user-selected ISO is
    // caught before the verification pass (TOCTOU immutability check).
    m_isoSizeBytes = isoBytes;
    m_isoModifiedMs = isoInfo.lastModified().toMSecsSinceEpoch();

    return true;
}

namespace {
struct DiskSafetyRow {
    bool isBoot = false;
    bool isSystem = false;
    bool isReadOnly = false;
    qint64 sizeBytes = -1;
    QString uniqueId;
};

// Parse a single "True"/"False" token (case-insensitive). Returns false if it is
// neither -- an unrecognized value must fail closed, never default to "safe".
bool parseTriBool(const QString& field, bool* value) {
    const QString t = field.trimmed();
    if (t.compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0) {
        *value = true;
        return true;
    }
    if (t.compare(QStringLiteral("False"), Qt::CaseInsensitive) == 0) {
        *value = false;
        return true;
    }
    return false;
}

// Parse the "IsBoot|IsSystem|IsReadOnly|Size|UniqueId" probe row. Fails closed
// (returns false) on a wrong field count, an unparseable flag, a non-positive
// size, or an empty UniqueId, so a short/malformed row can never be read as "not
// boot/system, safe to erase".
bool parseDiskSafetyRow(const QString& out, DiskSafetyRow& row) {
    constexpr int kDiskSafetyFieldCount = 5;
    constexpr int kFieldIsBoot = 0;
    constexpr int kFieldIsSystem = 1;
    constexpr int kFieldIsReadOnly = 2;
    constexpr int kFieldSizeBytes = 3;
    constexpr int kFieldUniqueId = 4;
    const QStringList parts = out.split(QLatin1Char('|'));
    if (parts.size() != kDiskSafetyFieldCount) {
        return false;
    }
    if (!parseTriBool(parts.at(kFieldIsBoot), &row.isBoot) ||
        !parseTriBool(parts.at(kFieldIsSystem), &row.isSystem) ||
        !parseTriBool(parts.at(kFieldIsReadOnly), &row.isReadOnly)) {
        return false;
    }
    bool sizeOk = false;
    row.sizeBytes = parts.at(kFieldSizeBytes).trimmed().toLongLong(&sizeOk);
    row.uniqueId = parts.at(kFieldUniqueId).trimmed();
    return sizeOk && row.sizeBytes > 0 && !row.uniqueId.isEmpty();
}

// Run the Get-Disk safety probe for @p diskNumber. Returns true and fills @p row
// ONLY on a clean exit with parseable output for a disk that is NOT boot/system/
// read-only. On any failure writes a human message to *err and returns false.
bool probeDiskSafety(const QString& diskNumber,
                     const sak::CancelCheck& cancel,
                     DiskSafetyRow& row,
                     QString* err) {
    const QString query = QString(
                              "try { $d = Get-Disk -Number %1 -ErrorAction Stop; "
                              "Write-Output ('{0}|{1}|{2}|{3}|{4}' -f "
                              "$d.IsBoot, $d.IsSystem, $d.IsReadOnly, $d.Size, $d.UniqueId) } "
                              "catch { Write-Output 'ERROR' }")
                              .arg(diskNumber);
    const auto result = sak::runPowerShell(query, sak::kTimeoutProcessShortMs, true, false, cancel);
    const QString out = result.std_out.trimmed();
    if (!result.completedSuccessfully() || out.isEmpty() || out == QStringLiteral("ERROR")) {
        *err = QString("Could not verify target disk %1 is safe to erase").arg(diskNumber);
        return false;
    }
    if (!parseDiskSafetyRow(out, row)) {
        *err = QString("Malformed/unverifiable disk-safety output for disk %1: '%2'")
                   .arg(diskNumber, out);
        return false;
    }
    if (row.isBoot || row.isSystem || row.isReadOnly) {
        *err = QString(
                   "Refusing to erase disk %1: it is the current OS boot/system disk or is "
                   "read-only")
                   .arg(diskNumber);
        return false;
    }
    return true;
}
}  // namespace

bool WindowsUSBCreator::guardTargetDiskSafe(const QString& diskNumber) {
    // diskNumber is already validated as a pure integer by the caller.
    m_targetDiskSizeBytes = -1;
    m_targetDiskUniqueId.clear();
    DiskSafetyRow row;
    QString err;
    if (!probeDiskSafety(diskNumber, [this]() { return m_cancelled.load(); }, row, &err)) {
        setError(err);
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }
    m_targetDiskSizeBytes = row.sizeBytes;
    m_targetDiskUniqueId = row.uniqueId;
    return true;
}

bool WindowsUSBCreator::reverifyTargetDiskIdentity(const QString& diskNumber) {
    // TOCTOU guard: a physical hot-plug/removal between guardTargetDiskSafe() and
    // this destructive clean can reassign Windows disk numbers. Re-probe now and
    // confirm the number still resolves to the SAME safe disk (identical UniqueId
    // and size) that was vetted -- otherwise fail closed rather than risk erasing
    // a different (possibly system) disk.
    DiskSafetyRow row;
    QString err;
    if (!probeDiskSafety(diskNumber, [this]() { return m_cancelled.load(); }, row, &err)) {
        setError(err);
        sak::logError(lastError().toStdString());
        return false;
    }
    if (row.uniqueId != m_targetDiskUniqueId || row.sizeBytes != m_targetDiskSizeBytes) {
        setError(QString("Target disk %1 identity changed since the safety check (hot-plug?) -- "
                         "refusing to erase")
                     .arg(diskNumber));
        sak::logError(lastError().toStdString());
        return false;
    }
    return true;
}

QString WindowsUSBCreator::system32ExePath(const QString& exeName) {
    // Resolve under the REAL system directory reported by GetSystemDirectoryW
    // (sak::system32Path), NEVER the %SystemRoot% environment variable: a poisoned
    // environment on an elevated launch must not be able to redirect diskpart.exe or
    // bcdboot.exe to an attacker-planted copy. Fail closed (empty) if the system
    // directory cannot be resolved -- never guess a default C:\Windows.
    const QString path = sak::system32Path(exeName);
    if (path.isEmpty()) {
        return {};
    }
    // Fail closed unless this is a real, existing regular file -- never a symlink/reparse point
    // that could redirect an elevated diskpart.exe/bcdboot.exe launch to attacker-planted code.
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        return {};
    }
    return QDir::toNativeSeparators(path);
}

bool WindowsUSBCreator::diskpartOutputIsError(const QString& output) {
    // diskpart commonly exits 0 even when an individual command failed, printing a
    // hard-failure marker to stdout. Treat those markers as failure (fail closed).
    // The list covers the selection failures ("There is no volume selected") and argument
    // rejections that leave the media unformatted while diskpart still exits 0 -- the three
    // original markers missed those entirely.
    static const QRegularExpression errorRe(
        QStringLiteral("DiskPart has encountered an error|Virtual Disk Service error|"
                       "Access is denied|There is no (disk|volume|partition) selected|"
                       "(is|are) not valid|DiskPart failed|could not complete the operation|"
                       "write protected|not convertible|not enough usable space|"
                       "The operation is not supported"),
        QRegularExpression::CaseInsensitiveOption);
    return errorRe.match(output).hasMatch();
}

bool WindowsUSBCreator::checkDiskpartResult(const sak::ProcessResult& result) {
    if (result.timed_out || result.cancelled) {
        setError("Diskpart timed out or was cancelled");
        sak::logError(lastError().toStdString());
        return false;
    }
    if (!result.std_err.trimmed().isEmpty()) {
        sak::logError(QString("Diskpart errors:\n%1").arg(result.std_err).toStdString());
    }
    if (result.exit_code != 0) {
        setError(QString("Diskpart failed with exit code %1. Ensure you are running as "
                         "Administrator.")
                     .arg(result.exit_code));
        sak::logError(lastError().toStdString());
        return false;
    }
    if (diskpartOutputIsError(result.std_out)) {
        setError("Diskpart reported an error in its output despite a zero exit code");
        sak::logError(lastError().toStdString());
        return false;
    }
    // A zero exit code with no output at all is not proof of success: diskpart always echoes
    // its banner and a per-command acknowledgement when it actually runs a script, so silence
    // means the script never executed. Fail closed rather than treat it as a completed format.
    if (result.std_out.trimmed().isEmpty()) {
        setError("Diskpart produced no output; the script did not run");
        sak::logError(lastError().toStdString());
        return false;
    }
    return true;
}

bool WindowsUSBCreator::runDiskpartScript(const QString& script,
                                          int timeoutMs,
                                          QString& outputOut) {
    outputOut.clear();
    const QString diskpartExe = system32ExePath(QStringLiteral("diskpart.exe"));
    if (diskpartExe.isEmpty()) {
        setError("Cannot resolve system diskpart.exe (SystemRoot unset or file missing)");
        sak::logError(lastError().toStdString());
        return false;
    }
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        setError("Failed to create diskpart script");
        sak::logError(lastError().toStdString());
        return false;
    }
    const QByteArray bytes = script.toLocal8Bit();
    if (scriptFile.write(bytes) != bytes.size()) {
        setError("Failed to write diskpart script");
        sak::logError(lastError().toStdString());
        return false;
    }
    if (!scriptFile.flush()) {
        setError("Failed to flush diskpart script to disk");
        sak::logError(lastError().toStdString());
        return false;
    }
    const auto result = sak::runProcess(diskpartExe,
                                        {QStringLiteral("/s"), scriptFile.fileName()},
                                        timeoutMs,
                                        [this]() { return m_cancelled.load(); });
    outputOut = result.std_out;
    return checkDiskpartResult(result);
}

QString WindowsUSBCreator::formatAndVerifyDrive(const QString& diskNumber) {
    // createBootableUSB only reaches this step after validateUSBInputs matched diskNumber
    // against ^\d{1,3}$.
    Q_ASSERT(!diskNumber.isEmpty());
    Q_EMIT progressUpdated(0);
    Q_EMIT statusChanged("Step 1/5: Formatting drive as NTFS...");
    sak::logInfo("STEP 1: Formatting disk...");

    if (!formatDriveNTFS(diskNumber)) {
        sak::logError("STEP 1 FAILED: Format failed");
        Q_EMIT failed(lastError());
        return {};
    }

    // VERIFY Step 1: Wait for partition and get drive letter
    Q_EMIT progressUpdated(kFormatVerificationProgress);
    Q_EMIT statusChanged("Waiting for partition to be recognized...");
    sak::logInfo("STEP 1: Verifying format and getting drive letter...");

    // Wait with progress updates
    for (int i = 0; i < kDriveRecognitionWaitIterations; ++i) {
        QThread::msleep(sak::kTimerPollingFastMs);
        Q_EMIT progressUpdated(kFormatVerificationProgress + (i * kDriveRecognitionProgressSpan /
                                                              kDriveRecognitionWaitIterations));
        if (m_cancelled) {
            break;
        }
    }

    QString driveLetter = getDriveLetterFromDiskNumber();
    if (driveLetter.isEmpty()) {
        setError(QString("STEP 1 VERIFICATION FAILED: No drive letter found for disk %1 after "
                         "format")
                     .arg(diskNumber));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return {};
    }

    Q_EMIT progressUpdated(kNtfsVerificationProgress);
    Q_EMIT statusChanged("Verifying NTFS filesystem...");

    if (!verifyNtfsFilesystem(driveLetter)) {
        return {};
    }

    Q_EMIT progressUpdated(kExtractionPreparationProgress);
    Q_EMIT statusChanged("Format verified, preparing extraction...");

    return driveLetter;
}

bool WindowsUSBCreator::verifyNtfsFilesystem(const QString& driveLetter) {
    // formatAndVerifyDrive returns before this call when the letter is empty, and
    // validateDriveLetter (its only producer) enforces exactly one A-Z character.
    Q_ASSERT(!driveLetter.isEmpty());
    Q_ASSERT(driveLetter.length() == 1);

    QString checkCmd = QString("(Get-Volume -DriveLetter %1).FileSystem").arg(driveLetter);
    const auto check_result =
        sak::runPowerShell(checkCmd, sak::kTimeoutProcessShortMs, true, false, [this]() {
            return m_cancelled.load();
        });
    if (check_result.timed_out || check_result.cancelled) {
        setError(QString("STEP 1 VERIFICATION FAILED: "
                         "Filesystem check timed out for drive %1")
                     .arg(driveLetter));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    // A non-zero exit means the query itself failed; a stray "NTFS" on stdout must not be
    // accepted as a verified filesystem. Fail closed just like getDriveLetterFromDiskNumber.
    if (check_result.exit_code != 0) {
        setError(QString("STEP 1 VERIFICATION FAILED: "
                         "Filesystem query failed for drive %1 (exit code %2)")
                     .arg(driveLetter)
                     .arg(check_result.exit_code));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    QString fs = check_result.std_out.trimmed();
    if (fs != "NTFS") {
        setError(QString("STEP 1 VERIFICATION FAILED: "
                         "Drive is %1, expected NTFS")
                     .arg(fs));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }
    sak::logInfo(QString("[x] STEP 1 VERIFIED: "
                         "Drive %1: formatted as NTFS")
                     .arg(driveLetter)
                     .toStdString());
    return true;
}

bool WindowsUSBCreator::verifyDriveLetterMapsToTarget(const QString& driveLetter) {
    // driveLetter is the single A-Z value validateDriveLetter produced, so it is safe to
    // interpolate. Re-map it to a disk number and require it to still be the pinned target;
    // fail closed otherwise so extraction never writes to a swapped/reassigned volume.
    const QString cmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(driveLetter);
    const auto result = sak::runPowerShell(
        cmd, sak::kTimeoutProcessMediumMs, true, false, [this]() { return m_cancelled.load(); });
    if (!result.succeeded()) {
        setError(
            QString("Could not confirm drive %1 still maps to target disk %2 before extraction")
                .arg(driveLetter, m_diskNumber));
        sak::logError(lastError().toStdString());
        return false;
    }
    const QString mapped = result.std_out.trimmed();
    bool ok = false;
    const int mappedDisk = mapped.toInt(&ok);
    if (!ok || mappedDisk != m_diskNumber.toInt()) {
        setError(QString("Drive %1 now maps to disk %2, not target disk %3 -- refusing to extract")
                     .arg(driveLetter, mapped, m_diskNumber));
        sak::logError(lastError().toStdString());
        return false;
    }
    return true;
}

bool WindowsUSBCreator::extractAndVerifyFiles(const QString& isoPath, const QString& driveLetter) {
    // Not asserted: copyISOContents below rejects a missing ISO (QFile::exists) and an
    // invalid drive letter (copyISO_normalizeDestination), both via m_lastError.
    // ==================== STEP 2: EXTRACT ====================
    // TOCTOU guard: confirm the drive letter still maps to the pinned target disk before 7z
    // writes anything, so a hot-plug that reassigned the letter cannot redirect the extraction
    // onto an unrelated NTFS volume.
    if (!verifyDriveLetterMapsToTarget(driveLetter)) {
        Q_EMIT failed(lastError());
        return false;
    }
    Q_EMIT statusChanged("Step 2/5: Extracting Windows installation files...");
    sak::logInfo("STEP 2: Extracting ISO contents...");

    if (!copyISOContents(isoPath, driveLetter)) {
        sak::logError("STEP 2 FAILED: Extraction failed");
        Q_EMIT failed(lastError());
        return false;
    }

    // VERIFY Step 2: Check critical files exist
    sak::logInfo("STEP 2: Verifying extraction...");
    Q_EMIT statusChanged("Step 2/5: Verifying extracted files...");

    QString basePath = driveLetter + ":\\";
    QStringList criticalFiles = {"setup.exe", "sources\\boot.wim", "bootmgr"};

    // cppcheck-suppress useStlAlgorithm ; loop has side effects (per-file error + logging)
    for (const QString& file : criticalFiles) {
        QString fullPath = basePath + file;
        if (!QFile::exists(fullPath)) {
            setError(QString("STEP 2 VERIFICATION FAILED: Missing critical file: %1").arg(file));
            sak::logError(lastError().toStdString());
            Q_EMIT failed(lastError());
            return false;
        }
        QFileInfo info(fullPath);
        sak::logInfo(QString("  [x] %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
    }

    // Check for install image
    bool hasInstall = QFile::exists(basePath + "sources\\install.wim") ||
                      QFile::exists(basePath + "sources\\install.esd");
    if (!hasInstall) {
        setError("STEP 2 VERIFICATION FAILED: No install.wim or install.esd found");
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 2 VERIFIED: All critical files extracted");
    Q_EMIT progressUpdated(kExtractionVerifiedProgress);

    return true;
}

bool WindowsUSBCreator::setAndVerifyBootFlag(const QString& diskNumber,
                                             const QString& driveLetter) {
    Q_EMIT statusChanged("Step 4/5: Setting bootable flag...");
    sak::logInfo("STEP 4: Setting bootable flag...");

    const QString diskpartScript = QString(
                                       "select disk %1\n"
                                       "select partition 1\n"
                                       "active\n")
                                       .arg(diskNumber);

    QString output;
    if (!runDiskpartScript(diskpartScript, sak::kTimeoutProcessLongMs, output)) {
        setError("STEP 4 FAILED: " + lastError());
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("STEP 4: Verifying bootable flag...");
    Q_EMIT statusChanged("Step 4/5: Verifying bootable flag...");

    if (!verifyBootableFlag(driveLetter)) {
        setError("STEP 4 VERIFICATION FAILED: " + lastError());
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 4 VERIFIED: Bootable flag is set (Active)");
    Q_EMIT progressUpdated(kBootFlagVerifiedProgress);
    return true;
}

bool WindowsUSBCreator::configureBootAndVerify(const QString& diskNumber,
                                               const QString& driveLetter) {
    // TOCTOU guard: bcdboot writes boot files to driveLetter. Re-pin the letter to the
    // original target disk before that write so a hot-plug that reassigned the letter
    // between extraction and here cannot redirect boot configuration onto an unrelated
    // volume. Fails closed on any mismatch or query failure.
    if (!verifyDriveLetterMapsToTarget(driveLetter)) {
        Q_EMIT failed(lastError());
        return false;
    }
    // ==================== STEP 3: MAKE BOOTABLE ====================
    Q_EMIT progressUpdated(kBootConfigurationStartProgress);
    Q_EMIT statusChanged("Step 3/5: Making drive bootable...");
    sak::logInfo("STEP 3: Making drive bootable...");

    if (!makeBootable(driveLetter)) {
        sak::logError("STEP 3 FAILED: Could not configure boot files");
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 3 COMPLETED: Boot configuration done");
    Q_EMIT progressUpdated(kBootConfigurationCompleteProgress);

    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return false;
    }

    // ==================== STEP 4: SET BOOT FLAG ====================
    return setAndVerifyBootFlag(diskNumber, driveLetter);
}

void WindowsUSBCreator::cancel() {
    m_cancelled = true;
    sak::logInfo("Windows USB creation cancelled");
}

QString WindowsUSBCreator::lastError() const {
    QMutexLocker locker(&m_errorMutex);
    return m_lastError;
}

bool WindowsUSBCreator::cleanAndPartitionDisk(const QString& diskNumber) {
    // The only path here is createBootableUSB -> formatAndVerifyDrive -> formatDriveNTFS,
    // gated on validateUSBInputs' ^\d{1,3}$ regex.
    Q_ASSERT(!diskNumber.isEmpty());
    // Honor a cancellation that arrived before this destructive step launches, so the
    // clean is not started after the user asked to stop.
    if (m_cancelled) {
        setError("Operation cancelled");
        return false;
    }
    // Re-pin the target's identity immediately before the destructive clean so a
    // hot-plug that reassigned disk numbers cannot redirect the wipe (TOCTOU).
    if (!reverifyTargetDiskIdentity(diskNumber)) {
        return false;
    }

    const QString diskpartScript = QString(
                                       "select disk %1\n"
                                       "clean\n"
                                       "create partition primary\n"
                                       "select partition 1\n"
                                       "active\n"
                                       "exit\n")
                                       .arg(diskNumber);

    sak::logInfo(QString("Running diskpart script:\n%1").arg(diskpartScript).toStdString());

    QString output;
    if (!runDiskpartScript(diskpartScript, sak::kTimeoutProcessLongMs, output)) {
        return false;
    }
    sak::logInfo(QString("Diskpart output:\n%1").arg(output).toStdString());
    return true;
}

bool WindowsUSBCreator::formatPartitionNTFS(const QString& diskNumber) {
    // The only path here is createBootableUSB -> formatAndVerifyDrive -> formatDriveNTFS,
    // gated on validateUSBInputs' ^\d{1,3}$ regex.
    Q_ASSERT(!diskNumber.isEmpty());
    // Honor a cancellation that arrived before this second destructive step launches.
    if (m_cancelled) {
        setError("Operation cancelled");
        return false;
    }
    // TOCTOU guard: cleanAndPartitionDisk re-pinned identity before the clean, but the format
    // is a SEPARATE destructive DiskPart run. Re-pin again immediately before it so a hot-plug
    // that reassigned disk numbers between the two scripts cannot redirect the format.
    if (!reverifyTargetDiskIdentity(diskNumber)) {
        return false;
    }
    sak::logInfo("Waiting for Windows to recognize partition...");
    Q_EMIT statusChanged("Formatting partition as NTFS...");
    QThread::msleep(sak::kTimerStatusMessageMs);

    const QString formatScript = QString(
                                     "select disk %1\n"
                                     "select partition 1\n"
                                     "format FS=NTFS QUICK label=\"BOOT\"\n"
                                     "exit\n")
                                     .arg(diskNumber);

    sak::logInfo(QString("Running format script:\n%1").arg(formatScript).toStdString());

    QString formatOutput;
    if (!runDiskpartScript(formatScript, sak::kTimeoutProcessVeryLongMs, formatOutput)) {
        return false;
    }
    sak::logInfo(QString("Format output:\n%1").arg(formatOutput).toStdString());

    // Wait for format to complete
    sak::logInfo("Waiting for format to settle...");
    QThread::msleep(sak::kTimerStatusMessageMs);

    return true;
}

bool WindowsUSBCreator::formatDriveNTFS(const QString& diskNumber) {
    // formatAndVerifyDrive, the only caller, is reached only after validateUSBInputs
    // matched diskNumber against ^\d{1,3}$.
    Q_ASSERT(!diskNumber.isEmpty());
    // Use the official Microsoft method: diskpart clean, then format command
    // Based on Windows Server USB creation documentation and Rufus implementation

    sak::logInfo(QString("Formatting disk %1 as NTFS").arg(diskNumber).toStdString());
    Q_EMIT statusChanged("Preparing USB drive...");

    // Step 1: Clean the disk and create MBR partition using diskpart
    if (!cleanAndPartitionDisk(diskNumber)) {
        return false;
    }

    // Step 2: Wait for partition and format with NTFS
    if (!formatPartitionNTFS(diskNumber)) {
        return false;
    }

    sak::logInfo(QString("Successfully formatted disk %1 as NTFS").arg(diskNumber).toStdString());
    return true;
}

QString WindowsUSBCreator::getDriveLetterFromDiskNumber() {
    if (m_diskNumber.isEmpty()) {
        setError("Cannot query drive letter: No disk number set");
        sak::logError(lastError().toStdString());
        return QString();
    }

    // Validate disk number is numeric
    bool ok = false;
    int diskNum = m_diskNumber.toInt(&ok);
    if (!ok || diskNum < 0) {
        setError(QString("Invalid disk number format: '%1'").arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return QString();
    }

    sak::logInfo(QString("Querying drive letter for disk %1").arg(m_diskNumber).toStdString());

    QString cmd = QString(
                      "(Get-Partition -DiskNumber %1 | Get-Volume | Where-Object "
                      "{$_.DriveLetter -ne $null} | Select-Object -First 1).DriveLetter")
                      .arg(m_diskNumber);
    const auto drive_result = sak::runPowerShell(
        cmd, sak::kTimeoutProcessMediumMs, true, false, [this]() { return m_cancelled.load(); });

    if (drive_result.timed_out || drive_result.cancelled) {
        setError(QString("Timeout querying drive letter for disk %1").arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return QString();
    }

    if (drive_result.exit_code != 0) {
        QString errors = drive_result.std_err.trimmed();
        setError(QString("PowerShell query failed for disk %1: %2").arg(m_diskNumber, errors));
        sak::logError(lastError().toStdString());
        return QString();
    }

    QString driveLetter = validateDriveLetter(drive_result.std_out.trimmed());
    if (driveLetter.isEmpty()) {
        return {};
    }

    sak::logInfo(QString("[x] Successfully mapped disk %1 "
                         "to drive letter %2")
                     .arg(m_diskNumber, driveLetter)
                     .toStdString());
    return driveLetter;
}

QString WindowsUSBCreator::validateDriveLetter(const QString& rawLetter) {
    // rawLetter is not asserted: an empty value is rejected below. m_diskNumber is an
    // invariant here -- getDriveLetterFromDiskNumber, the only caller, returns early when
    // it is empty, and createBootableUSB pins it before validateUSBInputs accepts it.
    Q_ASSERT(!m_diskNumber.isEmpty());

    if (rawLetter.isEmpty()) {
        setError(QString("No drive letter assigned to disk %1. "
                         "Drive may not be formatted or "
                         "partition not recognized.")
                     .arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return {};
    }

    if (rawLetter.length() != 1 || !rawLetter[0].isLetter()) {
        setError(QString("Invalid drive letter from PowerShell: "
                         "'%1' (expected single A-Z character)")
                     .arg(rawLetter));
        sak::logError(lastError().toStdString());
        return {};
    }

    return rawLetter.toUpper();
}
