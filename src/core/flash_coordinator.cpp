// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/flash_coordinator.h"
#include "sak/flash_worker.h"
#include "sak/drive_unmounter.h"
#include "sak/logger.h"
#include <QThread>
#include <windows.h>
#include <winioctl.h>

FlashCoordinator::FlashCoordinator(QObject* parent)
    : QObject(parent)
    , m_state(sak::FlashState::Idle)
    , m_verificationEnabled(true)
    , m_bufferSize(256 * 1024 * 1024) // 256MB for better performance
    , m_bufferCount(16)
    , m_isCancelled(false)
{
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
    if (isFlashing()) {
        sak::log_error("Flash already in progress");
        return false;
    }
    
    if (targetDrives.isEmpty()) {
        sak::log_error("No target drives specified");
        Q_EMIT flashError("No target drives specified");
        return false;
    }
    
    sak::log_info(QString("Starting flash: %1 to %2 drives")
        .arg(imagePath).arg(targetDrives.size()).toStdString());
    
    m_isCancelled = false;
    m_targetDrives = targetDrives;
    
    // Validate targets
    m_state = sak::FlashState::Validating;
    Q_EMIT stateChanged(m_state, "Validating targets...");
    
    if (!validateTargets(targetDrives)) {
        sak::log_error("Target validation failed");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Validation failed");
        Q_EMIT flashError("Target validation failed");
        return false;
    }
    
    // Create image source
    if (CompressedImageSource::isCompressed(imagePath)) {
        m_imageSource = std::make_unique<CompressedImageSource>(imagePath);
    } else {
        m_imageSource = std::make_unique<FileImageSource>(imagePath);
    }
    
    if (!m_imageSource->open()) {
        sak::log_error("Failed to open image source");
        m_state = sak::FlashState::Failed;
        Q_EMIT stateChanged(m_state, "Failed to open image");
        Q_EMIT flashError("Failed to open image file");
        return false;
    }
    
    m_progress.totalBytes = m_imageSource->size() * targetDrives.size();
    
    // Calculate source checksum for verification
    if (m_verificationEnabled) {
        sak::log_info("Calculating source checksum...");
        m_sourceChecksum = m_imageSource->calculateChecksum();
    }
    
    // Unmount volumes
    m_state = sak::FlashState::Unmounting;
    Q_EMIT stateChanged(m_state, "Unmounting volumes...");
    
    if (!unmountVolumes(targetDrives)) {
        sak::log_warning("Some volumes could not be unmounted");
        // Continue anyway
    }
    
    // Create workers for each drive
    m_state = sak::FlashState::Flashing;
    Q_EMIT stateChanged(m_state, QString("Writing to %1 drives...").arg(targetDrives.size()));
    
    for (const QString& drive : targetDrives) {
        // Create a new image source for each worker
        std::unique_ptr<ImageSource> workerSource;
        if (CompressedImageSource::isCompressed(imagePath)) {
            workerSource = std::make_unique<CompressedImageSource>(imagePath);
        } else {
            workerSource = std::make_unique<FileImageSource>(imagePath);
        }
        
        auto worker = std::make_unique<FlashWorker>(std::move(workerSource), drive);
        worker->setVerificationEnabled(m_verificationEnabled);
        worker->setBufferSize(m_bufferSize);
        
        // Connect signals
        connect(worker.get(), &FlashWorker::progressUpdated,
                this, &FlashCoordinator::onWorkerProgress);
        connect(worker.get(), &FlashWorker::verificationCompleted,
                this, &FlashCoordinator::onWorkerCompleted);
        connect(worker.get(), &FlashWorker::error,
                this, &FlashCoordinator::onWorkerFailed);
        
        // Start worker
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
    
    sak::log_info("Cancelling flash operation");
    m_isCancelled = true;
    
    // Cancel all workers
    for (auto& worker : m_workers) {
        worker->request_stop();
    }
    
    m_state = sak::FlashState::Cancelled;
    Q_EMIT stateChanged(m_state, "Cancelled by user");
}

bool FlashCoordinator::isFlashing() const {
    return m_state == sak::FlashState::Flashing || 
           m_state == sak::FlashState::Verifying ||
           m_state == sak::FlashState::Decompressing;
}

sak::FlashState FlashCoordinator::state() const {
    return m_state;
}

sak::FlashProgress FlashCoordinator::progress() const {
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
    (void)percentage;
    (void)bytesWritten;
    updateProgress();
}

void FlashCoordinator::onWorkerCompleted(const sak::ValidationResult& result) {
    FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }
    
    QString devicePath = worker->targetDevice();
    sak::log_info(QString("Drive completed: %1").arg(devicePath).toStdString());
    
    m_progress.completedDrives++;
    m_progress.activeDrives--;
    
    // Check if verification passed
    if (!result.passed) {
        sak::log_error(QString("Verification failed for drive: %1").arg(devicePath).toStdString());
        m_progress.failedDrives++;
        m_result.failedDrives.append(devicePath);
        QString errorMsg = result.errors.isEmpty() 
            ? "Verification failed" 
            : result.errors.first();
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
        
        Q_EMIT stateChanged(m_state, QString("Completed: %1 successful, %2 failed")
            .arg(m_result.successfulDrives.size())
            .arg(m_result.failedDrives.size()));
        
        Q_EMIT flashCompleted(m_result);
        
        cleanupWorkers();
    }
}

void FlashCoordinator::onWorkerFailed(const QString& error) {
    FlashWorker* worker = qobject_cast<FlashWorker*>(sender());
    if (!worker) {
        return;
    }
    
    QString devicePath = worker->targetDevice();
    sak::log_error(QString("Drive failed: %1 - %2").arg(devicePath, error).toStdString());
    
    m_progress.failedDrives++;
    m_progress.activeDrives--;
    
    m_result.failedDrives.append(devicePath);
    m_result.errorMessages.append(QString("%1: %2").arg(devicePath, error));
    
    Q_EMIT driveFailed(devicePath, error);
    
    // Check if all drives are done
    if (m_progress.completedDrives + m_progress.failedDrives >= m_targetDrives.size()) {
        m_state = sak::FlashState::Failed;
        m_result.success = false;
        
        Q_EMIT stateChanged(m_state, QString("Failed: %1 successful, %2 failed")
            .arg(m_result.successfulDrives.size())
            .arg(m_result.failedDrives.size()));
        
        Q_EMIT flashCompleted(m_result);
        
        cleanupWorkers();
    }
}

bool FlashCoordinator::validateTargets(const QStringList& targetDrives) {
    for (const QString& devicePath : targetDrives) {
        // Open device handle to verify it exists and is accessible
        HANDLE hDevice = CreateFileW(
            reinterpret_cast<LPCWSTR>(devicePath.utf16()),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        
        if (hDevice == INVALID_HANDLE_VALUE) {
            DWORD error = GetLastError();
            sak::log_error(QString("Failed to open device %1: Error %2")
                .arg(devicePath).arg(error).toStdString());
            Q_EMIT flashError(QString("Cannot access device %1. Error: %2")
                .arg(devicePath).arg(error));
            return false;
        }
        
        // Get device geometry to verify it's a valid disk
        DISK_GEOMETRY geometry;
        DWORD bytesReturned = 0;
        if (!DeviceIoControl(
            hDevice,
            IOCTL_DISK_GET_DRIVE_GEOMETRY,
            nullptr, 0,
            &geometry, sizeof(geometry),
            &bytesReturned,
            nullptr))
        {
            DWORD error = GetLastError();
            CloseHandle(hDevice);
            sak::log_error(QString("Failed to get geometry for %1: Error %2")
                .arg(devicePath).arg(error).toStdString());
            Q_EMIT flashError(QString("Device %1 is not a valid disk. Error: %2")
                .arg(devicePath).arg(error));
            return false;
        }
        
        CloseHandle(hDevice);
        sak::log_info(QString("Validated device: %1").arg(devicePath).toStdString());
    }
    
    return true;
}

bool FlashCoordinator::unmountVolumes(const QStringList& targetDrives) {
    DriveUnmounter unmounter;
    
    for (const QString& devicePath : targetDrives) {
        sak::log_info(QString("Unmounting volumes on %1").arg(devicePath).toStdString());
        
        // Extract drive number from path (e.g., "\\.\PhysicalDrive1" -> 1)
        QString driveNumStr = devicePath;
        driveNumStr.remove(0, driveNumStr.lastIndexOf("PhysicalDrive") + 13);
        bool ok = false;
        int driveNumber = driveNumStr.toInt(&ok);
        
        if (!ok || driveNumber < 0 || driveNumber > 99) {
            sak::log_error(QString("Invalid device path format or drive number out of range: %1").arg(devicePath).toStdString());
            Q_EMIT flashError(QString("Invalid device path format: %1").arg(devicePath));
            return false;
        }
        
        if (!unmounter.unmountDrive(driveNumber)) {
            sak::log_error(QString("Failed to unmount volumes on %1").arg(devicePath).toStdString());
            Q_EMIT flashError(QString("Failed to unmount volumes on %1. "
                "Please close any applications using this drive and try again.")
                .arg(devicePath));
            return false;
        }
        
        sak::log_info(QString("Successfully unmounted volumes on %1").arg(devicePath).toStdString());
    }
    
    return true;
}

void FlashCoordinator::updateProgress() {
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
    // Wait for all workers to finish
    for (auto& worker : m_workers) {
        if (worker->isRunning()) {
            sak::log_info("Requesting worker thread to stop...");
            worker->request_stop();
            
            if (!worker->wait(5000)) {
                // Worker didn't stop gracefully, force termination
                sak::log_error("Worker thread did not stop gracefully, forcing termination");
                worker->terminate();
                
                // Wait briefly for terminate to complete
                if (!worker->wait(2000)) {
                    sak::log_error("Worker thread did not terminate, resources may leak");
                }
            } else {
                sak::log_info("Worker thread stopped gracefully");
            }
        }
    }
    
    m_workers.clear();
    
    if (m_imageSource) {
        m_imageSource->close();
        m_imageSource.reset();
    }
}









