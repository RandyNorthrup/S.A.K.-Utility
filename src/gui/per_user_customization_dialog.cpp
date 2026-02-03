#include "sak/per_user_customization_dialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QTreeWidgetItem>

namespace sak {

PerUserCustomizationDialog::PerUserCustomizationDialog(UserProfile& profile, QWidget* parent)
    : QDialog(parent)
    , m_profile(profile)
{
    setupUi();
    populateTree();
    updateSummary();
    
    setWindowTitle(QString("Customize Backup for %1").arg(profile.username));
    resize(900, 700);
}

void PerUserCustomizationDialog::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    
    // User info header
    auto* headerLayout = new QVBoxLayout();
    m_usernameLabel = new QLabel(QString("<h3>User: %1</h3>").arg(m_profile.username));
    headerLayout->addWidget(m_usernameLabel);
    
    m_profilePathLabel = new QLabel(QString("Profile Path: %1").arg(m_profile.profile_path));
    m_profilePathLabel->setStyleSheet("color: #64748b;");
    headerLayout->addWidget(m_profilePathLabel);
    
    mainLayout->addLayout(headerLayout);
    
    // Separator
    auto* separator1 = new QFrame();
    separator1->setFrameShape(QFrame::HLine);
    separator1->setFrameShadow(QFrame::Sunken);
    mainLayout->addWidget(separator1);
    
    // Standard folders section
    auto* foldersGroup = new QGroupBox("Standard Folders");
    auto* foldersLayout = new QVBoxLayout(foldersGroup);
    
    // Selection buttons
    auto* selectionLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton("Select All");
    m_selectNoneButton = new QPushButton("Select None");
    m_selectRecommendedButton = new QPushButton("Select Recommended");
    m_selectRecommendedButton->setToolTip("Documents, Desktop, Pictures, Downloads");
    
    selectionLayout->addWidget(m_selectAllButton);
    selectionLayout->addWidget(m_selectNoneButton);
    selectionLayout->addWidget(m_selectRecommendedButton);
    selectionLayout->addStretch();
    
    foldersLayout->addLayout(selectionLayout);
    
    // Folder tree
    m_folderTree = new QTreeWidget();
    m_folderTree->setColumnCount(3);  // Reduce to 3 columns: Folder (with checkbox), Size, Files
    m_folderTree->setHeaderLabels({"Folder", "Size", "Files"});
    m_folderTree->setColumnWidth(0, 500);  // Folder name - includes checkbox via Qt
    m_folderTree->setColumnWidth(1, 100);  // Size
    m_folderTree->setColumnWidth(2, 80);   // File count
    m_folderTree->setAlternatingRowColors(true);
    m_folderTree->setSelectionMode(QAbstractItemView::NoSelection);
    m_folderTree->setRootIsDecorated(true);  // Show expand/collapse arrows
    m_folderTree->setIndentation(20);      // Normal indentation
    m_folderTree->header()->setStretchLastSection(false);
    m_folderTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    
    foldersLayout->addWidget(m_folderTree);
    
    // Expand/collapse buttons
    auto* treeButtonsLayout = new QHBoxLayout();
    auto* expandAllBtn = new QPushButton("Expand All");
    auto* collapseAllBtn = new QPushButton("Collapse All");
    connect(expandAllBtn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onExpandAll);
    connect(collapseAllBtn, &QPushButton::clicked, this, &PerUserCustomizationDialog::onCollapseAll);
    treeButtonsLayout->addWidget(expandAllBtn);
    treeButtonsLayout->addWidget(collapseAllBtn);
    treeButtonsLayout->addStretch();
    foldersLayout->addLayout(treeButtonsLayout);
    
    // Custom folder buttons
    auto* customLayout = new QHBoxLayout();
    m_addCustomButton = new QPushButton("Add Custom Folder...");
    m_removeButton = new QPushButton("Remove Selected");
    m_removeButton->setEnabled(false);
    
    customLayout->addWidget(m_addCustomButton);
    customLayout->addWidget(m_removeButton);
    customLayout->addStretch();
    
    foldersLayout->addLayout(customLayout);
    
    mainLayout->addWidget(foldersGroup);
    
    // Application data section
    m_appDataGroup = new QGroupBox("Application Data (Selective)");
    auto* appDataLayout = new QVBoxLayout(m_appDataGroup);
    
    m_browserBookmarksCheck = new QCheckBox("Browser Bookmarks (Chrome, Edge, Firefox)");
    m_browserBookmarksCheck->setToolTip("Safe to backup - bookmarks only, no cache");
    appDataLayout->addWidget(m_browserBookmarksCheck);
    
    m_emailSignaturesCheck = new QCheckBox("Email Signatures (Outlook)");
    appDataLayout->addWidget(m_emailSignaturesCheck);
    
    m_officeTemplatesCheck = new QCheckBox("Office Templates");
    appDataLayout->addWidget(m_officeTemplatesCheck);
    
    m_vsCodeSettingsCheck = new QCheckBox("VS Code Settings");
    m_vsCodeSettingsCheck->setToolTip("Settings.json and keybindings.json only");
    appDataLayout->addWidget(m_vsCodeSettingsCheck);
    
    auto* warningLabel = new QLabel(
        "⚠ Warning: Full AppData backup is NOT recommended. "
        "It contains machine-specific files that can corrupt profiles."
    );
    warningLabel->setWordWrap(true);
    warningLabel->setStyleSheet("color: #b91c1c; padding: 8px; background-color: #fee2e2; border-radius: 10px;");
    appDataLayout->addWidget(warningLabel);
    
    mainLayout->addWidget(m_appDataGroup);
    
    // Summary
    m_summaryLabel = new QLabel();
    m_summaryLabel->setStyleSheet("padding: 10px; background-color: #e0f2fe; border-radius: 10px;");
    mainLayout->addWidget(m_summaryLabel);
    
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
    m_okButton->setMinimumWidth(100);
    
    m_cancelButton = new QPushButton("Cancel");
    m_cancelButton->setMinimumWidth(100);
    
    buttonLayout->addWidget(m_okButton);
    buttonLayout->addWidget(m_cancelButton);
    
    mainLayout->addLayout(buttonLayout);
    
    // Connections
    connect(m_selectAllButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onSelectAll);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onSelectNone);
    connect(m_selectRecommendedButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onSelectRecommended);
    connect(m_addCustomButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onAddCustomFolder);
    connect(m_removeButton, &QPushButton::clicked, this, &PerUserCustomizationDialog::onRemoveFolder);
    
    connect(m_folderTree, &QTreeWidget::itemChanged, this, &PerUserCustomizationDialog::onTreeItemChanged);
    
    connect(m_browserBookmarksCheck, &QCheckBox::stateChanged, this, &PerUserCustomizationDialog::updateSummary);
    connect(m_emailSignaturesCheck, &QCheckBox::stateChanged, this, &PerUserCustomizationDialog::updateSummary);
    connect(m_officeTemplatesCheck, &QCheckBox::stateChanged, this, &PerUserCustomizationDialog::updateSummary);
    connect(m_vsCodeSettingsCheck, &QCheckBox::stateChanged, this, &PerUserCustomizationDialog::updateSummary);
    
    connect(m_okButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}

void PerUserCustomizationDialog::populateTree() {
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

void PerUserCustomizationDialog::addFolderToTree(const FolderSelection& selection, QTreeWidgetItem* parent) {
    QTreeWidgetItem* folderItem = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_folderTree);
    
    // Get absolute path
    QDir profileDir(m_profile.profile_path);
    QString absolutePath = profileDir.filePath(selection.relative_path);
    QDir dir(absolutePath);
    
    if (!dir.exists()) {
        // Folder doesn't exist, just add placeholder
        folderItem->setFlags(folderItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
        folderItem->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);
        folderItem->setText(0, selection.display_name);
        folderItem->setText(1, "Not Found");
        folderItem->setText(2, "-");
        folderItem->setData(0, Qt::UserRole, selection.relative_path);
        folderItem->setData(0, Qt::UserRole + 1, true); // Mark as folder
        return;
    }
    
    // Column 0: Tri-state checkbox for folder (Qt handles checkbox in column 0)
    folderItem->setFlags(folderItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
    folderItem->setCheckState(0, selection.selected ? Qt::Checked : Qt::Unchecked);
    
    // Column 0 also contains folder name
    folderItem->setText(0, QString("[FOLDER] %1").arg(selection.display_name));
    folderItem->setData(0, Qt::UserRole, selection.relative_path);
    folderItem->setData(0, Qt::UserRole + 1, true); // Mark as folder
    
    // Recursively add subdirectories and files (with depth limit and lazy loading)
    qint64 totalSize = 0;
    int totalFiles = 0;
    const int MAX_DEPTH = 2; // Only scan 2 levels deep initially
    addDirectoryContents(dir, folderItem, totalSize, totalFiles, selection.selected, 0, MAX_DEPTH);
    
    // Column 2: Size
    QString sizeStr;
    if (totalSize > 0) {
        double sizeMB = totalSize / (1024.0 * 1024.0);
        if (sizeMB >= 1024) {
            sizeStr = QString("%1 GB").arg(sizeMB / 1024.0, 0, 'f', 2);
        } else if (sizeMB >= 1) {
            sizeStr = QString("%1 MB").arg(sizeMB, 0, 'f', 1);
        } else {
            sizeStr = QString("%1 KB").arg(totalSize / 1024.0, 0, 'f', 1);
        }
    } else {
        sizeStr = "0 KB";
    }
    folderItem->setText(1, sizeStr);
    
    // Column 2: File count
    folderItem->setText(2, QString::number(totalFiles));
}

void PerUserCustomizationDialog::onSelectAll() {
    m_folderTree->blockSignals(true);
    QTreeWidgetItemIterator it(m_folderTree);
    while (*it) {
        (*it)->setCheckState(0, Qt::Checked);
        ++it;
    }
    m_folderTree->blockSignals(false);
    updateSummary();
}

void PerUserCustomizationDialog::onSelectNone() {
    m_folderTree->blockSignals(true);
    QTreeWidgetItemIterator it(m_folderTree);
    while (*it) {
        (*it)->setCheckState(0, Qt::Unchecked);
        ++it;
    }
    m_folderTree->blockSignals(false);
    updateSummary();
}

void PerUserCustomizationDialog::onSelectRecommended() {
    // Documents, Desktop, Pictures, Downloads
    QStringList recommended = {"Documents", "Desktop", "Pictures", "Downloads"};
    
    m_folderTree->blockSignals(true);
    QTreeWidgetItemIterator it(m_folderTree);
    while (*it) {
        QString folderName = (*it)->text(1);
        bool shouldSelect = recommended.contains(folderName);
        (*it)->setCheckState(0, shouldSelect ? Qt::Checked : Qt::Unchecked);
        ++it;
    }
    m_folderTree->blockSignals(false);
    updateSummary();
}

void PerUserCustomizationDialog::onAddCustomFolder() {
    QString folderPath = QFileDialog::getExistingDirectory(
        this,
        "Select Custom Folder to Backup",
        m_profile.profile_path,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (folderPath.isEmpty()) {
        return;
    }
    
    // Make path relative to profile path if possible
    QDir profileDir(m_profile.profile_path);
    QString relativePath = profileDir.relativeFilePath(folderPath);
    
    // Check if already exists
    for (const auto& sel : m_profile.folder_selections) {
        if (sel.relative_path == relativePath) {
            QMessageBox::warning(this, "Duplicate Folder",
                "This folder is already in the backup list.");
            return;
        }
    }
    
    // Calculate actual size and file count (with reasonable limits)
    QDir dir(folderPath);
    qint64 totalSize = 0;
    int fileCount = 0;
    const int MAX_SCAN_DEPTH = 10; // Scan deeper for custom folders
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
    QTreeWidgetItem* currentItem = m_folderTree->currentItem();
    if (!currentItem) {
        QMessageBox::information(this, "Remove Folder", "Please select a folder to remove.");
        return;
    }
    
    // Get the folder information
    QString displayText = currentItem->text(0);
    QString relativePath = currentItem->data(0, Qt::UserRole).toString();
    
    // Only allow removal of top-level custom folders
    if (relativePath.isEmpty() || currentItem->parent() != nullptr) {
        QMessageBox::information(this, "Remove Folder", 
            "Only top-level custom folders can be removed.\n"
            "Standard folders (Documents, Desktop, etc.) cannot be removed.");
        return;
    }
    
    // Find the folder selection
    auto it = std::find_if(m_profile.folder_selections.begin(), 
                          m_profile.folder_selections.end(),
                          [&relativePath](const FolderSelection& sel) {
                              return sel.relative_path == relativePath;
                          });
    
    if (it == m_profile.folder_selections.end()) {
        QMessageBox::warning(this, "Remove Folder", "Folder not found in profile.");
        return;
    }
    
    // Only allow removal of custom folders
    if (it->type != FolderType::Custom) {
        QMessageBox::information(this, "Remove Folder",
            "Only custom folders can be removed.\n"
            "Standard folders (Documents, Desktop, etc.) are part of the default profile.");
        return;
    }
    
    // Confirm removal
    auto reply = QMessageBox::question(this, "Confirm Removal",
        QString("Remove folder \"%1\" from backup?\n\nThis will not delete the actual folder from disk.")
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
    if (column != 0) return;
    
    // Manually propagate state changes from parent to children
    // Qt's AutoTristate updates parents based on children, but not the reverse
    if (item->childCount() > 0) {
        m_folderTree->blockSignals(true);
        Qt::CheckState state = item->checkState(0);
        setChildrenCheckState(item, state);
        m_folderTree->blockSignals(false);
    }
    
    // Update corresponding folder selection for top-level folders
    QString relativePath = item->data(0, Qt::UserRole).toString();
    if (!relativePath.isEmpty()) {
        for (auto& sel : m_profile.folder_selections) {
            if (sel.relative_path == relativePath) {
                sel.selected = (item->checkState(0) == Qt::Checked);
                break;
            }
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

void PerUserCustomizationDialog::addDirectoryContents(const QDir& dir, QTreeWidgetItem* parent, qint64& totalSize, int& totalFiles, bool checked, int depth, int maxDepth) {
    // Depth limit to prevent stack overflow
    if (depth >= maxDepth) {
        return;
    }
    
    // Get all entries (files and directories) with error handling
    QFileInfoList entries;
    try {
        entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::DirsFirst);
    } catch (...) {
        // Skip directories we can't read
        return;
    }
    
    // Limit items per directory to prevent UI slowdown
    const int MAX_ITEMS_PER_DIR = 500;
    int itemCount = 0;
    
    for (const QFileInfo& entry : entries) {
        if (itemCount >= MAX_ITEMS_PER_DIR) {
            // Add a placeholder for remaining items
            QTreeWidgetItem* moreItem = new QTreeWidgetItem(parent);
            moreItem->setText(0, QString("... (%1 more items)").arg(entries.size() - itemCount));
            moreItem->setFlags(Qt::ItemIsEnabled);
            break;
        }
        
        // Skip if we can't read the entry
        if (!entry.isReadable()) {
            continue;
        }
        
        QTreeWidgetItem* childItem = new QTreeWidgetItem(parent);
        
        if (entry.isDir()) {
            // Skip symbolic links to prevent infinite loops
            if (entry.isSymLink()) {
                childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
                childItem->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
                childItem->setText(0, QString("[LINK] %1").arg(entry.fileName()));
                childItem->setData(0, Qt::UserRole + 1, true);
                childItem->setText(1, "-");
                childItem->setText(2, "-");
                itemCount++;
                continue;
            }
            
            // Directory - add tri-state checkbox
            childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            childItem->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
            // Column 0: Folder name with checkbox
            childItem->setText(0, QString("[DIR] %1").arg(entry.fileName()));
            childItem->setData(0, Qt::UserRole + 1, true); // Mark as folder
            
            // Recursively add subdirectory contents
            qint64 subDirSize = 0;
            int subDirFiles = 0;
            QDir subDir(entry.filePath());
            addDirectoryContents(subDir, childItem, subDirSize, subDirFiles, checked, depth + 1, maxDepth);
            
            totalSize += subDirSize;
            totalFiles += subDirFiles;
            
            // Display subdirectory size and file count
            QString sizeStr;
            if (subDirSize > 0) {
                double sizeMB = subDirSize / (1024.0 * 1024.0);
                if (sizeMB >= 1024) {
                    sizeStr = QString("%1 GB").arg(sizeMB / 1024.0, 0, 'f', 2);
                } else if (sizeMB >= 1) {
                    sizeStr = QString("%1 MB").arg(sizeMB, 0, 'f', 1);
                } else {
                    sizeStr = QString("%1 KB").arg(subDirSize / 1024.0, 0, 'f', 1);
                }
            } else {
                sizeStr = "0 KB";
            }
            childItem->setText(1, sizeStr);
            childItem->setText(2, QString::number(subDirFiles));
            
        } else if (entry.isFile()) {
            // File - add regular checkbox
            childItem->setFlags(childItem->flags() | Qt::ItemIsUserCheckable);
            childItem->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
            // Column 0: File name with checkbox
            childItem->setText(0, entry.fileName());
            childItem->setData(0, Qt::UserRole + 1, false); // Mark as file
            
            qint64 fileSize = entry.size();
            totalSize += fileSize;
            totalFiles++;
            
            // Display file size
            QString sizeStr;
            double sizeMB = fileSize / (1024.0 * 1024.0);
            if (sizeMB >= 1024) {
                sizeStr = QString("%1 GB").arg(sizeMB / 1024.0, 0, 'f', 2);
            } else if (sizeMB >= 1) {
                sizeStr = QString("%1 MB").arg(sizeMB, 0, 'f', 1);
            } else if (fileSize >= 1024) {
                sizeStr = QString("%1 KB").arg(fileSize / 1024.0, 0, 'f', 1);
            } else {
                sizeStr = QString("%1 bytes").arg(fileSize);
            }
            childItem->setText(2, sizeStr);
            childItem->setText(3, "-");
        }
    }
}

void PerUserCustomizationDialog::calculateDirectorySize(const QDir& dir, qint64& totalSize, int& fileCount, int depth, int maxDepth) {
    // Prevent excessive recursion
    if (depth >= maxDepth) {
        return;
    }
    
    // Limit total file count for performance
    const int MAX_FILE_COUNT = 50000;
    if (fileCount >= MAX_FILE_COUNT) {
        return;
    }
    
    QFileInfoList entries;
    try {
        entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Readable);
    } catch (...) {
        return;
    }
    
    for (const QFileInfo& entry : entries) {
        if (!entry.isReadable()) {
            continue;
        }
        
        if (entry.isDir() && !entry.isSymLink()) {
            QDir subDir(entry.filePath());
            calculateDirectorySize(subDir, totalSize, fileCount, depth + 1, maxDepth);
        } else if (entry.isFile()) {
            totalSize += entry.size();
            fileCount++;
            
            if (fileCount >= MAX_FILE_COUNT) {
                return;
            }
        }
    }
}

void PerUserCustomizationDialog::setChildrenCheckState(QTreeWidgetItem* item, Qt::CheckState state) {
    if (!item) return;
    
    // Only propagate Checked or Unchecked states (not PartiallyChecked)
    if (state == Qt::PartiallyChecked) return;
    
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        child->setCheckState(0, state);
        setChildrenCheckState(child, state);  // Recursively update all descendants
    }
}

void PerUserCustomizationDialog::updateParentCheckState(QTreeWidgetItem* item) {
    if (!item) return;
    
    int checkedCount = 0;
    int uncheckedCount = 0;
    int childCount = item->childCount();
    
    if (childCount == 0) return;
    
    for (int i = 0; i < childCount; ++i) {
        Qt::CheckState childState = item->child(i)->checkState(0);
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
    if (!item) return;
    
    // Update children when parent changes
    Qt::CheckState parentState = item->checkState(0);
    for (int i = 0; i < item->childCount(); ++i) {
        item->child(i)->setCheckState(0, parentState);
    }
    
    // Update parent based on children
    if (item->parent()) {
        QTreeWidgetItem* parent = item->parent();
        int checkedCount = 0;
        int uncheckedCount = 0;
        
        for (int i = 0; i < parent->childCount(); ++i) {
            Qt::CheckState childState = parent->child(i)->checkState(0);
            if (childState == Qt::Checked) checkedCount++;
            else if (childState == Qt::Unchecked) uncheckedCount++;
        }
        
        if (checkedCount == parent->childCount()) {
            parent->setCheckState(0, Qt::Checked);
        } else if (uncheckedCount == parent->childCount()) {
            parent->setCheckState(0, Qt::Unchecked);
        } else {
            parent->setCheckState(0, Qt::PartiallyChecked);
        }
    }
}

void PerUserCustomizationDialog::updateSummary() {
    qint64 totalSize = calculateTotalSize();
    int selectedCount = 0;
    
    for (const auto& sel : m_profile.folder_selections) {
        if (sel.selected) {
            selectedCount++;
        }
    }
    
    // Add app data items
    int appDataItems = 0;
    if (m_browserBookmarksCheck->isChecked()) appDataItems++;
    if (m_emailSignaturesCheck->isChecked()) appDataItems++;
    if (m_officeTemplatesCheck->isChecked()) appDataItems++;
    if (m_vsCodeSettingsCheck->isChecked()) appDataItems++;
    
    QString summary = QString(
        "<b>Backup Summary:</b> %1 folders selected"
    ).arg(selectedCount);
    
    if (appDataItems > 0) {
        summary += QString(" + %1 app data item(s)").arg(appDataItems);
    }
    
    if (totalSize > 0) {
        double sizeGB = totalSize / (1024.0 * 1024.0 * 1024.0);
        summary += QString(" | Estimated size: <b>%1 GB</b>").arg(sizeGB, 0, 'f', 2);
    }
    
    m_summaryLabel->setText(summary);
}

qint64 PerUserCustomizationDialog::calculateTotalSize() const {
    qint64 total = 0;
    for (const auto& sel : m_profile.folder_selections) {
        if (sel.selected) {
            total += sel.size_bytes;
        }
    }
    return total;
}

QVector<FolderSelection> PerUserCustomizationDialog::getFolderSelections() const {
    // Return updated selections from tree
    QVector<FolderSelection> selections = m_profile.folder_selections;
    
    // Note: selections are already updated in onTreeItemChanged
    // No need to re-sync from tree widget
    
    // Add app data selections as special folder entries
    if (m_browserBookmarksCheck && m_browserBookmarksCheck->isChecked()) {
        FolderSelection bookmarks;
        bookmarks.type = FolderType::AppData_Roaming;
        bookmarks.display_name = "Browser Bookmarks";
        bookmarks.relative_path = "AppData\\Roaming";
        bookmarks.selected = true;
        bookmarks.include_patterns = QStringList{
            "*/Google/Chrome/User Data/*/Bookmarks",
            "*/Microsoft/Edge/User Data/*/Bookmarks",
            "*/Mozilla/Firefox/Profiles/*/places.sqlite"
        };
        selections.append(bookmarks);
    }
    
    if (m_emailSignaturesCheck && m_emailSignaturesCheck->isChecked()) {
        FolderSelection signatures;
        signatures.type = FolderType::AppData_Roaming;
        signatures.display_name = "Email Signatures";
        signatures.relative_path = "AppData\\Roaming";
        signatures.selected = true;
        signatures.include_patterns = QStringList{
            "*/Microsoft/Signatures/*"
        };
        selections.append(signatures);
    }
    
    if (m_officeTemplatesCheck && m_officeTemplatesCheck->isChecked()) {
        FolderSelection templates;
        templates.type = FolderType::AppData_Roaming;
        templates.display_name = "Office Templates";
        templates.relative_path = "AppData\\Roaming";
        templates.selected = true;
        templates.include_patterns = QStringList{
            "*/Microsoft/Templates/*"
        };
        selections.append(templates);
    }
    
    if (m_vsCodeSettingsCheck && m_vsCodeSettingsCheck->isChecked()) {
        FolderSelection vscode;
        vscode.type = FolderType::AppData_Roaming;
        vscode.display_name = "VS Code Settings";
        vscode.relative_path = "AppData\\Roaming";
        vscode.selected = true;
        vscode.include_patterns = QStringList{
            "*/Code/User/settings.json",
            "*/Code/User/keybindings.json",
            "*/Code/User/snippets/*"
        };
        selections.append(vscode);
    }
    
    return selections;
}

} // namespace sak
