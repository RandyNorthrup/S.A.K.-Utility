// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/chocolatey_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"
#include "sak/user_profile_restore_selection.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_user_scanner.h"

#include <QApplication>
#include <QFile>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QtConcurrent>
#include <QVBoxLayout>

#include <algorithm>

namespace sak {
namespace {
enum MappingColumn {
    KMappingColumnSelect,
    KMappingColumnSource,
    KMappingColumnArrow,
    KMappingColumnDestination,
    KMappingColumnMergeMode,
    KMappingColumnCount,
};

enum MergeColumn {
    KMergeColumnSelect,
    KMergeColumnFile,
    KMergeColumnConflict,
    KMergeColumnStatus,
    KMergeColumnCount,
};

enum FolderColumn {
    KFolderColumnSelect,
    KFolderColumnPath,
    KFolderColumnFolder,
    KFolderColumnSize,
    KFolderColumnFiles,
    KFolderColumnCount,
};

enum EthernetColumn {
    KEthernetColumnSelect,
    KEthernetColumnAdapter,
    KEthernetColumnDhcp,
    KEthernetColumnIpAddress,
    KEthernetColumnSubnet,
    KEthernetColumnGateway,
    KEthernetColumnDns,
    KEthernetColumnCount,
};

constexpr int kTreePackageColumn = 2;
constexpr int kTreeDetailColumn = 2;
constexpr int kSizeDisplayPrecision = 2;
constexpr int kMegabyteDisplayPrecision = 1;

// Select the merge-mode combo entry whose data equals `mode` (no-op if absent).
void setMergeModeValue(QComboBox* mode_combo, MergeMode mode) {
    const int index = mode_combo->findData(static_cast<int>(mode));
    if (index >= 0) {
        mode_combo->setCurrentIndex(index);
    }
}

// Keep the merge mode consistent with the destination selection (R5-P11-17): the
// "(Create New User)" entry (empty data) can only pair with CreateNewUser, and an
// existing destination can only pair with a replace/merge mode. This overrides only an
// incompatible mode, so it never blocks a legitimate manual Replace-vs-Merge choice.
void syncMergeModeToDestination(QComboBox* dest_combo, QComboBox* mode_combo) {
    if ((dest_combo == nullptr) || (mode_combo == nullptr)) {
        return;
    }
    const bool create_new = dest_combo->currentData().toString().isEmpty();
    const auto current_mode = static_cast<MergeMode>(mode_combo->currentData().toInt());
    if (create_new) {
        setMergeModeValue(mode_combo, MergeMode::CreateNewUser);
    } else if (current_mode == MergeMode::CreateNewUser) {
        setMergeModeValue(mode_combo, MergeMode::ReplaceDestination);
    }
}

// A new-user destination requires CreateNewUser; an existing destination forbids it.
bool isMappingPairValid(const QString& dest_username, MergeMode mode) {
    if (dest_username.isEmpty()) {
        return mode == MergeMode::CreateNewUser;
    }
    return mode != MergeMode::CreateNewUser;
}
}  // namespace

// ============================================================================
// Page 2: User Mapping
// ============================================================================

UserProfileRestoreUserMappingPage::UserProfileRestoreUserMappingPage(QWidget* parent)
    : QWizardPage(parent), m_scanner(new WindowsUserScanner(this)) {
    setTitle(tr("Map Users"));
    setSubTitle(tr("Map backup users to destination users on this system"));

    setupUi();
}

void UserProfileRestoreUserMappingPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* info_label =
        new QLabel(tr("Map each user from the backup to a user on this system. "
                      "You can map to an existing user (merge data) or create a new user."),
                   this);
    info_label->setWordWrap(true);
    layout->addWidget(info_label);

    // Auto-map button
    auto* button_layout = new QHBoxLayout();
    m_autoMapButton = new QPushButton(tr("Auto-Map by Username"), this);
    m_autoMapButton->setToolTip(
        tr("Pairs each backup user to a local account with the same "
           "username"));
    m_autoMapButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_autoMapButton);
    button_layout->addStretch(1);
    layout->addLayout(button_layout);

    // Mapping table
    m_mappingTable = new QTableWidget(0, KMappingColumnCount, this);
    m_mappingTable->setHorizontalHeaderLabels(
        {tr("Select"), tr("Source User"), tr("->"), tr("Destination User"), tr("Merge Mode")});
    m_mappingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_mappingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mappingTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_mappingTable);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);

    // Connections
    connect(m_autoMapButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreUserMappingPage::onAutoMap);
    connect(m_mappingTable,
            &QTableWidget::cellChanged,
            this,
            &UserProfileRestoreUserMappingPage::onMappingChanged);

    Q_ASSERT(m_autoMapButton);
}

void UserProfileRestoreUserMappingPage::initializePage() {
    // Scan destination users
    m_destinationUsers = m_scanner->scanUsers();

    loadMappingTable();
    updateSummary();
}

void UserProfileRestoreUserMappingPage::loadMappingTable() {
    Q_ASSERT(m_mappingTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    const BackupManifest manifest = wiz->manifest();

    m_mappingTable->setRowCount(0);

    for (const auto& backup_user : manifest.users) {
        const int row = m_mappingTable->rowCount();
        m_mappingTable->insertRow(row);

        // Checkbox
        auto* check_item = new QTableWidgetItem();
        check_item->setCheckState(Qt::Checked);
        m_mappingTable->setItem(row, 0, check_item);

        // Source user
        auto* source_item = new QTableWidgetItem(backup_user.username);
        source_item->setFlags(source_item->flags() & ~Qt::ItemIsEditable);
        m_mappingTable->setItem(row, 1, source_item);

        // Arrow
        auto* arrow_item = new QTableWidgetItem("->");
        arrow_item->setFlags(arrow_item->flags() & ~Qt::ItemIsEditable);
        arrow_item->setTextAlignment(Qt::AlignCenter);
        m_mappingTable->setItem(row, KMappingColumnArrow, arrow_item);

        // Destination user combo
        auto* dest_combo = new QComboBox();
        dest_combo->addItem(tr("(Create New User)"), QString());
        for (const auto& dest_user : m_destinationUsers) {
            dest_combo->addItem(dest_user.username, dest_user.username);
        }
        m_mappingTable->setCellWidget(row, KMappingColumnDestination, dest_combo);

        // Merge mode combo
        auto* mode_combo = new QComboBox();
        mode_combo->addItem(tr("Replace Destination"),
                            static_cast<int>(MergeMode::ReplaceDestination));
        mode_combo->addItem(tr("Merge Into Destination"),
                            static_cast<int>(MergeMode::MergeIntoDestination));
        mode_combo->addItem(tr("Create New User"), static_cast<int>(MergeMode::CreateNewUser));
        m_mappingTable->setCellWidget(row, KMappingColumnMergeMode, mode_combo);

        // Destination defaults to "(Create New User)" (empty data), so start the merge mode on
        // CreateNewUser to keep the dest/mode pair consistent for a plain click-through.
        syncMergeModeToDestination(dest_combo, mode_combo);

        // Connect signals
        connect(dest_combo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this,
                [this, row](int) { onMappingChanged(row, KMappingColumnDestination); });
        connect(mode_combo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this,
                [this, row](int) { onMappingChanged(row, KMappingColumnMergeMode); });
    }
}

void UserProfileRestoreUserMappingPage::onAutoMap() {
    Q_ASSERT(m_mappingTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    const BackupManifest manifest = wiz->manifest();

    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        auto* dest_combo =
            qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnDestination));
        if (dest_combo == nullptr) {
            continue;
        }

        const QString source_username = m_mappingTable->item(row, 1)->text();
        const int match_index = dest_combo->findText(source_username);
        if (match_index >= 0) {
            dest_combo->setCurrentIndex(match_index);
        }
    }

    updateSummary();
}

void UserProfileRestoreUserMappingPage::onMappingChanged(int row, int column) {
    if (column == KMappingColumnDestination) {
        // Realign the merge mode with the newly chosen destination (also covers Auto-Map,
        // which drives the destination combo for matched rows).
        auto* dest_combo =
            qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnDestination));
        auto* mode_combo =
            qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnMergeMode));
        syncMergeModeToDestination(dest_combo, mode_combo);
    }
    updateSummary();
}

void UserProfileRestoreUserMappingPage::updateSummary() {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_mappingTable);
    const int total_mappings = m_mappingTable->rowCount();
    int selected_mappings = 0;
    int new_users = 0;
    int merges = 0;

    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        if (m_mappingTable->item(row, 0)->checkState() != Qt::Checked) {
            continue;
        }
        selected_mappings++;

        auto* dest_combo =
            qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnDestination));
        if ((dest_combo != nullptr) && dest_combo->currentData().toString().isEmpty()) {
            new_users++;
        } else {
            merges++;
        }
    }

    m_summaryLabel->setText(tr("Summary: %1 of %2 users selected | %3 merges, %4 new users")
                                .arg(selected_mappings)
                                .arg(total_mappings)
                                .arg(merges)
                                .arg(new_users));
}

UserMapping UserProfileRestoreUserMappingPage::buildMappingForRow(
    int row, const BackupManifest& manifest) const {
    UserMapping mapping;
    mapping.source_username = m_mappingTable->item(row, 1)->text();
    mapping.selected = true;

    // Find source SID
    auto src_it = std::ranges::find_if(manifest.users, [&](const auto& u) {
        return u.username == mapping.source_username;
    });
    if (src_it != manifest.users.end()) {
        mapping.source_sid = src_it->sid;
    }

    // Get destination user
    auto* dest_combo =
        qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnDestination));
    if (dest_combo != nullptr) {
        mapping.destination_username = dest_combo->currentData().toString();
    }

    // Find destination SID
    if (!mapping.destination_username.isEmpty()) {
        auto dest_it = std::ranges::find_if(m_destinationUsers, [&](const auto& u) {
            return u.username == mapping.destination_username;
        });
        if (dest_it != m_destinationUsers.end()) {
            mapping.destination_sid = dest_it->sid;
        }
    }

    // Get merge mode
    auto* mode_combo =
        qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, KMappingColumnMergeMode));
    if (mode_combo != nullptr) {
        mapping.mode = static_cast<MergeMode>(mode_combo->currentData().toInt());
    }

    return mapping;
}

bool UserProfileRestoreUserMappingPage::validatePage() {
    Q_ASSERT(m_mappingTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }

    const BackupManifest manifest = wiz->manifest();
    QVector<UserMapping> mappings;

    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        if (m_mappingTable->item(row, 0)->checkState() != Qt::Checked) {
            continue;
        }
        mappings.append(buildMappingForRow(row, manifest));
    }

    if (mappings.isEmpty()) {
        sak::logWarning("No users selected for restore operation");
        sak::showWarningLogged(this,
                               tr("No Users Selected"),
                               tr("Please select at least one user to restore."));
        return false;
    }

    for (const auto& mapping : mappings) {
        if (isMappingPairValid(mapping.destination_username, mapping.mode)) {
            continue;
        }
        sak::logWarning("Restore mapping has an incompatible destination/merge-mode pair");
        sak::showWarningLogged(
            this,
            tr("Incompatible Merge Mode"),
            tr("Cannot restore '%1': its merge mode does not match its destination. Choose "
               "'Create New User' for a new account, or Replace/Merge for an existing one.")
                .arg(mapping.source_username));
        return false;
    }

    wiz->setUserMappings(mappings);
    return true;
}

// ============================================================================
// Page 3: Merge Configuration
// ============================================================================

UserProfileRestoreMergeConfigPage::UserProfileRestoreMergeConfigPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Merge Configuration"));
    setSubTitle(tr("Configure how to merge data for each user"));

    setupUi();
}

void UserProfileRestoreMergeConfigPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* info_label = new QLabel(
        tr("For each user mapping, configure the merge behavior and conflict resolution."), this);
    info_label->setWordWrap(true);
    layout->addWidget(info_label);

    // Merge table
    m_mergeTable = new QTableWidget(0, KMergeColumnCount, this);
    m_mergeTable->setHorizontalHeaderLabels(
        {tr("Source -> Destination"), tr("Merge Mode"), tr("Conflict Resolution"), tr("Status")});
    m_mergeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_mergeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mergeTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_mergeTable);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);

    // Connections
    connect(m_mergeTable,
            &QTableWidget::cellChanged,
            this,
            &UserProfileRestoreMergeConfigPage::onMergeSettingsChanged);
}

void UserProfileRestoreMergeConfigPage::initializePage() {
    loadMergeTable();
    updateSummary();
}

void UserProfileRestoreMergeConfigPage::loadMergeTable() {
    Q_ASSERT(m_mergeTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    const QVector<UserMapping> mappings = wiz->userMappings();

    m_mergeTable->setRowCount(0);

    for (const auto& mapping : mappings) {
        const int row = m_mergeTable->rowCount();
        m_mergeTable->insertRow(row);

        // Mapping info
        const QString mapping_text =
            mapping.destination_username.isEmpty()
                ? tr("%1 -> (New User)").arg(mapping.source_username)
                : tr("%1 -> %2").arg(mapping.source_username, mapping.destination_username);

        auto* mapping_item = new QTableWidgetItem(mapping_text);
        mapping_item->setFlags(mapping_item->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, 0, mapping_item);

        // Merge mode (read-only display)
        auto* mode_item = new QTableWidgetItem(mergeModeToString(mapping.mode));
        mode_item->setFlags(mode_item->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, 1, mode_item);

        // Conflict resolution combo
        auto* conflict_combo = new QComboBox();
        conflict_combo->addItem(tr("Skip Duplicate"),
                                static_cast<int>(ConflictResolution::SkipDuplicate));
        conflict_combo->addItem(tr("Rename with Suffix"),
                                static_cast<int>(ConflictResolution::RenameWithSuffix));
        conflict_combo->addItem(tr("Keep Newer"), static_cast<int>(ConflictResolution::KeepNewer));
        conflict_combo->addItem(tr("Keep Larger"),
                                static_cast<int>(ConflictResolution::KeepLarger));
        conflict_combo->addItem(tr("Prompt User"),
                                static_cast<int>(ConflictResolution::PromptUser));
        conflict_combo->setCurrentIndex(1);  // Default to RenameWithSuffix
        m_mergeTable->setCellWidget(row, KMergeColumnConflict, conflict_combo);

        // Status
        auto* status_item = new QTableWidgetItem(tr("Ready"));
        status_item->setFlags(status_item->flags() & ~Qt::ItemIsEditable);
        m_mergeTable->setItem(row, KMergeColumnStatus, status_item);

        // Connect signals
        connect(conflict_combo,
                QOverload<int>::of(&QComboBox::currentIndexChanged),
                this,
                [this, row](int) { onMergeSettingsChanged(row, KMergeColumnConflict); });
    }
}

void UserProfileRestoreMergeConfigPage::onMergeSettingsChanged(int row, int column) {
    Q_UNUSED(row);
    Q_UNUSED(column);
    updateSummary();
}

void UserProfileRestoreMergeConfigPage::updateSummary() {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_mergeTable);
    int replace_count = 0;
    int merge_count = 0;
    int new_count = 0;

    for (int row = 0; row < m_mergeTable->rowCount(); ++row) {
        const QString mode_text = m_mergeTable->item(row, 1)->text();

        if (mode_text.contains("Replace", Qt::CaseInsensitive)) {
            replace_count++;
        } else if (mode_text.contains("Merge", Qt::CaseInsensitive)) {
            merge_count++;
        } else if (mode_text.contains("New", Qt::CaseInsensitive)) {
            new_count++;
        }
    }

    m_summaryLabel->setText(tr("Operations: %1 replace, %2 merge, %3 new users")
                                .arg(replace_count)
                                .arg(merge_count)
                                .arg(new_count));
}

bool UserProfileRestoreMergeConfigPage::validatePage() {
    Q_ASSERT(m_mergeTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }

    // Update mappings with conflict resolution settings
    QVector<UserMapping> mappings = wiz->userMappings();

    for (int row = 0; row < m_mergeTable->rowCount() && row < mappings.size(); ++row) {
        auto* conflict_combo =
            qobject_cast<QComboBox*>(m_mergeTable->cellWidget(row, KMergeColumnConflict));
        if (conflict_combo != nullptr) {
            mappings[row].conflict_resolution =
                static_cast<ConflictResolution>(conflict_combo->currentData().toInt());
        }
    }

    wiz->setUserMappings(mappings);
    return true;
}

// ============================================================================
// Page 4: Folder Selection
// ============================================================================

UserProfileRestoreFolderSelectionPage::UserProfileRestoreFolderSelectionPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Select Folders"));
    setSubTitle(tr("Choose which folders to restore for each user"));

    setupUi();
}

void UserProfileRestoreFolderSelectionPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* info_label = new QLabel(
        tr("Select the folders you want to restore. Uncheck folders to skip them."), this);
    info_label->setWordWrap(true);
    layout->addWidget(info_label);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_layout->addWidget(m_selectAllButton);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch(1);
    layout->addLayout(button_layout);

    // Folder table
    m_folderTable = new QTableWidget(0, KFolderColumnCount, this);
    m_folderTable->setHorizontalHeaderLabels(
        {tr("Select"), tr("User"), tr("Folder"), tr("Size"), tr("Files")});
    m_folderTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_folderTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_folderTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_folderTable);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);

    // Connections
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreFolderSelectionPage::onSelectAll);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreFolderSelectionPage::onSelectNone);
    connect(m_folderTable,
            &QTableWidget::cellChanged,
            this,
            &UserProfileRestoreFolderSelectionPage::onFolderSelectionChanged);
}

void UserProfileRestoreFolderSelectionPage::initializePage() {
    loadFolderTable();
    updateSummary();
}

void UserProfileRestoreFolderSelectionPage::loadFolderTable() {
    Q_ASSERT(m_folderTable);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    BackupManifest manifest = wiz->manifest();
    const QVector<UserMapping> mappings = wiz->userMappings();

    m_folderTable->setRowCount(0);

    for (const auto& mapping : mappings) {
        auto user_it = std::ranges::find_if(manifest.users, [&](const auto& u) {
            return u.username == mapping.source_username;
        });
        if (user_it == manifest.users.end()) {
            continue;
        }

        for (const auto& folder : user_it->backed_up_folders) {
            const int row = m_folderTable->rowCount();
            m_folderTable->insertRow(row);

            // Checkbox. Stash the folder's relative path so validatePage can
            // persist the selection back onto the manifest without depending on
            // row order or the human-readable display name.
            auto* check_item = new QTableWidgetItem();
            check_item->setCheckState(folder.selected ? Qt::Checked : Qt::Unchecked);
            check_item->setData(Qt::UserRole, folder.relative_path);
            m_folderTable->setItem(row, 0, check_item);

            // User
            auto* user_item = new QTableWidgetItem(mapping.source_username);
            user_item->setFlags(user_item->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, 1, user_item);

            // Folder name
            auto* folder_item = new QTableWidgetItem(folder.display_name);
            folder_item->setFlags(folder_item->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, KFolderColumnFolder, folder_item);

            // Size
            const double size_mb = static_cast<double>(folder.size_bytes) / sak::kBytesPerMBf;
            auto* size_item = new QTableWidgetItem(
                QString("%1 MB").arg(size_mb, 0, 'f', kMegabyteDisplayPrecision));
            size_item->setFlags(size_item->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, KFolderColumnSize, size_item);

            // File count
            auto* files_item = new QTableWidgetItem(QString::number(folder.file_count));
            files_item->setFlags(files_item->flags() & ~Qt::ItemIsEditable);
            m_folderTable->setItem(row, KFolderColumnFiles, files_item);
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
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_folderTable);
    const int total_folders = m_folderTable->rowCount();
    int selected_folders = 0;
    qint64 total_size = 0;
    int total_files = 0;

    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        if (m_folderTable->item(row, 0)->checkState() == Qt::Checked) {
            selected_folders++;

            QString size_text = m_folderTable->item(row, KFolderColumnSize)->text();
            size_text.remove(" MB");
            total_size += static_cast<qint64>(size_text.toDouble() * sak::kBytesPerMB);

            total_files += m_folderTable->item(row, KFolderColumnFiles)->text().toInt();
        }
    }

    const double total_gb = static_cast<double>(total_size) / sak::kBytesPerGBf;

    m_summaryLabel->setText(tr("Selected: %1 of %2 folders | %3 files | %4 GB")
                                .arg(selected_folders)
                                .arg(total_folders)
                                .arg(total_files)
                                .arg(total_gb, 0, 'f', kSizeDisplayPrecision));
}

bool UserProfileRestoreFolderSelectionPage::validatePage() {
    Q_ASSERT(m_folderTable);
    // Check if at least one folder is selected
    int selected_count = 0;
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        if (m_folderTable->item(row, 0)->checkState() == Qt::Checked) {
            selected_count++;
        }
    }

    if (selected_count == 0) {
        sak::logWarning("No folders selected for restore operation");
        sak::showWarningLogged(this,
                               tr("No Folders Selected"),
                               tr("Please select at least one folder to restore."));
        return false;
    }

    // Persist the per-folder selection onto the wizard's manifest so the restore
    // worker skips unchecked folders. Without this the selection is display-only
    // and every backed-up folder is restored regardless of the checkboxes.
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    QVector<FolderRestoreChoice> choices;
    choices.reserve(m_folderTable->rowCount());
    for (int row = 0; row < m_folderTable->rowCount(); ++row) {
        FolderRestoreChoice choice;
        choice.username = m_folderTable->item(row, 1)->text();
        choice.relative_path = m_folderTable->item(row, 0)->data(Qt::UserRole).toString();
        choice.selected = m_folderTable->item(row, 0)->checkState() == Qt::Checked;
        choices.append(choice);
    }
    BackupManifest manifest = wiz->manifest();
    applyFolderRestoreSelections(manifest, choices);
    wiz->setManifest(manifest);
    return true;
}

// ============================================================================
// Page 5: Permission Settings
// ============================================================================
// Page 4a: Application Data Restore
// ============================================================================

UserProfileRestoreAppDataPage::UserProfileRestoreAppDataPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Restore Application Data"));
    setSubTitle(tr("Select application data and settings to restore (all selected by default)"));

    setupUi();
}

void UserProfileRestoreAppDataPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("The backup contains application data and settings. "
                      "Select the items you want to restore. All items are selected by default."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    m_appDataTree = new QTreeWidget(this);
    m_appDataTree->setHeaderLabels({tr("Application Data"), tr("Path"), tr("Size")});
    m_appDataTree->setAlternatingRowColors(true);
    m_appDataTree->setRootIsDecorated(true);
    m_appDataTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(kTreePackageColumn,
                                                  QHeaderView::ResizeToContents);
    m_appDataTree->setEnabled(false);
    connect(m_appDataTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileRestoreAppDataPage::onItemChanged);
    layout->addWidget(m_appDataTree);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreAppDataPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreAppDataPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No application data found in backup"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileRestoreAppDataPage::initializePage() {
    if (!m_loaded) {
        loadAppDataSources();
    }
}

void UserProfileRestoreAppDataPage::cleanupPage() {
    // A Back navigation can precede the user selecting a DIFFERENT backup on the
    // welcome page. Drop the one-time load state and clear the populated rows so the
    // next forward visit reloads from the current verified manifest and never
    // persists a previous backup's app-data selections into the restore. Fail closed.
    m_loaded = false;
    if (m_appDataTree != nullptr) {
        m_appDataTree->blockSignals(true);
        m_appDataTree->clear();
        m_appDataTree->blockSignals(false);
        m_appDataTree->setEnabled(false);
    }
    if (m_selectAllButton != nullptr) {
        m_selectAllButton->setEnabled(false);
    }
    if (m_selectNoneButton != nullptr) {
        m_selectNoneButton->setEnabled(false);
    }
    QWizardPage::cleanupPage();
}

bool UserProfileRestoreAppDataPage::isComplete() const {
    return true;  // Always complete -- app data restore is optional
}

void UserProfileRestoreAppDataPage::loadAppDataSources() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_summaryLabel);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    // Trust ONLY the checksum-verified manifest's embedded copy; the standalone
    // app_data_sources.json sidecar is not integrity protected. Without a verified
    // manifest the per-source exclusion UI is disabled (fail closed).
    const BackupManifest& manifest = wiz->manifest();
    if (manifest.manifest_checksum.isEmpty() || !manifest.verifyManifestChecksum()) {
        m_statusLabel->setText(
            tr("Backup has no verified integrity checksum -- app-data selection is disabled"));
        m_summaryLabel->setText(
            tr("Application-data restore requires an integrity-checked backup"));
        return;
    }

    QVector<AppDataSourceInfo> sources = manifest.app_data_sources;
    for (auto& info : sources) {
        info.selected = true;  // All selected by default for restore
    }

    m_loaded = true;

    if (sources.isEmpty()) {
        m_statusLabel->setText(tr("No application data found in backup"));
        return;
    }

    m_statusLabel->setText(tr("Found %1 application data source(s) in backup").arg(sources.size()));
    m_appDataTree->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);

    populateTree(sources);
}

bool UserProfileRestoreAppDataPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    // Reconstruct the selection directly from the tree: each category's leaf items
    // carry the source name (col 0) and profile-relative path (col 1). The worker
    // uses the unchecked paths to skip those app-data subtrees during restore.
    QVector<AppDataSourceInfo> sources;
    for (int c = 0; c < m_appDataTree->topLevelItemCount(); ++c) {
        auto* category = m_appDataTree->topLevelItem(c);
        for (int i = 0; i < category->childCount(); ++i) {
            auto* leaf = category->child(i);
            AppDataSourceInfo info;
            info.name = leaf->text(0);
            info.relative_path = leaf->text(1);
            info.category = category->text(0);
            info.selected = leaf->checkState(0) == Qt::Checked;
            sources.append(info);
        }
    }
    wiz->setAppDataSources(sources);
    return true;
}

void UserProfileRestoreAppDataPage::populateTree(const QVector<AppDataSourceInfo>& sources) {
    Q_ASSERT(m_appDataTree);
    Q_ASSERT(m_summaryLabel);
    m_appDataTree->blockSignals(true);
    m_appDataTree->clear();

    QMap<QString, QVector<const AppDataSourceInfo*>> categories;
    for (const auto& source : sources) {
        const QString cat = source.category.isEmpty() ? tr("Other") : source.category;
        categories[cat].append(&source);
    }

    int total_selected = 0;
    int total = 0;

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* category_item = new QTreeWidgetItem(m_appDataTree);
        category_item->setText(0, it.key());
        category_item->setFlags(category_item->flags() | Qt::ItemIsUserCheckable);

        int cat_selected = 0;
        for (const auto* source : it.value()) {
            auto* item = new QTreeWidgetItem(category_item);
            item->setText(0, source->name);
            item->setText(1, source->relative_path);
            const double size_mb = static_cast<double>(source->size_bytes) / sak::kBytesPerMBf;
            item->setText(kTreeDetailColumn,
                          QString("%1 MB").arg(size_mb, 0, 'f', kMegabyteDisplayPrecision));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, Qt::Checked);  // All selected by default
            cat_selected++;
            total++;
        }

        total_selected += cat_selected;
        category_item->setCheckState(0, Qt::Checked);
        category_item->setExpanded(true);
    }

    m_appDataTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 of %2 application data source(s) selected for restore")
                                .arg(total_selected)
                                .arg(total));
}

void UserProfileRestoreAppDataPage::updateParentCheckState(QTreeWidgetItem* parent) {
    int checked_count = 0;
    const int total_count = parent->childCount();

    for (int i = 0; i < total_count; ++i) {
        if (parent->child(i)->checkState(0) == Qt::Checked) {
            checked_count++;
        }
    }

    if (checked_count == 0) {
        parent->setCheckState(0, Qt::Unchecked);
    } else if (checked_count == total_count) {
        parent->setCheckState(0, Qt::Checked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void UserProfileRestoreAppDataPage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_appDataTree);
    if (column != 0) {
        return;
    }

    m_appDataTree->blockSignals(true);

    if (item->childCount() > 0) {
        const Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
        }
    } else if (item->parent() != nullptr) {
        updateParentCheckState(item->parent());
    }

    m_appDataTree->blockSignals(false);

    int total = 0;
    int selected = 0;
    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            total++;
            if (category->child(j)->checkState(0) == Qt::Checked) {
                selected++;
            }
        }
    }

    m_summaryLabel->setText(
        tr("%1 of %2 application data source(s) selected for restore").arg(selected).arg(total));
}

void UserProfileRestoreAppDataPage::onSelectAll() {
    Q_ASSERT(m_appDataTree);
    m_appDataTree->blockSignals(true);
    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        category->setCheckState(0, Qt::Checked);
        for (int j = 0; j < category->childCount(); ++j) {
            category->child(j)->setCheckState(0, Qt::Checked);
        }
    }
    m_appDataTree->blockSignals(false);
    if (m_appDataTree->topLevelItemCount() > 0) {
        onItemChanged(m_appDataTree->topLevelItem(0), 0);
    }
}

void UserProfileRestoreAppDataPage::onSelectNone() {
    Q_ASSERT(m_appDataTree);
    m_appDataTree->blockSignals(true);
    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        category->setCheckState(0, Qt::Unchecked);
        for (int j = 0; j < category->childCount(); ++j) {
            category->child(j)->setCheckState(0, Qt::Unchecked);
        }
    }
    m_appDataTree->blockSignals(false);
    if (m_appDataTree->topLevelItemCount() > 0) {
        onItemChanged(m_appDataTree->topLevelItem(0), 0);
    }
}

// ============================================================================
// Page 5a: Restore WiFi Networks
// ============================================================================

UserProfileRestoreNetworksPage::UserProfileRestoreNetworksPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Restore WiFi Networks"));
    setSubTitle(tr("Select WiFi network profiles to restore (all selected by default)"));

    setupUi();
}

void UserProfileRestoreNetworksPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label = new QLabel(tr("The backup contains saved WiFi network profiles. "
                                            "Select the ones you want to import on this machine. "
                                            "All profiles are selected by default."),
                                         this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    m_networkTree = new QTreeWidget(this);
    m_networkTree->setHeaderLabels({tr("Network Name (SSID)"), tr("Security Type")});
    m_networkTree->setAlternatingRowColors(true);
    m_networkTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_networkTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_networkTree->setEnabled(false);
    connect(m_networkTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileRestoreNetworksPage::onItemChanged);
    layout->addWidget(m_networkTree);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreNetworksPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreNetworksPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No WiFi profiles found in backup"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileRestoreNetworksPage::initializePage() {
    if (!m_loaded) {
        loadNetworkProfiles();
    }
}

void UserProfileRestoreNetworksPage::cleanupPage() {
    // See UserProfileRestoreAppDataPage::cleanupPage. A Back navigation may precede
    // choosing a different backup, so drop the one-time load state (including the
    // retained xml_data) and clear the tree. Fail closed against stale profiles that
    // would otherwise be re-imported via an elevated netsh wlan add profile.
    m_loaded = false;
    m_profiles.clear();
    if (m_networkTree != nullptr) {
        m_networkTree->blockSignals(true);
        m_networkTree->clear();
        m_networkTree->blockSignals(false);
        m_networkTree->setEnabled(false);
    }
    if (m_selectAllButton != nullptr) {
        m_selectAllButton->setEnabled(false);
    }
    if (m_selectNoneButton != nullptr) {
        m_selectNoneButton->setEnabled(false);
    }
    QWizardPage::cleanupPage();
}

bool UserProfileRestoreNetworksPage::isComplete() const {
    return true;  // Always complete -- network restore is optional
}

void UserProfileRestoreNetworksPage::loadNetworkProfiles() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_summaryLabel);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    // Trust ONLY the checksum-verified manifest's embedded copy of the WiFi
    // selections; the standalone wifi_profiles.json sidecar is not integrity
    // protected and could be tampered before an elevated `netsh wlan add profile`
    // apply. A backup with no manifest checksum, or one whose checksum does not
    // verify, gets no network restore (fail closed).
    const BackupManifest& manifest = wiz->manifest();
    if (manifest.manifest_checksum.isEmpty() || !manifest.verifyManifestChecksum()) {
        m_statusLabel->setText(
            tr("Backup has no verified integrity checksum -- WiFi restore is disabled"));
        m_summaryLabel->setText(tr("Network restore requires an integrity-checked backup"));
        return;
    }

    QVector<WifiProfileInfo> profiles = manifest.wifi_profiles;
    for (auto& info : profiles) {
        info.selected = true;  // All selected by default
    }

    m_loaded = true;

    if (profiles.isEmpty()) {
        m_statusLabel->setText(tr("No WiFi profiles found in backup"));
        return;
    }

    m_statusLabel->setText(tr("Found %1 WiFi profile(s) in backup").arg(profiles.size()));
    m_networkTree->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);

    // Retain the full profiles (with xml_data) so validatePage() can forward them.
    m_profiles = profiles;
    populateTree(profiles);
}

bool UserProfileRestoreNetworksPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    // Tree row order matches m_profiles; carry each profile's checkbox state
    // (with its xml_data) back to the wizard for the worker to re-import.
    QVector<WifiProfileInfo> selected = m_profiles;
    const int rows = m_networkTree->topLevelItemCount();
    for (int i = 0; i < selected.size() && i < rows; ++i) {
        selected[i].selected = m_networkTree->topLevelItem(i)->checkState(0) == Qt::Checked;
    }
    wiz->setWifiProfiles(selected);
    return true;
}

void UserProfileRestoreNetworksPage::populateTree(const QVector<WifiProfileInfo>& profiles) {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_networkTree);
    m_networkTree->blockSignals(true);
    m_networkTree->clear();

    for (const auto& profile : profiles) {
        auto* item = new QTreeWidgetItem(m_networkTree);
        item->setText(0, profile.profile_name);
        item->setText(1, profile.security_type.isEmpty() ? tr("Unknown") : profile.security_type);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Checked);  // All selected by default
    }

    m_networkTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 of %2 WiFi profile(s) selected for restore")
                                .arg(profiles.size())
                                .arg(profiles.size()));
}

void UserProfileRestoreNetworksPage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_networkTree);
    Q_UNUSED(item)
    if (column != 0) {
        return;
    }

    int selected = 0;
    const int total = m_networkTree->topLevelItemCount();
    for (int i = 0; i < total; ++i) {
        if (m_networkTree->topLevelItem(i)->checkState(0) == Qt::Checked) {
            selected++;
        }
    }

    m_summaryLabel->setText(
        tr("%1 of %2 WiFi profile(s) selected for restore").arg(selected).arg(total));
}

void UserProfileRestoreNetworksPage::onSelectAll() {
    m_networkTree->blockSignals(true);
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        m_networkTree->topLevelItem(i)->setCheckState(0, Qt::Checked);
    }
    m_networkTree->blockSignals(false);
    if (m_networkTree->topLevelItemCount() > 0) {
        onItemChanged(m_networkTree->topLevelItem(0), 0);
    }
}

void UserProfileRestoreNetworksPage::onSelectNone() {
    m_networkTree->blockSignals(true);
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        m_networkTree->topLevelItem(i)->setCheckState(0, Qt::Unchecked);
    }
    m_networkTree->blockSignals(false);
    if (m_networkTree->topLevelItemCount() > 0) {
        onItemChanged(m_networkTree->topLevelItem(0), 0);
    }
}

// ============================================================================
// Page 5b: Restore Ethernet Settings
// ============================================================================

UserProfileRestoreEthernetPage::UserProfileRestoreEthernetPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Restore Ethernet Settings"));
    setSubTitle(tr("Select ethernet configurations to restore (all selected by default)"));

    setupUi();
}

void UserProfileRestoreEthernetPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("The backup contains saved ethernet adapter configurations. "
                      "Select which configurations to apply. All are selected by default. "
                      "Static IP settings will be applied using netsh."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    m_ethernetTable = new QTableWidget(0, KEthernetColumnCount, this);
    m_ethernetTable->setHorizontalHeaderLabels({tr("Select"),
                                                tr("Adapter"),
                                                tr("DHCP"),
                                                tr("IP Address"),
                                                tr("Subnet"),
                                                tr("Gateway"),
                                                tr("DNS")});
    m_ethernetTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_ethernetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ethernetTable->verticalHeader()->setVisible(false);
    m_ethernetTable->setEnabled(false);
    layout->addWidget(m_ethernetTable);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreEthernetPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreEthernetPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No ethernet configuration data found in backup"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileRestoreEthernetPage::initializePage() {
    if (!m_loaded) {
        loadEthernetConfigs();
    }
}

void UserProfileRestoreEthernetPage::cleanupPage() {
    // See UserProfileRestoreAppDataPage::cleanupPage. Drop the one-time load state
    // and the retained IP/DNS fields on Back so a later backup change cannot re-apply
    // a previous backup's adapter settings via an elevated netsh interface ip set.
    m_loaded = false;
    m_configs.clear();
    if (m_ethernetTable != nullptr) {
        m_ethernetTable->setRowCount(0);
        m_ethernetTable->setEnabled(false);
    }
    if (m_selectAllButton != nullptr) {
        m_selectAllButton->setEnabled(false);
    }
    if (m_selectNoneButton != nullptr) {
        m_selectNoneButton->setEnabled(false);
    }
    QWizardPage::cleanupPage();
}

bool UserProfileRestoreEthernetPage::isComplete() const {
    return true;  // Always complete -- ethernet restore is optional
}

void UserProfileRestoreEthernetPage::loadEthernetConfigs() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_summaryLabel);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    // Trust ONLY the checksum-verified manifest's embedded copy; the standalone
    // ethernet_configs.json sidecar is not integrity protected and could be tampered
    // before an elevated `netsh interface ip set` apply. Without a verified manifest
    // the Ethernet restore is disabled (fail closed).
    const BackupManifest& manifest = wiz->manifest();
    if (manifest.manifest_checksum.isEmpty() || !manifest.verifyManifestChecksum()) {
        m_statusLabel->setText(
            tr("Backup has no verified integrity checksum -- Ethernet restore is disabled"));
        m_summaryLabel->setText(tr("Network restore requires an integrity-checked backup"));
        return;
    }

    QVector<EthernetConfigInfo> configs = manifest.ethernet_configs;
    for (auto& info : configs) {
        info.selected = true;  // All selected by default
    }

    m_loaded = true;

    if (configs.isEmpty()) {
        m_statusLabel->setText(tr("No ethernet configurations found in backup"));
        return;
    }

    m_statusLabel->setText(tr("Found %1 ethernet configuration(s) in backup").arg(configs.size()));
    m_ethernetTable->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);

    // Retain the full configs (IP/DNS fields) so validatePage() can forward them.
    m_configs = configs;
    populateTable(configs);
}

bool UserProfileRestoreEthernetPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }
    // Table row order matches m_configs; carry each adapter's checkbox state
    // (with its full static-IP/DNS fields) back to the wizard.
    QVector<EthernetConfigInfo> selected = m_configs;
    const int rows = m_ethernetTable->rowCount();
    for (int i = 0; i < selected.size() && i < rows; ++i) {
        auto* check_item = m_ethernetTable->item(i, 0);
        selected[i].selected = check_item != nullptr && check_item->checkState() == Qt::Checked;
    }
    wiz->setEthernetConfigs(selected);
    return true;
}

void UserProfileRestoreEthernetPage::populateTable(const QVector<EthernetConfigInfo>& configs) {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_ethernetTable);
    m_ethernetTable->setRowCount(0);

    for (const auto& config : configs) {
        const int row = m_ethernetTable->rowCount();
        m_ethernetTable->insertRow(row);

        auto* check_item = new QTableWidgetItem();
        check_item->setCheckState(Qt::Checked);  // All selected by default
        m_ethernetTable->setItem(row, 0, check_item);

        auto* name_item = new QTableWidgetItem(config.adapter_name);
        name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 1, name_item);

        auto* dhcp_item = new QTableWidgetItem(config.dhcp_enabled ? tr("Yes") : tr("No"));
        dhcp_item->setFlags(dhcp_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, KEthernetColumnDhcp, dhcp_item);

        auto* ip_item = new QTableWidgetItem(config.ip_address);
        ip_item->setFlags(ip_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, KEthernetColumnIpAddress, ip_item);

        auto* subnet_item = new QTableWidgetItem(config.subnet_mask);
        subnet_item->setFlags(subnet_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, KEthernetColumnSubnet, subnet_item);

        auto* gw_item = new QTableWidgetItem(config.default_gateway);
        gw_item->setFlags(gw_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, KEthernetColumnGateway, gw_item);

        QString dns = config.dns_primary;
        if (!config.dns_secondary.isEmpty()) {
            dns += ", " + config.dns_secondary;
        }
        auto* dns_item = new QTableWidgetItem(dns);
        dns_item->setFlags(dns_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, KEthernetColumnDns, dns_item);
    }

    const int total = m_ethernetTable->rowCount();
    m_summaryLabel->setText(tr("%1 ethernet configuration(s) selected for restore").arg(total));
}

void UserProfileRestoreEthernetPage::onSelectAll() {
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, 0)->setCheckState(Qt::Checked);
    }
    m_summaryLabel->setText(
        tr("%1 ethernet configuration(s) selected for restore").arg(m_ethernetTable->rowCount()));
}

void UserProfileRestoreEthernetPage::onSelectNone() {
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, 0)->setCheckState(Qt::Unchecked);
    }
    m_summaryLabel->setText(tr("0 ethernet configuration(s) selected for restore"));
}

// ============================================================================
// Page 5: Permission Settings
// ============================================================================

UserProfileRestorePermissionSettingsPage::UserProfileRestorePermissionSettingsPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Permission & Verification"));
    setSubTitle(tr("Configure permission handling and verification options"));

    setupUi();
}

void UserProfileRestorePermissionSettingsPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Permission mode
    auto* perm_group = new QWidget(this);
    auto* perm_layout = new QGridLayout(perm_group);
    perm_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    perm_layout->addWidget(new QLabel(tr("Permission Mode:"), perm_group), 0, 0);
    m_permissionModeCombo = new QComboBox(perm_group);
    m_permissionModeCombo->addItem(tr("Strip All ACLs (Safest)"),
                                   static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Assign to Destination User"),
                                   static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Preserve Original"),
                                   static_cast<int>(PermissionMode::PreserveOriginal));
    perm_layout->addWidget(m_permissionModeCombo, 0, 1);

    layout->addWidget(perm_group);

    // Conflict resolution
    auto* conflict_group = new QWidget(this);
    auto* conflict_layout = new QGridLayout(conflict_group);
    conflict_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    conflict_layout->addWidget(new QLabel(tr("Conflict Resolution:"), conflict_group), 0, 0);
    m_conflictResolutionCombo = new QComboBox(conflict_group);
    m_conflictResolutionCombo->addItem(tr("Skip Duplicate"),
                                       static_cast<int>(ConflictResolution::SkipDuplicate));
    m_conflictResolutionCombo->addItem(tr("Rename with Suffix"),
                                       static_cast<int>(ConflictResolution::RenameWithSuffix));
    m_conflictResolutionCombo->addItem(tr("Keep Newer"),
                                       static_cast<int>(ConflictResolution::KeepNewer));
    m_conflictResolutionCombo->addItem(tr("Keep Larger"),
                                       static_cast<int>(ConflictResolution::KeepLarger));
    m_conflictResolutionCombo->setCurrentIndex(1);  // Default to RenameWithSuffix
    conflict_layout->addWidget(m_conflictResolutionCombo, 0, 1);

    layout->addWidget(conflict_group);

    setupUi_optionsAndConnections(layout);
}

void UserProfileRestorePermissionSettingsPage::setupUi_optionsAndConnections(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    layout->addSpacing(sak::ui::kMarginXLarge);

    // Options
    m_verifyCheckBox = new QCheckBox(tr("Verify file integrity after restore"), this);
    m_verifyCheckBox->setChecked(true);
    layout->addWidget(m_verifyCheckBox);

    m_createBackupCheckBox = new QCheckBox(tr("Create backup of existing files before overwriting"),
                                           this);
    m_createBackupCheckBox->setChecked(false);
    layout->addWidget(m_createBackupCheckBox);

    layout->addSpacing(sak::ui::kMarginXLarge);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgWarningPanel,
                                                          sak::ui::kColorTextBody,
                                                          sak::ui::kCssPaddingXXLargePx,
                                                          sak::ui::kCssRadiusXLargePx));
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    layout->addStretch(1);

    // Connections
    connect(m_permissionModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    connect(m_conflictResolutionCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    constexpr auto kCheckBoxChangedSignal = &QCheckBox::toggled;
    connect(m_verifyCheckBox,
            kCheckBoxChangedSignal,
            this,
            &UserProfileRestorePermissionSettingsPage::onSettingsChanged);
    connect(m_createBackupCheckBox,
            kCheckBoxChangedSignal,
            this,
            &UserProfileRestorePermissionSettingsPage::onSettingsChanged);

    onSettingsChanged();
}

void UserProfileRestorePermissionSettingsPage::onSettingsChanged() {
    updateSummary();
}

void UserProfileRestorePermissionSettingsPage::updateSummary() {
    Q_ASSERT(m_permissionModeCombo);
    Q_ASSERT(m_conflictResolutionCombo);
    const QString perm_mode = m_permissionModeCombo->currentText();
    const QString conflict_mode = m_conflictResolutionCombo->currentText();

    QString warning;
    const PermissionMode mode =
        static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());

    if (mode == PermissionMode::PreserveOriginal) {
        warning = tr(
            "(!) <b>Warning:</b> Preserving original permissions may cause access issues "
            "if SIDs don't match.");
    } else if (mode == PermissionMode::StripAll) {
        warning = tr(
            "[OK] <b>Recommended:</b> Stripping ACLs ensures files inherit safe permissions "
            "from parent folders.");
    } else if (mode == PermissionMode::AssignToDestination) {
        warning = tr("(i) <b>Info:</b> Files will be owned by the destination user.");
    }

    const QString summary = QString(
                                "<b>Configuration Summary:</b><br>"
                                "* Permission Mode: %1<br>"
                                "* Conflict Resolution: %2<br>"
                                "* Verify Integrity: %3<br>"
                                "* Backup Existing: %4<br><br>"
                                "%5")
                                .arg(perm_mode,
                                     conflict_mode,
                                     m_verifyCheckBox->isChecked() ? tr("Yes") : tr("No"),
                                     m_createBackupCheckBox->isChecked() ? tr("Yes") : tr("No"),
                                     warning);

    m_summaryLabel->setText(summary);
}

bool UserProfileRestorePermissionSettingsPage::validatePage() {
    Q_ASSERT(m_permissionModeCombo);
    Q_ASSERT(m_conflictResolutionCombo);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return false;
    }

    wiz->setPermissionMode(
        static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt()));
    wiz->setConflictResolution(
        static_cast<ConflictResolution>(m_conflictResolutionCombo->currentData().toInt()));
    wiz->setVerifyFiles(m_verifyCheckBox->isChecked());
    wiz->setCreateBackup(m_createBackupCheckBox->isChecked());

    return true;
}

// ============================================================================
// Page 5: App Restore
// ============================================================================

UserProfileRestoreAppRestorePage::UserProfileRestoreAppRestorePage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Restore Applications"));
    setSubTitle(tr("Install applications that were saved during backup (optional)"));

    setupUi();
}

void UserProfileRestoreAppRestorePage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("The backup contains a list of installed applications. "
                      "Select the ones you want to install on this machine via Chocolatey. "
                      "You can also skip this step by clicking Next."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    m_appTree = new QTreeWidget(this);
    m_appTree->setHeaderLabels({tr("Application"), tr("Version"), tr("Chocolatey Package")});
    m_appTree->setAlternatingRowColors(true);
    m_appTree->setRootIsDecorated(true);
    m_appTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_appTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_appTree->header()->setSectionResizeMode(kTreePackageColumn, QHeaderView::ResizeToContents);
    m_appTree->setEnabled(false);
    connect(m_appTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileRestoreAppRestorePage::onItemChanged);
    layout->addWidget(m_appTree);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreAppRestorePage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreAppRestorePage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);

    button_layout->addStretch();

    m_installButton = new QPushButton(tr("Install Selected Apps"), this);
    m_installButton->setEnabled(false);
    m_installButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_installButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreAppRestorePage::onInstallApps);
    button_layout->addWidget(m_installButton);

    layout->addLayout(button_layout);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    layout->addWidget(m_progressBar);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No application data found in backup"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileRestoreAppRestorePage::initializePage() {
    if (!m_loaded) {
        loadApps();
    }
}

void UserProfileRestoreAppRestorePage::cleanupPage() {
    // A Back navigation may precede a backup change on the welcome page. Drop the
    // one-time load state and the loaded app list so the next forward visit reloads
    // installed_apps.json from the CURRENT backup path -- never offering a previous
    // backup's apps for install. Skip while an install is in flight: the detached
    // task still posts progress to this page's widgets. Fail closed.
    if (m_installing) {
        QWizardPage::cleanupPage();
        return;
    }
    m_loaded = false;
    m_apps.clear();
    if (m_appTree != nullptr) {
        m_appTree->blockSignals(true);
        m_appTree->clear();
        m_appTree->blockSignals(false);
        m_appTree->setEnabled(false);
    }
    if (m_selectAllButton != nullptr) {
        m_selectAllButton->setEnabled(false);
    }
    if (m_selectNoneButton != nullptr) {
        m_selectNoneButton->setEnabled(false);
    }
    if (m_installButton != nullptr) {
        m_installButton->setEnabled(false);
    }
    QWizardPage::cleanupPage();
}

bool UserProfileRestoreAppRestorePage::isComplete() const {
    // Always complete -- app installation is optional
    return !m_installing;
}

void UserProfileRestoreAppRestorePage::loadApps() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_summaryLabel);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        return;
    }

    const QString apps_file_path = wiz->backupPath() + "/installed_apps.json";
    QFile file(apps_file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_statusLabel->setText(
            tr("No installed_apps.json found in backup -- skipping app restore"));
        m_summaryLabel->setText(tr("No application data available in this backup"));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) {
        m_statusLabel->setText(tr("Invalid installed_apps.json format"));
        return;
    }

    const QJsonArray apps_array = doc.array();
    m_apps.clear();
    m_apps.reserve(apps_array.size());

    for (const auto& val : apps_array) {
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
static QPair<int, int> countEnabledCategoryApps(QTreeWidgetItem* category) {
    int total = 0, selected = 0;
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        auto* item = category->child(child_index);
        if (!(item->flags() & Qt::ItemIsEnabled)) {
            continue;
        }
        total++;
        if (item->checkState(0) == Qt::Checked) {
            selected++;
        }
    }
    return {total, selected};
}

int UserProfileRestoreAppRestorePage::populateCategoryApps(
    QTreeWidgetItem* category_item,
    const QVector<const RestoreAppInfo*>& apps,
    int& total_with_package) {
    int cat_selected = 0;
    for (const auto* app : apps) {
        auto* app_item = new QTreeWidgetItem(category_item);
        app_item->setText(0, app->name);
        app_item->setText(1, app->version);
        app_item->setText(kTreePackageColumn,
                          app->choco_package.isEmpty() ? tr("(no match)") : app->choco_package);
        app_item->setFlags(app_item->flags() | Qt::ItemIsUserCheckable);

        if (app->choco_package.isEmpty()) {
            app_item->setCheckState(0, Qt::Unchecked);
            app_item->setForeground(kTreePackageColumn, QBrush(Qt::gray));
            app_item->setFlags(app_item->flags() & ~Qt::ItemIsEnabled);
            continue;
        }

        app_item->setCheckState(0, app->selected ? Qt::Checked : Qt::Unchecked);
        total_with_package++;
        cat_selected += app->selected ? 1 : 0;
    }
    return cat_selected;
}

void UserProfileRestoreAppRestorePage::populateTree(const QVector<RestoreAppInfo>& apps) {
    Q_ASSERT(m_appTree);
    Q_ASSERT(m_summaryLabel);
    m_appTree->blockSignals(true);
    m_appTree->clear();

    // Group by category
    QMap<QString, QVector<const RestoreAppInfo*>> categories;
    for (const auto& app : apps) {
        const QString cat = app.category.isEmpty() ? tr("Other") : app.category;
        categories[cat].append(&app);
    }

    int total_selected = 0;
    int total_with_package = 0;

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* category_item = new QTreeWidgetItem(m_appTree);
        category_item->setText(0, it.key());
        category_item->setFlags(category_item->flags() | Qt::ItemIsUserCheckable);

        const int cat_selected =
            populateCategoryApps(category_item, it.value(), total_with_package);
        total_selected += cat_selected;

        // Set parent check state
        if (cat_selected == 0) {
            category_item->setCheckState(0, Qt::Unchecked);
        } else if (cat_selected == it.value().size()) {
            category_item->setCheckState(0, Qt::Checked);
        } else {
            category_item->setCheckState(0, Qt::PartiallyChecked);
        }

        category_item->setExpanded(true);
    }

    m_appTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 application(s) selected for installation (%2 have Chocolatey "
                               "packages)")
                                .arg(total_selected)
                                .arg(total_with_package));
}

void UserProfileRestoreAppRestorePage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_appTree);
    Q_ASSERT(m_summaryLabel);
    if (column != 0) {
        return;
    }

    m_appTree->blockSignals(true);

    if (item->childCount() > 0) {
        // Parent item -- propagate to enabled children
        const Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            auto* child = item->child(i);
            if (!(child->flags() & Qt::ItemIsEnabled)) {
                continue;
            }
            child->setCheckState(0, state);
        }
    } else if (item->parent() != nullptr) {
        updateParentCheckState(item->parent());
    }

    m_appTree->blockSignals(false);

    // Count selected
    int total = 0;
    int selected = 0;
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        auto [t, s] = countEnabledCategoryApps(m_appTree->topLevelItem(category_index));
        total += t;
        selected += s;
    }

    m_summaryLabel->setText(tr("%1 application(s) selected for installation out of %2 available")
                                .arg(selected)
                                .arg(total));
}

void UserProfileRestoreAppRestorePage::updateParentCheckState(QTreeWidgetItem* parent) {
    int checked_count = 0;
    int enabled_count = 0;

    for (int i = 0; i < parent->childCount(); ++i) {
        auto* child = parent->child(i);
        if (!(child->flags() & Qt::ItemIsEnabled)) {
            continue;
        }
        enabled_count++;
        if (child->checkState(0) == Qt::Checked) {
            checked_count++;
        }
    }

    if (enabled_count == 0 || checked_count == 0) {
        parent->setCheckState(0, Qt::Unchecked);
    } else if (checked_count == enabled_count) {
        parent->setCheckState(0, Qt::Checked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void UserProfileRestoreAppRestorePage::onSelectAll() {
    Q_ASSERT(m_appTree);
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* child = category->child(child_index);
            if (!(child->flags() & Qt::ItemIsEnabled)) {
                continue;
            }
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
    Q_ASSERT(m_appTree);
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* child = category->child(child_index);
            if (!(child->flags() & Qt::ItemIsEnabled)) {
                continue;
            }
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
    Q_ASSERT(m_installButton);
    Q_ASSERT(m_selectAllButton);
    // Collect selected apps
    const QVector<RestoreAppInfo> selected_apps = collectSelectedApps();

    if (selected_apps.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("No Apps Selected"),
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
    m_progressBar->setRange(0, static_cast<int>(selected_apps.size()));
    m_progressBar->setValue(0);

    auto* watcher = new QFutureWatcher<QPair<int, int>>(this);
    connect(watcher, &QFutureWatcher<QPair<int, int>>::finished, this, [this, watcher]() {
        const auto [installed, failed] = watcher->result();
        watcher->deleteLater();

        m_installing = false;
        m_installButton->setEnabled(true);
        m_selectAllButton->setEnabled(true);
        m_selectNoneButton->setEnabled(true);
        m_appTree->setEnabled(true);
        Q_EMIT completeChanged();

        m_statusLabel->setText(
            tr("Installation complete: %1 succeeded, %2 failed").arg(installed).arg(failed));
        m_summaryLabel->setText(
            tr("App installation finished -- %1 installed, %2 failed. Click Next to "
               "continue.")
                .arg(installed)
                .arg(failed));
    });

    const QPointer<UserProfileRestoreAppRestorePage> self(this);
    watcher->setFuture(QtConcurrent::run([self, selected_apps]() -> QPair<int, int> {
        // Task is page-agnostic: it captures only a QPointer and posts UI updates
        // through it, so a page destroyed mid-install cannot be dereferenced.
        return installAppsSequentially(selected_apps, self);
    }));
    // B3-15 detached-mutation decision: this is a Chocolatey install that can run for MINUTES, so
    // the wizard-page destructor deliberately does NOT waitForFinished() (that would freeze
    // teardown for the whole install). It is safe to leave running detached: the background task
    // derefs only the QPointer above (dropped once the page is gone) and the finished handler is
    // bound to a child QFutureWatcher that Qt tears down with the page, so no slot fires on a dead
    // page. The install self-completes; aborting a half-done package install would be worse than
    // letting it finish. (Short, bounded mutations -- netsh/copy/archive -- ARE joined at teardown
    // instead.)
}

QVector<RestoreAppInfo> UserProfileRestoreAppRestorePage::collectSelectedApps() const {
    Q_ASSERT(m_appTree);
    QVector<RestoreAppInfo> selected_apps;
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            auto* app_item = category->child(child_index);
            if (((app_item->flags() & Qt::ItemIsEnabled) != 0) &&
                app_item->checkState(0) == Qt::Checked) {
                RestoreAppInfo info;
                info.name = app_item->text(0);
                info.choco_package = app_item->text(kTreePackageColumn);
                selected_apps.append(info);
            }
        }
    }
    return selected_apps;
}

QPair<int, int> UserProfileRestoreAppRestorePage::installAppsSequentially(
    const QVector<RestoreAppInfo>& apps, QPointer<UserProfileRestoreAppRestorePage> page) {
    auto choco_manager = std::make_shared<ChocolateyManager>();
    const QString choco_path = QApplication::applicationDirPath() + "/tools/chocolatey";
    choco_manager->initialize(choco_path);

    int installed = 0;
    int failed = 0;
    const int total = static_cast<int>(apps.size());

    for (int i = 0; i < apps.size(); ++i) {
        const auto& app = apps[i];
        // Post to the page only while it is alive; the inner QPointer re-check
        // covers destruction between posting and delivery.
        if (auto* live = page.data()) {
            QMetaObject::invokeMethod(
                live,
                [page, name = app.name, index = i, total]() {
                    if (page) {
                        page->m_statusLabel->setText(page->tr("Installing %1 (%2/%3)...")
                                                         .arg(name)
                                                         .arg(index + 1)
                                                         .arg(total));
                    }
                },
                Qt::QueuedConnection);
        }

        ChocolateyManager::InstallConfig config;
        config.package_name = app.choco_package;
        config.auto_confirm = true;

        auto result = choco_manager->installPackage(config);
        if (result.success) {
            installed++;
        } else {
            failed++;
        }

        if (auto* live = page.data()) {
            QMetaObject::invokeMethod(
                live,
                [page, value = i + 1]() {
                    if (page) {
                        page->m_progressBar->setValue(value);
                    }
                },
                Qt::QueuedConnection);
        }
    }

    return {installed, failed};
}

}  // namespace sak
