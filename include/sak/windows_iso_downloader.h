// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/uup_dump_api.h"
#include "sak/uup_iso_builder.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <memory>

/**
 * @brief Windows ISO downloader using UUP dump
 *
 * High-level orchestrator that drives the full ISO acquisition workflow:
 *   1. Browse available Windows builds from UUP dump
 *   2. Select language, edition, and target path
 *   3. Download UUP files from Microsoft CDN via aria2c
 *   4. Convert UUP files to a bootable ISO via wimlib converter
 *
 * This class owns a UupDumpApi (network client) and a UupIsoBuilder
 * (background download + conversion engine). It exposes a clean,
 * step-by-step API that the download dialog drives.
 *
 * All bundled tools (aria2c, converter, wimlib) must be present at
 * build time. Only actual Windows UUP files are fetched at runtime
 * when the user initiates a download.
 *
 * Thread-Safety: Must be constructed on the GUI thread.
 */
class WindowsISODownloader : public QObject {
    Q_OBJECT

public:
    explicit WindowsISODownloader(QObject* parent = nullptr);
    ~WindowsISODownloader() override;

    WindowsISODownloader(const WindowsISODownloader&) = delete;
    WindowsISODownloader& operator=(const WindowsISODownloader&) = delete;
    WindowsISODownloader(WindowsISODownloader&&) = delete;
    WindowsISODownloader& operator=(WindowsISODownloader&&) = delete;

    // ---- Step 1: Fetch builds ----

    /**
     * @brief Fetch available Windows builds for a given architecture and channel
     * @param arch Architecture ("amd64" or "arm64")
     * @param channel Release channel (Retail, Beta, Dev, etc.)
     */
    void fetchBuilds(const QString& arch = "amd64",
                     UupDumpApi::ReleaseChannel channel = UupDumpApi::ReleaseChannel::Retail);

    // ---- Step 2: Select build and fetch languages ----

    /**
     * @brief Fetch available languages for a selected build
     * @param updateId Build UUID from BuildInfo
     */
    void fetchLanguages(const QString& updateId);

    // ---- Step 3: Select language and fetch editions ----

    /**
     * @brief Fetch available editions for a build + language combo
     * @param updateId Build UUID
     * @param lang Language code (e.g., "en-us")
     */
    void fetchEditions(const QString& updateId, const QString& lang);

    // ---- Step 4: Download and build ISO ----

    /**
     * @brief Start downloading UUP files and building the ISO
     * @param updateId Build UUID
     * @param lang Language code (e.g., "en-us")
     * @param edition Edition code (e.g., "PROFESSIONAL")
     * @param savePath Where to save the final .iso file
     */
    void startDownload(const QString& updateId,
                       const QString& lang,
                       const QString& edition,
                       const QString& savePath);

    /**
     * @brief Cancel any in-progress operation (API call or ISO build)
     */
    void cancel();

    /**
     * @brief Check if a download/build is currently running
     */
    bool isDownloading() const;

    /**
     * @brief Get the UUP dump API client (for direct access if needed)
     */
    UupDumpApi* api() { return m_api.get(); }

    /**
     * @brief Get the ISO builder (for direct access if needed)
     */
    UupIsoBuilder* builder() { return m_builder.get(); }

    /**
     * @brief Get available architectures
     */
    static QStringList availableArchitectures();

    /**
     * @brief Get available release channels
     */
    static QList<UupDumpApi::ReleaseChannel> availableChannels();

Q_SIGNALS:
    // ---- Build browsing ----
    void buildsFetched(const QList<UupDumpApi::BuildInfo>& builds);

    // ---- Language/Edition selection ----
    void languagesFetched(const QStringList& langCodes,
                          const QMap<QString, QString>& langNames);
    void editionsFetched(const QStringList& editions,
                         const QMap<QString, QString>& editionNames);

    // ---- Download/Build progress ----
    void filesFetched(const QString& updateName,
                      const QList<UupDumpApi::FileInfo>& files);
    void downloadStarted(int fileCount, qint64 totalBytes);
    void phaseChanged(UupIsoBuilder::Phase phase, const QString& description);
    void progressUpdated(int overallPercent, const QString& detail);
    void speedUpdated(double downloadSpeedMBps);

    // ---- Completion ----
    void downloadComplete(const QString& isoPath, qint64 fileSize);
    void downloadError(const QString& error);
    void statusMessage(const QString& message);

private Q_SLOTS:
    void onFilesFetched(const QString& updateName,
                        const QList<UupDumpApi::FileInfo>& files);
    void onApiError(const QString& error);

private:
    std::unique_ptr<UupDumpApi> m_api;
    std::unique_ptr<UupIsoBuilder> m_builder;

    // Pending download parameters (stored between getFiles call and builder start)
    QString m_pendingSavePath;
    QString m_pendingEdition;
    QString m_pendingLang;
    QString m_pendingUpdateId;
    bool m_downloadRequested = false;
};
