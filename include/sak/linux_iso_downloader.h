// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/linux_distro_catalog.h"

#include <QObject>
#include <QString>
#include <QProcess>
#include <QTimer>
#include <QCryptographicHash>
#include <memory>

/**
 * @brief Orchestrator for downloading Linux ISO images
 *
 * Manages the complete download pipeline:
 *   1. Resolve download URL from the distro catalog
 *   2. Download ISO via bundled aria2c (multi-connection, resumable)
 *   3. Fetch and verify SHA256/SHA1 checksum
 *   4. Report progress and completion
 *
 * Uses the same bundled aria2c as the Windows ISO downloader for
 * consistent high-speed downloads with multi-connection support.
 *
 * Thread-Safety: All methods must be called from the GUI thread.
 * aria2c runs as a child process; progress is polled via QTimer.
 *
 * @see LinuxDistroCatalog for distro metadata
 * @see LinuxISODownloadDialog for the UI
 */
class LinuxISODownloader : public QObject {
    Q_OBJECT

public:
    /// @brief Download pipeline phase
    enum class Phase {
        Idle,                ///< No download in progress
        ResolvingVersion,    ///< Checking GitHub API for latest version
        Downloading,         ///< aria2c is downloading the ISO
        VerifyingChecksum,   ///< Computing and comparing SHA256/SHA1
        Completed,           ///< Download and verification succeeded
        Failed               ///< An error occurred
    };
    Q_ENUM(Phase)

    explicit LinuxISODownloader(QObject* parent = nullptr);
    ~LinuxISODownloader() override;

    // Disable copy/move
    LinuxISODownloader(const LinuxISODownloader&) = delete;
    LinuxISODownloader& operator=(const LinuxISODownloader&) = delete;
    LinuxISODownloader(LinuxISODownloader&&) = delete;
    LinuxISODownloader& operator=(LinuxISODownloader&&) = delete;

    /**
     * @brief Get the distro catalog
     * @return Reference to the catalog (owned by this downloader)
     */
    LinuxDistroCatalog* catalog() const { return m_catalog.get(); }

    /**
     * @brief Start downloading a Linux ISO
     *
     * Resolves the download URL (checking GitHub API for GitHub-hosted distros),
     * then downloads via aria2c. Emits progress signals throughout.
     *
     * @param distroId Distro identifier from the catalog
     * @param savePath Full path where the ISO should be saved
     */
    void startDownload(const QString& distroId, const QString& savePath);

    /**
     * @brief Cancel an in-progress download
     *
     * Kills aria2c, removes partial files, and emits downloadError.
     */
    void cancel();

    /**
     * @brief Check if a download is currently in progress
     */
    bool isDownloading() const { return m_phase != Phase::Idle &&
                                        m_phase != Phase::Completed &&
                                        m_phase != Phase::Failed; }

    /**
     * @brief Get the current download phase
     */
    Phase currentPhase() const { return m_phase; }

Q_SIGNALS:
    /**
     * @brief Emitted when the download phase changes
     * @param phase New phase
     * @param description Human-readable phase description
     */
    void phaseChanged(LinuxISODownloader::Phase phase, const QString& description);

    /**
     * @brief Emitted periodically with download progress
     * @param percent Overall progress percentage (0-100)
     * @param detail Detail text (e.g., "3.2 GB / 6.2 GB")
     */
    void progressUpdated(int percent, const QString& detail);

    /**
     * @brief Emitted with current download speed
     * @param speedMBps Download speed in MB/s
     */
    void speedUpdated(double speedMBps);

    /**
     * @brief Emitted when download and verification complete successfully
     * @param isoPath Full path to the downloaded ISO file
     * @param fileSize Size of the downloaded file in bytes
     */
    void downloadComplete(const QString& isoPath, qint64 fileSize);

    /**
     * @brief Emitted when an error occurs at any stage
     * @param error Human-readable error message
     */
    void downloadError(const QString& error);

    /**
     * @brief Emitted with status messages for the UI
     * @param message Status text
     */
    void statusMessage(const QString& message);

private Q_SLOTS:
    void onVersionCheckCompleted(const QString& distroId,
                                 const LinuxDistroCatalog::DistroInfo& distro,
                                 bool changed);
    void onVersionCheckFailed(const QString& distroId, const QString& error);
    void onAria2cFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProgressPollTimer();

private:
    void setPhase(Phase phase, const QString& description);
    void startAria2cDownload(const QString& url, const QString& savePath,
                             const QString& fileName);
    void verifyChecksum();
    void onChecksumVerified(bool match, const QString& expected, const QString& actual);
    QString findAria2c() const;
    void cleanupPartialFiles();

    std::unique_ptr<LinuxDistroCatalog> m_catalog;
    QProcess* m_aria2cProcess = nullptr;
    QTimer* m_progressTimer = nullptr;

    // Current download state
    Phase m_phase = Phase::Idle;
    QString m_currentDistroId;
    QString m_savePath;
    QString m_downloadUrl;
    QString m_checksumUrl;
    QString m_checksumType;
    QString m_expectedFileName;
    qint64 m_totalSize = 0;
    bool m_cancelled = false;
};
