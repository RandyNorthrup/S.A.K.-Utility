// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_controller.cpp
/// @brief Orchestrates all email inspection workers

#include "sak/email_inspector_controller.h"

#include "sak/email_export_worker.h"
#include "sak/email_profile_manager.h"
#include "sak/email_report_generator.h"
#include "sak/email_search_worker.h"
#include "sak/logger.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>

#include <climits>

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

// ============================================================================
// File Operations
// ============================================================================

void EmailInspectorController::openFile(const QString& file_path) {
    Q_ASSERT(!file_path.isEmpty());

    if (m_state != State::Idle) {
        Q_EMIT errorOccurred(QStringLiteral("Cannot open file: another operation in progress"));
        return;
    }

    closeFile();
    setState(State::Opening);

    QFileInfo fi(file_path);
    QString suffix = fi.suffix().toLower();

    if (suffix == QStringLiteral("pst") || suffix == QStringLiteral("ost")) {
        m_file_type = FileType::Pst;
        sak::logInfo("Opening PST/OST file: {}", file_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Opening %1...").arg(fi.fileName()));
        m_pst_parser->open(file_path);
    } else if (suffix == QStringLiteral("mbox")) {
        m_file_type = FileType::Mbox;
        sak::logInfo("Opening MBOX file: {}", file_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Opening %1...").arg(fi.fileName()));
        m_mbox_parser->open(file_path);
    } else {
        m_file_type = FileType::None;
        setState(State::Idle);
        Q_EMIT errorOccurred(QStringLiteral("Unsupported file format: %1").arg(suffix));
    }
}

void EmailInspectorController::closeFile() {
    if (m_file_type == FileType::Pst) {
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
    if (m_file_type == FileType::Pst) {
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
    if (m_file_type == FileType::Pst) {
        setState(State::LoadingFolderItems);
        m_pst_parser->loadFolderItems(folder_node_id, offset, limit);
    } else if (m_file_type == FileType::Mbox) {
        setState(State::LoadingFolderItems);
        m_mbox_parser->loadMessages(offset, limit);
    }
}

void EmailInspectorController::loadItemDetail(uint64_t item_node_id) {
    if (m_file_type == FileType::Pst) {
        setState(State::LoadingItemDetail);
        m_pst_parser->loadItemDetail(item_node_id);
    } else if (m_file_type == FileType::Mbox) {
        Q_ASSERT(item_node_id <= static_cast<uint64_t>(INT_MAX));
        setState(State::LoadingItemDetail);
        m_mbox_parser->loadMessageDetail(static_cast<int>(item_node_id));
    }
}

void EmailInspectorController::loadItemProperties(uint64_t item_node_id) {
    if (m_file_type == FileType::Pst) {
        setState(State::LoadingProperties);
        m_pst_parser->loadItemProperties(item_node_id);
    } else if (m_file_type == FileType::Mbox) {
        // MBOX has no MAPI properties; emit empty result
        Q_EMIT itemPropertiesLoaded(item_node_id, {});
    }
}

void EmailInspectorController::loadAttachmentContent(uint64_t message_node_id,
                                                     int attachment_index) {
    if (m_file_type == FileType::Pst) {
        m_pst_parser->loadAttachmentContent(message_node_id, attachment_index);
    } else if (m_file_type == FileType::Mbox) {
        int msg_idx = static_cast<int>(message_node_id);
        (void)QtConcurrent::run([this, msg_idx, attachment_index]() {
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

    setState(State::Searching);
    m_search_count++;

    Q_EMIT logOutput(QStringLiteral("Searching for \"%1\"...").arg(criteria.query_text));

    if (m_file_type == FileType::Pst) {
        (void)QtConcurrent::run(
            [this, criteria]() { m_search_worker->search(m_pst_parser.get(), criteria); });
    } else if (m_file_type == FileType::Mbox) {
        (void)QtConcurrent::run(
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

    setState(State::Exporting);
    Q_EMIT logOutput(QStringLiteral("Starting export to %1...").arg(config.output_path));

    if (m_file_type == FileType::Pst) {
        (void)QtConcurrent::run(
            [this, config]() { m_export_worker->exportItems(m_pst_parser.get(), config); });
    } else if (m_file_type == FileType::Mbox) {
        (void)QtConcurrent::run(
            [this, config]() { m_export_worker->exportMboxItems(m_mbox_parser.get(), config); });
    }
}

// ============================================================================
// Profile Manager
// ============================================================================

void EmailInspectorController::discoverProfiles() {
    setState(State::DiscoveringProfiles);
    Q_EMIT logOutput(QStringLiteral("Discovering email client profiles..."));

    (void)QtConcurrent::run([this]() { m_profile_manager->discoverProfiles(); });
}

void EmailInspectorController::backupProfiles(const QVector<int>& profile_indices,
                                              const QString& backup_path) {
    setState(State::BackingUp);
    Q_EMIT logOutput(QStringLiteral("Backing up profiles to %1...").arg(backup_path));

    (void)QtConcurrent::run([this, profile_indices, backup_path]() {
        m_profile_manager->backupProfiles(profile_indices, backup_path);
    });
}

void EmailInspectorController::restoreProfiles(const QString& manifest_path) {
    setState(State::Restoring);
    Q_EMIT logOutput(QStringLiteral("Restoring profiles from %1...").arg(manifest_path));

    (void)QtConcurrent::run(
        [this, manifest_path]() { m_profile_manager->restoreProfiles(manifest_path); });
}

// ============================================================================
// Report Generation
// ============================================================================

void EmailInspectorController::generateReport(const QString& output_path,
                                              const QString& technician,
                                              const QString& ticket,
                                              const QString& customer) {
    setState(State::GeneratingReport);

    QDir output_dir(output_path);
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

    bool any_written = false;

    // Write HTML report
    QString html = m_report_generator->generateHtml(data);
    QString html_path = output_path + QStringLiteral("/email_report.html");
    QFile html_file(html_path);
    if (!html_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning("Report: could not write HTML: {}", html_path.toStdString());
    } else {
        qint64 bytes = html_file.write(html.toUtf8());
        html_file.close();
        if (bytes < 0) {
            sak::logWarning("Report: HTML write error: {}", html_path.toStdString());
        } else {
            any_written = true;
        }
    }

    // Write JSON report
    QByteArray json = m_report_generator->generateJson(data);
    QString json_path = output_path + QStringLiteral("/email_report.json");
    QFile json_file(json_path);
    if (!json_file.open(QIODevice::WriteOnly)) {
        sak::logWarning("Report: could not write JSON: {}", json_path.toStdString());
    } else {
        qint64 bytes = json_file.write(json);
        json_file.close();
        if (bytes < 0) {
            sak::logWarning("Report: JSON write error: {}", json_path.toStdString());
        } else {
            any_written = true;
        }
    }

    if (any_written) {
        sak::logInfo("Report generated at: {}", output_path.toStdString());
        Q_EMIT logOutput(QStringLiteral("Report saved to %1").arg(output_path));
        Q_EMIT reportGenerated(output_path);
    } else {
        const auto message = QStringLiteral(
            "Report generation failed: "
            "could not write any files");
        sak::logError("Report: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
    }
    setState(State::Idle);
}

// ============================================================================
// Cancel
// ============================================================================

void EmailInspectorController::cancelOperation() {
    m_pst_parser->cancel();
    m_mbox_parser->cancel();
    m_search_worker->cancel();
    m_export_worker->cancel();
    m_profile_manager->cancel();
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
        Q_EMIT errorOccurred(err);
        setState(State::Idle);
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
                Q_EMIT logOutput(QStringLiteral("Export complete: %1 items (%2)")
                                     .arg(result.items_exported)
                                     .arg(result.export_format));
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
