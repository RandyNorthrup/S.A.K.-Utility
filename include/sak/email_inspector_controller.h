// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_controller.h
/// @brief Orchestrates all email inspection workers and panel state

#pragma once

#include "sak/email_types.h"

#include <QFutureSynchronizer>
#include <QObject>
#include <QSet>

#include <functional>
#include <memory>

class PstParser;
class MboxParser;
class EmailSearchWorker;
class EmailExportWorker;
class EmailProfileManager;
class EmailReportGenerator;

class EmailInspectorController : public QObject {
    Q_OBJECT

public:
    /// Controller state machine
    enum class State {
        Idle,
        Opening,
        LoadingFolderItems,
        LoadingItemDetail,
        LoadingProperties,
        Searching,
        Exporting,
        DiscoveringProfiles,
        BackingUp,
        Restoring,
        GeneratingReport
    };
    Q_ENUM(State)

    explicit EmailInspectorController(QObject* parent = nullptr);
    ~EmailInspectorController() override;

    // Non-copyable, non-movable
    EmailInspectorController(const EmailInspectorController&) = delete;
    EmailInspectorController& operator=(const EmailInspectorController&) = delete;
    EmailInspectorController(EmailInspectorController&&) = delete;
    EmailInspectorController& operator=(EmailInspectorController&&) = delete;

    // ====================================================================
    // File Operations
    // ====================================================================

    /// Open a PST, OST, or MBOX file
    void openFile(const QString& file_path);

    /// Close the currently opened file
    void closeFile();

    /// Whether a file is currently open
    [[nodiscard]] bool isFileOpen() const;

    /// Get info about the currently open file
    [[nodiscard]] sak::PstFileInfo fileInfo() const;

    // ====================================================================
    // Folder / Item Navigation
    // ====================================================================

    /// Load items for a specific folder
    void loadFolderItems(uint64_t folder_node_id, int offset, int limit);

    /// Load full detail for a specific item
    void loadItemDetail(uint64_t item_node_id);

    /// Load raw MAPI properties for an item
    void loadItemProperties(uint64_t item_node_id);

    /// Load attachment content
    void loadAttachmentContent(uint64_t message_node_id, int attachment_index);

    // ====================================================================
    // Search
    // ====================================================================

    /// Start a search across the currently open file
    void startSearch(const sak::EmailSearchCriteria& criteria);

    // ====================================================================
    // Export
    // ====================================================================

    /// Export items from PST/OST
    void exportItems(const sak::EmailExportConfig& config);

    // ====================================================================
    // Profile Manager
    // ====================================================================

    /// Discover all email client profiles
    void discoverProfiles();

    /// Backup selected profiles
    void backupProfiles(const QVector<int>& profile_indices, const QString& backup_path);

    /// Restore profiles from a manifest
    void restoreProfiles(const QString& manifest_path);

    // ====================================================================
    // Report
    // ====================================================================

    /// Generate an inspection report
    void generateReport(const QString& output_path,
                        const QString& technician,
                        const QString& ticket,
                        const QString& customer);

    // ====================================================================
    // Cancel / State
    // ====================================================================

    /// Cancel the current operation
    void cancelOperation();

    /// Get the current state
    [[nodiscard]] State currentState() const;

Q_SIGNALS:
    // State
    void stateChanged(EmailInspectorController::State state);

    // File
    void fileOpened(sak::PstFileInfo info);
    void folderTreeLoaded(sak::PstFolderTree tree);
    void fileClosed();

    // Navigation
    void folderItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total_count);
    void itemDetailLoaded(sak::PstItemDetail detail);
    void itemPropertiesLoaded(uint64_t item_id, QVector<sak::MapiProperty> properties);
    void attachmentContentReady(uint64_t message_id, int index, QByteArray data, QString filename);
    /// Emitted when a specific attachment's content could NOT be produced (extract
    /// failure, or a request refused because an operation was already in progress).
    /// Carries the same (message_id, index) identity as attachmentContentReady so a
    /// batch save correlates the failure to the exact request it issued. Every
    /// loadAttachmentContent call resolves to exactly one of ready/failed, so a batch
    /// never hangs on an unresolved request and never miscounts an unrelated error.
    void attachmentContentFailed(uint64_t message_id, int index, QString error);

    // Search
    void searchHit(sak::EmailSearchHit hit);
    void searchComplete(int total_hits);
    /// Terminal outcome for a cancelled search: partial_hits were found before the user stopped
    /// it. Distinct from searchComplete so the UI reports a cancel as a cancel (R5-P6-18).
    void searchCancelled(int partial_hits);

    // Export
    void exportStarted(int total_items);
    void exportProgress(int done, int total, qint64 bytes);
    void exportComplete(sak::EmailExportResult result);

    // Profile Manager
    void profilesDiscovered(QVector<sak::EmailClientProfile> profiles);
    void backupProgress(int files_done, int total, qint64 bytes);
    void backupComplete(QString path, int files, qint64 bytes);
    void restoreComplete(int profiles_restored);

    // Report
    void reportGenerated(QString output_path);

    // Common
    void progressUpdated(int percent, QString status);
    void errorOccurred(QString error);
    void logOutput(QString message);

    // MBOX-specific
    void mboxOpened(int message_count);
    void mboxMessagesLoaded(QVector<sak::MboxMessage> messages, int total_count);
    void mboxMessageDetailLoaded(sak::MboxMessageDetail detail);

private:
    void setState(State new_state);
    /// True when a tracked background task (search/export/profile/report) or a file
    /// open is in flight. Navigation must be refused in these states because it would
    /// race the same parser QFile that the background task is reading (B7-B).
    [[nodiscard]] bool isBusyWithBackgroundOp() const;
    void runTracked(std::function<void()> task);
    void connectPstSignals();
    void connectMboxSignals();
    void connectSearchSignals();
    void connectExportSignals();
    void connectProfileSignals();
    State m_state = State::Idle;

    // File type tracking
    enum class FileType {
        None,
        Pst,
        Ost,
        Mbox
    };
    FileType m_file_type = FileType::None;

    // Workers
    std::unique_ptr<PstParser> m_pst_parser;
    std::unique_ptr<MboxParser> m_mbox_parser;
    std::unique_ptr<EmailSearchWorker> m_search_worker;
    std::unique_ptr<EmailExportWorker> m_export_worker;
    std::unique_ptr<EmailProfileManager> m_profile_manager;
    std::unique_ptr<EmailReportGenerator> m_report_generator;

    /// Retains every detached QtConcurrent::run future so cancelOperation() (and
    /// thus the destructor) can block until no background thread is still using
    /// the parsers/workers before they are destroyed.
    QFutureSynchronizer<void> m_tasks;

    // Cached data for reports
    sak::PstFileInfo m_cached_file_info;
    sak::PstFolderTree m_cached_folder_tree;
    QVector<sak::EmailClientProfile> m_cached_profiles;
    QVector<sak::EmailExportResult> m_cached_exports;
    int m_search_count = 0;
    int m_total_search_hits = 0;
};
