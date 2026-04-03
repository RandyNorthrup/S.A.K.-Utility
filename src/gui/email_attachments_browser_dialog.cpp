// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachments_browser_dialog.cpp
/// @brief Modal dialog for browsing all attachments in a mailbox

#include "sak/email_attachments_browser_dialog.h"

#include "sak/email_attachment_saver.h"
#include "sak/email_constants.h"
#include "sak/email_inspector_controller.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFileDialog>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Table column indices
// ============================================================================

enum AttachmentColumn {
    ColFilename = 0,
    ColSize,
    ColType,
    ColSourceSubject,
    ColSourceSender,
    ColSourceDate,
    ColCount
};

// ============================================================================
// Type filter categories
// ============================================================================

static const QString kFilterAll = QStringLiteral("All Types");
static const QString kFilterImages = QStringLiteral("Images");
static const QString kFilterDocuments = QStringLiteral("Documents");
static const QString kFilterArchives = QStringLiteral("Archives");
static const QString kFilterAudio = QStringLiteral("Audio/Video");
static const QString kFilterOther = QStringLiteral("Other");

// ============================================================================
// Constructor / Destructor
// ============================================================================

EmailAttachmentsBrowserDialog::EmailAttachmentsBrowserDialog(::EmailInspectorController* controller,
                                                             const PstFolderTree& folder_tree,
                                                             QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setWindowTitle(tr("Attachments Browser"));
    setModal(true);
    resize(kWizardLargeWidth + 100, kWizardLargeHeight);
    setupUi();
    collectFolderIds(folder_tree);
    QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::startScan);
}

EmailAttachmentsBrowserDialog::~EmailAttachmentsBrowserDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void EmailAttachmentsBrowserDialog::setupUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    root->setSpacing(ui::kSpacingMedium);

    root->addLayout(createToolbarRow());

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFormat(tr("Scanning... %p%"));
    root->addWidget(m_progress_bar);

    root->addWidget(createAttachmentTable(), 1);
    root->addLayout(createButtonRow());
}

QHBoxLayout* EmailAttachmentsBrowserDialog::createToolbarRow() {
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(ui::kSpacingMedium);

    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(
        tr("\xF0\x9F\x94\x8D Search attachments by filename or email..."));
    m_search_edit->setClearButtonEnabled(true);
    connect(m_search_edit,
            &QLineEdit::textChanged,
            this,
            &EmailAttachmentsBrowserDialog::onSearchTextChanged);
    toolbar->addWidget(m_search_edit, 1);

    m_search_timer = new QTimer(this);
    m_search_timer->setSingleShot(true);
    m_search_timer->setInterval(email::kSearchDebounceMs);
    connect(
        m_search_timer, &QTimer::timeout, this, &EmailAttachmentsBrowserDialog::onSearchTimerFired);

    m_type_filter = new QComboBox(this);
    m_type_filter->addItems(
        {kFilterAll, kFilterImages, kFilterDocuments, kFilterArchives, kFilterAudio, kFilterOther});
    m_type_filter->setMinimumWidth(140);
    connect(m_type_filter,
            &QComboBox::currentIndexChanged,
            this,
            &EmailAttachmentsBrowserDialog::onTypeFilterChanged);
    toolbar->addWidget(m_type_filter);

    return toolbar;
}

QTableWidget* EmailAttachmentsBrowserDialog::createAttachmentTable() {
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels(
        {tr("Filename"), tr("Size"), tr("Type"), tr("Source Email"), tr("Sender"), tr("Date")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSortingEnabled(true);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);

    m_table->setColumnWidth(ColFilename, 220);
    m_table->setColumnWidth(ColSize, 80);
    m_table->setColumnWidth(ColType, 100);
    m_table->setColumnWidth(ColSourceSubject, 200);
    m_table->setColumnWidth(ColSourceSender, 140);

    connect(m_table,
            &QTableWidget::itemSelectionChanged,
            this,
            &EmailAttachmentsBrowserDialog::onSelectionChanged);

    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table,
            &QTableWidget::customContextMenuRequested,
            this,
            &EmailAttachmentsBrowserDialog::onTableContextMenu);

    return m_table;
}

QHBoxLayout* EmailAttachmentsBrowserDialog::createButtonRow() {
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(ui::kSpacingMedium);

    m_status_label = new QLabel(this);
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextSecondary));
    buttons->addWidget(m_status_label, 1);

    m_save_selected_button = new QPushButton(tr("Save Selected"), this);
    m_save_selected_button->setStyleSheet(ui::kSecondaryButtonStyle);
    m_save_selected_button->setEnabled(false);
    connect(m_save_selected_button,
            &QPushButton::clicked,
            this,
            &EmailAttachmentsBrowserDialog::onSaveSelectedClicked);
    buttons->addWidget(m_save_selected_button);

    m_save_all_button = new QPushButton(tr("Save All Visible"), this);
    m_save_all_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_save_all_button->setEnabled(false);
    connect(m_save_all_button,
            &QPushButton::clicked,
            this,
            &EmailAttachmentsBrowserDialog::onSaveAllVisibleClicked);
    buttons->addWidget(m_save_all_button);

    m_close_button = new QPushButton(tr("Close"), this);
    m_close_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(m_close_button, &QPushButton::clicked, this, &QDialog::close);
    buttons->addWidget(m_close_button);

    return buttons;
}

// ============================================================================
// Folder collection — recursively gather all folder IDs
// ============================================================================

void EmailAttachmentsBrowserDialog::collectFolderIds(const PstFolderTree& tree) {
    for (const auto& folder : tree) {
        collectFolderIdsRecursive(folder);
    }
}

void EmailAttachmentsBrowserDialog::collectFolderIdsRecursive(const PstFolder& folder) {
    if (folder.content_count > 0) {
        m_all_folder_ids.append(folder.node_id);
    }
    for (const auto& child : folder.children) {
        collectFolderIdsRecursive(child);
    }
}

// ============================================================================
// Scan — iterate all folders, collect items with attachments
// ============================================================================

void EmailAttachmentsBrowserDialog::startScan() {
    m_folders_scanned = 0;
    m_progress_bar->setRange(0, m_all_folder_ids.size());
    m_progress_bar->setValue(0);
    m_status_label->setText(tr("Scanning folders for attachments..."));

    connect(m_controller,
            &::EmailInspectorController::folderItemsLoaded,
            this,
            &EmailAttachmentsBrowserDialog::onFolderItemsLoaded);
    connect(m_controller,
            &::EmailInspectorController::itemDetailLoaded,
            this,
            &EmailAttachmentsBrowserDialog::onItemDetailLoaded);
    connect(m_controller,
            &::EmailInspectorController::attachmentContentReady,
            this,
            &EmailAttachmentsBrowserDialog::onAttachmentContentReady);
    connect(m_controller,
            &::EmailInspectorController::errorOccurred,
            this,
            &EmailAttachmentsBrowserDialog::onErrorOccurred);

    scanNextFolder();
}

void EmailAttachmentsBrowserDialog::scanNextFolder() {
    if (m_folders_scanned >= m_all_folder_ids.size()) {
        // All folders scanned — now load details for items with attachments
        m_details_total = m_pending_detail_ids.size();
        m_details_loaded = 0;
        if (m_details_total == 0) {
            m_scan_complete = true;
            m_progress_bar->hide();
            updateStatusLabel();
            return;
        }
        m_progress_bar->setRange(0, m_details_total);
        m_progress_bar->setValue(0);
        m_progress_bar->setFormat(tr("Loading attachment details... %p%"));
        m_status_label->setText(tr("Loading details for %1 items...").arg(m_details_total));
        QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::requestNextDetail);
        return;
    }
    m_current_folder_offset = 0;
    uint64_t folder_id = m_all_folder_ids[m_folders_scanned];
    m_controller->loadFolderItems(folder_id, 0, email::kMaxItemsPerLoad);
}

void EmailAttachmentsBrowserDialog::requestNextDetail() {
    if (m_details_loaded >= m_pending_detail_ids.size()) {
        m_scan_complete = true;
        m_progress_bar->hide();
        rebuildTable();
        updateStatusLabel();
        m_save_all_button->setEnabled(!m_all_attachments.isEmpty());
        return;
    }
    m_controller->loadItemDetail(m_pending_detail_ids[m_details_loaded]);
}

// ============================================================================
// Data loading slots
// ============================================================================

void EmailAttachmentsBrowserDialog::onFolderItemsLoaded(uint64_t /*folder_id*/,
                                                        QVector<sak::PstItemSummary> items,
                                                        int total) {
    for (const auto& item : items) {
        if (item.has_attachments) {
            m_pending_detail_ids.append(item.node_id);
            m_message_folder_map.insert(item.node_id, m_all_folder_ids[m_folders_scanned]);
        }
    }

    m_current_folder_offset += static_cast<int>(items.size());

    // If this folder has more items, load the next page
    if (m_current_folder_offset < total && !items.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_folders_scanned < m_all_folder_ids.size()) {
                uint64_t fid = m_all_folder_ids[m_folders_scanned];
                m_controller->loadFolderItems(fid,
                                              m_current_folder_offset,
                                              email::kMaxItemsPerLoad);
            }
        });
        return;
    }

    ++m_folders_scanned;
    m_current_folder_offset = 0;
    m_progress_bar->setValue(m_folders_scanned);
    m_status_label->setText(tr("Scanning folder %1 of %2... (%3 items with attachments)")
                                .arg(m_folders_scanned)
                                .arg(m_all_folder_ids.size())
                                .arg(m_pending_detail_ids.size()));
    QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::scanNextFolder);
}

void EmailAttachmentsBrowserDialog::onItemDetailLoaded(sak::PstItemDetail detail) {
    for (const auto& att : detail.attachments) {
        // Skip embedded messages (method 5) and OLE objects (method 6)
        if (att.attach_method == 5 || att.attach_method == 6) {
            continue;
        }
        AttachmentEntry entry;
        entry.message_node_id = detail.node_id;
        entry.source_folder_id = m_message_folder_map.value(detail.node_id, 0);
        entry.attachment_index = att.index;
        entry.filename = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        entry.size_bytes = att.size_bytes;
        entry.mime_type = att.mime_type;
        entry.source_subject = detail.subject;
        entry.source_sender = detail.sender_name;
        entry.source_date = detail.date;
        m_all_attachments.append(std::move(entry));
    }

    ++m_details_loaded;
    m_progress_bar->setValue(m_details_loaded);

    // Periodically refresh table during scan
    constexpr int kTableRefreshInterval = 50;
    if (m_details_loaded % kTableRefreshInterval == 0) {
        rebuildTable();
        updateStatusLabel();
    }

    QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::requestNextDetail);
}

// ============================================================================
// Table management
// ============================================================================

void EmailAttachmentsBrowserDialog::rebuildTable() {
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    for (int idx = 0; idx < m_all_attachments.size(); ++idx) {
        const auto& entry = m_all_attachments[idx];
        if (!matchesFilters(entry)) {
            continue;
        }

        int row = m_table->rowCount();
        m_table->insertRow(row);

        auto* name_item = new QTableWidgetItem(entry.filename);
        name_item->setData(Qt::UserRole, idx);
        m_table->setItem(row, ColFilename, name_item);

        auto* size_item = new QTableWidgetItem(formatBytes(entry.size_bytes));
        size_item->setData(Qt::UserRole + 1, entry.size_bytes);
        m_table->setItem(row, ColSize, size_item);

        m_table->setItem(row,
                         ColType,
                         new QTableWidgetItem(typeCategory(entry.mime_type, entry.filename)));

        m_table->setItem(row, ColSourceSubject, new QTableWidgetItem(entry.source_subject));
        m_table->setItem(row, ColSourceSender, new QTableWidgetItem(entry.source_sender));

        QString date_str;
        if (entry.source_date.isValid()) {
            date_str = entry.source_date.toString(QStringLiteral("yyyy-MM-dd hh:mm"));
        }
        m_table->setItem(row, ColSourceDate, new QTableWidgetItem(date_str));
    }

    m_table->setSortingEnabled(true);
    m_save_all_button->setEnabled(m_table->rowCount() > 0);
}

bool EmailAttachmentsBrowserDialog::matchesFilters(const AttachmentEntry& entry) const {
    // Text search
    if (!m_search_text.isEmpty()) {
        bool found = entry.filename.contains(m_search_text, Qt::CaseInsensitive) ||
                     entry.source_subject.contains(m_search_text, Qt::CaseInsensitive) ||
                     entry.source_sender.contains(m_search_text, Qt::CaseInsensitive);
        if (!found) {
            return false;
        }
    }

    // Type filter
    QString selected = m_type_filter->currentText();
    if (selected == kFilterAll) {
        return true;
    }
    QString category = typeCategory(entry.mime_type, entry.filename);
    return category == selected;
}

void EmailAttachmentsBrowserDialog::updateStatusLabel() {
    int visible = m_table->rowCount();
    int total = m_all_attachments.size();
    if (m_scan_complete) {
        if (visible == total) {
            m_status_label->setText(tr("%1 attachments").arg(total));
        } else {
            m_status_label->setText(tr("%1 of %2 attachments (filtered)").arg(visible).arg(total));
        }
    } else {
        m_status_label->setText(tr("Scanning... %1 attachments found so far").arg(total));
    }
}

// ============================================================================
// Search / filter slots
// ============================================================================

void EmailAttachmentsBrowserDialog::onSearchTextChanged(const QString& text) {
    m_search_text = text;
    m_search_timer->start();
}

void EmailAttachmentsBrowserDialog::onSearchTimerFired() {
    rebuildTable();
    updateStatusLabel();
}

void EmailAttachmentsBrowserDialog::onTypeFilterChanged(int /*index*/) {
    rebuildTable();
    updateStatusLabel();
}

void EmailAttachmentsBrowserDialog::onSelectionChanged() {
    bool has_selection = !m_table->selectedItems().isEmpty();
    m_save_selected_button->setEnabled(has_selection);
}

void EmailAttachmentsBrowserDialog::onTableContextMenu(const QPoint& pos) {
    auto* item = m_table->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    int row = item->row();
    auto* name_item = m_table->item(row, ColFilename);
    if (name_item == nullptr) {
        return;
    }
    int att_idx = name_item->data(Qt::UserRole).toInt();
    if (att_idx < 0 || att_idx >= m_all_attachments.size()) {
        return;
    }
    const auto& entry = m_all_attachments[att_idx];

    QMenu menu(this);
    menu.addAction(tr("Save Attachment..."), this, [this, entry] { saveOneAttachment(entry); });
    menu.addSeparator();
    menu.addAction(tr("View Containing Email"), this, [this, entry] {
        m_navigate_folder_id = entry.source_folder_id;
        m_navigate_message_id = entry.message_node_id;
        accept();
    });
    menu.addSeparator();
    menu.addAction(tr("Copy Filename"), this, [entry] {
        QApplication::clipboard()->setText(entry.filename);
    });
    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

// ============================================================================
// Save actions
// ============================================================================

void EmailAttachmentsBrowserDialog::saveOneAttachment(const AttachmentEntry& entry) {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Save Attachment"));
    if (dir.isEmpty()) {
        return;
    }
    m_batch_save.begin(dir, 1);
    m_status_label->setText(tr("Saving attachment..."));
    m_controller->loadAttachmentContent(entry.message_node_id, entry.attachment_index);
}

void EmailAttachmentsBrowserDialog::onSaveSelectedClicked() {
    auto selected_rows = m_table->selectionModel()->selectedRows();
    if (selected_rows.isEmpty()) {
        return;
    }

    QString dir = QFileDialog::getExistingDirectory(this, tr("Save Selected Attachments"));
    if (dir.isEmpty()) {
        return;
    }

    int count = 0;
    QVector<std::pair<uint64_t, int>> to_save;
    for (const auto& index : selected_rows) {
        auto* item = m_table->item(index.row(), ColFilename);
        if (item == nullptr) {
            continue;
        }
        int att_idx = item->data(Qt::UserRole).toInt();
        if (att_idx < 0 || att_idx >= m_all_attachments.size()) {
            continue;
        }
        const auto& entry = m_all_attachments[att_idx];
        to_save.append({entry.message_node_id, entry.attachment_index});
        ++count;
    }
    if (count == 0) {
        return;
    }
    m_batch_save.begin(dir, count);
    m_status_label->setText(tr("Saving %1 attachments...").arg(count));
    for (const auto& [node_id, att_index] : to_save) {
        m_controller->loadAttachmentContent(node_id, att_index);
    }
}

void EmailAttachmentsBrowserDialog::onSaveAllVisibleClicked() {
    if (m_table->rowCount() == 0) {
        return;
    }

    QString dir = QFileDialog::getExistingDirectory(this, tr("Save All Visible Attachments"));
    if (dir.isEmpty()) {
        return;
    }

    int count = 0;
    QVector<std::pair<uint64_t, int>> to_save;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, ColFilename);
        if (item == nullptr) {
            continue;
        }
        int att_idx = item->data(Qt::UserRole).toInt();
        if (att_idx < 0 || att_idx >= m_all_attachments.size()) {
            continue;
        }
        const auto& entry = m_all_attachments[att_idx];
        to_save.append({entry.message_node_id, entry.attachment_index});
        ++count;
    }
    if (count == 0) {
        return;
    }
    m_batch_save.begin(dir, count);
    m_status_label->setText(tr("Saving %1 attachments...").arg(count));
    for (const auto& [node_id, att_index] : to_save) {
        m_controller->loadAttachmentContent(node_id, att_index);
    }
}

void EmailAttachmentsBrowserDialog::onAttachmentContentReady(uint64_t /*message_id*/,
                                                             int /*index*/,
                                                             QByteArray content,
                                                             QString filename) {
    if (!m_batch_save.isActive()) {
        return;
    }

    m_batch_save.recordOne(filename, content);
    if (m_batch_save.isComplete()) {
        m_status_label->setText(m_batch_save.summaryText());
        m_batch_save.reset();
    }
}

void EmailAttachmentsBrowserDialog::onErrorOccurred(const QString& message) {
    // During detail loading — skip this item and continue
    if (!m_scan_complete && m_details_total > 0) {
        ++m_details_loaded;
        m_progress_bar->setValue(m_details_loaded);
        QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::requestNextDetail);
        return;
    }
    // During folder scanning — skip this folder and continue
    if (m_folders_scanned < m_all_folder_ids.size()) {
        ++m_folders_scanned;
        m_current_folder_offset = 0;
        m_progress_bar->setValue(m_folders_scanned);
        QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::scanNextFolder);
        return;
    }
    // During save — count as failure and update status
    if (m_batch_save.isActive()) {
        m_batch_save.recordError();
        sak::logWarning("Attachment save error: {}", message.toStdString());
        if (m_batch_save.isComplete()) {
            m_status_label->setText(m_batch_save.summaryText());
            m_batch_save.reset();
        }
    }
}

// ============================================================================
// Utility functions
// ============================================================================

QString EmailAttachmentsBrowserDialog::formatBytes(qint64 bytes) {
    constexpr qint64 kKilo = 1024;
    constexpr qint64 kMega = kKilo * 1024;
    constexpr qint64 kGiga = kMega * 1024;

    if (bytes >= kGiga) {
        return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / kGiga, 0, 'f', 1);
    }
    if (bytes >= kMega) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kMega, 0, 'f', 1);
    }
    if (bytes >= kKilo) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / kKilo, 0, 'f', 1);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

// Data-driven extension → category mapping
static const QHash<QString, QString>& extensionCategories() {
    static const QHash<QString, QString> map = {
        // Images
        {".jpg", kFilterImages},
        {".jpeg", kFilterImages},
        {".png", kFilterImages},
        {".gif", kFilterImages},
        {".bmp", kFilterImages},
        {".svg", kFilterImages},
        // Audio/Video
        {".mp3", kFilterAudio},
        {".mp4", kFilterAudio},
        {".avi", kFilterAudio},
        {".wav", kFilterAudio},
        // Archives
        {".zip", kFilterArchives},
        {".7z", kFilterArchives},
        {".rar", kFilterArchives},
        {".tar", kFilterArchives},
        {".gz", kFilterArchives},
        // Documents
        {".pdf", kFilterDocuments},
        {".doc", kFilterDocuments},
        {".docx", kFilterDocuments},
        {".xls", kFilterDocuments},
        {".xlsx", kFilterDocuments},
        {".ppt", kFilterDocuments},
        {".pptx", kFilterDocuments},
        {".txt", kFilterDocuments},
        {".csv", kFilterDocuments},
        {".rtf", kFilterDocuments},
    };
    return map;
}

struct MimePrefix {
    const char* prefix;
    const QString* category;
};

static QString classifyByMime(const QString& lower_mime) {
    static const MimePrefix prefixes[] = {
        {"image/", &kFilterImages},
        {"audio/", &kFilterAudio},
        {"video/", &kFilterAudio},
    };
    for (const auto& [prefix, category] : prefixes) {
        if (lower_mime.startsWith(QLatin1String(prefix))) {
            return *category;
        }
    }

    static const QStringList archive_tokens = {QStringLiteral("zip"),
                                               QStringLiteral("compressed"),
                                               QStringLiteral("archive")};
    for (const auto& token : archive_tokens) {
        if (lower_mime.contains(token)) {
            return kFilterArchives;
        }
    }

    static const QStringList doc_tokens = {QStringLiteral("pdf"),
                                           QStringLiteral("document"),
                                           QStringLiteral("spreadsheet"),
                                           QStringLiteral("presentation"),
                                           QStringLiteral("msword"),
                                           QStringLiteral("ms-excel"),
                                           QStringLiteral("ms-powerpoint")};
    for (const auto& token : doc_tokens) {
        if (lower_mime.contains(token)) {
            return kFilterDocuments;
        }
    }

    return {};
}

QString EmailAttachmentsBrowserDialog::typeCategory(const QString& mime_type,
                                                    const QString& filename) {
    QString lower_name = filename.toLower();
    int dot = lower_name.lastIndexOf(QLatin1Char('.'));
    if (dot >= 0) {
        QString ext = lower_name.mid(dot);
        auto iter = extensionCategories().constFind(ext);
        if (iter != extensionCategories().constEnd()) {
            return iter.value();
        }
    }

    QString result = classifyByMime(mime_type.toLower());
    return result.isEmpty() ? kFilterOther : result;
}

}  // namespace sak
