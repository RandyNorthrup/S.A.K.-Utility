// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/flash_coordinator.h"

#include "sak/drive_unmounter.h"
#include "sak/flash_worker.h"
#include "sak/input_validator.h"
#include "sak/logger.h"

#include <QMutexLocker>
#include <QThread>

#include <windows.h>

#include <winioctl.h>

FlashCoordinator::FlashCoordinator(QObject* parent)
    : QObject(parent)
    , m_state(sak::FlashState::Idle)
    , m_verificationEnabled(true)
    , m_bufferSize(256 * 1024 * 1024)  // 256MB for better performance
    , m_bufferCount(16)
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
    Q_ASSERT(m_imageSource);
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
    Q_ASSERT(m_imageSource);
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

        connect(
            worker.get(), &FlashWorker::progressUpdated, this, &FlashCoordinator::onWorkerProgress);
        connect(worker.get(),
                &FlashWorker::verificationCompleted,
                this,
                &FlashCoordinator::onWorkerCompleted);
        connect(worker.get(), &FlashWorker::error, this, &FlashCoordinator::onWorkerFailed);

        // When verification is disabled, verificationCompleted never fires.
        // Use writeCompleted as the completion signal in that case.
        if (!m_verificationEnabled) {
            connect(
                worker.get(), &FlashWorker::writeCompleted, this, [this](qint64 /*bytesWritten*/) {
                    sak::ValidationResult result;
                    result.passed = true;
                    onWorkerCompleted(result);
                });
        }

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

void FlashCoordinator::onWorkerCompleted(const sak::ValidationResult& result) {
    Q_ASSERT(!m_targetDrives.empty());
    Q_ASSERT(!m_targetDrives.isEmpty());
    FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }

    QString devicePath = worker->targetDevice();
    sak::logInfo(QString("Drive completed: %1").arg(devicePath).toStdString());

    QMutexLocker locker(&m_mutex);
    m_progress.completedDrives++;
    m_progress.activeDrives--;

    // Check if verification passed
    if (!result.passed) {
        sak::logError(QString("Verification failed for drive: %1").arg(devicePath).toStdString());
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        QString errorMsg = result.errors.isEmpty() ? "Verification failed" : result.errors.first();
        m_result.errorMessages.append(QString("%1: %2").arg(devicePath).arg(errorMsg));
        Q_EMIT driveFailed(devicePath, errorMsg);
    } else {
        m_result.successfulDrives.append(devicePath);
        Q_EMIT driveCompleted(devicePath, result.targetChecksum);
    }

    // Check if all drives are done
    if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
        m_state = sak::FlashState::Completed;
        m_result.success = m_progress.failedDrives == 0;

        Q_EMIT stateChanged(m_state,
                            QString("Completed: %1 successful, %2 failed")
                                .arg(m_result.successfulDrives.size())
                                .arg(m_result.failedDrives.size()));

        Q_EMIT flashCompleted(m_result);

        cleanupWorkers();
    }
}

void FlashCoordinator::onWorkerFailed(const QString& error) {
    Q_ASSERT(!m_targetDrives.empty());
    Q_ASSERT(!error.isEmpty());
    FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }

    QString devicePath = worker->targetDevice();
    sak::logError(QString("Drive failed: %1 - %2").arg(devicePath, error).toStdString());

    QMutexLocker locker(&m_mutex);
    m_progress.failedDrives++;
    m_progress.activeDrives--;

    m_result.failedDrives.append(devicePath);
    m_result.errorMessages.append(QString("%1: %2").arg(devicePath, error));

    Q_EMIT driveFailed(devicePath, error);

    // Check if all drives are done
    if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
        m_state = sak::FlashState::Failed;
        m_result.success = false;

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
    for (const QString& devicePath : targetDrives) {
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
            sak::logError(QString("Failed to open device %1: Error %2")
                              .arg(devicePath)
                              .arg(error)
                              .toStdString());
            Q_EMIT flashError(
                QString("Cannot access device %1. Error: %2").arg(devicePath).arg(error));
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
        sak::logInfo(QString("Validated device: %1").arg(devicePath).toStdString());
    }

    return true;
}

bool FlashCoordinator::unmountVolumes(const QStringList& targetDrives) {
    Q_ASSERT(!targetDrives.isEmpty());
    DriveUnmounter unmounter;

    for (const QString& devicePath : targetDrives) {
        sak::logInfo(QString("Unmounting volumes on %1").arg(devicePath).toStdString());

        // Extract drive number from path (e.g., "\\.\PhysicalDrive1" -> 1)
        QString driveNumStr = devicePath;
        driveNumStr.remove(0, driveNumStr.lastIndexOf("PhysicalDrive") + 13);
        bool ok = false;
        int driveNumber = driveNumStr.toInt(&ok);

        if (!ok || driveNumber < 0 || driveNumber > 99) {
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

void FlashCoordinator::cleanupWorkers() {
    Q_ASSERT(m_imageSource);
    // Wait for all workers to finish with cooperative stop
    for (auto& worker : m_workers) {
        if (!worker->isRunning()) {
            continue;
        }
        sak::logInfo("Requesting worker thread to stop...");
        worker->requestStop();

        if (!worker->wait(15'000)) {
            sak::logError("Worker thread did not stop within 15s -- potential resource leak");
        } else {
            sak::logInfo("Worker thread stopped gracefully");
        }
    }

    m_workers.clear();

    if (m_imageSource) {
        m_imageSource->close();
        m_imageSource.reset();
    }
}
