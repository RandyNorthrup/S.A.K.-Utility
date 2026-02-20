// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/uup_dump_api.h"

#include <QObject>
#include <QThread>
#include <QString>
#include <QProcess>
#include <QElapsedTimer>
#include <QTimer>
#include <QDir>
#include <atomic>
#include <memory>

/**
 * @brief Background UUP file downloader and ISO builder
 *
 * Orchestrates the complete pipeline of downloading Windows UUP files
 * via aria2c and converting them to a bootable ISO using the bundled
 * uup-converter-wimlib tools. Runs in a background thread with progress
 * reporting back to the GUI thread.
 *
 * Pipeline phases:
 *   1. Preparation  (5%)  - Generate aria2c input file, set up work directory
 *   2. Download     (60%) - Download UUP files via aria2c with integrity checks
 *   3. Conversion   (35%) - Convert UUP → ISO using wimlib converter
 *
 * All bundled tools (aria2c.exe, converter scripts, wimlib-imagex.exe)
 * must be present in the application's tools/uup/ directory at build time.
 * Only actual Windows UUP files are downloaded at runtime.
 *
 * Thread-Safety:
 *   - startBuild() and cancel() may be called from any thread
 *   - All signals are emitted in a way that is safe for cross-thread connections
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
     * Terminates any running aria2c or converter processes and
     * cleans up the work directory. Safe to call from any thread.
     */
    void cancel();

    /**
     * @brief Get the current phase
     */
    Phase currentPhase() const { return m_phase; }

    /**
     * @brief Check if a build is currently in progress
     */
    bool isRunning() const { return m_phase != Phase::Idle && m_phase != Phase::Completed && m_phase != Phase::Failed; }

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
    void executeDownload();
    void executeConversion();
    void finalizeBuild();
    void cleanupWorkDir();

    // Tool path resolution
    QString findAria2Path() const;
    QString findConverterDir() const;
    QString find7zPath() const;

    // aria2c input file generation
    bool generateAria2InputFile(const QString& outputPath);

    // Check if a file is already fully downloaded and verified
    bool isFileAlreadyDownloaded(const UupDumpApi::FileInfo& fileInfo,
                                const QString& downloadDir) const;

    // Progress parsing
    void parseAria2Progress(const QString& line);
    void parseConverterProgress(const QString& line);
    void updateOverallProgress();

    // State
    Phase m_phase = Phase::Idle;
    std::atomic<bool> m_cancelled{false};

    // Build parameters
    QList<UupDumpApi::FileInfo> m_files;
    QString m_outputIsoPath;
    QString m_edition;
    QString m_lang;
    QString m_updateId;
    QString m_workDir;
    qint64 m_totalDownloadBytes = 0;
    bool m_allFilesAlreadyDownloaded = false;

    // Process management
    std::unique_ptr<QProcess> m_aria2Process;
    std::unique_ptr<QProcess> m_converterProcess;
    QThread* m_workerThread = nullptr;

    // Progress tracking
    QElapsedTimer m_phaseTimer;
    QTimer* m_progressPollTimer = nullptr;
    int m_downloadPercent = 0;
    int m_conversionPercent = 0;
    double m_currentSpeedMBps = 0.0;
    qint64 m_downloadedBytes = 0;

    // Phase weight in overall progress (must sum to 100)
    static constexpr int PHASE_PREPARE_WEIGHT = 5;
    static constexpr int PHASE_DOWNLOAD_WEIGHT = 60;
    static constexpr int PHASE_CONVERT_WEIGHT = 35;
};
