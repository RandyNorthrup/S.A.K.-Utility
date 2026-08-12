// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_controller.cpp
/// @brief Orchestrates all email inspection workers

#include "sak/email_inspector_controller.h"

#include "sak/email_export_worker.h"
#include "sak/email_profile_manager.h"
#include "sak/email_report_generator.h"
#include "sak/email_search_worker.h"
#include "sak/io_write_utils.h"
#include "sak/logger.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>

#include <climits>

namespace {
bool writeReportFile(const QString& path, const QByteArray& content, const char* label) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning("Report: could not write {}: {}", label, path.toStdString());
        return false;
    }

    // Reject a short write, not just an error return: a partial report file read as
    // a completed one is worse than none (B7-18).
    const bool ok = sak::writeFully(file, content);
    file.close();
    if (!ok) {
        sak::logWarning("Report: {} write error: {}", label, path.toStdString());
        return false;
    }
    return true;
}

/// Accumulate per-item-type totals from the folder tree into the report statistics.
/// Folders are classified by their container class (mail folders -- IPF.Note or an
/// unset class -- count as emails); walked recursively so subfolders are included.
void accumulateFolderStats(const sak::PstFolderTree& tree, EmailReportGenerator::ReportData& data) {
    for (const auto& folder : tree) {
        const QString& cls = folder.container_class;
        if (cls.startsWith(QStringLiteral("IPF.Contact"), Qt::CaseInsensitive)) {
            data.total_contacts += folder.content_count;
        } else if (cls.startsWith(QStringLiteral("IPF.Appointment"), Qt::CaseInsensitive)) {
            data.total_calendar_items += folder.content_count;
        } else if (cls.startsWith(QStringLiteral("IPF.Task"), Qt::CaseInsensitive)) {
            data.total_tasks += folder.content_count;
        } else if (cls.startsWith(QStringLiteral("IPF.StickyNote"), Qt::CaseInsensitive)) {
            data.total_notes += folder.content_count;
        } else {
            data.total_emails += folder.content_count;
        }
        accumulateFolderStats(folder.children, data);
    }
}

/// Clean up a partial HTML/JSON report pair and build its failure message.
/// If one output was written before the other failed, remove the written file so no
/// misleading half-pair is left behind, then return the "could not write ..." message
/// naming the outputs that failed (B7-B). Removals happen in html-then-json order to
/// match the original inline side-effect order.
QString cleanupPartialReport(const QString& html_path,
                             bool html_ok,
                             const QString& json_path,
                             bool json_ok) {
    if (html_ok) {
        QFile::remove(html_path);
    }
    if (json_ok) {
        QFile::remove(json_path);
    }
    QStringList failed;
    if (!html_ok) {
        failed.append(QStringLiteral("HTML"));
    }
    if (!json_ok) {
        failed.append(QStringLiteral("JSON"));
    }
    return QStringLiteral("Report generation failed: could not write %1")
        .arg(failed.join(QStringLiteral(", ")));
}
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

EmailInspectorController::EmailInspectorController(QObject* parent)
    : QObject(parent)
    , m_pst_parser(std::make_unique<PstParser>(this))
    , m_mbox_parser(std::make_unique<MboxParser>(this))
    , m_search_worker(std::make_unique<EmailSearchWorker>(this))
    , m_export_worker(std::make_unique<EmailExportWorker>(this))
    , m_profile_manager(std::make_unique<EmailProfileManager>(this))
    , m_report_generator(std::make_unique<EmailReportGenerator>(this)) {
    connectPstSignals();
    connectMboxSignals();
    connectSearchSignals();
    connectExportSignals();
    connectProfileSignals();
}

EmailInspectorController::~EmailInspectorController() {
    cancelOperation();
}

// ============================================================================
// State Management
// ============================================================================

EmailInspectorController::State EmailInspectorController::currentState() const {
    return m_state;
}

void EmailInspectorController::setState(State new_state) {
    if (m_state != new_state) {
        m_state = new_state;
        Q_EMIT stateChanged(new_state);
    }
}

bool EmailInspectorController::isBusyWithBackgroundOp() const {
    switch (m_state) {
    case State::Idle:
    case State::LoadingFolderItems:
    case State::LoadingItemDetail:
    case State::LoadingProperties:
        // Idle, or a parser-serialized navigation load already in flight: safe to
        // dispatch the next navigation call.
        return false;
    default:
        // Opening / Searching / Exporting / profile ops / report: a background task
        // or file open is reading the parser; navigating now would race its QFile.
        return true;
    }
}

// ============================================================================
// File Operations
// ============================================================================

void EmailInspectorController::openFile(const QString& file_path) {
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot open file: another operation in progress"));
        return;
    }

    closeFile();
    setState(State::Opening);

    const QFileInfo fi(file_path);
    const QString suffix = fi.suffix().toLower();

    if (suffix == QStringLiteral("pst") || suffix == QStringLiteral("ost")) {
        const bool is_ost = (suffix == QStringLiteral("ost"));
        m_file_type = is_ost ? FileType::Ost : FileType::Pst;
        sak::logInfo("Opening {} file: {}", is_ost ? "OST" : "PST", file_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Opening %1...").arg(fi.fileName()));
        m_pst_parser->open(file_path);
    } else if (suffix == QStringLiteral("mbox")) {
        m_file_type = FileType::Mbox;
        sak::logInfo("Opening MBOX file: {}", file_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Opening %1...").arg(fi.fileName()));
        m_mbox_parser->open(file_path);
    } else {
        // Also the rejection for an empty path: it has no suffix, so it can never
        // match a supported format.
        m_file_type = FileType::None;
        setState(State::Idle);
        Q_EMIT errorOccurred(QStringLiteral("Unsupported file format: %1").arg(suffix));
    }
}

void EmailInspectorController::closeFile() {
    // Cancel and await any in-flight background task BEFORE tearing down the parser
    // it may still be reading through: close() otherwise races a QtConcurrent pool
    // thread still inside the parser (use-after-close). This mirrors the destructor
    // and the state guards that keep only one operation live at a time. (B7-15)
    cancelOperation();

    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        m_pst_parser->close();
    } else if (m_file_type == FileType::Mbox) {
        m_mbox_parser->close();
    }

    m_file_type = FileType::None;
    m_cached_file_info = {};
    m_cached_folder_tree.clear();
    setState(State::Idle);
    Q_EMIT fileClosed();
}

bool EmailInspectorController::isFileOpen() const {
    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        return m_pst_parser->isOpen();
    }
    if (m_file_type == FileType::Mbox) {
        return m_mbox_parser->isOpen();
    }
    return false;
}

sak::PstFileInfo EmailInspectorController::fileInfo() const {
    return m_cached_file_info;
}

// ============================================================================
// Folder / Item Navigation
// ============================================================================

void EmailInspectorController::loadFolderItems(uint64_t folder_node_id, int offset, int limit) {
    if (isBusyWithBackgroundOp()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot navigate: operation in progress"));
        return;
    }
    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        setState(State::LoadingFolderItems);
        m_pst_parser->loadFolderItems(folder_node_id, offset, limit);
    } else if (m_file_type == FileType::Mbox) {
        setState(State::LoadingFolderItems);
        m_mbox_parser->loadMessages(offset, limit);
    }
}

void EmailInspectorController::loadItemDetail(uint64_t item_node_id) {
    if (isBusyWithBackgroundOp()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot navigate: operation in progress"));
        return;
    }
    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        setState(State::LoadingItemDetail);
        m_pst_parser->loadItemDetail(item_node_id);
    } else if (m_file_type == FileType::Mbox) {
        // MBOX item ids are message indices. Narrowing an id above INT_MAX would
        // wrap to a different, in-range index and silently load the wrong
        // message, so reject it here instead of casting.
        if (item_node_id > static_cast<uint64_t>(INT_MAX)) {
            Q_EMIT errorOccurred(QStringLiteral("Invalid MBOX message id: %1").arg(item_node_id));
            return;
        }
        setState(State::LoadingItemDetail);
        m_mbox_parser->loadMessageDetail(static_cast<int>(item_node_id));
    }
}

void EmailInspectorController::loadItemProperties(uint64_t item_node_id) {
    if (isBusyWithBackgroundOp()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot navigate: operation in progress"));
        return;
    }
    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        setState(State::LoadingProperties);
        m_pst_parser->loadItemProperties(item_node_id);
    } else if (m_file_type == FileType::Mbox) {
        // MBOX has no MAPI properties; emit empty result
        Q_EMIT itemPropertiesLoaded(item_node_id, {});
    }
}

void EmailInspectorController::loadAttachmentContent(uint64_t message_node_id,
                                                     int attachment_index) {
    if (isBusyWithBackgroundOp()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot navigate: operation in progress"));
        return;
    }
    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        m_pst_parser->loadAttachmentContent(message_node_id, attachment_index);
    } else if (m_file_type == FileType::Mbox) {
        const int msg_idx = static_cast<int>(message_node_id);
        runTracked([this, msg_idx, attachment_index]() {
            auto result = m_mbox_parser->readAttachmentData(msg_idx, attachment_index);
            if (result) {
                auto detail = m_mbox_parser->readMessageDetail(msg_idx);
                QString filename;
                if (detail && attachment_index < detail->attachments.size()) {
                    filename = detail->attachments[attachment_index].long_filename;
                }
                Q_EMIT attachmentContentReady(
                    static_cast<uint64_t>(msg_idx), attachment_index, *result, filename);
            } else {
                Q_EMIT errorOccurred(QStringLiteral("Failed to extract attachment"));
            }
        });
    }
}

// ============================================================================
// Search
// ============================================================================

void EmailInspectorController::startSearch(const sak::EmailSearchCriteria& criteria) {
    if (m_state != State::Idle && m_state != State::LoadingFolderItems) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot search: operation in progress"));
        return;
    }
    // With no file open, neither branch below launches a task, so the state would
    // latch at Searching forever. Refuse without changing state (B7-23).
    if (!isFileOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot search: no file is open"));
        return;
    }

    setState(State::Searching);
    m_search_count++;

    Q_EMIT logOutput(QStringLiteral("Searching for \"%1\"...").arg(criteria.query_text));

    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        runTracked([this, criteria]() { m_search_worker->search(m_pst_parser.get(), criteria); });
    } else if (m_file_type == FileType::Mbox) {
        runTracked(
            [this, criteria]() { m_search_worker->searchMbox(m_mbox_parser.get(), criteria); });
    }
}

// ============================================================================
// Export
// ============================================================================

void EmailInspectorController::exportItems(const sak::EmailExportConfig& config) {
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot export: operation in progress"));
        return;
    }
    // No open file -> no export task is launched; refuse rather than latch the state
    // at Exporting forever (B7-23).
    if (!isFileOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot export: no file is open"));
        return;
    }

    setState(State::Exporting);
    Q_EMIT logOutput(QStringLiteral("Starting export to %1...").arg(config.output_path));

    if (m_file_type == FileType::Pst || m_file_type == FileType::Ost) {
        runTracked([this, config]() { m_export_worker->exportItems(m_pst_parser.get(), config); });
    } else if (m_file_type == FileType::Mbox) {
        runTracked(
            [this, config]() { m_export_worker->exportMboxItems(m_mbox_parser.get(), config); });
    }
}

// ============================================================================
// Profile Manager
// ============================================================================

void EmailInspectorController::discoverProfiles() {
    // Single-flight (B7-16): discovery/backup/restore share the profile manager's
    // m_profiles / m_backup_dest_names / m_cancelled, so refuse to launch a second
    // background operation while any is running instead of racing those members.
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot discover profiles: another operation in progress"));
        return;
    }
    setState(State::DiscoveringProfiles);
    Q_EMIT logOutput(QStringLiteral("Discovering email client profiles..."));

    runTracked([this]() { m_profile_manager->discoverProfiles(); });
}

void EmailInspectorController::backupProfiles(const QVector<int>& profile_indices,
                                              const QString& backup_path) {
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot back up profiles: another operation in progress"));
        return;
    }
    setState(State::BackingUp);
    Q_EMIT logOutput(QStringLiteral("Backing up profiles to %1...").arg(backup_path));

    runTracked([this, profile_indices, backup_path]() {
        m_profile_manager->backupProfiles(profile_indices, backup_path);
    });
}

void EmailInspectorController::restoreProfiles(const QString& manifest_path) {
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot restore profiles: another operation in progress"));
        return;
    }
    setState(State::Restoring);
    Q_EMIT logOutput(QStringLiteral("Restoring profiles from %1...").arg(manifest_path));

    runTracked([this, manifest_path]() { m_profile_manager->restoreProfiles(manifest_path); });
}

// ============================================================================
// Report Generation
// ============================================================================

void EmailInspectorController::generateReport(const QString& output_path,
                                              const QString& technician,
                                              const QString& ticket,
                                              const QString& customer) {
    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot generate report: another operation in progress"));
        return;
    }
    setState(State::GeneratingReport);

    const QDir output_dir(output_path);
    if (!output_dir.mkpath(QStringLiteral("."))) {
        const auto message = QStringLiteral("Cannot create report directory: %1").arg(output_path);
        sak::logError("Report: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        setState(State::Idle);
        return;
    }

    EmailReportGenerator::ReportData data;
    data.technician_name = technician;
    data.ticket_number = ticket;
    data.customer_name = customer;
    data.report_date = QDateTime::currentDateTime();
    data.file_info = m_cached_file_info;
    data.folder_tree = m_cached_folder_tree;
    data.export_results = m_cached_exports;
    data.discovered_profiles = m_cached_profiles;
    data.searches_performed = m_search_count;
    data.total_search_hits = m_total_search_hits;
    // Populate the per-item-type totals from the cached folder tree so the report's
    // statistics section is not left at zero (B7-B).
    accumulateFolderStats(m_cached_folder_tree, data);

    // Both outputs must succeed: a report where only one of HTML/JSON was written is a
    // partial, misleading deliverable, so surface which output failed and fail closed
    // rather than reporting the report generated (B7-B).
    const QString html_path = output_path + QStringLiteral("/email_report.html");
    const bool html_ok =
        writeReportFile(html_path, m_report_generator->generateHtml(data).toUtf8(), "HTML");

    const QString json_path = output_path + QStringLiteral("/email_report.json");
    const bool json_ok = writeReportFile(json_path, m_report_generator->generateJson(data), "JSON");

    if (html_ok && json_ok) {
        sak::logInfo("Report generated at: {}", output_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Report saved to %1").arg(output_path));
        Q_EMIT reportGenerated(output_path);
    } else {
        // A partial HTML/JSON pair is a misleading deliverable: remove whichever file was
        // written and fail closed, naming the output(s) that failed (B7-B).
        const auto message = cleanupPartialReport(html_path, html_ok, json_path, json_ok);
        sak::logError("Report: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
    }
    setState(State::Idle);
}

// ============================================================================
// Cancel
// ============================================================================

void EmailInspectorController::runTracked(std::function<void()> task) {
    // Retain the future so cancelOperation() can wait for it: a bare, discarded
    // QtConcurrent::run would let the pool thread outlive the controller and
    // dereference freed parser/worker members.
    m_tasks.addFuture(QtConcurrent::run(std::move(task)));
}

void EmailInspectorController::cancelOperation() {
    m_pst_parser->cancel();
    m_mbox_parser->cancel();
    m_search_worker->cancel();
    m_export_worker->cancel();
    m_profile_manager->cancel();
    // Block until every in-flight task observes the cancel flags and returns, so
    // no background thread is still inside the parsers/workers when they (and this
    // controller) are destroyed.
    // SAK-ALLOW-BLOCKING: every parser and worker was cancelled on the lines above and
    // polls that flag, so the tasks return on their own; abandoning the wait would leave
    // them running inside objects about to be freed.
    m_tasks.waitForFinished();
    m_tasks.clearFutures();
    setState(State::Idle);
}

// ============================================================================
// Signal Connections
// ============================================================================

void EmailInspectorController::connectPstSignals() {
    connect(m_pst_parser.get(), &PstParser::fileOpened, this, [this](sak::PstFileInfo info) {
        m_cached_file_info = info;
        Q_EMIT fileOpened(info);
        Q_EMIT logOutput(
            QStringLiteral("Opened: %1 (%2 items)").arg(info.display_name).arg(info.total_items));
    });

    connect(
        m_pst_parser.get(), &PstParser::folderTreeLoaded, this, [this](sak::PstFolderTree tree) {
            m_cached_folder_tree = tree;
            Q_EMIT folderTreeLoaded(tree);
            setState(State::Idle);
        });

    connect(m_pst_parser.get(),
            &PstParser::folderItemsLoaded,
            this,
            [this](uint64_t fid, QVector<sak::PstItemSummary> items, int total) {
                Q_EMIT folderItemsLoaded(fid, items, total);
                setState(State::Idle);
            });

    connect(
        m_pst_parser.get(), &PstParser::itemDetailLoaded, this, [this](sak::PstItemDetail detail) {
            Q_EMIT itemDetailLoaded(detail);
            setState(State::Idle);
        });

    connect(m_pst_parser.get(),
            &PstParser::itemPropertiesLoaded,
            this,
            [this](uint64_t id, QVector<sak::MapiProperty> props) {
                Q_EMIT itemPropertiesLoaded(id, props);
                setState(State::Idle);
            });

    connect(m_pst_parser.get(),
            &PstParser::attachmentContentReady,
            this,
            [this](uint64_t mid, int idx, QByteArray data, QString name) {
                Q_EMIT attachmentContentReady(mid, idx, data, name);
            });

    connect(m_pst_parser.get(),
            &PstParser::progressUpdated,
            this,
            &EmailInspectorController::progressUpdated);

    connect(m_pst_parser.get(), &PstParser::errorOccurred, this, [this](QString err) {
        Q_EMIT errorOccurred(err);
        setState(State::Idle);
    });
}

void EmailInspectorController::connectMboxSignals() {
    connect(
        m_mbox_parser.get(), &MboxParser::fileOpened, this, [this](const QString& path, int count) {
            Q_UNUSED(path);
            sak::PstFileInfo info;
            info.file_path = m_mbox_parser->filePath();
            info.display_name = QFileInfo(info.file_path).fileName();
            info.total_items = count;
            info.is_unicode = true;
            m_cached_file_info = info;
            Q_EMIT mboxOpened(count);
            Q_EMIT fileOpened(info);
            setState(State::Idle);
        });

    connect(m_mbox_parser.get(),
            &MboxParser::messagesLoaded,
            this,
            [this](QVector<sak::MboxMessage> msgs, int total) {
                Q_EMIT mboxMessagesLoaded(msgs, total);
                setState(State::Idle);
            });

    connect(m_mbox_parser.get(),
            &MboxParser::messageDetailLoaded,
            this,
            [this](sak::MboxMessageDetail detail) {
                Q_EMIT mboxMessageDetailLoaded(detail);
                setState(State::Idle);
            });

    connect(m_mbox_parser.get(),
            &MboxParser::progressUpdated,
            this,
            &EmailInspectorController::progressUpdated);

    connect(m_mbox_parser.get(), &MboxParser::errorOccurred, this, [this](QString err) {
        Q_EMIT errorOccurred(err);
        setState(State::Idle);
    });
}

void EmailInspectorController::connectSearchSignals() {
    connect(m_search_worker.get(),
            &EmailSearchWorker::searchHit,
            this,
            &EmailInspectorController::searchHit);

    connect(m_search_worker.get(), &EmailSearchWorker::searchComplete, this, [this](int total) {
        m_total_search_hits += total;
        Q_EMIT searchComplete(total);
        Q_EMIT logOutput(QStringLiteral("Search complete: %1 hits").arg(total));
        setState(State::Idle);
    });

    connect(m_search_worker.get(), &EmailSearchWorker::errorOccurred, this, [this](QString err) {
        // A search errorOccurred is NON-terminal: EmailSearchWorker reports a per-folder read
        // failure and keeps scanning the remaining folders, then always emits searchComplete on
        // exit. Clearing the busy state here would drop m_state to Idle mid-scan and let a second
        // operation (export/search) enter the unsynchronized parser while the pool thread is still
        // reading it. Forward the error only; the terminal searchComplete handler is the single
        // place that returns to Idle.
        Q_EMIT errorOccurred(err);
    });
}

void EmailInspectorController::connectExportSignals() {
    connect(m_export_worker.get(),
            &EmailExportWorker::exportStarted,
            this,
            &EmailInspectorController::exportStarted);

    connect(m_export_worker.get(),
            &EmailExportWorker::exportProgress,
            this,
            &EmailInspectorController::exportProgress);

    connect(m_export_worker.get(),
            &EmailExportWorker::exportComplete,
            this,
            [this](sak::EmailExportResult result) {
                m_cached_exports.append(result);
                Q_EMIT exportComplete(result);
                // EmailExportWorker delivers outright failures, cancellations and partial exports
                // through this SAME exportComplete channel (its errorOccurred is declared but never
                // emitted), recording the reason in result.errors / result.items_failed. Logging
                // every one as "Export complete" reports failures as successes, so branch on the
                // real outcome and surface an error when nothing (or not everything) was exported.
                if (result.items_exported == 0 &&
                    (!result.errors.isEmpty() || result.items_failed > 0)) {
                    Q_EMIT errorOccurred(
                        result.errors.isEmpty()
                            ? QStringLiteral("Export failed: %1 item(s) could not be exported")
                                  .arg(result.items_failed)
                            : QStringLiteral("Export failed: %1")
                                  .arg(result.errors.join(QStringLiteral("; "))));
                } else if (result.items_failed > 0 || !result.errors.isEmpty()) {
                    Q_EMIT errorOccurred(
                        QStringLiteral("Export incomplete: %1 item(s) exported, %2 failed -- %3")
                            .arg(result.items_exported)
                            .arg(result.items_failed)
                            .arg(result.errors.join(QStringLiteral("; "))));
                } else {
                    Q_EMIT logOutput(QStringLiteral("Export complete: %1 items (%2)")
                                         .arg(result.items_exported)
                                         .arg(result.export_format));
                }
                setState(State::Idle);
            });

    connect(m_export_worker.get(), &EmailExportWorker::errorOccurred, this, [this](QString err) {
        Q_EMIT errorOccurred(err);
        setState(State::Idle);
    });
}

void EmailInspectorController::connectProfileSignals() {
    connect(m_profile_manager.get(),
            &EmailProfileManager::profilesDiscovered,
            this,
            [this](QVector<sak::EmailClientProfile> profiles) {
                m_cached_profiles = profiles;
                Q_EMIT profilesDiscovered(profiles);
                Q_EMIT logOutput(QStringLiteral("Found %1 email profiles").arg(profiles.size()));
                setState(State::Idle);
            });

    connect(m_profile_manager.get(),
            &EmailProfileManager::backupProgress,
            this,
            &EmailInspectorController::backupProgress);

    connect(m_profile_manager.get(),
            &EmailProfileManager::backupComplete,
            this,
            [this](QString path, int files, qint64 bytes) {
                Q_EMIT backupComplete(path, files, bytes);
                setState(State::Idle);
            });

    connect(
        m_profile_manager.get(), &EmailProfileManager::restoreComplete, this, [this](int count) {
            Q_EMIT restoreComplete(count);
            setState(State::Idle);
        });

    connect(
        m_profile_manager.get(), &EmailProfileManager::errorOccurred, this, [this](QString err) {
            Q_EMIT errorOccurred(err);
            setState(State::Idle);
        });
}
