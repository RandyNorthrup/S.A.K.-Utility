// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/uup_dump_api.h"

#include <QDir>
#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <memory>
#include <optional>

/**
 * @brief Background UUP file downloader and ISO builder
 *
 * Orchestrates the complete pipeline of downloading Windows UUP files
 * via aria2c and converting them to a bootable ISO using the bundled
 * UUPMediaConverter tool. Runs in a background thread with progress
 * reporting back to the GUI thread.
 *
 * Pipeline phases:
 *   1. Preparation  (5%)  - Generate aria2c input file, set up work directory
 *   2. Download     (60%) - Download UUP files via aria2c with integrity checks
 *   3. Conversion   (35%) - Convert UUP -> ISO using UUPMediaConverter
 *
 * All bundled tools (aria2c.exe, UUPMediaConverter.exe)
 * must be present in the application's tools/uup/ directory at build time.
 * Only actual Windows UUP files are downloaded at runtime.
 *
 * Thread-Safety:
 *   - startBuild(), cancel(), and all other methods must be called on the
 *     thread that owns this object: they drive QProcess/QTimer members, which
 *     are bound to their owning thread. Marshal a cross-thread request with
 *     QMetaObject::invokeMethod(..., Qt::QueuedConnection).
 *   - Signals are emitted on this object's thread and are safe for queued
 *     cross-thread connections.
 *
 * Example:
 * @code
 * UupIsoBuilder builder;
 * connect(&builder, &UupIsoBuilder::progressUpdated,
 *     [](int percent, const QString& detail) {
 *         progressBar->setValue(percent);
 *         statusLabel->setText(detail);
 *     });
 * builder.startBuild(files, "C:/temp/uup_work", "C:/ISOs/Win11.iso");
 * @endcode
 */
class UupIsoBuilder : public QObject {
    Q_OBJECT

public:
    /// @brief Current phase of the build pipeline
    enum class Phase {
        Idle,               ///< Not started
        PreparingDownload,  ///< Setting up work directory and aria2c input
        DownloadingFiles,   ///< Downloading UUP files via aria2c
        ConvertingToISO,    ///< Converting downloaded UUP files to ISO
        Completed,          ///< ISO successfully created
        Failed              ///< An error occurred
    };
    Q_ENUM(Phase)

    explicit UupIsoBuilder(QObject* parent = nullptr);
    ~UupIsoBuilder() override;

    // Disable copy/move
    UupIsoBuilder(const UupIsoBuilder&) = delete;
    UupIsoBuilder& operator=(const UupIsoBuilder&) = delete;
    UupIsoBuilder(UupIsoBuilder&&) = delete;
    UupIsoBuilder& operator=(UupIsoBuilder&&) = delete;

    /**
     * @brief Start the download and ISO build process
     * @param files List of UUP files to download (from UupDumpApi::getFiles)
     * @param outputIsoPath Final path for the completed ISO file
     * @param edition Edition name for converter configuration (e.g., "PROFESSIONAL")
     * @param lang Language code for converter configuration (e.g., "en-us")
     * @param updateId Build UUID used to create a deterministic work directory
     *                 for download resumption after failures
     *
     * Creates a deterministic work directory based on the updateId so that
     * retried downloads can resume from previously downloaded files.
     * The work directory is cleaned up only on successful completion.
     */
    void startBuild(const QList<UupDumpApi::FileInfo>& files,
                    const QString& outputIsoPath,
                    const QString& edition,
                    const QString& lang,
                    const QString& updateId = {});

    /**
     * @brief Cancel the current build operation
     *
     * Terminates any running aria2c or converter processes and cleans up the
     * work directory. Must be called on this object's own thread (see the
     * Thread-Safety note above); marshal from another thread with a queued
     * QMetaObject::invokeMethod.
     */
    void cancel();

    /**
     * @brief Get the current phase
     */
    Phase currentPhase() const { return m_phase; }

    /**
     * @brief Names of expected files that are not present in @p downloadDir as a
     *        real regular file (never a directory, symlink, or junction) whose
     *        size matches the API-declared size exactly when one was declared; an
     *        unnamed expected entry is always reported. Empty means the download
     *        set is complete. Used to reject a partial aria2c result (exit code 7)
     *        before conversion. Unit-testable.
     */
    [[nodiscard]] static QStringList missingFiles(const QList<UupDumpApi::FileInfo>& expected,
                                                  const QString& downloadDir);

    /**
     * @brief Move a freshly built ISO from @p tempPath onto @p finalPath,
     *        replacing any prior ISO only now (convert-then-replace, so a failed
     *        conversion never destroys an existing good ISO). Refuses (false) unless
     *        @p tempPath is a real regular file and @p finalPath is absent or a real
     *        regular file, so a planted link is never renamed in place of an image.
     *        Unit-testable.
     */
    [[nodiscard]] static bool replaceFinalIso(const QString& tempPath, const QString& finalPath);

    /**
     * @brief Sum the API-declared sizes of @p files, rejecting negative sizes and
     *        detecting qint64 overflow. Returns nullopt (fail closed) on either, so
     *        malformed metadata cannot silently under-report the download total.
     *        Unit-testable.
     */
    [[nodiscard]] static std::optional<qint64> computeTotalDownloadBytes(
        const QList<UupDumpApi::FileInfo>& files);

    /**
     * @brief True if @p isoPath begins with a valid ISO 9660 Primary Volume
     *        Descriptor signature ("CD001" at byte offset 0x8001). A zero-exit
     *        converter that produced a non-ISO file is thereby rejected before the
     *        output is promoted to the final image. Unit-testable.
     */
    [[nodiscard]] static bool hasIso9660Signature(const QString& isoPath);

    /**
     * @brief Check if a build is currently in progress
     */
    bool isRunning() const {
        return m_phase != Phase::Idle && m_phase != Phase::Completed && m_phase != Phase::Failed;
    }

Q_SIGNALS:
    /**
     * @brief Emitted when the build pipeline moves to a new phase
     * @param phase The new phase
     * @param description Human-readable description of the phase
     */
    void phaseChanged(UupIsoBuilder::Phase phase, const QString& description);

    /**
     * @brief Emitted periodically with overall progress
     * @param overallPercent Progress from 0-100 across all phases
     * @param detail Human-readable detail string (current file, ETA, etc.)
     */
    void progressUpdated(int overallPercent, const QString& detail);

    /**
     * @brief Emitted during download phase with current speed
     * @param downloadSpeedMBps Current download speed in MB/s
     */
    void speedUpdated(double downloadSpeedMBps);

    /**
     * @brief Emitted when the ISO has been successfully created
     * @param isoPath Path to the completed ISO file
     * @param fileSize Size of the ISO file in bytes
     */
    void buildCompleted(const QString& isoPath, qint64 fileSize);

    /**
     * @brief Emitted when an error occurs during any phase
     * @param error Human-readable error description
     */
    void buildError(const QString& error);

private Q_SLOTS:
    void onAria2ReadyRead();
    void onAria2Finished(int exitCode, QProcess::ExitStatus exitStatus);
    void onConverterReadyRead();
    void onConverterFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProgressPollTimer();

private:
    // Phase execution
    void executePreparation();
    /// @brief Validate bundled tools and create work directory
    void prepareWorkspace();
    /// @brief Generate aria2c download manifest
    void downloadPackages();
    void executeDownload();
    /// @brief Build the full argument list for aria2c download
    QStringList buildAria2Arguments(const QString& inputFile, const QString& downloadDir) const;
    void executeConversion();
    /// @brief Validate admin privileges, create directories, and locate converter
    bool prepareConversionEnvironment(QString& uupsDir,
                                      QString& nativeConversionTempDir,
                                      QString& outputIsoPath,
                                      QString& uupMediaConverter);
    /// @brief Connect QProcess signals for the converter process
    void connectConverterSignals();
    /// @brief Reparse-check, clear, and recreate the elevated conversion temp dir
    bool ensureCleanConversionTempDir(const QString& conversionTempDir);
    /// @brief Reparse-check and remove our own stale .partial ISO before reuse
    bool clearStalePartialIso(const QString& partialIso);
    /// @brief Validate + promote the converter output on a clean exit code
    void finalizeSuccessfulConversion();
    void cleanupWorkDir();

    // Tool path resolution
    QString findAria2Path() const;
    QString findUupMediaConverterPath() const;

    // aria2c input file generation
    bool generateAria2InputFile(const QString& outputPath);
    /// @return false (fail closed) if the API-sourced url/fileName would inject an
    ///         aria2 directive (newline) or escape the download dir (traversal/absolute
    ///         out=); the caller then aborts the download-list generation.
    [[nodiscard]] bool writeAria2Entry(QTextStream& stream, const UupDumpApi::FileInfo& fileInfo);
    void logAria2SkippedFiles(int skippedFiles, qint64 skippedBytes);
    void collectConverterError(const QString& line);

    // Check if a file is already fully downloaded and verified
    bool isFileAlreadyDownloaded(const UupDumpApi::FileInfo& fileInfo,
                                 const QString& downloadDir) const;

    // Check if process is running with administrator privileges
    static bool isRunningAsAdmin();

    // Progress parsing
    /// @brief Poll download-phase progress and emit updates
    void pollDownloadProgress();
    /// @brief Poll conversion-phase progress and emit updates
    void pollConversionProgress();
    /// @brief Check for previously downloaded files when resuming a build
    void checkResumedDownloads();
    void parseAria2Progress(const QString& line);
    void parseConverterProgress(const QString& line);
    /// @brief Parse tagged stage percentages from converter output
    /// @return true if a stage match was found
    bool parseConverterStagePercent(const QString& line, bool& hasPercent, QString& detail);
    /// @brief Parse heuristic progress patterns from converter output
    void parseConverterProgressPatterns(const QString& line, bool& hasPercent, QString& detail);

    // State
    Phase m_phase = Phase::Idle;
    std::atomic<bool> m_cancelled{false};

    // Build parameters
    QList<UupDumpApi::FileInfo> m_files;
    QString m_outputIsoPath;
    QString m_converterOutputPath;  // temp ISO the converter writes; replaces m_outputIsoPath on ok
    QString m_edition;
    QString m_lang;
    QString m_updateId;
    QString m_workDir;
    qint64 m_totalDownloadBytes = 0;
    bool m_allFilesAlreadyDownloaded = false;

    // Process management
    std::unique_ptr<QProcess> m_aria2Process;
    std::unique_ptr<QProcess> m_converterProcess;

    // Progress tracking
    QElapsedTimer m_phaseTimer;
    QTimer* m_progressPollTimer = nullptr;
    int m_downloadPercent = 0;
    int m_conversionPercent = 0;
    double m_currentSpeedMBps = 0.0;
    qint64 m_downloadedBytes = 0;
    QString m_converterOutputTail;
    QStringList m_converterErrors;

    /// @brief Build a user-facing error message from collected converter errors
    QString classifyConverterFailure() const;

    // Phase weight in overall progress (must sum to 100)
    static constexpr int PHASE_PREPARE_WEIGHT = 5;
    static constexpr int PHASE_DOWNLOAD_WEIGHT = 60;
    static constexpr int PHASE_CONVERT_WEIGHT = 35;
};
