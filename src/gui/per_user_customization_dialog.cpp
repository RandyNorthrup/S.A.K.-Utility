// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/per_user_customization_dialog.h"

#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <numeric>

namespace sak {

namespace {

constexpr int kFolderNameColumnWidth = 500;
constexpr int kFolderSizeColumnWidth = 100;
constexpr int kFolderFilesColumnWidth = 80;
constexpr int kFolderTreeIndentationPx = 20;
constexpr int kInitialFolderScanMaxDepth = 2;
constexpr int kSizeDisplayPrecisionLarge = 2;
constexpr int kSizeDisplayPrecisionSmall = 1;

enum FolderColumn {
    FolderColName = 0,
    FolderColSize,
    FolderColFiles,
    FolderColCount
};

}  // namespace

PerUserCustomizationDialog::PerUserCustomizationDialog(UserProfile& profile, QWidget* parent)
    : QDialog(parent), m_target(profile), m_profile(profile) {
    // The dialog edits m_profile (a copy); the caller's live profile is only
    // updated in accept(), so Cancel discards all changes.
    setupUi();
    populateTree();
    updateSummary();

    setWindowTitle(QString("Customize Backup for %1").arg(profile.username));
    resize(sak::kWizardLargeWidth, sak::kWizardLargeHeight);
}

void PerUserCustomizationDialog::accept() {
    m_target = m_profile;  // Commit the working copy only when the user clicks OK.
    QDialog::accept();
}

void PerUserCustomizationDialog::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(sak::ui::kSpacingSmall);
    mainLayout->setContentsMargins(
        sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge);

    // User info header (compact). username/profile_path are enumerated off disk, so the name is
    // escaped into the bold template and the path label carries no markup of its own.
    m_usernameLabel =
        new QLabel(QString("<b>User: %1</b>").arg(m_profile.username.toHtmlEscaped()));
    mainLayout->addWidget(m_usernameLabel);

    m_profilePathLabel =
        sak::plainTextLabel(QString("Profile Path: %1").arg(m_profile.profile_path));
    m_profilePathLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    mainLayout->addWidget(m_profilePathLabel);

    setupUi_foldersSection(mainLayout);
    setupUi_appDataSection(mainLayout);
    setupUi_dialogButtons(mainLayout);

    Q_ASSERT(m_profilePathLabel);
}

void PerUserCustomizationDialog::setupUi_foldersSection(QVBoxLayout* mainLayout) {
    // Standard folders section
    auto* foldersGroup = new QGroupBox("Standard Folders");
    auto* foldersLayout = new QVBoxLayout(foldersGroup);
    foldersLayout->setSpacing(sak::ui::kSpacingTight);

    // Selection buttons
    auto* selectionLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton("Select All");
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectNoneButton = new QPushButton("Select None");
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_selectRecommendedButton = new QPushButton("Select Recommended");
    m_selectRecommendedButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectRecommendedButton->setToolTip("Selects Documents, Desktop, Pictures, and Downloads");

    selectionLayout->addWidget(m_selectAllButton);
    selectionLayout->addWidget(m_selectNoneButton);
    selectionLayout->addWidget(m_selectRecommendedButton);
    selectionLayout->addStretch();

    foldersLayout->addLayout(selectionLayout);

    // Folder tree
    m_folderTree = new QTreeWidget();
    m_folderTree->setColumnCount(FolderColCount);
    m_folderTree->setHeaderLabels({"Folder", "Size", "Files"});
    m_folderTree->setColumnWidth(FolderColName, kFolderNameColumnWidth);
    m_folderTree->setColumnWidth(FolderColSize, kFolderSizeColumnWidth);
    m_folderTree->setColumnWidth(FolderColFiles, kFolderFilesColumnWidth);
    m_folderTree->setAlternatingRowColors(true);
    m_folderTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_folderTree->setRootIsDecorated(true);
    m_folderTree->setIndentation(kFolderTreeIndentationPx);
    m_folderTree->header()->setStretchLastSection(false);
    m_folderTree->header()->setSectionResizeMode(FolderColName, QHeaderView::Stretch);

    foldersLayout->addWidget(m_folderTree, 1);

    // All action buttons in one row: left pair + stretch + right pair
    auto* actionButtonsLayout = new QHBoxLayout();
    auto* expandAllBtn = new QPushButton("Expand All");
    expandAllBtn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    auto* collapseAllBtn = new QPushButton("Collapse All");
    collapseAllBtn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(expandAllBtn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onExpandAll);
    connect(
        collapseAllBtn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onCollapseAll);

    m_addCustomButton = new QPushButton("Add Custom Folder...");
    m_addCustomButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_removeButton = new QPushButton("Remove Selected");
    m_removeButton->setEnabled(false);
    m_removeButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);

    actionButtonsLayout->addWidget(expandAllBtn);
    actionButtonsLayout->addWidget(collapseAllBtn);
    actionButtonsLayout->addStretch();
    actionButtonsLayout->addWidget(m_addCustomButton);
    actionButtonsLayout->addWidget(m_removeButton);

    foldersLayout->addLayout(actionButtonsLayout);

    mainLayout->addWidget(foldersGroup, 1);
}

void PerUserCustomizationDialog::setupUi_appDataSection(QVBoxLayout* mainLayout) {
    // Warning about AppData -- selection is handled on the wizard's App Data page
    auto* warningLabel = new QLabel(
        "(!) Warning: Full AppData backup is NOT recommended. "
        "It contains machine-specific files that can corrupt profiles. "
        "Application data is selected on the <b>Application Data</b> wizard step.");
    warningLabel->setWordWrap(true);
    warningLabel->setStyleSheet(sak::ui::warningPanelStyle());
    mainLayout->addWidget(warningLabel);

    // Summary
    m_summaryLabel = new QLabel();
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    mainLayout->addWidget(m_summaryLabel);
}

void PerUserCustomizationDialog::setupUi_dialogButtons(QVBoxLayout* mainLayout) {
    // Separator
    auto* separator2 = new QFrame();
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(separator2);

    // Dialog buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();

    m_okButton = new QPushButton("OK");
    m_okButton->setDefault(true);
    m_okButton->setMinimumWidth(sak::kButtonWidthSmall);
    m_okButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);

    m_cancelButton = new QPushButton("Cancel");
    m_cancelButton->setMinimumWidth(sak::kButtonWidthSmall);
    m_cancelButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);

    buttonLayout->addWidget(m_okButton);
    buttonLayout->addWidget(m_cancelButton);

    mainLayout->addLayout(buttonLayout);

    // Connections
    connect(
        m_selectAllButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onSelectAll);
    connect(
        m_selectNoneButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onSelectNone);
    connect(m_selectRecommendedButton,
            &QPushButton::clicked,
            this,
            &PerUserCustomizationDialog::onSelectRecommended);
    connect(m_addCustomButton,
            &QPushButton::clicked,
            this,
            &PerUserCustomizationDialog::onAddCustomFolder);
    connect(
        m_removeButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onRemoveFolder);

    connect(m_folderTree,
            &QTreeWidget::itemChanged,
            this,
            &PerUserCustomizationDialog::onTreeItemChanged);

    connect(m_okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

void PerUserCustomizationDialog::populateTree() {
    Q_ASSERT(m_folderTree);
    m_folderTree->clear();
    m_folderTree->setUpdatesEnabled(false);

    // Build hierarchical structure from flat folder list
    for (const auto& selection : m_profile.folder_selections) {
        addFolderToTree(selection, nullptr);
    }

    m_folderTree->setUpdatesEnabled(true);
    // Expand only top level items by default
    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i) {
        m_folderTree->topLevelItem(i)->setExpanded(false);
    }
}

void PerUserCustomizationDialog::addFolderToTree(const FolderSelection& selection,
                                                 QTreeWidgetItem* parent) {
    QTreeWidgetItem* folderItem = (parent != nullptr) ? new QTreeWidgetItem(parent)
                                                      : new QTreeWidgetItem(m_folderTree);

    // Get absolute path
    const QDir profileDir(m_profile.profile_path);
    const QString absolutePath = profileDir.filePath(selection.relative_path);
    const QDir dir(absolutePath);

    if (!dir.exists()) {
        // Folder doesn't exist, just add placeholder
        folderItem->setFlags(folderItem->flags() | Qt::ItemIsUserCheckable |
                             Qt::ItemIsAutoTristate);
        folderItem->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);
        folderItem->setText(0, selection.display_name);
        folderItem->setText(1, "Not Found");
        folderItem->setText(FolderColFiles, "-");
        folderItem->setData(0, Qt::UserRole, selection.relative_path);
        folderItem->setData(0, Qt::UserRole + 1, true);  // Mark as folder
        return;
    }

    // Column 0: Tri-state checkbox for folder (Qt handles checkbox in column 0)
    folderItem->setFlags(folderItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    folderItem->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);

    // Column 0 also contains folder name
    folderItem->setText(0, QString("[FOLDER] %1").arg(selection.display_name));
    folderItem->setData(0, Qt::UserRole, selection.relative_path);
    folderItem->setData(0, Qt::UserRole + 1, true);  // Mark as folder

    // Recursively add subdirectories and files (with depth limit and lazy loading)
    qint64 totalSize = 0;
    int totalFiles = 0;
    const int MAX_DEPTH = kInitialFolderScanMaxDepth;
    addDirectoryContents(dir,
                         folderItem,
                         {.total_size = totalSize,
                          .total_files = totalFiles,
                          .checked = selection.selected,
                          .depth = 0,
                          .max_depth = MAX_DEPTH});

    // Column 2: Size
    QString sizeStr;
    if (totalSize > 0) {
        const double sizeMB = static_cast<double>(totalSize) / sak::kBytesPerMBf;
        if (sizeMB >= sak::kBytesPerKBf) {
            sizeStr = QString("%1 GB").arg(
                sizeMB / sak::kBytesPerKBf, 0, 'f', kSizeDisplayPrecisionLarge);
        } else if (sizeMB >= 1) {
            sizeStr = QString("%1 MB").arg(sizeMB, 0, 'f', kSizeDisplayPrecisionSmall);
        } else {
            sizeStr = QString("%1 KB").arg(static_cast<double>(totalSize) / sak::kBytesPerKBf,
                                           0,
                                           'f',
                                           kSizeDisplayPrecisionSmall);
        }
    } else {
        sizeStr = "0 KB";
    }
    folderItem->setText(1, sizeStr);

    // Column 2: File count
    folderItem->setText(FolderColFiles, QString::number(totalFiles));
}

void PerUserCustomizationDialog::setAllFolderSelected(bool selected) {
    for (auto& sel : m_profile.folder_selections) {
        sel.selected = selected;
    }
}

void PerUserCustomizationDialog::applyProfileSelectionsToTree() {
    Q_ASSERT(m_folderTree);
    m_folderTree->blockSignals(true);
    for (int i = 0; i < m_folderTree->topLevelItemCount(); ++i) {
        auto* item = m_folderTree->topLevelItem(i);
        const QString rel = item->data(0, Qt::UserRole).toString();
        const auto it =
            std::ranges::find_if(m_profile.folder_selections,

                                 [&rel](const auto& s) { return s.relative_path == rel; });
        const bool sel = (it != m_profile.folder_selections.end()) && it->selected;
        const Qt::CheckState state = sel ? Qt::Checked : Qt::Unchecked;
        item->setCheckState(0, state);
        setChildrenCheckState(item, state);
    }
    m_folderTree->blockSignals(false);
}

void PerUserCustomizationDialog::onSelectAll() {
    // Update the profile (source of truth), then reflect into the tree -- the old
    // code only toggled checkboxes, leaving folder_selections unchanged.
    setAllFolderSelected(true);
    applyProfileSelectionsToTree();
    updateSummary();
}

void PerUserCustomizationDialog::onSelectNone() {
    setAllFolderSelected(false);
    applyProfileSelectionsToTree();
    updateSummary();
}

void PerUserCustomizationDialog::onSelectRecommended() {
    static const QStringList recommended = {"Documents", "Desktop", "Pictures", "Downloads"};
    for (auto& sel : m_profile.folder_selections) {
        sel.selected = recommended.contains(sel.display_name);
    }
    applyProfileSelectionsToTree();
    updateSummary();
}

void PerUserCustomizationDialog::onAddCustomFolder() {
    const QString folderPath = QFileDialog::getExistingDirectory(
        this,
        "Select Custom Folder to Backup",
        m_profile.profile_path,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (folderPath.isEmpty()) {
        return;
    }

    // Make path relative to profile path if possible
    const QDir profileDir(m_profile.profile_path);
    QString relativePath = profileDir.relativeFilePath(folderPath);

    // Check if already exists
    const bool duplicate = std::ranges::any_of(m_profile.folder_selections,

                                               [&relativePath](const auto& sel) {
                                                   return sel.relative_path == relativePath;
                                               });
    if (duplicate) {
        sak::logWarning("Duplicate folder rejected in backup profile: {}",
                        relativePath.toStdString());
        sak::showWarningLogged(this,
                               "Duplicate Folder",
                               "This folder is already in the backup list.");
        return;
    }

    // Calculate actual size and file count (with reasonable limits)
    const QDir dir(folderPath);
    qint64 totalSize = 0;
    int fileCount = 0;
    const int MAX_SCAN_DEPTH = 10;  // Scan deeper for custom folders
    calculateDirectorySize(dir, totalSize, fileCount, 0, MAX_SCAN_DEPTH);

    // Create new selection
    FolderSelection newSelection;
    newSelection.type = FolderType::Custom;
    newSelection.display_name = QDir(folderPath).dirName();
    newSelection.relative_path = relativePath;
    newSelection.selected = true;
    newSelection.include_patterns = QStringList{"*"};
    newSelection.size_bytes = totalSize;
    newSelection.file_count = fileCount;

    m_profile.folder_selections.append(newSelection);
    addFolderToTree(newSelection, nullptr);
    updateSummary();
}

void PerUserCustomizationDialog::onRemoveFolder() {
    Q_ASSERT(m_folderTree);
    const QTreeWidgetItem* currentItem = m_folderTree->currentItem();
    if (currentItem == nullptr) {
        sak::showInformationLogged(this, "Remove Folder", "Please select a folder to remove.");
        return;
    }

    // Get the folder information
    const QString displayText = currentItem->text(0);
    QString relativePath = currentItem->data(0, Qt::UserRole).toString();

    // Only allow removal of top-level custom folders
    if (relativePath.isEmpty() || currentItem->parent() != nullptr) {
        sak::showInformationLogged(
            this,
            "Remove Folder",
            "Only top-level custom folders can be removed.\n"
            "Standard folders (Documents, Desktop, etc.) cannot be removed.");
        return;
    }

    // Find the folder selection
    auto it = std::ranges::find_if(m_profile.folder_selections,

                                   [&relativePath](const FolderSelection& sel) {
                                       return sel.relative_path == relativePath;
                                   });

    if (it == m_profile.folder_selections.end()) {
        sak::logWarning("Attempted to remove folder not found in profile: {}",
                        relativePath.toStdString());
        sak::showWarningLogged(this, "Remove Folder", "Folder not found in profile.");
        return;
    }

    // Only allow removal of custom folders
    if (it->type != FolderType::Custom) {
        sak::showInformationLogged(
            this,
            "Remove Folder",
            "Only custom folders can be removed.\n"
            "Standard folders (Documents, Desktop, etc.) are part of the default profile.");
        return;
    }

    // Confirm removal
    auto reply = sak::showQuestionLogged(
        this,
        "Confirm Removal",
        QString("Remove folder \"%1\" from backup?\n\nThis will not delete the actual folder from "
                "disk.")
            .arg(displayText),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Remove from profile
    m_profile.folder_selections.erase(it);

    // Remove from tree
    delete currentItem;

    // Update summary
    updateSummary();
}

void PerUserCustomizationDialog::onTreeItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_folderTree);
    Q_ASSERT(item);
    if (column != 0) {
        return;
    }

    // Manually propagate state changes from parent to children
    // Qt's AutoTristate updates parents based on children, but not the reverse
    if (item->childCount() > 0) {
        m_folderTree->blockSignals(true);
        const Qt::CheckState state = item->checkState(0);
        setChildrenCheckState(item, state);
        m_folderTree->blockSignals(false);
    }

    // Update corresponding folder selection for top-level folders
    QString relativePath = item->data(0, Qt::UserRole).toString();
    if (!relativePath.isEmpty()) {
        auto it =
            std::ranges::find_if(m_profile.folder_selections,

                                 [&](const auto& s) { return s.relative_path == relativePath; });
        if (it != m_profile.folder_selections.end()) {
            it->selected = (item->checkState(0) == Qt::Checked);
        }
    }

    updateSummary();
}

void PerUserCustomizationDialog::onExpandAll() {
    m_folderTree->expandAll();
}

void PerUserCustomizationDialog::onCollapseAll() {
    m_folderTree->collapseAll();
}

QString PerUserCustomizationDialog::formatFileSize(qint64 bytes) {
    return sak::formatBytes(bytes);
}

void PerUserCustomizationDialog::addDirectoryChildItem(const QFileInfo& entry,
                                                       QTreeWidgetItem* parent,
                                                       DirTraversalState& state) {
    QTreeWidgetItem* childItem = new QTreeWidgetItem(parent);

    if (entry.isDir()) {
        // Skip symbolic links to prevent infinite loops
        if (entry.isSymLink()) {
            childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
            childItem->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
            childItem->setText(0, QString("[LINK] %1").arg(entry.fileName()));
            childItem->setData(0, Qt::UserRole + 1, true);
            childItem->setText(1, "-");
            childItem->setText(FolderColFiles, "-");
            return;
        }

        // Directory - add tri-state checkbox
        childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        childItem->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
        childItem->setText(0, QString("[DIR] %1").arg(entry.fileName()));
        childItem->setData(0, Qt::UserRole + 1, true);

        // Recursively add subdirectory contents
        qint64 subDirSize = 0;
        int subDirFiles = 0;
        const QDir subDir(entry.filePath());
        addDirectoryContents(subDir,
                             childItem,
                             {.total_size = subDirSize,
                              .total_files = subDirFiles,
                              .checked = state.checked,
                              .depth = state.depth + 1,
                              .max_depth = state.max_depth});

        state.total_size += subDirSize;
        state.total_files += subDirFiles;

        childItem->setText(1, formatFileSize(subDirSize));
        childItem->setText(FolderColFiles, QString::number(subDirFiles));

    } else if (entry.isFile()) {
        childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
        childItem->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
        childItem->setText(0, entry.fileName());
        childItem->setData(0, Qt::UserRole + 1, false);

        const qint64 fileSize = entry.size();
        state.total_size += fileSize;
        state.total_files++;

        childItem->setText(FolderColSize, formatFileSize(fileSize));
        childItem->setText(FolderColFiles, "-");
    }
}

void PerUserCustomizationDialog::addDirectoryContents(const QDir& dir,
                                                      QTreeWidgetItem* parent,
                                                      DirTraversalState state) {
    // Depth limit to prevent stack overflow
    if (state.depth >= state.max_depth) {
        return;
    }

    // Get all entries (files and directories) with error handling
    QFileInfoList entries;
    try {
        entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable,
                                    QDir::Name | QDir::DirsFirst);
    } catch (const std::exception& e) {
        sak::logWarning("Failed to list directory contents: {} ({})",
                        dir.absolutePath().toStdString(),
                        e.what());
        return;
    }

    // Limit items per directory to prevent UI slowdown
    const int MAX_ITEMS_PER_DIR = 500;
    int itemCount = 0;

    for (const QFileInfo& entry : entries) {
        if (itemCount >= MAX_ITEMS_PER_DIR) {
            QTreeWidgetItem* moreItem = new QTreeWidgetItem(parent);
            moreItem->setText(0, QString("... (%1 more items)").arg(entries.size() - itemCount));
            moreItem->setFlags(Qt::ItemIsEnabled);
            break;
        }

        if (!entry.isReadable()) {
            continue;
        }

        addDirectoryChildItem(entry, parent, state);
        itemCount++;
    }
}

void PerUserCustomizationDialog::calculateDirectorySize(
    const QDir& dir, qint64& totalSize, int& fileCount, int depth, int maxDepth) {
    // Prevent excessive recursion
    if (depth >= maxDepth) {
        return;
    }

    // Limit total file count for performance
    const int MAX_FILE_COUNT = 50'000;
    if (fileCount >= MAX_FILE_COUNT) {
        return;
    }

    QFileInfoList entries;
    try {
        entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable);
    } catch (const std::exception& e) {
        sak::logWarning("Failed to enumerate directory for size calculation: {} ({})",
                        dir.absolutePath().toStdString(),
                        e.what());
        return;
    }

    for (const QFileInfo& entry : entries) {
        if (!entry.isReadable()) {
            continue;
        }
        if (fileCount >= MAX_FILE_COUNT) {
            return;
        }

        if (entry.isDir() && !entry.isSymLink()) {
            const QDir subDir(entry.filePath());
            calculateDirectorySize(subDir, totalSize, fileCount, depth + 1, maxDepth);
        } else if (entry.isFile()) {
            totalSize += entry.size();
            fileCount++;
        }
    }
}

void PerUserCustomizationDialog::setChildrenCheckState(QTreeWidgetItem* item,
                                                       Qt::CheckState state) const {
    if (item == nullptr) {
        return;
    }

    // Only propagate Checked or Unchecked states (not PartiallyChecked)
    if (state == Qt::PartiallyChecked) {
        return;
    }

    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        child->setCheckState(0, state);
        setChildrenCheckState(child, state);  // Recursively update all descendants
    }
}

void PerUserCustomizationDialog::updateParentCheckState(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }

    int checkedCount = 0;
    int uncheckedCount = 0;
    const int childCount = item->childCount();

    if (childCount == 0) {
        return;
    }

    for (int i = 0; i < childCount; ++i) {
        const Qt::CheckState childState = item->child(i)->checkState(0);
        if (childState == Qt::Checked) {
            checkedCount++;
        } else if (childState == Qt::Unchecked) {
            uncheckedCount++;
        }
    }

    if (checkedCount == childCount) {
        item->setCheckState(0, Qt::Checked);
    } else if (uncheckedCount == childCount) {
        item->setCheckState(0, Qt::Unchecked);
    } else {
        item->setCheckState(0, Qt::PartiallyChecked);
    }

    // Recursively update parent
    updateParentCheckState(item->parent());
}

void PerUserCustomizationDialog::updateFolderCheckStates(QTreeWidgetItem* item) {
    // Tri-state checkbox logic for parent/child relationships
    if (item == nullptr) {
        return;
    }

    // Update children when parent changes
    const Qt::CheckState parentState = item->checkState(0);
    for (int i = 0; i < item->childCount(); ++i) {
        item->child(i)->setCheckState(0, parentState);
    }

    // Update parent based on children
    QTreeWidgetItem* parent = item->parent();
    if (parent == nullptr) {
        return;
    }

    int checkedCount = 0;
    int uncheckedCount = 0;

    for (int i = 0; i < parent->childCount(); ++i) {
        const Qt::CheckState childState = parent->child(i)->checkState(0);
        if (childState == Qt::Checked) {
            checkedCount++;
        } else if (childState == Qt::Unchecked) {
            uncheckedCount++;
        }
    }

    if (checkedCount == parent->childCount()) {
        parent->setCheckState(0, Qt::Checked);
    } else if (uncheckedCount == parent->childCount()) {
        parent->setCheckState(0, Qt::Unchecked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void PerUserCustomizationDialog::updateSummary() {
    const qint64 totalSize = calculateTotalSize();
    const int selectedCount =
        static_cast<int>(std::count_if(m_profile.folder_selections.begin(),
                                       m_profile.folder_selections.end(),
                                       [](const auto& sel) { return sel.selected; }));

    QString summary = QString("<b>Backup Summary:</b> %1 folders selected").arg(selectedCount);

    if (totalSize > 0) {
        const double sizeGB = static_cast<double>(totalSize) / sak::kBytesPerGBf;
        summary += QString(" | Estimated size: <b>%1 GB</b>")
                       .arg(sizeGB, 0, 'f', kSizeDisplayPrecisionLarge);
    }

    m_summaryLabel->setText(summary);
}

qint64 PerUserCustomizationDialog::calculateTotalSize() const {
    return std::accumulate(m_profile.folder_selections.begin(),
                           m_profile.folder_selections.end(),
                           qint64{0},
                           [](qint64 acc, const auto& sel) {
                               return sel.selected ? acc + sel.size_bytes : acc;
                           });
}

QVector<FolderSelection> PerUserCustomizationDialog::getFolderSelections() const {
    // Return updated selections from tree
    // Note: selections are already updated in onTreeItemChanged
    return m_profile.folder_selections;
}

}  // namespace sak
