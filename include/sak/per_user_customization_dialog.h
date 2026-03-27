// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>

class QDir;
class QFileInfo;
class QTreeWidgetItem;
class QVBoxLayout;

namespace sak {

/**
 * @brief Dialog for customizing which folders/files to backup for a specific user
 *
 * Allows granular control over:
 * - Standard folders (Documents, Desktop, Pictures, etc.)
 * - Application data (selective AppData items)
 * - Custom folder additions
 * - Include/exclude patterns per folder
 */
class PerUserCustomizationDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param profile User profile to customize
     * @param parent Parent widget
     */
    explicit PerUserCustomizationDialog(UserProfile& profile, QWidget* parent = nullptr);

    /**
     * @brief Get updated folder selections
     * @return Modified folder selections
     */
    QVector<FolderSelection> getFolderSelections() const;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onSelectRecommended();
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onAddCustomFolder();
    void onRemoveFolder();
    void onExpandAll();
    void onCollapseAll();
    void updateSummary();

private:
    void setupUi();
    void setupUi_foldersSection(QVBoxLayout* mainLayout);
    void setupUi_appDataSection(QVBoxLayout* mainLayout);
    void setupUi_dialogButtons(QVBoxLayout* mainLayout);
    void populateTree();
    void addFolderToTree(const FolderSelection& selection, QTreeWidgetItem* parent = nullptr);
    /// @brief Accumulated state for recursive directory tree traversal
    struct DirTraversalState {
        qint64& total_size;
        int& total_files;
        bool checked;
        int depth;
        int max_depth;
    };

    void addDirectoryContents(const QDir& dir, QTreeWidgetItem* parent, DirTraversalState state);
    void addDirectoryChildItem(const QFileInfo& entry,
                               QTreeWidgetItem* parent,
                               DirTraversalState& state);
    static QString formatFileSize(qint64 bytes);
    void calculateDirectorySize(
        const QDir& dir, qint64& totalSize, int& fileCount, int depth = 0, int maxDepth = 10);
    void setChildrenCheckState(QTreeWidgetItem* item, Qt::CheckState state) const;
    void updateParentCheckState(QTreeWidgetItem* item);
    void updateFolderCheckStates(QTreeWidgetItem* item);
    qint64 calculateTotalSize() const;

    UserProfile& m_profile;

    // UI Components
    QLabel* m_usernameLabel{nullptr};
    QLabel* m_profilePathLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};

    QTreeWidget* m_folderTree{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QPushButton* m_selectRecommendedButton{nullptr};
    QPushButton* m_addCustomButton{nullptr};
    QPushButton* m_removeButton{nullptr};
    QPushButton* m_customizeButton{nullptr};

    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
};

}  // namespace sak
