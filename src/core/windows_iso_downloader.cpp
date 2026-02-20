// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file windows_iso_downloader.cpp
 * @brief Windows ISO downloader using UUP dump API + aria2c + wimlib converter
 *
 * Orchestrates the complete pipeline: browse builds → select language/edition →
 * download UUP files → convert to bootable ISO. All tools are bundled at build
 * time; only actual Windows UUP files are fetched at runtime.
 */

#include "sak/windows_iso_downloader.h"
#include "sak/logger.h"

// ============================================================================
// Construction / Destruction
// ============================================================================

WindowsISODownloader::WindowsISODownloader(QObject* parent)
    : QObject(parent)
    , m_api(std::make_unique<UupDumpApi>(this))
    , m_builder(std::make_unique<UupIsoBuilder>(this))
{
    // Forward API signals
    connect(m_api.get(), &UupDumpApi::buildsFetched,
            this, &WindowsISODownloader::buildsFetched);
    connect(m_api.get(), &UupDumpApi::languagesFetched,
            this, &WindowsISODownloader::languagesFetched);
    connect(m_api.get(), &UupDumpApi::editionsFetched,
            this, &WindowsISODownloader::editionsFetched);
    connect(m_api.get(), &UupDumpApi::filesFetched,
            this, &WindowsISODownloader::onFilesFetched);
    connect(m_api.get(), &UupDumpApi::apiError,
            this, &WindowsISODownloader::onApiError);

    // Forward builder signals
    connect(m_builder.get(), &UupIsoBuilder::phaseChanged,
            this, &WindowsISODownloader::phaseChanged);
    connect(m_builder.get(), &UupIsoBuilder::progressUpdated,
            this, &WindowsISODownloader::progressUpdated);
    connect(m_builder.get(), &UupIsoBuilder::speedUpdated,
            this, &WindowsISODownloader::speedUpdated);
    connect(m_builder.get(), &UupIsoBuilder::buildCompleted,
            this, &WindowsISODownloader::downloadComplete);
    connect(m_builder.get(), &UupIsoBuilder::buildError,
            this, &WindowsISODownloader::downloadError);

    sak::logInfo("WindowsISODownloader initialized (UUP dump backend)");
}

WindowsISODownloader::~WindowsISODownloader()
{
    cancel();
}

// ============================================================================
// Step 1: Fetch Builds
// ============================================================================

void WindowsISODownloader::fetchBuilds(const QString& arch,
                                       UupDumpApi::ReleaseChannel channel)
{
    Q_EMIT statusMessage(QString("Fetching available %1 builds (%2)...")
                             .arg(arch)
                             .arg(UupDumpApi::channelToDisplayName(channel)));
    m_api->fetchAvailableBuilds(arch, channel);
}

// ============================================================================
// Step 2: Fetch Languages
// ============================================================================

void WindowsISODownloader::fetchLanguages(const QString& updateId)
{
    Q_EMIT statusMessage("Fetching available languages...");
    m_api->listLanguages(updateId);
}

// ============================================================================
// Step 3: Fetch Editions
// ============================================================================

void WindowsISODownloader::fetchEditions(const QString& updateId,
                                         const QString& lang)
{
    Q_EMIT statusMessage("Fetching available editions...");
    m_api->listEditions(updateId, lang);
}

// ============================================================================
// Step 4: Download and Build ISO
// ============================================================================

void WindowsISODownloader::startDownload(const QString& updateId,
                                         const QString& lang,
                                         const QString& edition,
                                         const QString& savePath)
{
    if (isDownloading()) {
        Q_EMIT downloadError("A download is already in progress");
        return;
    }

    m_pendingSavePath = savePath;
    m_pendingEdition = edition;
    m_pendingLang = lang;
    m_pendingUpdateId = updateId;
    m_downloadRequested = true;

    Q_EMIT statusMessage("Fetching download links from Microsoft...");
    sak::logInfo("Requesting UUP file links for build " +
                  updateId.toStdString() + " (" + lang.toStdString() +
                  ", " + edition.toStdString() + ")");

    m_api->getFiles(updateId, lang, edition);
}

void WindowsISODownloader::onFilesFetched(const QString& updateName,
                                          const QList<UupDumpApi::FileInfo>& files)
{
    // Forward the signal for informational purposes
    Q_EMIT filesFetched(updateName, files);

    if (!m_downloadRequested) {
        return; // Files were fetched for informational purposes only
    }

    m_downloadRequested = false;

    if (files.isEmpty()) {
        Q_EMIT downloadError("No download files returned for selected build. "
                            "The build may no longer be available.");
        return;
    }

    // Calculate total download size
    qint64 totalBytes = 0;
    for (const auto& file : files) {
        totalBytes += file.size;
    }

    sak::logInfo("Starting UUP download: " + std::to_string(files.size()) +
                  " files, " + std::to_string(totalBytes / (1024 * 1024)) + " MB");

    Q_EMIT downloadStarted(files.size(), totalBytes);
    Q_EMIT statusMessage(QString("Downloading %1 files (%2 GB)...")
                             .arg(files.size())
                             .arg(totalBytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2));

    m_builder->startBuild(files, m_pendingSavePath, m_pendingEdition, m_pendingLang, m_pendingUpdateId);
}

void WindowsISODownloader::onApiError(const QString& error)
{
    m_downloadRequested = false;
    Q_EMIT downloadError(error);
}

// ============================================================================
// Cancel
// ============================================================================

void WindowsISODownloader::cancel()
{
    m_downloadRequested = false;
    m_api->cancelAll();
    m_builder->cancel();
}

// ============================================================================
// State Queries
// ============================================================================

bool WindowsISODownloader::isDownloading() const
{
    return m_builder->isRunning() || m_downloadRequested;
}

QStringList WindowsISODownloader::availableArchitectures()
{
    return {"amd64", "arm64"};
}

QList<UupDumpApi::ReleaseChannel> WindowsISODownloader::availableChannels()
{
    return UupDumpApi::allChannels();
}







