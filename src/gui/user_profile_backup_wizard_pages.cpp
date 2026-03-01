// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_wizard.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/app_scanner.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDateTime>
#include <QTimer>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QPointer>

namespace sak {

// ============================================================================
// UserProfileBackupCustomizeDataPage
// ============================================================================

UserProfileBackupCustomizeDataPage::UserProfileBackupCustomizeDataPage(QVector<UserProfile>& users, QWidget* parent)
    : QWizardPage(parent)
    , m_users(users)
{
    setTitle(tr("Customize Per-User Data"));
    setSubTitle(tr("Customize which folders and application data to backup for each user"));
    
    setupUi();
}

void UserProfileBackupCustomizeDataPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    m_instructionLabel = new QLabel(this);
    m_instructionLabel->setWordWrap(true);
    layout->addWidget(m_instructionLabel);
    
    // User table (2 columns: username, folders selected)
    m_userTable = new QTableWidget(0, 2, this);
    m_userTable->setHorizontalHeaderLabels({tr("Username"), tr("Folders Selected")});
    m_userTable->horizontalHeader()->setStretchLastSection(true);
    m_userTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_userTable->verticalHeader()->setVisible(false);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_userTable);
    
    // Customize button (for selected row)
    auto* buttonLayout = new QHBoxLayout();
    m_customizeButton = new QPushButton(tr("Customize Selected User"), this);
    m_customizeButton->setIcon(QIcon::fromTheme("configure"));
    m_customizeButton->setEnabled(false);
    connect(m_customizeButton, &QPushButton::clicked, this, &UserProfileBackupCustomizeDataPage::onCustomizeUser);
    connect(m_userTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_customizeButton->setEnabled(!m_userTable->selectedItems().isEmpty());
    });
    buttonLayout->addWidget(m_customizeButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupCustomizeDataPage::initializePage() {
    m_instructionLabel->setText(tr(
        "By default, common folders (Documents, Desktop, Pictures, Downloads) are selected for each user. "
        "Click <b>Customize</b> to change folder selections, add custom folders, or select specific application data."
    ));
    
    populateUserList();
    updateSummary();
}

bool UserProfileBackupCustomizeDataPage::isComplete() const {
    // Always complete - customization is optional
    return true;
}

void UserProfileBackupCustomizeDataPage::populateUserList() {
    m_userTable->setRowCount(0);
    
    int row = 0;
    for (auto& user : m_users) {
        if (!user.is_selected) continue;
        
        m_userTable->insertRow(row);
        
        // Username
        auto* nameItem = new QTableWidgetItem(user.username);
        m_userTable->setItem(row, 0, nameItem);
        
        // Folder count
        int selectedCount = 0;
        for (const auto& folder : user.folder_selections) {
            if (folder.selected) selectedCount++;
        }
        auto* folderItem = new QTableWidgetItem(tr("%1 folders selected").arg(selectedCount));
        m_userTable->setItem(row, 1, folderItem);
        
        row++;
    }
}

void UserProfileBackupCustomizeDataPage::onCustomizeUser() {
    int selectedRow = m_userTable->currentRow();
    if (selectedRow < 0) return;
    
    // Find the selected user
    int userIndex = 0;
    int currentRow = 0;
    for (auto& user : m_users) {
        if (!user.is_selected) {
            userIndex++;
            continue;
        }
        if (currentRow == selectedRow) {
            // Open customization dialog
            PerUserCustomizationDialog dialog(user, this);
            if (dialog.exec() == QDialog::Accepted) {
                // Update table
                int selectedCount = 0;
                for (const auto& folder : user.folder_selections) {
                    if (folder.selected) selectedCount++;
                }
                m_userTable->item(selectedRow, 1)->setText(tr("%1 folders selected").arg(selectedCount));
                updateSummary();
            }
            break;
        }
        currentRow++;
        userIndex++;
    }
}

void UserProfileBackupCustomizeDataPage::updateSummary() {
    int totalUsers = 0;
    int totalFolders = 0;
    qint64 totalSize = 0;
    
    for (const auto& user : m_users) {
        if (!user.is_selected) continue;
        totalUsers++;
        
        for (const auto& folder : user.folder_selections) {
            if (folder.selected) {
                totalFolders++;
                totalSize += folder.size_bytes;
            }
        }
    }
    
    double totalGB = totalSize / sak::kBytesPerGBf;
    m_summaryLabel->setText(tr("%1 user(s), %2 total folders | Estimated: %3 GB")
        .arg(totalUsers)
        .arg(totalFolders)
        .arg(totalGB, 0, 'f', 2));
}

// ============================================================================
// UserProfileBackupSmartFiltersPage
// ============================================================================

UserProfileBackupSmartFiltersPage::UserProfileBackupSmartFiltersPage(SmartFilter& filter, QWidget* parent)
    : QWizardPage(parent)
    , m_filter(filter)
{
    setTitle(tr("Smart Filter Configuration"));
    setSubTitle(tr("Configure automatic file and folder exclusions to prevent corruption"));
    
    setupUi();
}

void UserProfileBackupSmartFiltersPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "Smart filters automatically exclude files that can corrupt user profiles or waste space. "
        "You can adjust these settings or keep the recommended defaults."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);
    
    setupUi_filterSettings(layout);
    setupUi_exclusionsAndControls(layout);
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupSmartFiltersPage::setupUi_filterSettings(QVBoxLayout* layout) {
    auto* gridLayout = new QGridLayout();
    int row = 0;
    
    // File size limit
    m_enableFileSizeLimitCheck = new QCheckBox(tr("Limit single file size:"), this);
    connect(m_enableFileSizeLimitCheck, &QCheckBox::toggled, [this](bool enabled) {
        m_maxFileSizeSpinBox->setEnabled(enabled);
        updateSummary();
    });
    gridLayout->addWidget(m_enableFileSizeLimitCheck, row, 0);
    
    m_maxFileSizeSpinBox = new QSpinBox(this);
    m_maxFileSizeSpinBox->setRange(1, 10000);
    m_maxFileSizeSpinBox->setSuffix(tr(" MB"));
    m_maxFileSizeSpinBox->setValue(2048);  // Default 2 GB
    connect(m_maxFileSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), 
            this, &UserProfileBackupSmartFiltersPage::updateSummary);
    gridLayout->addWidget(m_maxFileSizeSpinBox, row, 1);
    row++;
    
    // Folder size warning
    m_enableFolderSizeLimitCheck = new QCheckBox(tr("Warn if folder exceeds:"), this);
    connect(m_enableFolderSizeLimitCheck, &QCheckBox::toggled, [this](bool enabled) {
        m_maxFolderSizeSpinBox->setEnabled(enabled);
        updateSummary();
    });
    gridLayout->addWidget(m_enableFolderSizeLimitCheck, row, 0);
    
    m_maxFolderSizeSpinBox = new QSpinBox(this);
    m_maxFolderSizeSpinBox->setRange(1, 1000);
    m_maxFolderSizeSpinBox->setSuffix(tr(" GB"));
    m_maxFolderSizeSpinBox->setValue(50);  // Default 50 GB
    connect(m_maxFolderSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), 
            this, &UserProfileBackupSmartFiltersPage::updateSummary);
    gridLayout->addWidget(m_maxFolderSizeSpinBox, row, 1);
    row++;
    
    layout->addLayout(gridLayout);
}

void UserProfileBackupSmartFiltersPage::setupUi_exclusionsAndControls(QVBoxLayout* layout) {
    // Automatic exclusions
    auto* exclusionsGroup = new QWidget(this);
    auto* exclusionsLayout = new QVBoxLayout(exclusionsGroup);
    exclusionsLayout->setContentsMargins(0, 0, 0, 0);
    
    auto* exclusionsLabel = new QLabel(tr("<b>Automatic Exclusions:</b>"), this);
    exclusionsLayout->addWidget(exclusionsLabel);
    
    m_excludeCacheCheck = new QCheckBox(tr("Exclude cache directories (WebCache, GPUCache, etc.)"), this);
    connect(m_excludeCacheCheck, &QCheckBox::toggled, this, &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusionsLayout->addWidget(m_excludeCacheCheck);
    
    m_excludeTempCheck = new QCheckBox(tr("Exclude temporary files (*.tmp, *.cache, *.temp)"), this);
    connect(m_excludeTempCheck, &QCheckBox::toggled, this, &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusionsLayout->addWidget(m_excludeTempCheck);
    
    m_excludeLockCheck = new QCheckBox(tr("Exclude lock files (*.lock, *.lck)"), this);
    connect(m_excludeLockCheck, &QCheckBox::toggled, this, &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusionsLayout->addWidget(m_excludeLockCheck);
    
    layout->addWidget(exclusionsGroup);
    
    // Dangerous files info
    auto* dangerousLayout = new QHBoxLayout();
    auto* dangerousLabel = new QLabel(tr(
        "\u26a0 <b>Always excluded:</b> Registry hives (NTUSER.DAT, UsrClass.dat), system folders"
    ), this);
    dangerousLabel->setWordWrap(true);
    dangerousLayout->addWidget(dangerousLabel, 1);
    
    m_viewDangerousButton = new QPushButton(tr("View Full List..."), this);
    connect(m_viewDangerousButton, &QPushButton::clicked, this, &UserProfileBackupSmartFiltersPage::onViewDangerousList);
    dangerousLayout->addWidget(m_viewDangerousButton);
    layout->addLayout(dangerousLayout);
    
    layout->addStretch();
    
    // Reset button
    auto* resetLayout = new QHBoxLayout();
    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    m_resetButton->setIcon(QIcon::fromTheme("edit-undo"));
    connect(m_resetButton, &QPushButton::clicked, this, &UserProfileBackupSmartFiltersPage::onResetToDefaults);
    resetLayout->addWidget(m_resetButton);
    resetLayout->addStretch();
    layout->addLayout(resetLayout);
}

void UserProfileBackupSmartFiltersPage::initializePage() {
    loadFilterSettings();
    updateSummary();
}

void UserProfileBackupSmartFiltersPage::loadFilterSettings() {
    // Load enable flags
    m_enableFileSizeLimitCheck->setChecked(m_filter.enable_file_size_limit);
    m_enableFolderSizeLimitCheck->setChecked(m_filter.enable_folder_size_limit);
    
    // Load from m_filter (convert bytes to MB/GB for UI)
    qint64 fileSizeMB = m_filter.max_single_file_size / sak::kBytesPerMB;
    m_maxFileSizeSpinBox->setValue(static_cast<int>(fileSizeMB));
    m_maxFileSizeSpinBox->setEnabled(m_filter.enable_file_size_limit);
    
    qint64 folderSizeGB = m_filter.max_folder_size / sak::kBytesPerGB;
    m_maxFolderSizeSpinBox->setValue(static_cast<int>(folderSizeGB));
    m_maxFolderSizeSpinBox->setEnabled(m_filter.enable_folder_size_limit);
    
    // Enable checkboxes based on whether the filter category has any rules defined
    bool hasCacheRules = !m_filter.exclude_folders.isEmpty();
    bool hasTempRules = !m_filter.exclude_patterns.isEmpty();
    bool hasLockRules = !m_filter.dangerous_files.isEmpty();
    
    m_excludeCacheCheck->setEnabled(hasCacheRules);
    m_excludeCacheCheck->setChecked(hasCacheRules);
    if (!hasCacheRules) {
        m_excludeCacheCheck->setToolTip(tr("No cache exclusion rules defined"));
    } else {
        m_excludeCacheCheck->setToolTip(QString());
    }
    
    m_excludeTempCheck->setEnabled(hasTempRules);
    m_excludeTempCheck->setChecked(hasTempRules);
    if (!hasTempRules) {
        m_excludeTempCheck->setToolTip(tr("No temporary file exclusion patterns defined"));
    } else {
        m_excludeTempCheck->setToolTip(QString());
    }
    
    m_excludeLockCheck->setEnabled(hasLockRules);
    m_excludeLockCheck->setChecked(hasLockRules);
    if (!hasLockRules) {
        m_excludeLockCheck->setToolTip(tr("No dangerous file rules defined"));
    } else {
        m_excludeLockCheck->setToolTip(QString());
    }
}

void UserProfileBackupSmartFiltersPage::onResetToDefaults() {
    m_filter.initializeDefaults();
    loadFilterSettings();
    updateSummary();
}

void UserProfileBackupSmartFiltersPage::onViewDangerousList() {
    QString dangerousList;
    dangerousList += tr("<h3>Always Excluded Files:</h3>");
    dangerousList += "<ul>";
    for (const auto& file : m_filter.dangerous_files) {
        dangerousList += QString("<li>%1</li>").arg(file);
    }
    dangerousList += "</ul>";
    
    dangerousList += tr("<h3>Excluded Patterns:</h3>");
    dangerousList += "<ul>";
    for (const auto& pattern : m_filter.exclude_patterns) {
        dangerousList += QString("<li>%1</li>").arg(pattern);
    }
    dangerousList += "</ul>";
    
    dangerousList += tr("<h3>Excluded Folders:</h3>");
    dangerousList += "<ul>";
    for (const auto& folder : m_filter.exclude_folders) {
        dangerousList += QString("<li>%1</li>").arg(folder);
    }
    dangerousList += "</ul>";
    
    QMessageBox::information(this, tr("Dangerous Files List"), dangerousList);
}

void UserProfileBackupSmartFiltersPage::updateSummary() {
    // Update filter enable flags
    m_filter.enable_file_size_limit = m_enableFileSizeLimitCheck->isChecked();
    m_filter.enable_folder_size_limit = m_enableFolderSizeLimitCheck->isChecked();
    
    // Update filter from UI (convert MB/GB back to bytes)
    m_filter.max_single_file_size = static_cast<qint64>(m_maxFileSizeSpinBox->value()) * sak::kBytesPerMB;
    m_filter.max_folder_size = static_cast<qint64>(m_maxFolderSizeSpinBox->value()) * sak::kBytesPerGB;
    
    // Count total exclusions
    int exclusionCount = m_filter.dangerous_files.size() + 
                        m_filter.exclude_patterns.size() + 
                        m_filter.exclude_folders.size();
    
    QString limitText;
    if (m_filter.enable_file_size_limit) {
        limitText = tr("File limit: %1 MB").arg(m_maxFileSizeSpinBox->value());
    } else {
        limitText = tr("No file size limit");
    }
    
    m_summaryLabel->setText(tr("🛡 %1 exclusion rules active | %2")
        .arg(exclusionCount)
        .arg(limitText));
}

// ============================================================================
// UserProfileBackupSettingsPage
// ============================================================================

UserProfileBackupSettingsPage::UserProfileBackupSettingsPage(BackupManifest& manifest, QWidget* parent)
    : QWizardPage(parent)
    , m_manifest(manifest)
{
    setTitle(tr("Backup Settings"));
    setSubTitle(tr("Configure backup destination and options"));
    
    setupUi();
}

void UserProfileBackupSettingsPage::setupUi_destinationAndCompression(QVBoxLayout* layout) {
    // Destination path
    auto* destLayout = new QHBoxLayout();
    destLayout->addWidget(new QLabel(tr("Backup destination:"), this));
    m_destinationEdit = new QLineEdit(this);
    m_destinationEdit->setPlaceholderText(tr("Select backup folder..."));
    connect(m_destinationEdit, &QLineEdit::textChanged, this, &UserProfileBackupSettingsPage::updateSummary);
    destLayout->addWidget(m_destinationEdit, 1);
    m_browseButton = new QPushButton(tr("Browse..."), this);
    connect(m_browseButton, &QPushButton::clicked, this, &UserProfileBackupSettingsPage::onBrowseDestination);
    destLayout->addWidget(m_browseButton);
    layout->addLayout(destLayout);
    
    // Compression
    auto* compressionLayout = new QHBoxLayout();
    compressionLayout->addWidget(new QLabel(tr("Compression:"), this));
    m_compressionCombo = new QComboBox(this);
    m_compressionCombo->addItems({tr("None"), tr("Fast"), tr("Balanced"), tr("Maximum")});
    m_compressionCombo->setCurrentIndex(2); // Default to Balanced
    m_compressionCombo->setToolTip(tr("Choose compression level:\n"
                                        "• None: No compression, fastest\n"
                                        "• Fast: Quick compression, larger files\n"
                                        "• Balanced: Good balance (recommended)\n"
                                        "• Maximum: Best compression, slower"));
    compressionLayout->addWidget(m_compressionCombo);
    compressionLayout->addStretch();
    layout->addLayout(compressionLayout);
}

void UserProfileBackupSettingsPage::setupUi_encryptionAndPermissions(QVBoxLayout* layout) {
    // Encryption
    auto* encryptionLayout = new QHBoxLayout();
    m_encryptionCheck = new QCheckBox(tr("Encrypt backup"), this);
    m_encryptionCheck->setToolTip(tr("Protects the backup with AES-256 encryption — you'll need the password to restore"));
    connect(m_encryptionCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_passwordEdit->setEnabled(checked);
        m_passwordConfirmEdit->setEnabled(checked);
    });
    encryptionLayout->addWidget(m_encryptionCheck);
    encryptionLayout->addStretch();
    layout->addLayout(encryptionLayout);
    
    auto* passwordLayout = new QHBoxLayout();
    passwordLayout->addWidget(new QLabel(tr("Password:"), this));
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setEnabled(false);
    m_passwordEdit->setPlaceholderText(tr("Enter encryption password"));
    passwordLayout->addWidget(m_passwordEdit);
    layout->addLayout(passwordLayout);
    
    auto* confirmLayout = new QHBoxLayout();
    confirmLayout->addWidget(new QLabel(tr("Confirm:"), this));
    m_passwordConfirmEdit = new QLineEdit(this);
    m_passwordConfirmEdit->setEchoMode(QLineEdit::Password);
    m_passwordConfirmEdit->setEnabled(false);
    m_passwordConfirmEdit->setPlaceholderText(tr("Confirm encryption password"));
    confirmLayout->addWidget(m_passwordConfirmEdit);
    layout->addLayout(confirmLayout);
    
    // Permission mode
    auto* permLayout = new QHBoxLayout();
    permLayout->addWidget(new QLabel(tr("Permission handling:"), this));
    m_permissionModeCombo = new QComboBox(this);
    m_permissionModeCombo->addItem(tr("Strip All (Recommended)"), static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Preserve Original"), static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Assign to Destination"), static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Hybrid (Try Preserve, Fallback Strip)"), static_cast<int>(PermissionMode::Hybrid));
    m_permissionModeCombo->setCurrentIndex(0); // StripAll by default
    connect(m_permissionModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &UserProfileBackupSettingsPage::updateSummary);
    permLayout->addWidget(m_permissionModeCombo, 1);
    layout->addLayout(permLayout);
    
    // Permission explanation
    auto* permExplainLabel = new QLabel(tr(
        "ℹ <b>Strip All:</b> Removes ACLs to prevent permission conflicts (safest). "
        "<b>Preserve:</b> Keeps original permissions (may cause errors). "
        "<b>Assign Standard:</b> Sets full control for destination user."
    ), this);
    permExplainLabel->setWordWrap(true);
    permExplainLabel->setStyleSheet(QString("QLabel { padding: 6px; color: %1; }").arg(sak::ui::kColorTextMuted));
    layout->addWidget(permExplainLabel);
}

void UserProfileBackupSettingsPage::setupUi_summaryAndRegistration(QVBoxLayout* layout) {
    // Verification
    m_verifyCheck = new QCheckBox(tr("Verify files after backup (MD5 checksums)"), this);
    m_verifyCheck->setChecked(true);
    layout->addWidget(m_verifyCheck);
    
    layout->addStretch();
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
    
    // Register wizard fields for validation
    registerField("destination*", m_destinationEdit);
    registerField("compressionLevel", m_compressionCombo, "currentIndex");
    registerField("encryptionEnabled", m_encryptionCheck);
    registerField("encryptionPassword", m_passwordEdit);
}

void UserProfileBackupSettingsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "Choose where to save the backup and configure additional options."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);
    
    setupUi_destinationAndCompression(layout);
    setupUi_encryptionAndPermissions(layout);
    setupUi_summaryAndRegistration(layout);
}

void UserProfileBackupSettingsPage::initializePage() {
    // Suggest default backup location
    QString defaultPath = QDir::homePath() + "/UserProfileBackup_" + 
                         QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_destinationEdit->setText(defaultPath);
    updateSummary();
}

bool UserProfileBackupSettingsPage::validatePage() {
    // Check destination path
    if (m_destinationEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("No Destination"), 
            tr("Please select a backup destination folder."));
        return false;
    }
    
    QDir destDir(m_destinationEdit->text());
    if (destDir.exists()) {
        auto reply = QMessageBox::question(this, tr("Folder Exists"),
            tr("The destination folder already exists. Continue anyway?"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return false;
        }
    }
    
    // Validate encryption settings
    if (m_encryptionCheck->isChecked()) {
        QString password = m_passwordEdit->text();
        QString confirm = m_passwordConfirmEdit->text();
        
        if (password.isEmpty()) {
            QMessageBox::warning(this, tr("Missing Password"),
                tr("Please enter an encryption password."));
            m_passwordEdit->setFocus();
            return false;
        }
        
        if (password != confirm) {
            QMessageBox::warning(this, tr("Password Mismatch"),
                tr("Passwords do not match. Please re-enter."));
            m_passwordConfirmEdit->clear();
            m_passwordConfirmEdit->setFocus();
            return false;
        }
        
        if (password.length() < 8) {
            QMessageBox::warning(this, tr("Weak Password"),
                tr("Password must be at least 8 characters long."));
            m_passwordEdit->setFocus();
            return false;
        }
    }
    
    // Save settings to manifest
    m_destinationPath = m_destinationEdit->text();
    m_manifest.version = "1.0";
    m_manifest.created = QDateTime::currentDateTime();
    m_manifest.source_machine = QSysInfo::machineHostName();
    
    // Create execute page now that we have destination path
    auto* wizard = qobject_cast<UserProfileBackupWizard*>(this->wizard());
    if (wizard) {
        // Get users from wizard
        const auto& users = wizard->property("scannedUsers").value<QVector<UserProfile>>();
        wizard->setPage(UserProfileBackupWizard::Page_Execute, 
            new UserProfileBackupExecutePage(m_manifest, users, m_destinationPath, wizard));
    }
    
    return true;
}

void UserProfileBackupSettingsPage::onBrowseDestination() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("Select Backup Destination"),
        m_destinationEdit->text().isEmpty() ? QDir::homePath() : m_destinationEdit->text());
    if (!dir.isEmpty()) {
        m_destinationEdit->setText(dir);
    }
}

void UserProfileBackupSettingsPage::updateSummary() {
    QString dest = m_destinationEdit->text().isEmpty() ? 
        tr("Not selected") : QDir::toNativeSeparators(m_destinationEdit->text());
    
    QString permMode;
    auto currentMode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());
    if (currentMode == PermissionMode::StripAll) {
        permMode = tr("Strip ACLs");
    } else if (currentMode == PermissionMode::PreserveOriginal) {
        permMode = tr("Preserve");
    } else if (currentMode == PermissionMode::AssignToDestination) {
        permMode = tr("Assign Destination");
    } else if (currentMode == PermissionMode::Hybrid) {
        permMode = tr("Hybrid");
    }
    
    m_summaryLabel->setText(tr("💾 Destination: %1 | Permissions: %2 | Verify: %3")
        .arg(dest)
        .arg(permMode)
        .arg(m_verifyCheck->isChecked() ? tr("Yes") : tr("No")));
}

// ============================================================================
// UserProfileBackupInstalledAppsPage
// ============================================================================

UserProfileBackupInstalledAppsPage::UserProfileBackupInstalledAppsPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Installed Applications"));
    setSubTitle(tr("Select installed applications to include in the backup for later restoration"));

    setupUi();
}

void UserProfileBackupInstalledAppsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "Click <b>Scan Applications</b> to detect all installed programs. "
        "Selected applications will be saved to the backup so they can be "
        "reinstalled via Chocolatey on the destination machine."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);

    // Scan button and status
    auto* scanLayout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Applications"), this);
    m_scanButton->setIcon(QIcon::fromTheme("view-refresh"));
    connect(m_scanButton, &QPushButton::clicked, this, &UserProfileBackupInstalledAppsPage::onScanApps);
    scanLayout->addWidget(m_scanButton);

    m_statusLabel = new QLabel(tr("Click Scan Applications to begin"), this);
    scanLayout->addWidget(m_statusLabel, 1);
    layout->addLayout(scanLayout);

    // Progress
    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    // Tree widget with hierarchical checkboxes
    m_appTree = new QTreeWidget(this);
    m_appTree->setHeaderLabels({tr("Application"), tr("Version"), tr("Publisher")});
    m_appTree->setAlternatingRowColors(true);
    m_appTree->setRootIsDecorated(true);
    m_appTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_appTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_appTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_appTree->setEnabled(false);
    connect(m_appTree, &QTreeWidget::itemChanged,
            this, &UserProfileBackupInstalledAppsPage::onItemChanged);
    layout->addWidget(m_appTree);

    // Selection buttons
    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this, &UserProfileBackupInstalledAppsPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &UserProfileBackupInstalledAppsPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; border-radius: 10px; }").arg(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No applications scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupInstalledAppsPage::initializePage() {
    // Don't auto-scan — let the user click Scan when ready
    updateNextButtonText();
}

void UserProfileBackupInstalledAppsPage::cleanupPage() {
    // Restore default button text when leaving the page
    auto* wiz = wizard();
    if (wiz) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupInstalledAppsPage::updateNextButtonText() {
    auto* wiz = wizard();
    if (!wiz) return;

    bool hasSelection = false;
    for (int c = 0; c < m_appTree->topLevelItemCount(); ++c) {
        auto* cat = m_appTree->topLevelItem(c);
        for (int a = 0; a < cat->childCount(); ++a) {
            if (cat->child(a)->checkState(0) == Qt::Checked) {
                hasSelection = true;
                break;
            }
        }
        if (hasSelection) break;
    }

    wiz->setButtonText(QWizard::NextButton,
                       hasSelection ? tr("Next >") : tr("Skip >"));
}

bool UserProfileBackupInstalledAppsPage::isComplete() const {
    // Always complete — app selection is optional
    return true;
}

QString UserProfileBackupInstalledAppsPage::categorizeApp(const QString& name, const QString& publisher) {
    Q_UNUSED(publisher)
    const QString n = name.toLower();

    if (n.contains("chrome") || n.contains("firefox") || n.contains("edge") ||
        n.contains("opera") || n.contains("brave") || n.contains("vivaldi") || n.contains("browser"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Browsers");
    if (n.contains("visual studio") || n.contains("vscode") || n.contains("jetbrains") ||
        n.contains("intellij") || n.contains("pycharm") || n.contains("webstorm") ||
        n.contains("rider") || n.contains("clion") || n.contains("android studio") ||
        n.contains("eclipse") || n.contains("sublime") || n.contains("notepad++") ||
        n.contains("atom") || n.contains("code"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Development");
    if (n.contains("office") || n.contains("word") || n.contains("excel") ||
        n.contains("powerpoint") || n.contains("outlook") || n.contains("onenote") ||
        n.contains("libreoffice") || n.contains("openoffice"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Productivity");
    if (n.contains("discord") || n.contains("slack") || n.contains("teams") ||
        n.contains("zoom") || n.contains("skype") || n.contains("telegram") ||
        n.contains("signal") || n.contains("whatsapp"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Communication");
    if (n.contains("steam") || n.contains("epic games") || n.contains("origin") ||
        n.contains("battle.net") || n.contains("gog") || n.contains("ubisoft") || n.contains("game"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Gaming");
    if (n.contains("photoshop") || n.contains("illustrator") || n.contains("gimp") ||
        n.contains("blender") || n.contains("inkscape") || n.contains("paint") ||
        n.contains("krita") || n.contains("figma") || n.contains("canva"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Graphics & Design");
    if (n.contains("vlc") || n.contains("spotify") || n.contains("itunes") ||
        n.contains("audacity") || n.contains("obs") || n.contains("handbrake") ||
        n.contains("media") || n.contains("player") || n.contains("foobar") || n.contains("winamp"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Media");
    if (n.contains("7-zip") || n.contains("winrar") || n.contains("peazip") ||
        n.contains("ccleaner") || n.contains("everything") || n.contains("totalcommander") ||
        n.contains("wiztree") || n.contains("treesize") || n.contains("windirstat") || n.contains("revo"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Utilities");
    if (n.contains("norton") || n.contains("kaspersky") || n.contains("malwarebytes") ||
        n.contains("avast") || n.contains("avg") || n.contains("bitdefender") ||
        n.contains("security") || n.contains("antivirus"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Security");
    if (n.contains("nvidia") || n.contains("amd") || n.contains("realtek") ||
        n.contains("intel") || n.contains("driver") || n.contains("logitech") ||
        n.contains("corsair") || n.contains("razer"))
        return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Drivers & Hardware");
    return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Other");
}

void UserProfileBackupInstalledAppsPage::onScanApps() {
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning installed applications..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);  // Indeterminate
    m_appTree->clear();

    // Use a QPointer so we can detect if the page was destroyed while scanning
    QPointer<UserProfileBackupInstalledAppsPage> safeThis(this);

    // Run heavy scanning work on a background thread to avoid freezing the UI
    auto* watcher = new QFutureWatcher<QVector<InstalledAppInfo>>(this);

    connect(watcher, &QFutureWatcher<QVector<InstalledAppInfo>>::finished, this, [safeThis, watcher]() {
        watcher->deleteLater();
        if (!safeThis) return;  // Page was destroyed

        auto appInfos = watcher->result();

        safeThis->m_scanned = true;
        safeThis->m_scanButton->setEnabled(true);
        safeThis->m_selectAllButton->setEnabled(true);
        safeThis->m_selectNoneButton->setEnabled(true);
        safeThis->m_appTree->setEnabled(true);
        safeThis->m_scanProgress->setVisible(false);

        safeThis->m_statusLabel->setText(
            QCoreApplication::translate("UserProfileBackupInstalledAppsPage",
                "Found %1 application(s)").arg(appInfos.size()));
        safeThis->populateTree(appInfos);
        safeThis->updateNextButtonText();
        Q_EMIT safeThis->completeChanged();
    });

    watcher->setFuture(QtConcurrent::run([]() -> QVector<InstalledAppInfo> {
        // Scan installed apps from registry + AppX (no Chocolatey calls).
        // Backup only needs name/version/publisher — restore handles Chocolatey matching.
        AppScanner scanner;
        auto apps = scanner.scanAll();

        sak::logDebug("onScanApps background: scanAll returned {} apps", apps.size());

        QVector<InstalledAppInfo> appInfos;
        appInfos.reserve(apps.size());

        for (const auto& app : apps) {
            InstalledAppInfo info;
            info.name = app.name;
            info.version = app.version;
            info.publisher = app.publisher;
            info.selected = true;
            info.category = categorizeApp(app.name, app.publisher);
            appInfos.append(info);
        }

        sak::logDebug("onScanApps background: returning {} app infos", appInfos.size());
        return appInfos;
    }));
}

void UserProfileBackupInstalledAppsPage::populateTree(const QVector<InstalledAppInfo>& apps) {
    m_appTree->blockSignals(true);
    m_appTree->clear();

    // Group apps by category
    QMap<QString, QVector<const InstalledAppInfo*>> categories;
    for (const auto& app : apps) {
        categories[app.category].append(&app);
    }

    int totalSelected = 0;
    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* categoryItem = new QTreeWidgetItem(m_appTree);
        categoryItem->setText(0, it.key());
        categoryItem->setFlags(categoryItem->flags() | Qt::ItemIsUserCheckable);
        categoryItem->setCheckState(0, Qt::Checked);

        for (const auto* app : it.value()) {
            auto* appItem = new QTreeWidgetItem(categoryItem);
            appItem->setText(0, app->name);
            appItem->setText(1, app->version);
            appItem->setText(2, app->publisher);
            appItem->setFlags(appItem->flags() | Qt::ItemIsUserCheckable);
            appItem->setCheckState(0, app->selected ? Qt::Checked : Qt::Unchecked);

            if (app->selected) totalSelected++;
        }

        categoryItem->setExpanded(true);
    }

    m_appTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 application(s) selected out of %2")
                                .arg(totalSelected).arg(apps.size()));
}

void UserProfileBackupInstalledAppsPage::onItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;

    m_appTree->blockSignals(true);

    // If it's a parent (category) item, propagate to children
    if (item->childCount() > 0) {
        Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
        }
    } else {
        // Child changed — update parent's tri-state
        auto* parent = item->parent();
        if (parent) {
            updateParentCheckState(parent);
        }
    }

    m_appTree->blockSignals(false);

    // Update summary and save to wizard
    int total = 0;
    int selected = 0;
    QVector<InstalledAppInfo> selectedApps;

    for (int c = 0; c < m_appTree->topLevelItemCount(); ++c) {
        auto* category = m_appTree->topLevelItem(c);
        for (int a = 0; a < category->childCount(); ++a) {
            auto* appItem = category->child(a);
            total++;
            if (appItem->checkState(0) == Qt::Checked) {
                selected++;
                InstalledAppInfo info;
                info.name = appItem->text(0);
                info.version = appItem->text(1);
                info.publisher = appItem->text(2);
                info.category = category->text(0);
                info.selected = true;
                selectedApps.append(info);
            }
        }
    }

    m_summaryLabel->setText(tr("%1 application(s) selected out of %2")
                                .arg(selected).arg(total));

    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz) {
        wiz->setInstalledApps(selectedApps);
    }

    updateNextButtonText();
}

void UserProfileBackupInstalledAppsPage::updateParentCheckState(QTreeWidgetItem* parent) {
    int checkedCount = 0;
    int totalCount = parent->childCount();

    for (int i = 0; i < totalCount; ++i) {
        if (parent->child(i)->checkState(0) == Qt::Checked) {
            checkedCount++;
        }
    }

    if (checkedCount == 0) {
        parent->setCheckState(0, Qt::Unchecked);
    } else if (checkedCount == totalCount) {
        parent->setCheckState(0, Qt::Checked);
    } else {
        parent->setCheckState(0, Qt::PartiallyChecked);
    }
}

void UserProfileBackupInstalledAppsPage::onSelectAll() {
    m_appTree->blockSignals(true);
    for (int c = 0; c < m_appTree->topLevelItemCount(); ++c) {
        auto* category = m_appTree->topLevelItem(c);
        category->setCheckState(0, Qt::Checked);
        for (int a = 0; a < category->childCount(); ++a) {
            category->child(a)->setCheckState(0, Qt::Checked);
        }
    }
    m_appTree->blockSignals(false);

    // Trigger update via fake item change
    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

void UserProfileBackupInstalledAppsPage::onSelectNone() {
    m_appTree->blockSignals(true);
    for (int c = 0; c < m_appTree->topLevelItemCount(); ++c) {
        auto* category = m_appTree->topLevelItem(c);
        category->setCheckState(0, Qt::Unchecked);
        for (int a = 0; a < category->childCount(); ++a) {
            category->child(a)->setCheckState(0, Qt::Unchecked);
        }
    }
    m_appTree->blockSignals(false);

    // Trigger update
    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

} // namespace sak
