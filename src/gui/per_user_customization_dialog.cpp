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
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(sak::ui::kSpacingSmall);
    main_layout->setContentsMargins(
        sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge);

    // User info header (compact). username/profile_path are enumerated off disk, so the name is
    // escaped into the bold template and the path label carries no markup of its own.
    m_usernameLabel =
        new QLabel(QString("<b>User: %1</b>").arg(m_profile.username.toHtmlEscaped()));
    main_layout->addWidget(m_usernameLabel);

    m_profilePathLabel =
        sak::plainTextLabel(QString("Profile Path: %1").arg(m_profile.profile_path));
    m_profilePathLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    main_layout->addWidget(m_profilePathLabel);

    setupUi_foldersSection(main_layout);
    setupUi_appDataSection(main_layout);
    setupUi_dialogButtons(main_layout);

    Q_ASSERT(m_profilePathLabel);
}

void PerUserCustomizationDialog::setupUi_foldersSection(QVBoxLayout* main_layout) {
    // Standard folders section
    auto* folders_group = new QGroupBox("Standard Folders");
    auto* folders_layout = new QVBoxLayout(folders_group);
    folders_layout->setSpacing(sak::ui::kSpacingTight);

    // Selection buttons
    auto* selection_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton("Select All");
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectNoneButton = new QPushButton("Select None");
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_selectRecommendedButton = new QPushButton("Select Recommended");
    m_selectRecommendedButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectRecommendedButton->setToolTip("Selects Documents, Desktop, Pictures, and Downloads");

    selection_layout->addWidget(m_selectAllButton);
    selection_layout->addWidget(m_selectNoneButton);
    selection_layout->addWidget(m_selectRecommendedButton);
    selection_layout->addStretch();

    folders_layout->addLayout(selection_layout);

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

    folders_layout->addWidget(m_folderTree, 1);

    // All action buttons in one row: left pair + stretch + right pair
    auto* action_buttons_layout = new QHBoxLayout();
    auto* expand_all_btn = new QPushButton("Expand All");
    expand_all_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    auto* collapse_all_btn = new QPushButton("Collapse All");
    collapse_all_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(expand_all_btn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onExpandAll);
    connect(
        collapse_all_btn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onCollapseAll);

    m_addCustomButton = new QPushButton("Add Custom Folder...");
    m_addCustomButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_removeButton = new QPushButton("Remove Selected");
    m_removeButton->setEnabled(false);
    m_removeButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);

    action_buttons_layout->addWidget(expand_all_btn);
    action_buttons_layout->addWidget(collapse_all_btn);
    action_buttons_layout->addStretch();
    action_buttons_layout->addWidget(m_addCustomButton);
    action_buttons_layout->addWidget(m_removeButton);

    folders_layout->addLayout(action_buttons_layout);

    main_layout->addWidget(folders_group, 1);
}

void PerUserCustomizationDialog::setupUi_appDataSection(QVBoxLayout* main_layout) {
    // Warning about AppData -- selection is handled on the wizard's App Data page
    auto* warning_label = new QLabel(
        "(!) Warning: Full AppData backup is NOT recommended. "
        "It contains machine-specific files that can corrupt profiles. "
        "Application data is selected on the <b>Application Data</b> wizard step.");
    warning_label->setWordWrap(true);
    warning_label->setStyleSheet(sak::ui::warningPanelStyle());
    main_layout->addWidget(warning_label);

    // Summary
    m_summaryLabel = new QLabel();
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    main_layout->addWidget(m_summaryLabel);
}

void PerUserCustomizationDialog::setupUi_dialogButtons(QVBoxLayout* main_layout) {
    // Separator
    auto* separator2 = new QFrame();
    separator2->setFrameShape(QFrame::HLine);
    separator2->setFrameShadow(QFrame::Sunken);
    main_layout->addWidget(separator2);

    // Dialog buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();

    m_okButton = new QPushButton("OK");
    m_okButton->setDefault(true);
    m_okButton->setMinimumWidth(sak::kButtonWidthSmall);
    m_okButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);

    m_cancelButton = new QPushButton("Cancel");
    m_cancelButton->setMinimumWidth(sak::kButtonWidthSmall);
    m_cancelButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);

    button_layout->addWidget(m_okButton);
    button_layout->addWidget(m_cancelButton);

    main_layout->addLayout(button_layout);

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
    QTreeWidgetItem* folder_item = (parent != nullptr) ? new QTreeWidgetItem(parent)
                                                       : new QTreeWidgetItem(m_folderTree);

    // Get absolute path
    const QDir profile_dir(m_profile.profile_path);
    const QString absolute_path = profile_dir.filePath(selection.relative_path);
    const QDir dir(absolute_path);

    if (!dir.exists()) {
        // Folder doesn't exist, just add placeholder
        folder_item->setFlags(folder_item->flags() | Qt::ItemIsUserCheckable |
                              Qt::ItemIsAutoTristate);
        folder_item->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);
        folder_item->setText(0, selection.display_name);
        folder_item->setText(1, "Not Found");
        folder_item->setText(FolderColFiles, "-");
        folder_item->setData(0, Qt::UserRole, selection.relative_path);
        folder_item->setData(0, Qt::UserRole + 1, true);  // Mark as folder
        return;
    }

    // Column 0: Tri-state checkbox for folder (Qt handles checkbox in column 0)
    folder_item->setFlags(folder_item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    folder_item->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);

    // Column 0 also contains folder name
    folder_item->setText(0, QString("[FOLDER] %1").arg(selection.display_name));
    folder_item->setData(0, Qt::UserRole, selection.relative_path);
    folder_item->setData(0, Qt::UserRole + 1, true);  // Mark as folder

    // Recursively add subdirectories and files (with depth limit and lazy loading)
    qint64 total_size = 0;
    int total_files = 0;
    const int max_depth = kInitialFolderScanMaxDepth;
    addDirectoryContents(dir,
                         folder_item,
                         {.total_size = total_size,
                          .total_files = total_files,
                          .checked = selection.selected,
                          .depth = 0,
                          .max_depth = max_depth});

    // Column 2: Size
    QString size_str;
    if (total_size > 0) {
        const double size_mb = static_cast<double>(total_size) / sak::kBytesPerMBf;
        if (size_mb >= sak::kBytesPerKBf) {
            size_str = QString("%1 GB").arg(
                size_mb / sak::kBytesPerKBf, 0, 'f', kSizeDisplayPrecisionLarge);
        } else if (size_mb >= 1) {
            size_str = QString("%1 MB").arg(size_mb, 0, 'f', kSizeDisplayPrecisionSmall);
        } else {
            size_str = QString("%1 KB").arg(static_cast<double>(total_size) / sak::kBytesPerKBf,
                                            0,
                                            'f',
                                            kSizeDisplayPrecisionSmall);
        }
    } else {
        size_str = "0 KB";
    }
    folder_item->setText(1, size_str);

    // Column 2: File count
    folder_item->setText(FolderColFiles, QString::number(total_files));
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
    static const QStringList kRecommended = {"Documents", "Desktop", "Pictures", "Downloads"};
    for (auto& sel : m_profile.folder_selections) {
        sel.selected = kRecommended.contains(sel.display_name);
    }
    applyProfileSelectionsToTree();
    updateSummary();
}

void PerUserCustomizationDialog::onAddCustomFolder() {
    const QString folder_path = QFileDialog::getExistingDirectory(
        this,
        "Select Custom Folder to Backup",
        m_profile.profile_path,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (folder_path.isEmpty()) {
        return;
    }

    // Make path relative to profile path if possible
    const QDir profile_dir(m_profile.profile_path);
    QString relative_path = profile_dir.relativeFilePath(folder_path);

    // Check if already exists
    const bool duplicate = std::ranges::any_of(m_profile.folder_selections,

                                               [&relative_path](const auto& sel) {
                                                   return sel.relative_path == relative_path;
                                               });
    if (duplicate) {
        sak::logWarning("Duplicate folder rejected in backup profile: {}",
                        relative_path.toStdString());
        sak::showWarningLogged(this,
                               "Duplicate Folder",
                               "This folder is already in the backup list.");
        return;
    }

    // Calculate actual size and file count (with reasonable limits)
    const QDir dir(folder_path);
    qint64 total_size = 0;
    int file_count = 0;
    const int max_scan_depth = 10;  // Scan deeper for custom folders
    calculateDirectorySize(dir, total_size, file_count, 0, max_scan_depth);

    // Create new selection
    FolderSelection new_selection;
    new_selection.type = FolderType::Custom;
    new_selection.display_name = QDir(folder_path).dirName();
    new_selection.relative_path = relative_path;
    new_selection.selected = true;
    new_selection.include_patterns = QStringList{"*"};
    new_selection.size_bytes = total_size;
    new_selection.file_count = file_count;

    m_profile.folder_selections.append(new_selection);
    addFolderToTree(new_selection, nullptr);
    updateSummary();
}

void PerUserCustomizationDialog::onRemoveFolder() {
    Q_ASSERT(m_folderTree);
    const QTreeWidgetItem* current_item = m_folderTree->currentItem();
    if (current_item == nullptr) {
        sak::showInformationLogged(this, "Remove Folder", "Please select a folder to remove.");
        return;
    }

    // Get the folder information
    const QString display_text = current_item->text(0);
    QString relative_path = current_item->data(0, Qt::UserRole).toString();

    // Only allow removal of top-level custom folders
    if (relative_path.isEmpty() || current_item->parent() != nullptr) {
        sak::showInformationLogged(
            this,
            "Remove Folder",
            "Only top-level custom folders can be removed.\n"
            "Standard folders (Documents, Desktop, etc.) cannot be removed.");
        return;
    }

    // Find the folder selection
    auto it = std::ranges::find_if(m_profile.folder_selections,

                                   [&relative_path](const FolderSelection& sel) {
                                       return sel.relative_path == relative_path;
                                   });

    if (it == m_profile.folder_selections.end()) {
        sak::logWarning("Attempted to remove folder not found in profile: {}",
                        relative_path.toStdString());
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
            .arg(display_text),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    // Remove from profile
    m_profile.folder_selections.erase(it);

    // Remove from tree
    delete current_item;

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
    QString relative_path = item->data(0, Qt::UserRole).toString();
    if (!relative_path.isEmpty()) {
        auto it =
            std::ranges::find_if(m_profile.folder_selections,

                                 [&](const auto& s) { return s.relative_path == relative_path; });
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
    QTreeWidgetItem* child_item = new QTreeWidgetItem(parent);

    if (entry.isDir()) {
        // Skip symbolic links to prevent infinite loops
        if (entry.isSymLink()) {
            child_item->setFlags(child_item->flags() | Qt::ItemIsUserCheckable);
            child_item->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
            child_item->setText(0, QString("[LINK] %1").arg(entry.fileName()));
            child_item->setData(0, Qt::UserRole + 1, true);
            child_item->setText(1, "-");
            child_item->setText(FolderColFiles, "-");
            return;
        }

        // Directory - add tri-state checkbox
        child_item->setFlags(child_item->flags() | Qt::ItemIsUserCheckable |
                             Qt::ItemIsAutoTristate);
        child_item->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
        child_item->setText(0, QString("[DIR] %1").arg(entry.fileName()));
        child_item->setData(0, Qt::UserRole + 1, true);

        // Recursively add subdirectory contents
        qint64 sub_dir_size = 0;
        int sub_dir_files = 0;
        const QDir sub_dir(entry.filePath());
        addDirectoryContents(sub_dir,
                             child_item,
                             {.total_size = sub_dir_size,
                              .total_files = sub_dir_files,
                              .checked = state.checked,
                              .depth = state.depth + 1,
                              .max_depth = state.max_depth});

        state.total_size += sub_dir_size;
        state.total_files += sub_dir_files;

        child_item->setText(1, formatFileSize(sub_dir_size));
        child_item->setText(FolderColFiles, QString::number(sub_dir_files));

    } else if (entry.isFile()) {
        child_item->setFlags(child_item->flags() | Qt::ItemIsUserCheckable);
        child_item->setCheckState(0, state.checked ? Qt::Checked : Qt::Unchecked);
        child_item->setText(0, entry.fileName());
        child_item->setData(0, Qt::UserRole + 1, false);

        const qint64 file_size = entry.size();
        state.total_size += file_size;
        state.total_files++;

        child_item->setText(FolderColSize, formatFileSize(file_size));
        child_item->setText(FolderColFiles, "-");
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
    const int max_items_per_dir = 500;
    int item_count = 0;

    for (const QFileInfo& entry : entries) {
        if (item_count >= max_items_per_dir) {
            QTreeWidgetItem* more_item = new QTreeWidgetItem(parent);
            more_item->setText(0, QString("... (%1 more items)").arg(entries.size() - item_count));
            more_item->setFlags(Qt::ItemIsEnabled);
            break;
        }

        if (!entry.isReadable()) {
            continue;
        }

        addDirectoryChildItem(entry, parent, state);
        item_count++;
    }
}

void PerUserCustomizationDialog::calculateDirectorySize(
    const QDir& dir, qint64& total_size, int& file_count, int depth, int max_depth) {
    // Prevent excessive recursion
    if (depth >= max_depth) {
        return;
    }

    // Limit total file count for performance
    const int max_file_count = 50'000;
    if (file_count >= max_file_count) {
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
        if (file_count >= max_file_count) {
            return;
        }

        if (entry.isDir() && !entry.isSymLink()) {
            const QDir sub_dir(entry.filePath());
            calculateDirectorySize(sub_dir, total_size, file_count, depth + 1, max_depth);
        } else if (entry.isFile()) {
            total_size += entry.size();
            file_count++;
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

    int checked_count = 0;
    int unchecked_count = 0;
    const int child_count = item->childCount();

    if (child_count == 0) {
        return;
    }

    for (int i = 0; i < child_count; ++i) {
        const Qt::CheckState child_state = item->child(i)->checkState(0);
        if (child_state == Qt::Checked) {
            checked_count++;
        } else if (child_state == Qt::Unchecked) {
            unchecked_count++;
        }
    }

    if (checked_count == child_count) {
        item->setCheckState(0, Qt::Checked);
    } else if (unchecked_count == child_count) {
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
    const Qt::CheckState parent_state = item->checkState(0);
    for (int i = 0; i < item->childCount(); ++i) {
        item->child(i)->setCheckState(0, parent_state);
    }

    // Update parent based on children
    QTreeWidgetItem* parent = item->parent();
    if (parent == nullptr) {
        return;
    }

    int checked_count = 0;
    int unchecked_count = 0;

    for (int i = 0; i < parent->childCount(); ++i) {
        const Qt::CheckState child_state = parent->child(i)->checkState(0);
        if (child_state == Qt::Checked) {
            checked_count++;
        } else if (child_state == Qt::Unchecked) {
            unchecked_count++;
        }
    }

    if (checked_count == parent->childCount()) {
        parent->setCheckState(0, Qt::Checked);
    } else if (unchecked_count == parent->childCount()) {
        parent->setCheckState(0, Qt::Unchecked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void PerUserCustomizationDialog::updateSummary() {
    const qint64 total_size = calculateTotalSize();
    const int selected_count =
        static_cast<int>(std::count_if(m_profile.folder_selections.begin(),
                                       m_profile.folder_selections.end(),
                                       [](const auto& sel) { return sel.selected; }));

    QString summary = QString("<b>Backup Summary:</b> %1 folders selected").arg(selected_count);

    if (total_size > 0) {
        const double size_gb = static_cast<double>(total_size) / sak::kBytesPerGBf;
        summary += QString(" | Estimated size: <b>%1 GB</b>")
                       .arg(size_gb, 0, 'f', kSizeDisplayPrecisionLarge);
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
