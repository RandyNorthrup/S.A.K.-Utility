// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_panel.h
/// @brief UI panel for email & PST/OST/MBOX inspection

#pragma once

#include "sak/email_inspector_controller.h"
#include "sak/email_types.h"
#include "sak/widget_helpers.h"

#include <QIcon>
#include <QWidget>

#include <memory>
#include <type_traits>

class QCheckBox;
class QComboBox;
class QGroupBox;
class QHBoxLayout;
class QHeaderView;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTextBrowser;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;

namespace sak {

class LogToggleSwitch;

/// @brief Email Tool panel providing offline email forensics
///        and data extraction.
///
/// Modern email client layout with a ribbon toolbar at the top,
/// folder tree (left), item list (top-right), and a tabbed detail
/// panel (Content/Headers/Properties/Attachments) on bottom-right.
class EmailInspectorPanel : public QWidget {
    Q_OBJECT

public:
    explicit EmailInspectorPanel(QWidget* parent = nullptr);
    ~EmailInspectorPanel() override;

    // Non-copyable, non-movable
    EmailInspectorPanel(const EmailInspectorPanel&) = delete;
    EmailInspectorPanel& operator=(const EmailInspectorPanel&) = delete;
    EmailInspectorPanel(EmailInspectorPanel&&) = delete;
    EmailInspectorPanel& operator=(EmailInspectorPanel&&) = delete;

    /// Access the log toggle switch for MainWindow connection
    LogToggleSwitch* logToggle() const { return m_log_toggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // -- File Operations -------------------------------------------------
    void onOpenFileClicked();
    void onCloseFileClicked();

    // -- Navigation ------------------------------------------------------
    void onFolderTreeItemClicked(QTreeWidgetItem* item, int column);
    void onItemListCellClicked(int row, int column);
    void onItemListContextMenu(const QPoint& pos);
    void onFolderTreeContextMenu(const QPoint& pos);

    // -- Search ----------------------------------------------------------
    void onSearchClicked();
    void onSearchTextChanged();

    // -- Export ----------------------------------------------------------
    void onExportClicked();
    void onExportAttachmentsClicked();

    // -- Modals ----------------------------------------------------------
    void onScanForFilesClicked();
    void onContactsClicked();
    void onCalendarClicked();

    // -- Detail Tabs (Attachments) ---------------------------------------
    void onSaveAttachmentClicked();
    void onSaveAllAttachmentsClicked();

    // -- Controller Signals ----------------------------------------------
    void onStateChanged(EmailInspectorController::State state);
    void onFileOpened(sak::PstFileInfo info);
    void onFolderTreeLoaded(sak::PstFolderTree tree);
    void onFileClosed();
    void onFolderItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total);
    void onItemDetailLoaded(sak::PstItemDetail detail);
    void onItemPropertiesLoaded(uint64_t item_id, QVector<sak::MapiProperty> properties);
    void onAttachmentContentReady(uint64_t message_id,
                                  int index,
                                  QByteArray data,
                                  QString filename);
    void onSearchHit(sak::EmailSearchHit hit);
    void onSearchComplete(int total_hits);
    void onExportStarted(int total);
    void onExportProgress(int done, int total, qint64 bytes);
    void onExportComplete(sak::EmailExportResult result);
    void onErrorOccurred(QString message);

    // MBOX-specific
    void onMboxOpened(int message_count);
    void onMboxMessagesLoaded(QVector<sak::MboxMessage> messages, int total);
    void onMboxMessageDetailLoaded(sak::MboxMessageDetail detail);

private:
    // -- UI Setup --------------------------------------------------------
    void setupUi();
    void connectController();

    // -- Sub-Builders ----------------------------------------------------
    QWidget* createRibbon();
    QWidget* createFolderTreePanel();
    QWidget* createContentArea();
    QWidget* createItemListPanel();
    QWidget* createDetailPanel();
    QWidget* createContentTab();
    QWidget* createHeadersTab();
    QWidget* createPropertiesTab();
    QWidget* createAttachmentsTab();

    // -- Helpers ---------------------------------------------------------
    void setOperationRunning(bool running);
    void populateFolderTree(const sak::PstFolderTree& tree);
    void addFolderToTree(QTreeWidgetItem* parent, const sak::PstFolder& folder);
    [[nodiscard]] static const sak::PstFolder* findIpmSubtree(
        const QVector<sak::PstFolder>& folders);
    [[nodiscard]] static int folderSortOrder(const QString& name, const QString& container_class);
    [[nodiscard]] static QIcon folderIcon(const QString& name);
    [[nodiscard]] static bool isHiddenFolder(const QString& name, const QString& container_class);
    void collectSpecialFolderIds(const sak::PstFolder& folder);
    void populateItemList(const QVector<sak::PstItemSummary>& items);
    void displayItemDetail(const sak::PstItemDetail& detail);
    void displayTaskDetail(const sak::PstItemDetail& detail);
    void displayNoteDetail(const sak::PstItemDetail& detail);
    void displayProperties(const QVector<sak::MapiProperty>& props);
    void displayAttachments(const QVector<sak::PstAttachmentInfo>& attachments);
    void updateFileInfoBar(const sak::PstFileInfo& info);
    void updateStatusBar(const QString& message);
    void logMessage(const QString& message);
    [[nodiscard]] QString itemTypeIcon(sak::EmailItemType type) const;
    [[nodiscard]] static QString formatBytes(qint64 bytes);

    // -- Controller ------------------------------------------------------
    std::unique_ptr<EmailInspectorController> m_controller;

    // -- Layout ----------------------------------------------------------
    PanelHeaderWidgets m_header_widgets;

    // -- Ribbon Buttons (QToolButton with icon + text below) -------------
    QToolButton* m_scan_files_button{nullptr};
    QToolButton* m_open_button{nullptr};
    QToolButton* m_close_button{nullptr};
    QToolButton* m_export_emails_button{nullptr};
    QToolButton* m_contacts_button{nullptr};
    QToolButton* m_calendar_button{nullptr};
    QToolButton* m_attachments_button{nullptr};
    QLineEdit* m_search_edit{nullptr};
    QPushButton* m_search_button{nullptr};

    // Folder Tree
    QTreeWidget* m_folder_tree{nullptr};

    // Item List
    QTableWidget* m_item_list{nullptr};
    QLabel* m_item_count_label{nullptr};

    // Detail Panel
    QTabWidget* m_detail_tabs{nullptr};
    QTextBrowser* m_content_browser{nullptr};
    QTextBrowser* m_headers_browser{nullptr};
    QTableWidget* m_properties_table{nullptr};
    QTableWidget* m_attachments_table{nullptr};
    QPushButton* m_save_attachment_button{nullptr};
    QPushButton* m_save_all_attachments_button{nullptr};

    // Splitters
    QSplitter* m_main_splitter{nullptr};
    QSplitter* m_content_splitter{nullptr};

    // Status
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Search results
    QTableWidget* m_search_results_table{nullptr};

    // -- Log Toggle ------------------------------------------------------
    LogToggleSwitch* m_log_toggle{nullptr};

    // -- State -----------------------------------------------------------
    uint64_t m_current_folder_id{0};
    uint64_t m_current_item_id{0};
    sak::PstItemDetail m_current_detail;
    QVector<sak::PstItemSummary> m_current_items;

    // Cached folder IDs for modals
    QVector<uint64_t> m_contact_folder_ids;
    QVector<uint64_t> m_calendar_folder_ids;

    // -- Item List Columns -----------------------------------------------
    enum ItemColumn {
        ColAttachment = 0,
        ColSubject,
        ColFrom,
        ColDate,
        ColSize,
        ColType,
        ColCount
    };
};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

static_assert(std::is_base_of_v<QWidget, EmailInspectorPanel>,
              "EmailInspectorPanel must inherit QWidget.");
static_assert(!std::is_copy_constructible_v<EmailInspectorPanel>,
              "EmailInspectorPanel must not be copy-constructible.");

}  // namespace sak
