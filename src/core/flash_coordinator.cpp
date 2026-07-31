// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/flash_coordinator.h"

#include "sak/drive_unmounter.h"
#include "sak/flash_worker.h"
#include "sak/input_validator.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QMutexLocker>
#include <QSet>
#include <QThread>

#include <vector>

#include <windows.h>

#include <winioctl.h>

namespace {
constexpr qint64 kDefaultFlashBufferSizeMb = 256;
constexpr int kDefaultFlashBufferCount = 16;
constexpr int kMaxPhysicalDriveNumber = 99;
constexpr int kWorkerShutdownTimeoutMs = sak::kTimeoutThreadShutdownMs;

// Returns true when the given physical-drive number backs the current OS
// (system) volume. Used as an engine-level guard so a bad caller cannot raw
// write the running OS disk. Determined natively (no elevation needed); if the
// OS disk cannot be identified this returns false and the GUI's removable-only
// selection remains the gate.
bool physicalDriveHostsSystemVolume(int driveNumber) {
    wchar_t winDir[MAX_PATH] = {};
    const UINT len = GetWindowsDirectoryW(winDir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH || winDir[1] != L':') {
        return false;
    }
    const wchar_t volumePath[] = {L'\\', L'\\', L'.', L'\\', winDir[0], L':', L'\0'};
    HANDLE hVol = CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) {
        return false;
    }
    constexpr DWORD kMaxExtents = 16;
    const DWORD bufSize = sizeof(VOLUME_DISK_EXTENTS) + (kMaxExtents - 1) * sizeof(DISK_EXTENT);
    std::vector<unsigned char> buffer(bufSize, 0);
    auto* extents = reinterpret_cast<VOLUME_DISK_EXTENTS*>(buffer.data());
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(hVol,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    nullptr,
                                    0,
                                    extents,
                                    bufSize,
                                    &bytesReturned,
                                    nullptr);
    CloseHandle(hVol);
    if (!ok) {
        return false;
    }
    for (DWORD i = 0; i < extents->NumberOfDiskExtents && i < kMaxExtents; ++i) {
        if (static_cast<int>(extents->Extents[i].DiskNumber) == driveNumber) {
            return true;
        }
    }
    return false;
}

// Returns the first target path that appears more than once (case-insensitive,
// since Windows device paths are not case-sensitive), or an empty string if all
// targets are distinct.
QString firstDuplicateTarget(const QStringList& targetDrives) {
    QSet<QString> seen;
    for (const QString& devicePath : targetDrives) {
        const QString key = devicePath.trimmed().toLower();
        if (seen.contains(key)) {
            return devicePath;
        }
        seen.insert(key);
    }
    return QString();
}
}  // namespace

FlashCoordinator::FlashCoordinator(QObject* parent)
    : QObject(parent)
    , m_state(sak::FlashState::Idle)
    , m_verificationEnabled(true)
    , m_bufferSize(kDefaultFlashBufferSizeMb * sak::kBytesPerMB)
    , m_bufferCount(kDefaultFlashBufferCount)
    , m_isCancelled(false) {
    m_progress.state = sak::FlashState::Idle;
    m_progress.percentage = 0.0;
    m_progress.bytesWritten = 0;
    m_progress.totalBytes = 0;
    m_progress.speedMBps = 0.0;
    m_progress.activeDrives = 0;
    m_progress.failedDrives = 0;
    m_progress.completedDrives = 0;
}

FlashCoordinator::~FlashCoordinator() {
    if (isFlashing()) {
        cancel();
    }
    cleanupWorkers();
}

bool FlashCoordinator::startFlash(const QString& imagePath, const QStringList& targetDrives) {
    // m_imageSource is null on entry and created by prepareImageSource() below; do not assert it.
    Q_ASSERT(!imagePath.isEmpty());
    if (isFlashing()) {
        sak::logError("Flash already in progress");
        return false;
    }

    if (targetDrives.isEmpty()) {
        sak::logError("No target drives specified");
        Q_EMIT flashError("No target drives specified");
        return false;
    }

    sak::logInfo(QString("Starting flash: %1 to %2 drives")
                     .arg(imagePath)
                     .arg(targetDrives.size())
                     .toStdString());

    if (!validateImagePath(imagePath)) {
        return false;
    }

    m_isCancelled = false;
    m_targetDrives = targetDrives;

    // Reset per-run progress and result. The coordinator is long-lived and reused across flashes;
    // without this a stale non-zero completedDrives/failedDrives from a prior run satisfies the
    // finalize predicate on the first completion of the next run, aborting still-active workers.
    m_progress.completedDrives = 0;
    m_progress.failedDrives = 0;
    m_progress.activeDrives = 0;
    m_progress.bytesWritten = 0;
    m_progress.speedMBps = 0.0;
    m_progress.percentage = 0.0;
    m_result = sak::FlashResult{};
    m_reportedWorkers.clear();
    m_flashTimer.start();

    // Validate targets
    m_state = sak::FlashState::Validating;
    Q_EMIT stateChanged(m_state, "Validating targets...");

    if (!validateTargets(targetDrives)) {
        sak::logError("Target validation failed");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Validation failed");
        Q_EMIT flashError("Target validation failed");
        return false;
    }

    if (!prepareImageSource(imagePath)) {
        return false;
    }

    m_progress.totalBytes = m_imageSource->size() * targetDrives.size();

    // Source checksum is calculated by each FlashWorker on its own
    // thread, not here on the UI thread (avoids freezing the GUI
    // for minutes with large images).

    return unmountAndFlash(imagePath, targetDrives);
}

bool FlashCoordinator::validateImagePath(const QString& imagePath) {
    Q_ASSERT(!imagePath.isEmpty());
    sak::path_validation_config img_cfg;
    img_cfg.must_exist = true;
    img_cfg.must_be_file = true;
    img_cfg.check_read_permission = true;
    auto path_result =
        sak::input_validator::validatePath(std::filesystem::path(imagePath.toStdString()), img_cfg);
    if (!path_result) {
        sak::logError("Image path validation failed: {}", path_result.error_message);
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Invalid image path");
        Q_EMIT flashError(
            QString::fromStdString("Image path validation failed: " + path_result.error_message));
        return false;
    }
    return true;
}

bool FlashCoordinator::prepareImageSource(const QString& imagePath) {
    // m_imageSource is created here; asserting it non-null on entry is inverted (it is null).
    Q_ASSERT(!imagePath.isEmpty());
    if (CompressedImageSource::isCompressed(imagePath)) {
        m_imageSource = std::make_unique<CompressedImageSource>(imagePath);
    } else {
        m_imageSource = std::make_unique<FileImageSource>(imagePath);
    }

    if (!m_imageSource->open()) {
        sak::logError("Failed to open image source");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Failed to open image");
        Q_EMIT flashError("Failed to open image file");
        return false;
    }
    return true;
}

bool FlashCoordinator::unmountAndFlash(const QString& imagePath, const QStringList& targetDrives) {
    Q_ASSERT(!imagePath.isEmpty());
    Q_ASSERT(!targetDrives.isEmpty());
    m_state = sak::FlashState::Unmounting;
    Q_EMIT stateChanged(m_state, "Unmounting volumes...");

    if (!unmountVolumes(targetDrives)) {
        // unmountVolumes() already emitted flashError() with details
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Failed to unmount target volumes");
        return false;
    }

    m_state = sak::FlashState::Flashing;
    Q_EMIT stateChanged(m_state, QString("Writing to %1 drives...").arg(targetDrives.size()));

    for (const QString& drive : targetDrives) {
        std::unique_ptr<ImageSource> workerSource;
        if (CompressedImageSource::isCompressed(imagePath)) {
            workerSource = std::make_unique<CompressedImageSource>(imagePath);
        } else {
            workerSource = std::make_unique<FileImageSource>(imagePath);
        }

        auto worker = std::make_unique<FlashWorker>(std::move(workerSource), drive);
        worker->setVerificationEnabled(m_verificationEnabled);
        worker->setBufferSize(m_bufferSize);
        connectWorkerSignals(worker.get());
        worker->start();
        m_workers.push_back(std::move(worker));
    }

    m_progress.activeDrives = static_cast<int>(m_workers.size());

    return true;
}

void FlashCoordinator::cancel() {
    if (!isFlashing()) {
        return;
    }

    sak::logInfo("Cancelling flash operation");
    m_isCancelled = true;

    // Cancel all workers
    for (auto& worker : m_workers) {
        worker->requestStop();
    }

    m_state = sak::FlashState::Cancelled;
    Q_EMIT stateChanged(m_state, "Cancelled by user");
}

bool FlashCoordinator::isFlashing() const {
    QMutexLocker locker(&m_mutex);
    return m_state == sak::FlashState::Flashing || m_state == sak::FlashState::Verifying ||
           m_state == sak::FlashState::Decompressing;
}

sak::FlashState FlashCoordinator::state() const {
    QMutexLocker locker(&m_mutex);
    return m_state;
}

sak::FlashProgress FlashCoordinator::progress() const {
    QMutexLocker locker(&m_mutex);
    return m_progress;
}

void FlashCoordinator::setVerificationEnabled(bool enabled) {
    m_verificationEnabled = enabled;
}

bool FlashCoordinator::isVerificationEnabled() const {
    return m_verificationEnabled;
}

void FlashCoordinator::setBufferSize(qint64 sizeBytes) {
    m_bufferSize = sizeBytes;
}

void FlashCoordinator::setBufferCount(int count) {
    m_bufferCount = count;
}

void FlashCoordinator::onWorkerProgress(double percentage, qint64 bytesWritten) {
    Q_UNUSED(percentage);
    Q_UNUSED(bytesWritten);
    updateProgress();
}

void FlashCoordinator::connectWorkerSignals(FlashWorker* worker) {
    connect(worker, &FlashWorker::progressUpdated, this, &FlashCoordinator::onWorkerProgress);
    connect(
        worker, &FlashWorker::verificationCompleted, this, &FlashCoordinator::onWorkerCompleted);
    connect(worker, &FlashWorker::error, this, &FlashCoordinator::onWorkerFailed);

    // Safety net: an exception inside FlashWorker::execute() (e.g. std::bad_alloc)
    // surfaces ONLY as WorkerBase::failed/cancelled, which none of the FlashWorker
    // signals above cover. Without this the run would never finalize and the caller
    // would block until its timeout. Deduped by onWorkerFailedFor so a normal handled
    // error (which emits BOTH FlashWorker::error AND WorkerBase::failed) is not counted
    // twice.
    connect(worker, &WorkerBase::failed, this, [this, worker](int, const QString& msg) {
        onWorkerFailedFor(worker,
                          msg.isEmpty() ? QStringLiteral("Worker aborted unexpectedly") : msg);
    });
    connect(worker, &WorkerBase::cancelled, this, [this, worker]() {
        onWorkerFailedFor(worker, QStringLiteral("Worker cancelled before completion"));
    });

    // When verification is disabled, verificationCompleted never fires. Use
    // writeCompleted as the completion signal in that case.
    if (!m_verificationEnabled) {
        connect(worker, &FlashWorker::writeCompleted, this, [this](qint64 /*bytesWritten*/) {
            sak::ValidationResult result;
            result.passed = true;
            onWorkerCompleted(result);
        });
    }
}

void FlashCoordinator::onWorkerCompleted(const sak::ValidationResult& result) {
    Q_ASSERT(!m_targetDrives.isEmpty());
    const FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }

    QString devicePath = worker->targetDevice();
    sak::logInfo(QString("Drive completed: %1").arg(devicePath).toStdString());

    QMutexLocker locker(&m_mutex);
    // Mark this worker as having reported a terminal outcome so a later WorkerBase
    // failed()/cancelled() for the same drive is not double-counted as a failure.
    m_reportedWorkers.insert(worker);
    m_progress.activeDrives--;

    // Check if verification passed. completedDrives counts SUCCESSES only (onWorkerFailed
    // increments failedDrives); a failed verification must not bump both counters or it would
    // contribute 2 to the finalize sum below and tear down still-active workers early.
    if (!result.passed) {
        sak::logError(QString("Verification failed for drive: %1").arg(devicePath).toStdString());
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        QString errorMsg = result.errors.isEmpty() ? "Verification failed" : result.errors.first();
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath).arg(errorMsg));
        Q_EMIT driveFailed(devicePath, errorMsg);
    } else {
        m_progress.completedDrives++;
        m_result.successfulDrives.append(devicePath);
        // The verified device checksum (== source when verification is on) -- the only
        // checksum the result previously reported was an empty string.
        if (m_result.sourceChecksum.isEmpty() && !result.targetChecksum.isEmpty()) {
            m_result.sourceChecksum = result.targetChecksum;
        }
        Q_EMIT driveCompleted(devicePath, result.targetChecksum);
    }

    // Check if all drives are done
    if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
        m_state = sak::FlashState::Completed;
        m_result.success = m_progress.failedDrives == 0;
        finalizeResultMetrics();

        Q_EMIT stateChanged(m_state,
                            QString("Completed: %1 successful, %2 failed")
                                .arg(m_result.successfulDrives.size())
                                .arg(m_result.failedDrives.size()));

        Q_EMIT flashCompleted(m_result);

        cleanupWorkers();
    }
}

void FlashCoordinator::onWorkerFailed(const QString& error) {
    Q_ASSERT(!m_targetDrives.isEmpty());
    Q_ASSERT(!error.isEmpty());
    const FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }
    onWorkerFailedFor(worker, error);
}

void FlashCoordinator::onWorkerFailedFor(const FlashWorker* worker, const QString& error) {
    Q_ASSERT(!m_targetDrives.isEmpty());
    if (!worker) {
        return;
    }

    QMutexLocker locker(&m_mutex);
    // Dedup BEFORE dereferencing the worker. A normal handled error emits BOTH
    // FlashWorker::error and (as execute() returns unexpected) WorkerBase::failed; the
    // first finalizes the run and cleanupWorkers() DESTROYS the worker, so by the time
    // the second (deduped) call arrives the captured pointer may dangle. contains()
    // only compares the pointer value (no deref), so it safely rejects the stale
    // pointer here; calling worker->targetDevice() before this check would be a UAF.
    if (m_reportedWorkers.contains(worker)) {
        return;
    }
    m_reportedWorkers.insert(worker);

    QString devicePath = worker->targetDevice();
    sak::logError(QString("Drive failed: %1 - %2").arg(devicePath, error).toStdString());
    m_progress.failedDrives++;
    m_progress.activeDrives--;

    m_result.failedDrives.append(devicePath);
    m_result.errorMessages.append(QString("%1: %2").arg(devicePath, error));

    Q_EMIT driveFailed(devicePath, error);

    // Check if all drives are done
    if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
        m_state = sak::FlashState::Failed;
        m_result.success = false;
        finalizeResultMetrics();

        Q_EMIT stateChanged(m_state,
                            QString("Failed: %1 successful, %2 failed")
                                .arg(m_result.successfulDrives.size())
                                .arg(m_result.failedDrives.size()));

        Q_EMIT flashCompleted(m_result);

        cleanupWorkers();
    }
}

bool FlashCoordinator::validateTargets(const QStringList& targetDrives) {
    Q_ASSERT(!targetDrives.isEmpty());

    // Reject duplicate target paths: two workers writing the SAME physical disk
    // concurrently interleave their raw writes and corrupt each other.
    const QString duplicate = firstDuplicateTarget(targetDrives);
    if (!duplicate.isEmpty()) {
        sak::logError(QString("Duplicate target device rejected: %1").arg(duplicate).toStdString());
        Q_EMIT flashError(QString("Duplicate target device: %1").arg(duplicate));
        return false;
    }

    // cppcheck-suppress useStlAlgorithm ; early-return with per-device error reporting
    for (const QString& devicePath : targetDrives) {
        if (!validateSingleTarget(devicePath)) {
            return false;
        }
    }

    return true;
}

bool FlashCoordinator::validateSingleTarget(const QString& devicePath) {
    // Open device handle to verify it exists and is accessible
    HANDLE hDevice = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()),
                                 GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr,
                                 OPEN_EXISTING,
                                 0,
                                 nullptr);

    if (hDevice == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        sak::logError(
            QString("Failed to open device %1: Error %2").arg(devicePath).arg(error).toStdString());
        Q_EMIT flashError(QString("Cannot access device %1. Error: %2").arg(devicePath).arg(error));
        return false;
    }

    // Get device geometry to verify it's a valid disk
    DISK_GEOMETRY geometry;
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDevice,
                         IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         nullptr,
                         0,
                         &geometry,
                         sizeof(geometry),
                         &bytesReturned,
                         nullptr)) {
        DWORD error = GetLastError();
        CloseHandle(hDevice);
        sak::logError(QString("Failed to get geometry for %1: Error %2")
                          .arg(devicePath)
                          .arg(error)
                          .toStdString());
        Q_EMIT flashError(
            QString("Device %1 is not a valid disk. Error: %2").arg(devicePath).arg(error));
        return false;
    }

    CloseHandle(hDevice);

    // Engine-level safety gate: never raw-write the current OS disk. Parse the
    // physical drive number and refuse if it backs the system volume.
    QString driveNumStr = devicePath;
    const QString physicalDrivePrefix = QStringLiteral("PhysicalDrive");
    driveNumStr.remove(0,
                       driveNumStr.lastIndexOf(physicalDrivePrefix) + physicalDrivePrefix.size());
    bool numOk = false;
    const int driveNumber = driveNumStr.toInt(&numOk);
    if (numOk && physicalDriveHostsSystemVolume(driveNumber)) {
        sak::logError(QString("Refusing raw write to %1: it backs the OS system volume")
                          .arg(devicePath)
                          .toStdString());
        Q_EMIT flashError(
            QString("Refusing to write %1: it is the current OS disk").arg(devicePath));
        return false;
    }

    sak::logInfo(QString("Validated device: %1").arg(devicePath).toStdString());
    return true;
}

bool FlashCoordinator::unmountVolumes(const QStringList& targetDrives) {
    Q_ASSERT(!targetDrives.isEmpty());
    DriveUnmounter unmounter;

    for (const QString& devicePath : targetDrives) {
        sak::logInfo(QString("Unmounting volumes on %1").arg(devicePath).toStdString());

        // Extract drive number from path (e.g., "\\.\PhysicalDrive1" -> 1)
        QString driveNumStr = devicePath;
        const QString physicalDrivePrefix = QStringLiteral("PhysicalDrive");
        driveNumStr.remove(
            0, driveNumStr.lastIndexOf(physicalDrivePrefix) + physicalDrivePrefix.size());
        bool ok = false;
        int driveNumber = driveNumStr.toInt(&ok);

        if (!ok || driveNumber < 0 || driveNumber > kMaxPhysicalDriveNumber) {
            sak::logError(QString("Invalid device path format or drive number out of range: %1")
                              .arg(devicePath)
                              .toStdString());
            Q_EMIT flashError(QString("Invalid device path format: %1").arg(devicePath));
            return false;
        }

        if (!unmounter.unmountDrive(driveNumber)) {
            sak::logError(QString("Failed to unmount volumes on %1").arg(devicePath).toStdString());
            Q_EMIT flashError(
                QString("Failed to unmount volumes on %1. "
                        "Please close any applications using this drive and try again.")
                    .arg(devicePath));
            return false;
        }

        sak::logInfo(QString("Successfully unmounted volumes on %1").arg(devicePath).toStdString());
    }

    return true;
}

void FlashCoordinator::updateProgress() {
    QMutexLocker locker(&m_mutex);
    m_progress.bytesWritten = 0;
    double totalSpeed = 0.0;

    for (const auto& worker : m_workers) {
        m_progress.bytesWritten += worker->bytesWritten();
        totalSpeed += worker->speedMBps();
    }

    m_progress.speedMBps = totalSpeed;
    m_progress.percentage = m_progress.getOverallProgress();
    m_progress.currentOperation = QString("Writing to %1 drives...").arg(m_progress.activeDrives);

    Q_EMIT progressUpdated(m_progress);
}

void FlashCoordinator::finalizeResultMetrics() {
    // m_mutex is already held by the caller (the finalize branch of a worker handler).
    qint64 total_bytes = 0;
    for (const std::unique_ptr<FlashWorker>& worker : m_workers) {
        if (worker) {
            total_bytes += worker->bytesWritten();
        }
    }
    m_result.bytesWritten = total_bytes;
    m_result.elapsedSeconds = static_cast<double>(m_flashTimer.elapsed()) / 1000.0;
}

void FlashCoordinator::cleanupWorkers() {
    // m_imageSource may legitimately be null (called from the dtor when no flash ran, and after a
    // reset); the `if (m_imageSource)` guard below handles it. No entry assert.
    // Wait for all workers to finish with cooperative stop
    for (auto& worker : m_workers) {
        if (!worker->isRunning()) {
            continue;
        }
        sak::logInfo("Requesting worker thread to stop...");
        worker->requestStop();

        if (worker->wait(kWorkerShutdownTimeoutMs)) {
            sak::logInfo("Worker thread stopped gracefully");
            continue;
        }

        // Refuse-to-stop (wedged in a long WriteFile). Do NOT destroy it here:
        // ~FlashWorker would terminate() a live raw-disk write -> corrupted media
        // and a leaked handle. Detach it as a bounded, intentional leak so it
        // finishes its current write and self-cleans, instead of being killed
        // mid-write.
        sak::logError(
            "Worker thread did not stop within 15s -- detaching (bounded leak) "
            "to avoid terminating a live disk write");
        worker->setParent(nullptr);
        static_cast<void>(worker.release());
    }

    m_workers.clear();

    if (m_imageSource) {
        m_imageSource->close();
        m_imageSource.reset();
    }
}
