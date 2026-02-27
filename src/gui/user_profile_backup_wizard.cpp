// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_wizard.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QDir>
#include <QTimer>

namespace sak {

// ============================================================================
// UserProfileBackupWizard
// ============================================================================

UserProfileBackupWizard::UserProfileBackupWizard(QWidget* parent)
    : QWizard(parent)
{
    setWindowTitle(tr("User Profile Backup Wizard"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::HaveHelpButton, false);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setOption(QWizard::NoBackButtonOnLastPage, true);
    
    // Initialize smart filter with defaults
    m_smartFilter.initializeDefaults();
    
    // Add pages
    setPage(Page_Welcome, new UserProfileBackupWelcomePage(this));
    setPage(Page_SelectUsers, new UserProfileBackupSelectUsersPage(m_scannedUsers, this));
    setPage(Page_CustomizeData, new UserProfileBackupCustomizeDataPage(m_scannedUsers, this));
    setPage(Page_InstalledApps, new UserProfileBackupInstalledAppsPage(this));
    setPage(Page_SmartFilters, new UserProfileBackupSmartFiltersPage(m_smartFilter, this));
    setPage(Page_BackupSettings, new UserProfileBackupSettingsPage(m_manifest, this));
    
    // Note: Execute page will be created in BackupSettingsPage::validatePage
    // after we have the destination path
    
    // Set initial page
    setStartId(Page_Welcome);
    
    // Resize to comfortable size
    resize(700, 500);
}

UserProfileBackupWizard::~UserProfileBackupWizard() = default;

int UserProfileBackupWizard::getCompressionLevel() const {
    int index = field("compressionLevel").toInt();
    // Map combo index to compression level: 0=none(0), 1=fast(3), 2=balanced(6), 3=max(9)
    static const int levels[] = {0, 3, 6, 9};
    return (index >= 0 && index < 4) ? levels[index] : 6;
}

bool UserProfileBackupWizard::isEncryptionEnabled() const {
    return field("encryptionEnabled").toBool();
}

QString UserProfileBackupWizard::getEncryptionPassword() const {
    return field("encryptionPassword").toString();
}

// ============================================================================
// UserProfileBackupWelcomePage
// ============================================================================

UserProfileBackupWelcomePage::UserProfileBackupWelcomePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Welcome to User Profile Backup"));
    setSubTitle(tr("This wizard will guide you through backing up Windows user profiles"));
    
    setupUi();
}

void UserProfileBackupWelcomePage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Welcome message
    auto* welcomeLabel = new QLabel(this);
    welcomeLabel->setWordWrap(true);
    welcomeLabel->setText(tr(
        "<h3>What This Wizard Does</h3>"
        "<p>This wizard will help you create a complete backup of Windows user profiles, including:</p>"
        "<ul>"
        "<li><b>Documents, Desktop, Pictures, Downloads</b> - User data folders</li>"
        "<li><b>Application Data</b> - Selectively backup browser bookmarks, email signatures, etc.</li>"
        "<li><b>Installed Applications</b> - Scan and record installed software for reinstallation on a new machine</li>"
        "<li><b>Custom Folders</b> - Add any additional folders you need</li>"
        "</ul>"
        
        "<h3>Smart Filtering</h3>"
        "<p>The wizard automatically excludes dangerous files that can corrupt profiles:</p>"
        "<ul>"
        "<li>Registry hives (NTUSER.DAT, UsrClass.dat)</li>"
        "<li>Lock files and cache directories</li>"
        "<li>Temporary files and system folders</li>"
        "</ul>"
        
        "<h3>Application Backup</h3>"
        "<p>Optionally scan the system for installed applications and record them in the backup. "
        "During restoration, matched applications can be automatically reinstalled via Chocolatey.</p>"
        
        "<h3>Safe Restoration</h3>"
        "<p>Backups include metadata for intelligent restoration:</p>"
        "<ul>"
        "<li>User mapping (source user \u2192 destination user)</li>"
        "<li>Permission handling (strip ACLs to prevent conflicts)</li>"
        "<li>Conflict resolution (skip, rename, keep newer/larger)</li>"
        "<li>Multi-user merge capability</li>"
        "</ul>"
        
        "<p><b>Click Next to begin scanning for Windows user accounts.</b></p>"
    ));
    
    layout->addWidget(welcomeLabel);
    layout->addStretch();
}

// ============================================================================
// UserProfileBackupSelectUsersPage
// ============================================================================

UserProfileBackupSelectUsersPage::UserProfileBackupSelectUsersPage(QVector<UserProfile>& users, QWidget* parent)
    : QWizardPage(parent)
    , m_users(users)
    , m_scanner(new WindowsUserScanner(this))
{
    setTitle(tr("Select Users to Backup"));
    setSubTitle(tr("Scan and select which user profiles to include in the backup"));
    
    setupUi();
    
    // Connect scanner signals
    connect(m_scanner, &WindowsUserScanner::userFound, this, &UserProfileBackupSelectUsersPage::onUserScanned);
    connect(m_scanner, &WindowsUserScanner::scanProgress, this, [this](int current, int total) {
        m_scanProgress->setMaximum(total);
        m_scanProgress->setValue(current);
    });
}

void UserProfileBackupSelectUsersPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "Click <b>Scan Users</b> to detect all Windows user accounts on this computer. "
        "Then select which users you want to backup."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);
    
    // Scan button and status
    auto* scanLayout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Users"), this);
    m_scanButton->setIcon(QIcon::fromTheme("view-refresh"));
    connect(m_scanButton, &QPushButton::clicked, this, &UserProfileBackupSelectUsersPage::onScanUsers);
    scanLayout->addWidget(m_scanButton);
    
    m_statusLabel = new QLabel(tr("Click Scan Users to begin"), this);
    scanLayout->addWidget(m_statusLabel, 1);
    layout->addLayout(scanLayout);
    
    // Progress bar
    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);
    
    // User table (4 columns: checkbox, username, profile path, size)
    m_userTable = new QTableWidget(0, 4, this);
    m_userTable->setHorizontalHeaderLabels({tr("?"), tr("Username"), tr("Profile Path"), tr("Est. Size")});
    m_userTable->horizontalHeader()->setStretchLastSection(false);
    m_userTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_userTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_userTable->verticalHeader()->setVisible(false);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_userTable->setEnabled(false);
    connect(m_userTable, &QTableWidget::itemChanged, this, &UserProfileBackupSelectUsersPage::updateSummary);
    layout->addWidget(m_userTable);
    
    // Selection buttons
    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this, &UserProfileBackupSelectUsersPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);
    
    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &UserProfileBackupSelectUsersPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("QLabel { padding: 10px; background-color: #e0f2fe; border-radius: 10px; }");
    layout->addWidget(m_summaryLabel);
    updateSummary();
}

void UserProfileBackupSelectUsersPage::initializePage() {
    // Don't auto-scan — let the user click Scan Users when ready
}

bool UserProfileBackupSelectUsersPage::isComplete() const {
    // At least one user must be selected
    for (const auto& user : m_users) {
        if (user.is_selected) {
            return true;
        }
    }
    return false;
}

void UserProfileBackupSelectUsersPage::onScanUsers() {
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning Windows user accounts..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0); // Indeterminate
    m_userTable->setRowCount(0);
    m_users.clear();
    
    // Start scan (synchronous - runs quickly with NetUserEnum)
    m_users = m_scanner->scanUsers();
    
    // Process results
    m_scanned = true;
    m_scanButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_userTable->setEnabled(true);
    m_scanProgress->setVisible(false);
    
    if (m_users.isEmpty()) {
        m_statusLabel->setText(tr("No user accounts found"));
        QMessageBox::warning(this, tr("No Users"), 
            tr("No Windows user accounts were detected. Make sure you have permission to scan users."));
        return;
    }
    
    m_statusLabel->setText(tr("Found %1 user account(s)").arg(m_users.size()));
    populateTable();
    updateSummary();
    Q_EMIT completeChanged();
}

void UserProfileBackupSelectUsersPage::onUserScanned(const QString& username) {
    m_statusLabel->setText(tr("Found user: %1").arg(username));
}

void UserProfileBackupSelectUsersPage::populateTable() {
    m_userTable->blockSignals(true);
    m_userTable->setRowCount(m_users.size());
    
    for (int i = 0; i < m_users.size(); ++i) {
        auto& user = m_users[i];
        
        // Checkbox
        auto* checkItem = new QTableWidgetItem();
        checkItem->setCheckState(user.is_selected ? Qt::Checked : Qt::Unchecked);
        checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        m_userTable->setItem(i, 0, checkItem);
        
        // Username (mark current user)
        QString username = user.username;
        if (user.is_current_user) {
            username += tr(" (Current)");
        }
        auto* nameItem = new QTableWidgetItem(username);
        m_userTable->setItem(i, 1, nameItem);
        
        // Profile path
        auto* pathItem = new QTableWidgetItem(user.profile_path);
        m_userTable->setItem(i, 2, pathItem);
        
        // Size estimate
        QString sizeText = tr("Calculating...");
        if (user.total_size_estimated > 0) {
            double sizeGB = user.total_size_estimated / (1024.0 * 1024.0 * 1024.0);
            sizeText = QString("%1 GB").arg(sizeGB, 0, 'f', 1);
        }
        auto* sizeItem = new QTableWidgetItem(sizeText);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_userTable->setItem(i, 3, sizeItem);
    }
    
    m_userTable->blockSignals(false);
}

void UserProfileBackupSelectUsersPage::onSelectAll() {
    m_userTable->blockSignals(true);
    for (int i = 0; i < m_userTable->rowCount(); ++i) {
        m_userTable->item(i, 0)->setCheckState(Qt::Checked);
        m_users[i].is_selected = true;
    }
    m_userTable->blockSignals(false);
    updateSummary();
    Q_EMIT completeChanged();
}

void UserProfileBackupSelectUsersPage::onSelectNone() {
    m_userTable->blockSignals(true);
    for (int i = 0; i < m_userTable->rowCount(); ++i) {
        m_userTable->item(i, 0)->setCheckState(Qt::Unchecked);
        m_users[i].is_selected = false;
    }
    m_userTable->blockSignals(false);
    updateSummary();
    Q_EMIT completeChanged();
}

void UserProfileBackupSelectUsersPage::updateSummary() {
    // Update is_selected state from checkboxes
    for (int i = 0; i < m_userTable->rowCount(); ++i) {
        m_users[i].is_selected = (m_userTable->item(i, 0)->checkState() == Qt::Checked);
    }
    
    // Count selected users and total size
    int selectedCount = 0;
    qint64 totalSizeBytes = 0;
    for (const auto& user : m_users) {
        if (user.is_selected) {
            selectedCount++;
            totalSizeBytes += user.total_size_estimated;
        }
    }
    
    double totalGB = totalSizeBytes / (1024.0 * 1024.0 * 1024.0);
    
    if (selectedCount == 0) {
        m_summaryLabel->setText(tr("No users selected"));
    } else {
        m_summaryLabel->setText(tr("%1 user(s) selected | Estimated total size: %2 GB")
            .arg(selectedCount)
            .arg(totalGB, 0, 'f', 1));
    }
    
    // Notify wizard that completion state may have changed
    Q_EMIT completeChanged();
}

} // namespace sak

