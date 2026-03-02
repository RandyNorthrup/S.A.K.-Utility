// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_user_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QApplication>
#include <algorithm>

namespace sak {

// ============================================================================
// Page 2: User Mapping
// ============================================================================

UserProfileRestoreUserMappingPage::UserProfileRestoreUserMappingPage(QWidget* parent)
    : QWizardPage(parent)
    , m_scanner(new WindowsUserScanner(this))
{
    setTitle(tr("Map Users"));
    setSubTitle(tr("Map backup users to destination users on this system"));
    
    setupUi();
}

void UserProfileRestoreUserMappingPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* infoLabel = new QLabel(
        tr("Map each user from the backup to a user on this system. "
           "You can map to an existing user (merge data) or create a new user."),
        this
    );
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);
    
    // Auto-map button
    auto* buttonLayout = new QHBoxLayout();
    m_autoMapButton = new QPushButton(tr("Auto-Map by Username"), this);
    m_autoMapButton->setToolTip(tr("Pairs each backup user to a local account with the same username"));
    buttonLayout->addWidget(m_autoMapButton);
    buttonLayout->addStretch(1);
    layout->addLayout(buttonLayout);
    
    // Mapping table
    m_mappingTable = new QTableWidget(0, 5, this);
    m_mappingTable->setHorizontalHeaderLabels({
        tr("Select"), tr("Source User"), tr("→"), tr("Destination User"), tr("Merge Mode")
    });
    m_mappingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_mappingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mappingTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_mappingTable);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { background-color: %1; padding: 10px; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
    
    // Connections
    connect(m_autoMapButton, &QPushButton::clicked, this, &UserProfileRestoreUserMappingPage::onAutoMap);
    connect(m_mappingTable, &QTableWidget::cellChanged, this, &UserProfileRestoreUserMappingPage::onMappingChanged);
}

void UserProfileRestoreUserMappingPage::initializePage() {
    // Scan destination users
    m_destinationUsers = m_scanner->scanUsers();
    
    loadMappingTable();
    updateSummary();
}

void UserProfileRestoreUserMappingPage::loadMappingTable() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return;
    
    BackupManifest manifest = wiz->manifest();
    
    m_mappingTable->setRowCount(0);
    
    for (const auto& backupUser : manifest.users) {
        int row = m_mappingTable->rowCount();
        m_mappingTable->insertRow(row);
        
        // Checkbox
        auto* checkItem = new QTableWidgetItem();
        checkItem->setCheckState(Qt::Checked);
        m_mappingTable->setItem(row, 0, checkItem);
        
        // Source user
        auto* sourceItem = new QTableWidgetItem(backupUser.username);
        sourceItem->setFlags(sourceItem->flags() & ~Qt::ItemIsEditable);
        m_mappingTable->setItem(row, 1, sourceItem);
        
        // Arrow
        auto* arrowItem = new QTableWidgetItem("→");
        arrowItem->setFlags(arrowItem->flags() & ~Qt::ItemIsEditable);
        arrowItem->setTextAlignment(Qt::AlignCenter);
        m_mappingTable->setItem(row, 2, arrowItem);
        
        // Destination user combo
        auto* destCombo = new QComboBox();
        destCombo->addItem(tr("(Create New User)"), QString());
        for (const auto& destUser : m_destinationUsers) {
            destCombo->addItem(destUser.username, destUser.username);
        }
        m_mappingTable->setCellWidget(row, 3, destCombo);
        
        // Merge mode combo
        auto* modeCombo = new QComboBox();
        modeCombo->addItem(tr("Replace Destination"), static_cast<int>(MergeMode::ReplaceDestination));
        modeCombo->addItem(tr("Merge Into Destination"), static_cast<int>(MergeMode::MergeIntoDestination));
        modeCombo->addItem(tr("Create New User"), static_cast<int>(MergeMode::CreateNewUser));
        m_mappingTable->setCellWidget(row, 4, modeCombo);
        
        // Connect signals
        connect(destCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, row](int) { onMappingChanged(row, 3); });
        connect(modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, row](int) { onMappingChanged(row, 4); });
    }
}

void UserProfileRestoreUserMappingPage::onAutoMap() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return;
    
    BackupManifest manifest = wiz->manifest();
    
    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
        if (!destCombo) continue;
        
        QString sourceUsername = m_mappingTable->item(row, 1)->text();
        int matchIndex = destCombo->findText(sourceUsername);
        if (matchIndex >= 0) {
            destCombo->setCurrentIndex(matchIndex);
        }
    }
    
    updateSummary();
}

void UserProfileRestoreUserMappingPage::onMappingChanged(int row, int column) {
    Q_UNUSED(row);
    Q_UNUSED(column);
    updateSummary();
}

void UserProfileRestoreUserMappingPage::updateSummary() {
    int totalMappings = m_mappingTable->rowCount();
    int selectedMappings = 0;
    int newUsers = 0;
    int merges = 0;
    
    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        if (m_mappingTable->item(row, 0)->checkState() != Qt::Checked) {
            continue;
        }
        selectedMappings++;
        
        auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
        if (destCombo && destCombo->currentData().toString().isEmpty()) {
            newUsers++;
        } else {
            merges++;
        }
    }
    
    m_summaryLabel->setText(
        tr("Summary: %1 of %2 users selected | %3 merges, %4 new users")
            .arg(selectedMappings)
            .arg(totalMappings)
            .arg(merges)
            .arg(newUsers)
    );
}

UserMapping UserProfileRestoreUserMappingPage::buildMappingForRow(int row, const BackupManifest& manifest) const {
    UserMapping mapping;
    mapping.source_username = m_mappingTable->item(row, 1)->text();
    mapping.selected = true;
    
    // Find source SID
    auto srcIt = std::find_if(manifest.users.begin(), manifest.users.end(),
        [&](const auto& u) { return u.username == mapping.source_username; });
    if (srcIt != manifest.users.end()) {
        mapping.source_sid = srcIt->sid;
    }
    
    // Get destination user
    auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
    if (destCombo) {
        mapping.destination_username = destCombo->currentData().toString();
    }
    
    // Find destination SID
    if (!mapping.destination_username.isEmpty()) {
        auto destIt = std::find_if(m_destinationUsers.begin(), m_destinationUsers.end(),
            [&](const auto& u) { return u.username == mapping.destination_username; });
        if (destIt != m_destinationUsers.end()) {
            mapping.destination_sid = destIt->sid;
        }
    }
    
    // Get merge mode
    auto* modeCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 4));
    if (modeCombo) {
        mapping.mode = static_cast<MergeMode>(modeCombo->currentData().toInt());
    }
    
    return mapping;
}

bool UserProfileRestoreUserMappingPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return false;
    
    BackupManifest manifest = wiz->manifest();
    QVector<UserMapping> mappings;
    
    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        if (m_mappingTable->item(row, 0)->checkState() != Qt::Checked) {
            continue;
        }
        mappings.append(buildMappingForRow(row, manifest));
    }
    
    if (mappings.isEmpty()) {
        QMessageBox::warning(this, tr("No Users Selected"),
            tr("Please select at least one user to restore."));
        return false;
    }
    
    wiz->setUserMappings(mappings);
    return true;
}

// ============================================================================
// Page 3: Merge Configuration
// ============================================================================

UserProfileRestoreMergeConfigPage::UserProfileRestoreMergeConfigPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Merge Configuration"));
    setSubTitle(tr("Configure how to merge data for each user"));
    
    setupUi();
}

void UserProfileRestoreMergeConfigPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* infoLabel = new QLabel(
        tr("For each user mapping, configure the merge behavior and conflict resolution."),
        this
    );
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);
    
    // Merge table
    m_mergeTable = new QTableWidget(0, 4, this);
    m_mergeTable->setHorizontalHeaderLabels({
        tr("Source → Destination"), tr("Merge Mode"), tr("Conflict Resolution"), tr("Status")
    });
    m_mergeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_mergeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mergeTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_mergeTable);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { background-color: %1; padding: 10px; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
    
    // Connections
    connect(m_mergeTable, &QTableWidget::cellChanged, this, &UserProfileRestoreMergeConfigPage::onMergeSettingsChanged);
}

void UserProfileRestoreMergeConfigPage::initializePage() {
    loadMergeTable();
    updateSummary();
}

void UserProfileRestoreMergeConfigPage::loadMergeTable() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return;
    
    QVector<UserMapping> mappings = wiz->userMappings();
    
    m_mergeTable->setRowCount(0);
    
    for (const auto& mapping : mappings) {
        int row = m_mergeTable->rowCount();
        m_mergeTable->insertRow(row);
        
        // Mapping info
        QString mappingText = mapping.destination_username.isEmpty()
            ? tr("%1 → (New User)").arg(mapping.source_username)
            : tr("%1 → %2").arg(mapping.source_username, mapping.destination_username);
        
        auto* mappingItem = new QTableWidgetItem(mappingText);
        mappingItem->setFlags(mappingItem->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, 0, mappingItem);
        
        // Merge mode (read-only display)
        auto* modeItem = new QTableWidgetItem(mergeModeToString(mapping.mode));
        modeItem->setFlags(modeItem->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, 1, modeItem);
        
        // Conflict resolution combo
        auto* conflictCombo = new QComboBox();
        conflictCombo->addItem(tr("Skip Duplicate"), static_cast<int>(ConflictResolution::SkipDuplicate));
        conflictCombo->addItem(tr("Rename with Suffix"), static_cast<int>(ConflictResolution::RenameWithSuffix));
        conflictCombo->addItem(tr("Keep Newer"), static_cast<int>(ConflictResolution::KeepNewer));
        conflictCombo->addItem(tr("Keep Larger"), static_cast<int>(ConflictResolution::KeepLarger));
        conflictCombo->addItem(tr("Prompt User"), static_cast<int>(ConflictResolution::PromptUser));
        conflictCombo->setCurrentIndex(1); // Default to RenameWithSuffix
        m_mergeTable->setCellWidget(row, 2, conflictCombo);
        
        // Status
        auto* statusItem = new QTableWidgetItem(tr("Ready"));
        statusItem->setFlags(statusItem->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, 3, statusItem);
        
        // Connect signals
        connect(conflictCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this, row](int) { onMergeSettingsChanged(row, 2); });
    }
}

void UserProfileRestoreMergeConfigPage::onMergeSettingsChanged(int row, int column) {
    Q_UNUSED(row);
    Q_UNUSED(column);
    updateSummary();
}

void UserProfileRestoreMergeConfigPage::updateSummary() {
    int replaceCount = 0;
    int mergeCount = 0;
    int newCount = 0;
    
    for (int row = 0; row < m_mergeTable->rowCount(); ++row) {
        QString modeText = m_mergeTable->item(row, 1)->text();
        
        if (modeText.contains("Replace", Qt::CaseInsensitive)) {
            replaceCount++;
        } else if (modeText.contains("Merge", Qt::CaseInsensitive)) {
            mergeCount++;
        } else if (modeText.contains("New", Qt::CaseInsensitive)) {
            newCount++;
        }
    }
    
    m_summaryLabel->setText(
        tr("Operations: %1 replace, %2 merge, %3 new users")
            .arg(replaceCount)
            .arg(mergeCount)
            .arg(newCount)
    );
}

bool UserProfileRestoreMergeConfigPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return false;
    
    // Update mappings with conflict resolution settings
    QVector<UserMapping> mappings = wiz->userMappings();
    
    for (int row = 0; row < m_mergeTable->rowCount() && row < mappings.size(); ++row) {
        auto* conflictCombo = qobject_cast<QComboBox*>(m_mergeTable->cellWidget(row, 2));
        if (conflictCombo) {
            mappings[row].conflict_resolution = static_cast<ConflictResolution>(conflictCombo->currentData().toInt());
        }
    }
    
    wiz->setUserMappings(mappings);
    return true;
}

// ============================================================================
// Page 4: Folder Selection
// ============================================================================

UserProfileRestoreFolderSelectionPage::UserProfileRestoreFolderSelectionPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Select Folders"));
    setSubTitle(tr("Choose which folders to restore for each user"));
    
    setupUi();
}

void UserProfileRestoreFolderSelectionPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* infoLabel = new QLabel(
        tr("Select the folders you want to restore. Uncheck folders to skip them."),
        this
    );
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);
    
    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    buttonLayout->addWidget(m_selectAllButton);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch(1);
    layout->addLayout(buttonLayout);
    
    // Folder table
    m_folderTable = new QTableWidget(0, 5, this);
    m_folderTable->setHorizontalHeaderLabels({
        tr("Select"), tr("User"), tr("Folder"), tr("Size"), tr("Files")
    });
    m_folderTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_folderTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_folderTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_folderTable);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { background-color: %1; padding: 10px; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
    
    // Connections
    connect(m_selectAllButton, &QPushButton::clicked, this, &UserProfileRestoreFolderSelectionPage::onSelectAll);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &UserProfileRestoreFolderSelectionPage::onSelectNone);
    connect(m_folderTable, &QTableWidget::cellChanged, this, &UserProfileRestoreFolderSelectionPage::onFolderSelectionChanged);
}

void UserProfileRestoreFolderSelectionPage::initializePage() {
    loadFolderTable();
    updateSummary();
}

void UserProfileRestoreFolderSelectionPage::loadFolderTable() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return;
    
    BackupManifest manifest = wiz->manifest();
    QVector<UserMapping> mappings = wiz->userMappings();
    
    m_folderTable->setRowCount(0);
    
    for (const auto& mapping : mappings) {
        auto userIt = std::find_if(manifest.users.begin(), manifest.users.end(),
            [&](const auto& u) { return u.username == mapping.source_username; });
        if (userIt == manifest.users.end()) continue;
        
        for (const auto& folder : userIt->backed_up_folders) {
            int row = m_folderTable->rowCount();
            m_folderTable->insertRow(row);
            
            // Checkbox
            auto* checkItem = new QTableWidgetItem();
            checkItem->setCheckState(Qt::Checked);
            m_folderTable->setItem(row, 0, checkItem);
            
            // User
            auto* userItem = new QTableWidgetItem(mapping.source_username);
            userItem->setFlags(userItem->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, 1, userItem);
            
            // Folder name
            auto* folderItem = new QTableWidgetItem(folder.display_name);
            folderItem->setFlags(folderItem->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, 2, folderItem);
            
            // Size
            double sizeMB = folder.size_bytes / sak::kBytesPerMBf;
            auto* sizeItem = new QTableWidgetItem(QString("%1 MB").arg(sizeMB, 0, 'f', 1));
            sizeItem->setFlags(sizeItem->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, 3, sizeItem);
            
            // File count
            auto* filesItem = new QTableWidgetItem(QString::number(folder.file_count));
            filesItem->setFlags(filesItem->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, 4, filesItem);
        }
    }
}

void UserProfileRestoreFolderSelectionPage::onSelectAll() {
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        m_folderTable->item(row, 0)->setCheckState(Qt::Checked);
    }
    updateSummary();
}

void UserProfileRestoreFolderSelectionPage::onSelectNone() {
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        m_folderTable->item(row, 0)->setCheckState(Qt::Unchecked);
    }
    updateSummary();
}

void UserProfileRestoreFolderSelectionPage::onFolderSelectionChanged(int row, int column) {
    Q_UNUSED(row);
    Q_UNUSED(column);
    updateSummary();
}

void UserProfileRestoreFolderSelectionPage::updateSummary() {
    int totalFolders = m_folderTable->rowCount();
    int selectedFolders = 0;
    qint64 totalSize = 0;
    int totalFiles = 0;
    
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        if (m_folderTable->item(row, 0)->checkState() == Qt::Checked) {
            selectedFolders++;
            
            QString sizeText = m_folderTable->item(row, 3)->text();
            sizeText.remove(" MB");
            totalSize += static_cast<qint64>(sizeText.toDouble() * sak::kBytesPerMB);
            
            totalFiles += m_folderTable->item(row, 4)->text().toInt();
        }
    }
    
    double totalGB = totalSize / sak::kBytesPerGBf;
    
    m_summaryLabel->setText(
        tr("Selected: %1 of %2 folders | %3 files | %4 GB")
            .arg(selectedFolders)
            .arg(totalFolders)
            .arg(totalFiles)
            .arg(totalGB, 0, 'f', 2)
    );
}

bool UserProfileRestoreFolderSelectionPage::validatePage() {
    // Check if at least one folder is selected
    int selectedCount = 0;
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        if (m_folderTable->item(row, 0)->checkState() == Qt::Checked) {
            selectedCount++;
        }
    }
    
    if (selectedCount == 0) {
        QMessageBox::warning(this, tr("No Folders Selected"),
            tr("Please select at least one folder to restore."));
        return false;
    }
    
    // Note: Folder selection is applied during restore worker initialization
    return true;
}

// ============================================================================
// Page 5: Permission Settings
// ============================================================================

UserProfileRestorePermissionSettingsPage::UserProfileRestorePermissionSettingsPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Permission & Verification"));
    setSubTitle(tr("Configure permission handling and verification options"));
    
    setupUi();
}

void UserProfileRestorePermissionSettingsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Permission mode
    auto* permGroup = new QWidget(this);
    auto* permLayout = new QGridLayout(permGroup);
    permLayout->setContentsMargins(0, 0, 0, 0);
    
    permLayout->addWidget(new QLabel(tr("Permission Mode:"), permGroup), 0, 0);
    m_permissionModeCombo = new QComboBox(permGroup);
    m_permissionModeCombo->addItem(tr("Strip All ACLs (Safest)"), static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Assign to Destination User"), static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Preserve Original"), static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Hybrid (Safe + Assign)"), static_cast<int>(PermissionMode::Hybrid));
    permLayout->addWidget(m_permissionModeCombo, 0, 1);
    
    layout->addWidget(permGroup);
    
    // Conflict resolution
    auto* conflictGroup = new QWidget(this);
    auto* conflictLayout = new QGridLayout(conflictGroup);
    conflictLayout->setContentsMargins(0, 0, 0, 0);
    
    conflictLayout->addWidget(new QLabel(tr("Conflict Resolution:"), conflictGroup), 0, 0);
    m_conflictResolutionCombo = new QComboBox(conflictGroup);
    m_conflictResolutionCombo->addItem(tr("Skip Duplicate"), static_cast<int>(ConflictResolution::SkipDuplicate));
    m_conflictResolutionCombo->addItem(tr("Rename with Suffix"), static_cast<int>(ConflictResolution::RenameWithSuffix));
    m_conflictResolutionCombo->addItem(tr("Keep Newer"), static_cast<int>(ConflictResolution::KeepNewer));
    m_conflictResolutionCombo->addItem(tr("Keep Larger"), static_cast<int>(ConflictResolution::KeepLarger));
    m_conflictResolutionCombo->setCurrentIndex(1); // Default to RenameWithSuffix
    conflictLayout->addWidget(m_conflictResolutionCombo, 0, 1);
    
    layout->addWidget(conflictGroup);
    
    layout->addSpacing(20);
    
    // Options
    m_verifyCheckBox = new QCheckBox(tr("Verify file integrity after restore"), this);
    m_verifyCheckBox->setChecked(true);
    layout->addWidget(m_verifyCheckBox);
    
    m_createBackupCheckBox = new QCheckBox(tr("Create backup of existing files before overwriting"), this);
    m_createBackupCheckBox->setChecked(false);
    layout->addWidget(m_createBackupCheckBox);
    
    layout->addSpacing(20);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { background-color: %1; padding: 12px; border-radius: 10px; }").arg(sak::ui::kColorBgWarningPanel));
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);
    
    layout->addStretch(1);
    
    // Connections
    connect(m_permissionModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    connect(m_conflictResolutionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    connect(m_verifyCheckBox, &QCheckBox::stateChanged,
            this, &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    connect(m_createBackupCheckBox, &QCheckBox::stateChanged,
            this, &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    
    onSettingsChanged();
}

void UserProfileRestorePermissionSettingsPage::onSettingsChanged() {
    updateSummary();
}

void UserProfileRestorePermissionSettingsPage::updateSummary() {
    QString permMode = m_permissionModeCombo->currentText();
    QString conflictMode = m_conflictResolutionCombo->currentText();
    
    QString warning;
    PermissionMode mode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());
    
    if (mode == PermissionMode::PreserveOriginal) {
        warning = tr("⚠️ <b>Warning:</b> Preserving original permissions may cause access issues if SIDs don't match.");
    } else if (mode == PermissionMode::StripAll) {
        warning = tr("✅ <b>Recommended:</b> Stripping ACLs ensures files inherit safe permissions from parent folders.");
    } else if (mode == PermissionMode::AssignToDestination) {
        warning = tr("ℹ️ <b>Info:</b> Files will be owned by the destination user.");
    }
    
    QString summary = QString(
        "<b>Configuration Summary:</b><br>"
        "• Permission Mode: %1<br>"
        "• Conflict Resolution: %2<br>"
        "• Verify Integrity: %3<br>"
        "• Backup Existing: %4<br><br>"
        "%5"
    ).arg(
        permMode,
        conflictMode,
        m_verifyCheckBox->isChecked() ? tr("Yes") : tr("No"),
        m_createBackupCheckBox->isChecked() ? tr("Yes") : tr("No"),
        warning
    );
    
    m_summaryLabel->setText(summary);
}

bool UserProfileRestorePermissionSettingsPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return false;
    
    wiz->setPermissionMode(static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt()));
    wiz->setConflictResolution(static_cast<ConflictResolution>(m_conflictResolutionCombo->currentData().toInt()));
    wiz->setVerifyFiles(m_verifyCheckBox->isChecked());
    wiz->setCreateBackup(m_createBackupCheckBox->isChecked());
    
    return true;
}

// ============================================================================
// Page 5: App Restore
// ============================================================================

UserProfileRestoreAppRestorePage::UserProfileRestoreAppRestorePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Restore Applications"));
    setSubTitle(tr("Install applications that were saved during backup (optional)"));

    setupUi();
}

void UserProfileRestoreAppRestorePage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "The backup contains a list of installed applications. "
        "Select the ones you want to install on this machine via Chocolatey. "
        "You can also skip this step by clicking Next."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);

    // Status
    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    // Tree widget
    m_appTree = new QTreeWidget(this);
    m_appTree->setHeaderLabels({tr("Application"), tr("Version"), tr("Chocolatey Package")});
    m_appTree->setAlternatingRowColors(true);
    m_appTree->setRootIsDecorated(true);
    m_appTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_appTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_appTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_appTree->setEnabled(false);
    connect(m_appTree, &QTreeWidget::itemChanged,
            this, &UserProfileRestoreAppRestorePage::onItemChanged);
    layout->addWidget(m_appTree);

    // Selection buttons
    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this, &UserProfileRestoreAppRestorePage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &UserProfileRestoreAppRestorePage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);

    buttonLayout->addStretch();

    m_installButton = new QPushButton(tr("Install Selected Apps"), this);
    m_installButton->setEnabled(false);
    m_installButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_installButton, &QPushButton::clicked, this, &UserProfileRestoreAppRestorePage::onInstallApps);
    buttonLayout->addWidget(m_installButton);

    layout->addLayout(buttonLayout);

    // Progress
    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    layout->addWidget(m_progressBar);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No application data found in backup"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileRestoreAppRestorePage::initializePage() {
    if (!m_loaded) {
        loadApps();
    }
}

bool UserProfileRestoreAppRestorePage::isComplete() const {
    // Always complete — app installation is optional
    return !m_installing;
}

void UserProfileRestoreAppRestorePage::loadApps() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return;

    QString appsFilePath = wiz->backupPath() + "/installed_apps.json";
    QFile file(appsFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        m_statusLabel->setText(tr("No installed_apps.json found in backup — skipping app restore"));
        m_summaryLabel->setText(tr("No application data available in this backup"));
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) {
        m_statusLabel->setText(tr("Invalid installed_apps.json format"));
        return;
    }

    QJsonArray appsArray = doc.array();
    m_apps.clear();
    m_apps.reserve(appsArray.size());

    for (const auto& val : appsArray) {
        QJsonObject obj = val.toObject();
        RestoreAppInfo info;
        info.name = obj["name"].toString();
        info.version = obj["version"].toString();
        info.publisher = obj["publisher"].toString();
        info.choco_package = obj["choco_package"].toString();
        info.category = obj["category"].toString();
        info.selected = !info.choco_package.isEmpty();
        m_apps.append(info);
    }

    m_loaded = true;

    if (m_apps.isEmpty()) {
        m_statusLabel->setText(tr("No applications found in backup"));
        m_summaryLabel->setText(tr("The backup does not contain any application data"));
        return;
    }

    m_statusLabel->setText(tr("Found %1 application(s) in backup").arg(m_apps.size()));
    m_appTree->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_installButton->setEnabled(true);

    populateTree(m_apps);
}

/// @brief Count enabled apps in a single category for summary
static QPair<int,int> countEnabledCategoryApps(QTreeWidgetItem* category) {
    int total = 0, selected = 0;
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        auto* item = category->child(child_index);
        if (!(item->flags() & Qt::ItemIsEnabled)) continue;
        total++;
        if (item->checkState(0) == Qt::Checked) selected++;
    }
    return {total, selected};
}

int UserProfileRestoreAppRestorePage::populateCategoryApps(
        QTreeWidgetItem* categoryItem,
        const QVector<const RestoreAppInfo*>& apps,
        int& totalWithPackage) {
    int catSelected = 0;
    for (const auto* app : apps) {
        auto* appItem = new QTreeWidgetItem(categoryItem);
        appItem->setText(0, app->name);
        appItem->setText(1, app->version);
        appItem->setText(2, app->choco_package.isEmpty() ? tr("(no match)") : app->choco_package);
        appItem->setFlags(appItem->flags() | Qt::ItemIsUserCheckable);

        if (app->choco_package.isEmpty()) {
            appItem->setCheckState(0, Qt::Unchecked);
            appItem->setForeground(2, QBrush(Qt::gray));
            appItem->setFlags(appItem->flags() & ~Qt::ItemIsEnabled);
            continue;
        }

        appItem->setCheckState(0, app->selected ? Qt::Checked : Qt::Unchecked);
        totalWithPackage++;
        catSelected += app->selected ? 1 : 0;
    }
    return catSelected;
}

void UserProfileRestoreAppRestorePage::populateTree(const QVector<RestoreAppInfo>& apps) {
    m_appTree->blockSignals(true);
    m_appTree->clear();

    // Group by category
    QMap<QString, QVector<const RestoreAppInfo*>> categories;
    for (const auto& app : apps) {
        QString cat = app.category.isEmpty() ? tr("Other") : app.category;
        categories[cat].append(&app);
    }

    int totalSelected = 0;
    int totalWithPackage = 0;

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* categoryItem = new QTreeWidgetItem(m_appTree);
        categoryItem->setText(0, it.key());
        categoryItem->setFlags(categoryItem->flags() | Qt::ItemIsUserCheckable);

        int catSelected = populateCategoryApps(categoryItem, it.value(), totalWithPackage);
        totalSelected += catSelected;

        // Set parent check state
        if (catSelected == 0) {
            categoryItem->setCheckState(0, Qt::Unchecked);
        } else if (catSelected == it.value().size()) {
            categoryItem->setCheckState(0, Qt::Checked);
        } else {
            categoryItem->setCheckState(0, Qt::PartiallyChecked);
        }

        categoryItem->setExpanded(true);
    }

    m_appTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 application(s) selected for installation (%2 have Chocolatey packages)")
                                .arg(totalSelected).arg(totalWithPackage));
}

void UserProfileRestoreAppRestorePage::onItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;

    m_appTree->blockSignals(true);

    if (item->childCount() > 0) {
        // Parent item — propagate to enabled children
        Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            auto* child = item->child(i);
            if (!(child->flags() & Qt::ItemIsEnabled)) continue;
            child->setCheckState(0, state);
        }
    } else if (item->parent()) {
        updateParentCheckState(item->parent());
    }

    m_appTree->blockSignals(false);

    // Count selected
    int total = 0;
    int selected = 0;
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto [t, s] = countEnabledCategoryApps(m_appTree->topLevelItem(category_index));
        total += t;
        selected += s;
    }

    m_summaryLabel->setText(tr("%1 application(s) selected for installation out of %2 available")
                                .arg(selected).arg(total));
}

void UserProfileRestoreAppRestorePage::updateParentCheckState(QTreeWidgetItem* parent) {
    int checkedCount = 0;
    int enabledCount = 0;

    for (int i = 0; i < parent->childCount(); ++i) {
        auto* child = parent->child(i);
        if (!(child->flags() & Qt::ItemIsEnabled)) continue;
        enabledCount++;
        if (child->checkState(0) == Qt::Checked) checkedCount++;
    }

    if (enabledCount == 0 || checkedCount == 0) {
        parent->setCheckState(0, Qt::Unchecked);
    } else if (checkedCount == enabledCount) {
        parent->setCheckState(0, Qt::Checked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void UserProfileRestoreAppRestorePage::onSelectAll() {
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* child = category->child(child_index);
            if (!(child->flags() & Qt::ItemIsEnabled)) continue;
            child->setCheckState(0, Qt::Checked);
        }
        updateParentCheckState(category);
    }
    m_appTree->blockSignals(false);

    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

void UserProfileRestoreAppRestorePage::onSelectNone() {
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* child = category->child(child_index);
            if (!(child->flags() & Qt::ItemIsEnabled)) continue;
            child->setCheckState(0, Qt::Unchecked);
        }
        category->setCheckState(0, Qt::Unchecked);
    }
    m_appTree->blockSignals(false);

    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

void UserProfileRestoreAppRestorePage::onInstallApps() {
    // Collect selected apps
    QVector<RestoreAppInfo> selectedApps = collectSelectedApps();

    if (selectedApps.isEmpty()) {
        QMessageBox::information(this, tr("No Apps Selected"),
            tr("Please select at least one application to install."));
        return;
    }

    // Disable controls during install
    m_installing = true;
    m_installButton->setEnabled(false);
    m_selectAllButton->setEnabled(false);
    m_selectNoneButton->setEnabled(false);
    m_appTree->setEnabled(false);
    Q_EMIT completeChanged();

    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, selectedApps.size());
    m_progressBar->setValue(0);

    // Initialize Chocolatey and install
    auto [installed, failed] = installAppsSequentially(selectedApps);

    // Done
    m_installing = false;
    m_installButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_appTree->setEnabled(true);
    Q_EMIT completeChanged();

    m_statusLabel->setText(tr("Installation complete: %1 succeeded, %2 failed")
                                .arg(installed).arg(failed));
    m_summaryLabel->setText(tr("App installation finished — %1 installed, %2 failed. Click Next to continue.")
                                .arg(installed).arg(failed));
}

QVector<RestoreAppInfo> UserProfileRestoreAppRestorePage::collectSelectedApps() const {
    QVector<RestoreAppInfo> selectedApps;
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* appItem = category->child(child_index);
            if ((appItem->flags() & Qt::ItemIsEnabled) && appItem->checkState(0) == Qt::Checked) {
                RestoreAppInfo info;
                info.name = appItem->text(0);
                info.choco_package = appItem->text(2);
                selectedApps.append(info);
            }
        }
    }
    return selectedApps;
}

QPair<int,int> UserProfileRestoreAppRestorePage::installAppsSequentially(const QVector<RestoreAppInfo>& apps) {
    auto chocoManager = std::make_shared<ChocolateyManager>();
    QString chocoPath = QApplication::applicationDirPath() + "/tools/chocolatey";
    chocoManager->initialize(chocoPath);

    int installed = 0;
    int failed = 0;

    for (int i = 0; i < apps.size(); ++i) {
        const auto& app = apps[i];
        m_statusLabel->setText(tr("Installing %1 (%2/%3)...")
                                    .arg(app.name)
                                    .arg(i + 1)
                                    .arg(apps.size()));
        QApplication::processEvents();

        ChocolateyManager::InstallConfig config;
        config.package_name = app.choco_package;
        config.auto_confirm = true;

        auto result = chocoManager->installPackage(config);
        if (result.success) {
            installed++;
        } else {
            failed++;
        }

        m_progressBar->setValue(i + 1);
        QApplication::processEvents();
    }

    return {installed, failed};
}

} // namespace sak
