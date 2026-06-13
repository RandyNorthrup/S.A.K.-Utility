// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_explorer_panel.h
/// @brief File Management explorer tab with mounted and raw/image targets.

#pragma once

#include "sak/file_explorer_command_registry.h"
#include "sak/file_explorer_command_bar.h"
#include "sak/file_explorer_details_pane.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_omnibar.h"
#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_sidebar.h"
#include "sak/file_management_file_system.h"

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSplitter>
#include <QStringList>
#include <QTabWidget>
#include <QAbstractItemView>
#include <QWidget>

class QMenu;
class QPoint;
class QAction;
class QToolButton;

namespace sak {

class FileManagementExplorerPanel : public QWidget {
    Q_OBJECT

public:
    explicit FileManagementExplorerPanel(QWidget* parent = nullptr);

protected:
    void resizeEvent(QResizeEvent* event) override;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onRefreshMountedTargets();
    void onScanDiskTargets();
    void onAddManualTarget();
    void onTargetChanged(int index);
    void onPathReturnPressed();
    void onBackClicked();
    void onForwardClicked();
    void onUpClicked();
    void onOpenSelected();
    void onCopyPathClicked();
    void onNewFolderClicked();
    void onWriteFileClicked();
    void onRenameClicked();
    void onDeleteClicked();
    void onItemDoubleClicked(const QModelIndex& index);
    void onTableContextMenuRequested(const QPoint& position);
    void onTargetContextMenuRequested(const QPoint& position);

private:
    void setupUi();
    void installCommandShortcuts();
    void setTargets(QVector<FileManagementTarget> targets);
    void appendTarget(const FileManagementTarget& target);
    void rebuildTargetList(const QString& preferred_target_id = {});
    void appendSidebarHeader(const QString& text);
    void appendSidebarTarget(const FileManagementTarget& target, int target_index);
    void selectTargetById(const QString& target_id);
    void rememberRecentTarget(const QString& target_id);
    void loadSidebarState();
    void saveSidebarState() const;
    void applyViewSettings();
    void loadViewSettingsForCurrentLocation();
    void saveViewSettings() const;
    void setExplorerViewMode(FileExplorerViewMode mode);
    [[nodiscard]] QAbstractItemView* currentItemView() const;
    [[nodiscard]] FileManagementTarget currentTarget() const;
    [[nodiscard]] int targetIndexForId(const QString& target_id) const;
    [[nodiscard]] QString selectedPath() const;
    [[nodiscard]] bool selectedIsDirectory() const;
    [[nodiscard]] QString targetPathForName(const QString& name) const;
    [[nodiscard]] bool validateCurrentTargetIdentity(QString* blocker) const;
    void loadDirectory(const QString& path, bool add_history = true);
    void loadColumnsPreview(const QString& path);
    void populateTable(const FileManagementListResult& result);
    void previewSelectedFile();
    void showMutationResult(const QString& title, const FileManagementMutationResult& result);
    [[nodiscard]] FileExplorerSelection currentSelection() const;
    [[nodiscard]] FileExplorerCommandContext commandContext() const;
    void applyCommandState(QPushButton* button,
                           FileExplorerCommandId command,
                           const FileExplorerCommandContext& context);
    QAction* addCommandMenuAction(QMenu* menu,
                                  FileExplorerCommandId command,
                                  const FileExplorerCommandContext& context);
    void rebuildViewMenu(const FileExplorerCommandContext& context);
    void executeCommand(FileExplorerCommandId command);
    void promptCurrentFolderFilter();
    void showCommandPalette();
    void updateDetailsPane();
    void updateActionButtons();
    void logMessage(const QString& message);

    FileExplorerSidebar* m_sidebar{nullptr};
    FileExplorerCommandBar* m_command_bar{nullptr};
    FileExplorerOmnibar* m_omnibar{nullptr};
    FileExplorerPane* m_pane{nullptr};
    FileExplorerDetailsPane* m_details_pane{nullptr};
    QListWidget* m_target_list{nullptr};
    QSplitter* m_shell_splitter{nullptr};
    QPushButton* m_sidebar_toggle_button{nullptr};
    QPushButton* m_details_toggle_button{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QPushButton* m_scan_disks_button{nullptr};
    QPushButton* m_add_manual_button{nullptr};
    QLineEdit* m_path_edit{nullptr};
    QPushButton* m_back_button{nullptr};
    QPushButton* m_forward_button{nullptr};
    QPushButton* m_up_button{nullptr};
    QPushButton* m_search_button{nullptr};
    QPushButton* m_command_button{nullptr};
    QPushButton* m_open_button{nullptr};
    QPushButton* m_copy_path_button{nullptr};
    QPushButton* m_new_folder_button{nullptr};
    QPushButton* m_write_file_button{nullptr};
    QPushButton* m_rename_button{nullptr};
    QPushButton* m_delete_button{nullptr};
    QToolButton* m_view_button{nullptr};
    QLabel* m_summary_label{nullptr};
    QLabel* m_status_label{nullptr};
    QPlainTextEdit* m_preview_text{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
    QTabWidget* m_details_tabs{nullptr};
    FileExplorerItemModel* m_item_model{nullptr};
    QVector<FileManagementTarget> m_targets;
    QString m_current_path{QStringLiteral("/")};
    FileExplorerPaneState m_pane_state;
    QStringList m_favorite_target_ids;
    QStringList m_recent_target_ids;
    QString m_last_target_id;
    quint64 m_listing_revision{0};
    quint64 m_columns_preview_revision{0};
    int m_current_target_index{-1};
};

}  // namespace sak
