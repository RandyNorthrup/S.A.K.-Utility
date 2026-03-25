// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_attachments_browser_dialog.h
/// @brief Modal dialog for browsing all attachments in a mailbox file

#pragma once

#include "sak/email_inspector_controller.h"
#include "sak/email_types.h"

#include <QDialog>
#include <QHash>
#include <QVector>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTimer;
class QVBoxLayout;

namespace sak {

/// @brief Enriched attachment entry combining summary and detail data
struct AttachmentEntry {
    uint64_t message_node_id = 0;
    uint64_t source_folder_id = 0;
    int attachment_index = 0;
    QString filename;
    qint64 size_bytes = 0;
    QString mime_type;
    QString source_subject;
    QString source_sender;
    QDateTime source_date;
};

/// @brief Modal dialog that scans all folders for attachments and presents
///        them in a searchable, filterable table with export capability.
class EmailAttachmentsBrowserDialog : public QDialog {
    Q_OBJECT

public:
    explicit EmailAttachmentsBrowserDialog(::EmailInspectorController* controller,
                                           const PstFolderTree& folder_tree,
                                           QWidget* parent = nullptr);
    ~EmailAttachmentsBrowserDialog() override;

    /// After exec() returns Accepted, the caller reads these
    /// to navigate to the containing email in the main panel.
    [[nodiscard]] uint64_t navigateFolderId() const { return m_navigate_folder_id; }
    [[nodiscard]] uint64_t navigateMessageId() const { return m_navigate_message_id; }

private Q_SLOTS:
    void onSearchTextChanged(const QString& text);
    void onSearchTimerFired();
    void onTypeFilterChanged(int index);
    void onSaveSelectedClicked();
    void onSaveAllVisibleClicked();
    void onSelectionChanged();
    void onTableContextMenu(const QPoint& pos);
    void onFolderItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total);
    void onItemDetailLoaded(sak::PstItemDetail detail);
    void onAttachmentContentReady(uint64_t message_id,
                                  int index,
                                  QByteArray content,
                                  QString filename);

private:
    void setupUi();
    QHBoxLayout* createToolbarRow();
    QTableWidget* createAttachmentTable();
    QHBoxLayout* createButtonRow();
    void startScan();
    void scanNextFolder();
    void requestNextDetail();
    void rebuildTable();
    [[nodiscard]] bool matchesFilters(const AttachmentEntry& entry) const;
    void updateStatusLabel();
    void collectFolderIds(const PstFolderTree& tree);
    void collectFolderIdsRecursive(const PstFolder& folder);
    void saveOneAttachment(const AttachmentEntry& entry);
    [[nodiscard]] static QString formatBytes(qint64 bytes);
    [[nodiscard]] static QString typeCategory(const QString& mime_type, const QString& filename);

    ::EmailInspectorController* m_controller{nullptr};

    // Scan state
    QVector<uint64_t> m_all_folder_ids;
    int m_folders_scanned{0};
    QVector<uint64_t> m_pending_detail_ids;
    int m_details_loaded{0};
    int m_details_total{0};
    bool m_scan_complete{false};

    // Data
    QVector<AttachmentEntry> m_all_attachments;

    // UI widgets
    QLineEdit* m_search_edit{nullptr};
    QTimer* m_search_timer{nullptr};
    QComboBox* m_type_filter{nullptr};
    QTableWidget* m_table{nullptr};
    QPushButton* m_save_selected_button{nullptr};
    QPushButton* m_save_all_button{nullptr};
    QPushButton* m_close_button{nullptr};
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Search state
    QString m_search_text;

    // Save state — path for batch save-all
    QString m_save_dir;

    // Navigation state — set when user chooses "View Containing Email"
    uint64_t m_navigate_folder_id{0};
    uint64_t m_navigate_message_id{0};

    // Maps message node_id → folder_id for navigation
    QHash<uint64_t, uint64_t> m_message_folder_map;
};

}  // namespace sak
