// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachments_browser_dialog.cpp
/// @brief Modal dialog for browsing all attachments in a mailbox

#include "sak/email_attachments_browser_dialog.h"

#include "sak/email_attachment_saver.h"
#include "sak/email_constants.h"
#include "sak/email_inspector_controller.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/view_empty_state.h"
#include "sak/widget_helpers.h"

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

#include <array>

namespace sak {

namespace {
constexpr int kAttachMethodEmbeddedMessage = 5;
constexpr int kAttachMethodOleObject = 6;

// Size column: a plain QTableWidgetItem sorts on its DISPLAY string, so "9.0 KB" would sort
// above "10.0 MB" and the column would lie about which attachment is biggest. Order on the
// exact byte count carried alongside the text instead.
class AttachmentSizeItem : public QTableWidgetItem {
public:
    AttachmentSizeItem(const QString& text, qint64 size_bytes) : QTableWidgetItem(text) {
        setData(Qt::UserRole + 1, size_bytes);
    }

    bool operator<(const QTableWidgetItem& other) const override {
        return data(Qt::UserRole + 1).toLongLong() < other.data(Qt::UserRole + 1).toLongLong();
    }
};
}  // namespace

// ============================================================================
// Table column indices
// ============================================================================

enum AttachmentColumn {
    ColSelect = 0,
    ColFilename,
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

static QString attachmentKey(const AttachmentEntry& entry) {
    return QStringLiteral("%1:%2").arg(entry.message_node_id).arg(entry.attachment_index);
}

static QTableWidgetItem* makeAttachmentSelectItem(const AttachmentEntry& entry, bool checked) {
    auto* item = new QTableWidgetItem();
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled) &
                   ~Qt::ItemIsEditable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setTextAlignment(Qt::AlignCenter);
    item->setToolTip(QObject::tr("Select attachment for saving"));
    item->setData(Qt::UserRole, attachmentKey(entry));
    return item;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

EmailAttachmentsBrowserDialog::EmailAttachmentsBrowserDialog(::EmailInspectorController* controller,
                                                             const PstFolderTree& folder_tree,
                                                             QWidget* parent)
    : QDialog(parent), m_controller(controller) {
    setWindowTitle(tr("Attachments Browser"));
    setModal(true);
    resize(kWizardLargeWidth + email::kAttachmentBrowserExtraWidth, kWizardLargeHeight);
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
    m_type_filter->setMinimumWidth(email::kAttachmentTypeFilterMinWidth);
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
    m_table->setHorizontalHeaderLabels({tr("Select"),
                                        tr("Filename"),
                                        tr("Size"),
                                        tr("Type"),
                                        tr("Source Email"),
                                        tr("Sender"),
                                        tr("Date")});
    configureStandardTable(m_table, QAbstractItemView::ExtendedSelection);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setStretchLastSection(true);

    m_table->horizontalHeader()->setSectionResizeMode(ColSelect, QHeaderView::Fixed);
    m_table->setColumnWidth(ColSelect, email::kAttachmentIndicatorColumnWidth);
    m_table->setColumnWidth(ColFilename, email::kAttachmentFilenameColumnWidth);
    m_table->setColumnWidth(ColSize, email::kSizeColumnWidth);
    m_table->setColumnWidth(ColType, email::kTypeColumnWidth);
    m_table->setColumnWidth(ColSourceSubject, email::kAttachmentSourceSubjectColumnWidth);
    m_table->setColumnWidth(ColSourceSender, email::kAttachmentSourceSenderColumnWidth);

    connect(m_table,
            &QTableWidget::itemSelectionChanged,
            this,
            &EmailAttachmentsBrowserDialog::onSelectionChanged);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item == nullptr || item->column() != ColSelect) {
            return;
        }
        const QString key = item->data(Qt::UserRole).toString();
        if (key.isEmpty()) {
            return;
        }
        if (item->checkState() == Qt::Checked) {
            m_checked_attachment_keys.insert(key);
        } else {
            m_checked_attachment_keys.remove(key);
        }
        onSelectionChanged();
    });

    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table,
            &QTableWidget::customContextMenuRequested,
            this,
            &EmailAttachmentsBrowserDialog::onTableContextMenu);

    // Built after the widget so the overlay binds to the live model. A loading message
    // is driven over the table for the duration of the mailbox attachment scan.
    m_table_empty_state = new ui::ViewEmptyState(m_table,
                                                 tr("No attachments match the current filter"));

    return m_table;
}

QHBoxLayout* EmailAttachmentsBrowserDialog::createButtonRow() {
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(ui::kSpacingMedium);

    m_status_label = new QLabel(this);
    m_status_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextSecondary));
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
// Folder collection -- recursively gather all folder IDs
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
// Scan -- iterate all folders, collect items with attachments
// ============================================================================

void EmailAttachmentsBrowserDialog::startScan() {
    if (m_controller == nullptr) {
        // Fail closed: every request below would dereference it. Say so instead of crashing.
        m_progress_bar->hide();
        m_status_label->setText(tr("No mailbox reader is available. Nothing was scanned."));
        return;
    }
    m_table_empty_state->setLoading(tr("Scanning mailbox for attachments..."));
    m_folders_scanned = 0;
    m_progress_bar->setRange(0, static_cast<int>(m_all_folder_ids.size()));
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
            &::EmailInspectorController::attachmentContentFailed,
            this,
            &EmailAttachmentsBrowserDialog::onAttachmentContentFailed);
    connect(m_controller,
            &::EmailInspectorController::errorOccurred,
            this,
            &EmailAttachmentsBrowserDialog::onErrorOccurred);

    scanNextFolder();
}

void EmailAttachmentsBrowserDialog::scanNextFolder() {
    if (m_folders_scanned >= m_all_folder_ids.size()) {
        // All folders scanned -- now load details for items with attachments
        m_details_total = static_cast<int>(m_pending_detail_ids.size());
        m_details_loaded = 0;
        if (m_details_total == 0) {
            m_scan_complete = true;
            m_progress_bar->hide();
            m_table_empty_state->clearLoading();
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
    const uint64_t folder_id = m_all_folder_ids[m_folders_scanned];
    m_controller->loadFolderItems(folder_id, 0, email::kMaxItemsPerLoad);
}

void EmailAttachmentsBrowserDialog::requestNextDetail() {
    if (m_details_loaded >= m_pending_detail_ids.size()) {
        m_scan_complete = true;
        m_progress_bar->hide();
        m_table_empty_state->clearLoading();
        rebuildTable();
        updateStatusLabel();
        updateSaveControls();
        return;
    }
    m_controller->loadItemDetail(m_pending_detail_ids[m_details_loaded]);
}

// ============================================================================
// Data loading slots
// ============================================================================

void EmailAttachmentsBrowserDialog::onFolderItemsLoaded(uint64_t folder_id,
                                                        QVector<sak::PstItemSummary> items,
                                                        int total) {
    // Correlate the arrival with the folder this dialog actually asked for. The controller is
    // shared and onErrorOccurred can advance the cursor while a request is still in flight, so
    // an uncorrelated arrival would be counted against the wrong folder -- and once every
    // folder has been scanned there is no slot left to index at all, making
    // m_all_folder_ids[m_folders_scanned] an out-of-bounds read.
    if (m_folders_scanned >= m_all_folder_ids.size()) {
        return;
    }
    const uint64_t expected_folder_id = m_all_folder_ids[m_folders_scanned];
    if (folder_id != expected_folder_id) {
        return;
    }

    for (const auto& item : items) {
        if (item.has_attachments) {
            m_pending_detail_ids.append(item.node_id);
            m_message_folder_map.insert(item.node_id, expected_folder_id);
        }
    }

    m_current_folder_offset += static_cast<int>(items.size());

    // If this folder has more items, load the next page
    if (m_current_folder_offset < total && !items.isEmpty()) {
        QTimer::singleShot(0, this, [this]() {
            if (m_folders_scanned < m_all_folder_ids.size()) {
                const uint64_t fid = m_all_folder_ids[m_folders_scanned];
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
        if (att.attach_method == kAttachMethodEmbeddedMessage ||
            att.attach_method == kAttachMethodOleObject) {
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
    // Batch-filter matching rows first so we can size the table once and
    // skip the O(N) cost of per-row `insertRow`/re-sort/repaint cycles.
    QVector<int> matching;
    matching.reserve(m_all_attachments.size());
    for (int idx = 0; idx < m_all_attachments.size(); ++idx) {
        if (matchesFilters(m_all_attachments[idx])) {
            matching.append(idx);
        }
    }

    m_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_table);
    m_table->setSortingEnabled(false);
    m_table->setRowCount(static_cast<int>(matching.size()));

    for (int row = 0; row < matching.size(); ++row) {
        const int idx = matching.at(row);
        const auto& entry = m_all_attachments[idx];

        const bool checked = m_checked_attachment_keys.contains(attachmentKey(entry));
        m_table->setItem(row, ColSelect, makeAttachmentSelectItem(entry, checked));

        auto* name_item = new QTableWidgetItem(entry.filename);
        name_item->setData(Qt::UserRole, idx);
        m_table->setItem(row, ColFilename, name_item);

        m_table->setItem(row,
                         ColSize,
                         new AttachmentSizeItem(formatBytes(entry.size_bytes), entry.size_bytes));

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
    m_table->setUpdatesEnabled(true);
    updateSaveControls();
}

bool EmailAttachmentsBrowserDialog::matchesFilters(const AttachmentEntry& entry) const {
    // Text search
    if (!m_search_text.isEmpty()) {
        const bool found = entry.filename.contains(m_search_text, Qt::CaseInsensitive) ||
                           entry.source_subject.contains(m_search_text, Qt::CaseInsensitive) ||
                           entry.source_sender.contains(m_search_text, Qt::CaseInsensitive);
        if (!found) {
            return false;
        }
    }

    // Type filter
    const QString selected = m_type_filter->currentText();
    if (selected == kFilterAll) {
        return true;
    }
    const QString category = typeCategory(entry.mime_type, entry.filename);
    return category == selected;
}

void EmailAttachmentsBrowserDialog::updateStatusLabel() {
    const int visible = m_table->rowCount();
    const int total = static_cast<int>(m_all_attachments.size());
    const int checked = static_cast<int>(m_checked_attachment_keys.size());
    if (m_scan_complete) {
        if (visible == total) {
            m_status_label->setText(tr("%1 attachments, %2 selected").arg(total).arg(checked));
        } else {
            m_status_label->setText(tr("%1 of %2 attachments (filtered), %3 selected")
                                        .arg(visible)
                                        .arg(total)
                                        .arg(checked));
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
    updateSaveControls();
    if (m_scan_complete) {
        updateStatusLabel();
    }
}

void EmailAttachmentsBrowserDialog::onTableContextMenu(const QPoint& pos) {
    auto* item = m_table->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    const int row = item->row();
    auto* name_item = m_table->item(row, ColFilename);
    if (name_item == nullptr) {
        return;
    }
    // A missing or wrong-typed row index coerces to 0 through a bare toInt(), which would
    // silently offer the FIRST attachment for a row that no longer resolves. Fail closed.
    bool index_ok = false;
    const int att_idx = name_item->data(Qt::UserRole).toInt(&index_ok);
    if (!index_ok || att_idx < 0 || att_idx >= m_all_attachments.size()) {
        m_status_label->setText(tr("That row no longer resolves to an attachment."));
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

void EmailAttachmentsBrowserDialog::startBatch(const QString& dir,
                                               const QVector<AttachmentRef>& refs) {
    if (!m_batch_save.begin(dir, refs)) {
        m_status_label->setText(tr("Could not start the save. Nothing was written."));
        return;
    }
    // Lock the save controls for the duration: arrivals from a second batch begun
    // now would interleave with this one's and land in each other's slots.
    updateSaveControls();
    m_status_label->setText(tr("Saving %1 attachment(s)...").arg(refs.size()));
    for (const auto& ref : refs) {
        m_controller->loadAttachmentContent(ref.message_id, ref.index);
    }
}

void EmailAttachmentsBrowserDialog::updateSaveControls() {
    const bool idle = !m_batch_save.isActive();
    m_save_selected_button->setEnabled(idle && !m_checked_attachment_keys.isEmpty());
    m_save_all_button->setEnabled(idle && m_table->rowCount() > 0);
}

void EmailAttachmentsBrowserDialog::saveOneAttachment(const AttachmentEntry& entry) {
    if (m_batch_save.isActive()) {
        m_status_label->setText(tr("A save is already running. Wait for it to finish."));
        return;
    }
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save Attachment"));
    if (dir.isEmpty()) {
        return;
    }
    const QVector<AttachmentRef> refs{
        AttachmentRef{.message_id = entry.message_node_id, .index = entry.attachment_index}};
    startBatch(dir, refs);
}

void EmailAttachmentsBrowserDialog::onSaveSelectedClicked() {
    if (m_batch_save.isActive() || m_checked_attachment_keys.isEmpty()) {
        return;
    }

    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save Selected Attachments"));
    if (dir.isEmpty()) {
        return;
    }

    QVector<AttachmentRef> refs;
    refs.reserve(m_all_attachments.size());
    for (const auto& entry : m_all_attachments) {
        if (!m_checked_attachment_keys.contains(attachmentKey(entry))) {
            continue;
        }
        refs.append(
            AttachmentRef{.message_id = entry.message_node_id, .index = entry.attachment_index});
    }
    if (refs.isEmpty()) {
        return;
    }
    startBatch(dir, refs);
}

void EmailAttachmentsBrowserDialog::onSaveAllVisibleClicked() {
    if (m_batch_save.isActive() || m_table->rowCount() == 0) {
        return;
    }

    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save All Visible Attachments"));
    if (dir.isEmpty()) {
        return;
    }

    QVector<AttachmentRef> refs;
    if (!collectVisibleAttachmentRefs(&refs)) {
        m_status_label->setText(
            tr("The table changed while the save was starting. Nothing was written."));
        return;
    }
    if (refs.isEmpty()) {
        return;
    }
    startBatch(dir, refs);
}

// False when any visible row fails to resolve to a real attachment. A bare toInt() would turn
// a missing or wrong-typed row index into 0 -- saving the FIRST attachment in place of the row
// the user is looking at -- and skipping such a row would make "Save All Visible" write a
// silent subset. Both are fail-closed here: one bad row abandons the whole batch.
bool EmailAttachmentsBrowserDialog::collectVisibleAttachmentRefs(QVector<AttachmentRef>* refs) {
    refs->reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        auto* item = m_table->item(row, ColFilename);
        if (item == nullptr) {
            return false;
        }
        bool index_ok = false;
        const int att_idx = item->data(Qt::UserRole).toInt(&index_ok);
        if (!index_ok || att_idx < 0 || att_idx >= m_all_attachments.size()) {
            return false;
        }
        const auto& entry = m_all_attachments[att_idx];
        refs->append(
            AttachmentRef{.message_id = entry.message_node_id, .index = entry.attachment_index});
    }
    return true;
}

// Attachment payloads arrive here from the parser thread. Both are taken by const reference:
// on a QUEUED connection Qt copies each argument into the receiving thread's event queue and binds
// the parameter to THAT copy, which outlives the slot body -- so a const reference is safe and
// avoids a second copy of the whole attachment. The body only reads them.
void EmailAttachmentsBrowserDialog::onAttachmentContentReady(uint64_t message_id,
                                                             int index,
                                                             const QByteArray& content,
                                                             const QString& filename) {
    // Record only what this batch asked for. A stray arrival -- a leftover from a
    // previous batch, or one requested by another view -- would otherwise be
    // written into one of this batch's slots and counted as that slot's save.
    const AttachmentRef ref{.message_id = message_id, .index = index};
    if (!m_batch_save.expects(ref)) {
        return;
    }

    const auto result = m_batch_save.recordOne(ref, filename, content);
    if (!result.success) {
        sak::logWarning("Attachment save failed: {}", result.error_message.toStdString());
    }
    if (m_batch_save.isComplete()) {
        m_status_label->setText(m_batch_save.summaryText());
        m_batch_save.reset();
        updateSaveControls();
    }
}

void EmailAttachmentsBrowserDialog::onAttachmentContentFailed(uint64_t message_id,
                                                              int index,
                                                              const QString& error) {
    // Symmetric to onAttachmentContentReady: count the failure only against the exact
    // attachment this batch requested. recordError refuses any ref this batch is not
    // waiting on, so an unrelated controller failure -- one for another view, or a
    // request refused for a different operation -- can neither inflate the failed count
    // nor complete the batch early. Every loadAttachmentContent request resolves to
    // exactly one ready/failed, so the batch still always reaches its expected total.
    const AttachmentRef ref{.message_id = message_id, .index = index};
    if (!m_batch_save.recordError(ref)) {
        return;
    }
    sak::logWarning(
        "Attachment save error (message {} index {}): {}", message_id, index, error.toStdString());
    if (m_batch_save.isComplete()) {
        m_status_label->setText(m_batch_save.summaryText());
        m_batch_save.reset();
        updateSaveControls();
    }
}

void EmailAttachmentsBrowserDialog::onErrorOccurred(const QString& message) {
    // During detail loading -- skip this item and continue
    if (!m_scan_complete && m_details_total > 0) {
        ++m_details_loaded;
        m_progress_bar->setValue(m_details_loaded);
        QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::requestNextDetail);
        return;
    }
    // During folder scanning -- skip this folder and continue
    if (m_folders_scanned < m_all_folder_ids.size()) {
        ++m_folders_scanned;
        m_current_folder_offset = 0;
        m_progress_bar->setValue(m_folders_scanned);
        QTimer::singleShot(0, this, &EmailAttachmentsBrowserDialog::scanNextFolder);
        return;
    }
    // A save batch's failures arrive on the identity-carrying attachmentContentFailed
    // signal (onAttachmentContentFailed), not here. This generic error names no
    // attachment, so charging it to the batch is exactly the miscount that would count
    // an unrelated failure as a save failure. Nothing to do for a save-phase error.
    Q_UNUSED(message);
}

// ============================================================================
// Utility functions
// ============================================================================

QString EmailAttachmentsBrowserDialog::formatBytes(qint64 bytes) {
    // GB now renders to two decimals rather than one, matching every other compact caller. That
    // is the only visible change from sharing the formatter, and it is the point of sharing it.
    return sak::formatBytesCompact(bytes);
}

// Data-driven extension -> category mapping
static const QHash<QString, QString>& extensionCategories() {
    static const QHash<QString, QString> kMap = {
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
    return kMap;
}

struct MimePrefix {
    const char* prefix;
    const QString* category;
};

static QString classifyByMime(const QString& lower_mime) {
    static const std::array<MimePrefix, 3> kPrefixes = {{
        {.prefix = "image/", .category = &kFilterImages},
        {.prefix = "audio/", .category = &kFilterAudio},
        {.prefix = "video/", .category = &kFilterAudio},
    }};
    for (const auto& [prefix, category] : kPrefixes) {
        if (lower_mime.startsWith(QLatin1String(prefix))) {
            return *category;
        }
    }

    static const QStringList kArchiveTokens = {QStringLiteral("zip"),
                                               QStringLiteral("compressed"),
                                               QStringLiteral("archive")};
    for (const auto& token : kArchiveTokens) {
        if (lower_mime.contains(token)) {
            return kFilterArchives;
        }
    }

    static const QStringList kDocTokens = {QStringLiteral("pdf"),
                                           QStringLiteral("document"),
                                           QStringLiteral("spreadsheet"),
                                           QStringLiteral("presentation"),
                                           QStringLiteral("msword"),
                                           QStringLiteral("ms-excel"),
                                           QStringLiteral("ms-powerpoint")};
    for (const auto& token : kDocTokens) {
        if (lower_mime.contains(token)) {
            return kFilterDocuments;
        }
    }

    return {};
}

QString EmailAttachmentsBrowserDialog::typeCategory(const QString& mime_type,
                                                    const QString& filename) {
    const QString lower_name = filename.toLower();
    const int dot = static_cast<int>(lower_name.lastIndexOf(QLatin1Char('.')));
    if (dot >= 0) {
        const QString ext = lower_name.mid(dot);
        auto iter = extensionCategories().constFind(ext);
        if (iter != extensionCategories().constEnd()) {
            return iter.value();
        }
    }

    const QString result = classifyByMime(mime_type.toLower());
    return result.isEmpty() ? kFilterOther : result;
}

}  // namespace sak
