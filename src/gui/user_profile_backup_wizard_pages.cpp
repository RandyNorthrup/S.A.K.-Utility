// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_wizard.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/app_scanner.h"
#include "sak/wifi_profile_scanner.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QTextEdit>
#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QDateTime>
#include <QTimer>
#include <QCoreApplication>
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QPointer>
#include <algorithm>

namespace sak {

// ============================================================================
// UserProfileBackupCustomizeDataPage
// ============================================================================

UserProfileBackupCustomizeDataPage::UserProfileBackupCustomizeDataPage(QVector<UserProfile>& users,
    QWidget* parent)
    : QWizardPage(parent)
    , m_users(users)
{
    setTitle(tr("Customize Per-User Data"));
    setSubTitle(tr("Customize which folders and application data to backup for each user"));

    setupUi();
}

void UserProfileBackupCustomizeDataPage::setupUi() {
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
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
    connect(m_customizeButton, &QPushButton::clicked, this,
        &UserProfileBackupCustomizeDataPage::onCustomizeUser);
    connect(m_userTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_customizeButton->setEnabled(!m_userTable->selectedItems().isEmpty());
    });
    buttonLayout->addWidget(m_customizeButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupCustomizeDataPage::initializePage() {
    m_instructionLabel->setText(tr(
        "By default, common folders (Documents, Desktop, Pictures, Downloads) are selected for "
        "each user. "
        "Click <b>Customize</b> to change folder selections, add custom folders, or select "
        "specific application data."
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

UserProfile* UserProfileBackupCustomizeDataPage::findSelectedUserByRow(int selectedRow) {
    int currentRow = 0;
    for (auto& user : m_users) {
        if (!user.is_selected) continue;
        if (currentRow == selectedRow) return &user;
        currentRow++;
    }
    return nullptr;
}

void UserProfileBackupCustomizeDataPage::onCustomizeUser() {
    int selectedRow = m_userTable->currentRow();
    if (selectedRow < 0) return;

    auto* user = findSelectedUserByRow(selectedRow);
    if (!user) return;

    PerUserCustomizationDialog dialog(*user, this);
    if (dialog.exec() != QDialog::Accepted) return;

    int selectedCount = static_cast<int>(std::count_if(
        user->folder_selections.begin(), user->folder_selections.end(),
        [](const auto& f) { return f.selected; }));
    m_userTable->item(selectedRow, 1)->setText(tr("%1 folders selected").arg(selectedCount));
    updateSummary();
}

void UserProfileBackupCustomizeDataPage::updateSummary() {
    int totalUsers = 0;
    int totalFolders = 0;
    qint64 totalSize = 0;

    for (const auto& user : m_users) {
        if (!user.is_selected) continue;
        totalUsers++;

        for (const auto& folder : user.folder_selections) {
            if (!folder.selected) continue;
            totalFolders++;
            totalSize += folder.size_bytes;
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

UserProfileBackupSmartFiltersPage::UserProfileBackupSmartFiltersPage(SmartFilter& filter,
    QWidget* parent)
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
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
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

    m_excludeCacheCheck = new QCheckBox(tr("Exclude cache directories (WebCache, GPUCache, etc.)"),
        this);
    connect(m_excludeCacheCheck, &QCheckBox::toggled, this,
        &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusionsLayout->addWidget(m_excludeCacheCheck);

    m_excludeTempCheck = new QCheckBox(tr("Exclude temporary files (*.tmp, *.cache, *.temp)"),
        this);
    connect(m_excludeTempCheck, &QCheckBox::toggled, this,
        &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusionsLayout->addWidget(m_excludeTempCheck);

    m_excludeLockCheck = new QCheckBox(tr("Exclude lock files (*.lock, *.lck)"), this);
    connect(m_excludeLockCheck, &QCheckBox::toggled, this,
        &UserProfileBackupSmartFiltersPage::updateSummary);
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
    connect(m_viewDangerousButton, &QPushButton::clicked, this,
        &UserProfileBackupSmartFiltersPage::onViewDangerousList);
    dangerousLayout->addWidget(m_viewDangerousButton);
    layout->addLayout(dangerousLayout);

    layout->addStretch();

    // Reset button
    auto* resetLayout = new QHBoxLayout();
    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    m_resetButton->setIcon(QIcon::fromTheme("edit-undo"));
    connect(m_resetButton, &QPushButton::clicked, this,
        &UserProfileBackupSmartFiltersPage::onResetToDefaults);
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
    qint64 fileSizeMB = m_filter.max_single_file_size_bytes / sak::kBytesPerMB;
    m_maxFileSizeSpinBox->setValue(static_cast<int>(fileSizeMB));
    m_maxFileSizeSpinBox->setEnabled(m_filter.enable_file_size_limit);

    qint64 folderSizeGB = m_filter.max_folder_size_bytes / sak::kBytesPerGB;
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
    auto* dialog = new QDialog(this);
    dialog->setWindowTitle(tr("Dangerous Files & Exclusion Rules"));
    dialog->setMinimumSize(sak::kDialogWidthLarge, sak::kDialogHeightMedium);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto* layout = new QVBoxLayout(dialog);

    // Header
    auto* headerLabel = new QLabel(
        tr("<b>🛡 Files and patterns that are always excluded from backups</b>"), dialog);
    headerLabel->setWordWrap(true);
    headerLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                        "border-radius: 8px; font-size: 11pt; }")
                                            .arg(sak::ui::kColorBgWarningPanel));
    layout->addWidget(headerLabel);

    // Build rich content
    QString content;
    content += tr("<h3 style='color: %1;'>Always Excluded Files</h3>")
                   .arg(sak::ui::kColorError);
    content += "<ul>";
    for (const auto& file : m_filter.dangerous_files) {
        content += QString("<li><code>%1</code></li>").arg(file.toHtmlEscaped());
    }
    content += "</ul>";

    content += tr("<h3 style='color: %1;'>Excluded Patterns</h3>")
                   .arg(sak::ui::kColorWarning);
    content += "<ul>";
    for (const auto& pattern : m_filter.exclude_patterns) {
        content += QString("<li><code>%1</code></li>").arg(pattern.toHtmlEscaped());
    }
    content += "</ul>";

    content += tr("<h3 style='color: %1;'>Excluded Folders</h3>")
                   .arg(sak::ui::kColorTextSecondary);
    content += "<ul>";
    for (const auto& folder : m_filter.exclude_folders) {
        content += QString("<li><code>%1</code></li>").arg(folder.toHtmlEscaped());
    }
    content += "</ul>";

    auto* textBrowser = new QTextEdit(dialog);
    textBrowser->setReadOnly(true);
    textBrowser->setHtml(content);
    textBrowser->setStyleSheet(QString("QTextEdit { background-color: %1; border: 1px solid %2; "
                                        "border-radius: 6px; padding: 8px; }")
                                            .arg(sak::ui::kColorBgSurface,
                                                 sak::ui::kColorBorderDefault));
    layout->addWidget(textBrowser);

    auto* closeButton = new QPushButton(tr("Close"), dialog);
    closeButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(closeButton, 0, Qt::AlignRight);

    dialog->exec();
}

void UserProfileBackupSmartFiltersPage::updateSummary() {
    // Update filter enable flags
    m_filter.enable_file_size_limit = m_enableFileSizeLimitCheck->isChecked();
    m_filter.enable_folder_size_limit = m_enableFolderSizeLimitCheck->isChecked();

    // Update filter from UI (convert MB/GB back to bytes)
    m_filter.max_single_file_size_bytes =
        static_cast<qint64>(m_maxFileSizeSpinBox->value()) * sak::kBytesPerMB;
    m_filter.max_folder_size_bytes =
        static_cast<qint64>(m_maxFolderSizeSpinBox->value()) * sak::kBytesPerGB;

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

UserProfileBackupSettingsPage::UserProfileBackupSettingsPage(BackupManifest& manifest,
    QWidget* parent)
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
    connect(m_destinationEdit, &QLineEdit::textChanged, this,
        &UserProfileBackupSettingsPage::updateSummary);
    destLayout->addWidget(m_destinationEdit, 1);
    m_browseButton = new QPushButton(tr("Browse..."), this);
    connect(m_browseButton, &QPushButton::clicked, this,
        &UserProfileBackupSettingsPage::onBrowseDestination);
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
    m_encryptionCheck->setToolTip(tr("Protects the backup with AES-256 encryption — you'll need "
                                     "the password to restore"));
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
    m_permissionModeCombo->addItem(tr("Strip All (Recommended)"),
        static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Preserve Original"),
        static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Assign to Destination"),
        static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->addItem(tr("Hybrid (Try Preserve, Fallback Strip)"),
        static_cast<int>(PermissionMode::Hybrid));
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
    permExplainLabel->setStyleSheet(QString("QLabel { padding: 6px; color: %1; }")
        .arg(sak::ui::kColorTextMuted));
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
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
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
    connect(m_scanButton, &QPushButton::clicked, this,
        &UserProfileBackupInstalledAppsPage::onScanApps);
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
    connect(m_selectAllButton, &QPushButton::clicked, this,
        &UserProfileBackupInstalledAppsPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this,
        &UserProfileBackupInstalledAppsPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
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

/// @brief Check if any app in a category is checked
static bool categoryHasCheckedApp(QTreeWidgetItem* category) {
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        if (category->child(child_index)->checkState(0) == Qt::Checked) return true;
    }
    return false;
}

/// @brief Collect selected apps from one category into the output vector
static void collectCategoryApps(QTreeWidgetItem* category, int& total,
                                int& selected, QVector<InstalledAppInfo>& out) {
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        auto* appItem = category->child(child_index);
        total++;
        if (appItem->checkState(0) != Qt::Checked) continue;
        selected++;
        InstalledAppInfo info;
        info.name = appItem->text(0);
        info.version = appItem->text(1);
        info.publisher = appItem->text(2);
        info.category = category->text(0);
        info.selected = true;
        out.append(info);
    }
}

void UserProfileBackupInstalledAppsPage::updateNextButtonText() {
    auto* wiz = wizard();
    if (!wiz) return;

    bool hasSelection = false;
    for (int category_index =
        0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        if (categoryHasCheckedApp(m_appTree->topLevelItem(category_index))) {
            hasSelection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton,
                       hasSelection ? tr("Next >") : tr("Skip >"));
}

bool UserProfileBackupInstalledAppsPage::isComplete() const {
    // Always complete — app selection is optional
    return true;
}

QString UserProfileBackupInstalledAppsPage::categorizeApp(const QString& name,
    const QString& publisher) {
    Q_UNUSED(publisher)

    struct CategoryRule {
        const char* category;
        std::initializer_list<const char*> keywords;
    };
    static const CategoryRule kRules[] = {
        {"Browsers", {"chrome", "firefox", "edge", "opera", "brave", "vivaldi", "browser"}},
        {"Development", {"visual studio", "vscode", "jetbrains", "intellij", "pycharm",
            "webstorm", "rider", "clion", "android studio", "eclipse", "sublime",
            "notepad++", "atom", "code"}},
        {"Productivity", {"office", "word", "excel", "powerpoint", "outlook", "onenote",
            "libreoffice", "openoffice"}},
        {"Communication", {"discord", "slack", "teams", "zoom", "skype", "telegram",
            "signal", "whatsapp"}},
        {"Gaming", {"steam", "epic games", "origin", "battle.net", "gog", "ubisoft", "game"}},
        {"Graphics & Design", {"photoshop", "illustrator", "gimp", "blender", "inkscape",
            "paint", "krita", "figma", "canva"}},
        {"Media", {"vlc", "spotify", "itunes", "audacity", "obs", "handbrake", "media",
            "player", "foobar", "winamp"}},
        {"Utilities", {"7-zip", "winrar", "peazip", "ccleaner", "everything",
            "totalcommander", "wiztree", "treesize", "windirstat", "revo"}},
        {"Security", {"norton", "kaspersky", "malwarebytes", "avast", "avg", "bitdefender",
            "security", "antivirus"}},
        {"Drivers & Hardware", {"nvidia", "amd", "realtek", "intel", "driver", "logitech",
            "corsair", "razer"}},
    };

    const QString app_name_lower = name.toLower();
    for (const auto& rule : kRules) {
        for (const char* keyword : rule.keywords) {
            if (app_name_lower.contains(QLatin1String(keyword))) {
                return QCoreApplication::translate(
                    "UserProfileBackupInstalledAppsPage", rule.category);
            }
        }
    }
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

    connect(watcher, &QFutureWatcher<QVector<InstalledAppInfo>>::finished, this, [safeThis,
        watcher]() {
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
    } else if (item->parent()) {
        updateParentCheckState(item->parent());
    }

    m_appTree->blockSignals(false);

    // Update summary and save to wizard
    int total = 0;
    int selected = 0;
    QVector<InstalledAppInfo> selectedApps;

    for (int category_index =
        0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        collectCategoryApps(m_appTree->topLevelItem(category_index), total, selected, selectedApps);
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
    for (int category_index =
        0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        category->setCheckState(0, Qt::Checked);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            category->child(child_index)->setCheckState(0, Qt::Checked);
        }
    }
    m_appTree->blockSignals(false);

    // Recalculate summary and persist selection to wizard
    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

void UserProfileBackupInstalledAppsPage::onSelectNone() {
    m_appTree->blockSignals(true);
    for (int category_index =
        0; category_index < m_appTree->topLevelItemCount(); ++category_index) {
        auto* category = m_appTree->topLevelItem(category_index);
        category->setCheckState(0, Qt::Unchecked);
        for (int child_index = 0; child_index < category->childCount(); ++child_index) {
            category->child(child_index)->setCheckState(0, Qt::Unchecked);
        }
    }
    m_appTree->blockSignals(false);

    // Trigger update
    if (m_appTree->topLevelItemCount() > 0) {
        onItemChanged(m_appTree->topLevelItem(0), 0);
    }
}

// ============================================================================
// UserProfileBackupAppDataPage
// ============================================================================

/// @brief Common application data sources to detect
static QVector<AppDataSourceInfo> getCommonAppDataSources() {
    QVector<AppDataSourceInfo> sources = {
        // Browsers
        {"Chrome Profiles", "Browsers", "AppData/Local/Google/Chrome/User Data", 0, false, true},
        {"Firefox Profiles", "Browsers", "AppData/Roaming/Mozilla/Firefox/Profiles", 0, false, true},
        {"Edge Profiles", "Browsers", "AppData/Local/Microsoft/Edge/User Data", 0, false, true},
        {"Brave Profiles", "Browsers", "AppData/Local/BraveSoftware/Brave-Browser/User Data", 0, false, true},
        {"Opera Profiles", "Browsers", "AppData/Roaming/Opera Software/Opera Stable", 0, false, true},
        {"Vivaldi Profiles", "Browsers", "AppData/Local/Vivaldi/User Data", 0, false, true},

        // Email Clients
        {"Thunderbird Profiles", "Email", "AppData/Roaming/Thunderbird/Profiles", 0, false, true},
        {"Outlook Data", "Email", "AppData/Local/Microsoft/Outlook", 0, false, true},

        // IDEs & Editors
        {"VS Code Settings", "Development", "AppData/Roaming/Code/User", 0, false, true},
        {"VS Code Extensions", "Development", ".vscode/extensions", 0, false, true},
        {"JetBrains Settings", "Development", "AppData/Roaming/JetBrains", 0, false, true},
        {"Sublime Text Settings", "Development", "AppData/Roaming/Sublime Text 3", 0, false, true},
        {"Notepad++ Settings", "Development", "AppData/Roaming/Notepad++", 0, false, true},

        // Communication
        {"Discord Data", "Communication", "AppData/Roaming/discord", 0, false, true},
        {"Telegram Data", "Communication", "AppData/Roaming/Telegram Desktop", 0, false, true},
        {"Signal Data", "Communication", "AppData/Roaming/Signal", 0, false, true},
        {"Slack Data", "Communication", "AppData/Roaming/Slack", 0, false, true},

        // Gaming
        {"Steam Config", "Gaming", "AppData/Local/Steam", 0, false, true},
        {"Epic Games Settings", "Gaming", "AppData/Local/EpicGamesLauncher", 0, false, true},
        {"Minecraft Data", "Gaming", "AppData/Roaming/.minecraft", 0, false, true},

        // Productivity
        {"OneNote Cache", "Productivity", "AppData/Local/Microsoft/OneNote", 0, false, true},
        {"Sticky Notes", "Productivity", "AppData/Local/Packages/Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/LocalState", 0, false, true},

        // Media
        {"Spotify Data", "Media", "AppData/Roaming/Spotify", 0, false, true},
        {"VLC Settings", "Media", "AppData/Roaming/vlc", 0, false, true},
        {"OBS Studio", "Media", "AppData/Roaming/obs-studio", 0, false, true},

        // Utilities
        {"PuTTY Sessions", "Utilities", "AppData/Roaming/PuTTY", 0, false, true},
        {"WinSCP Settings", "Utilities", "AppData/Roaming/WinSCP", 0, false, true},
        {"FileZilla Settings", "Utilities", "AppData/Roaming/FileZilla", 0, false, true},
        {"PowerShell Profile", "Utilities", "Documents/PowerShell", 0, false, true},
        {"Windows Terminal Settings", "Utilities", "AppData/Local/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState", 0, false, true},

        // System
        {"SSH Keys", "System", ".ssh", 0, false, true},
        {"Git Config", "System", ".gitconfig", 0, false, true},
        {"npm Config", "System", "AppData/Roaming/npm", 0, false, true},
        {"pip Config", "System", "AppData/Roaming/pip", 0, false, true},
    };
    return sources;
}

UserProfileBackupAppDataPage::UserProfileBackupAppDataPage(QVector<UserProfile>& users,
    QWidget* parent)
    : QWizardPage(parent)
    , m_users(users)
{
    setTitle(tr("Application Data"));
    setSubTitle(tr("Select application data and settings to include in the backup"));

    setupUi();
}

void UserProfileBackupAppDataPage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    auto* instructionLabel = new QLabel(tr(
        "Click <b>Scan App Data</b> to detect application data on selected users. "
        "This backs up application settings, profiles, and configurations — "
        "not the applications themselves (use the Installed Applications step for that)."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);

    auto* scanLayout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan App Data"), this);
    connect(m_scanButton, &QPushButton::clicked, this,
        &UserProfileBackupAppDataPage::onScanAppData);
    scanLayout->addWidget(m_scanButton);
    m_statusLabel = new QLabel(tr("Click Scan App Data to begin"), this);
    scanLayout->addWidget(m_statusLabel, 1);
    layout->addLayout(scanLayout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_appDataTree = new QTreeWidget(this);
    m_appDataTree->setHeaderLabels({tr("Application Data"), tr("Path"), tr("Size")});
    m_appDataTree->setAlternatingRowColors(true);
    m_appDataTree->setRootIsDecorated(true);
    m_appDataTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_appDataTree->setEnabled(false);
    connect(m_appDataTree, &QTreeWidget::itemChanged,
            this, &UserProfileBackupAppDataPage::onItemChanged);
    layout->addWidget(m_appDataTree);

    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this,
        &UserProfileBackupAppDataPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this,
        &UserProfileBackupAppDataPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No application data scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupAppDataPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupAppDataPage::isComplete() const {
    return true; // Always complete — app data selection is optional
}

void UserProfileBackupAppDataPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupAppDataPage::onScanAppData() {
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning application data..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_appDataTree->clear();

    QVector<AppDataSourceInfo> allSources;
    auto commonSources = getCommonAppDataSources();

    // Check each common source against selected users
    for (const auto& user : m_users) {
        if (!user.is_selected) continue;

        for (auto source : commonSources) {
            QString fullPath = user.profile_path + "/" + source.relative_path;
            QFileInfo info(fullPath);
            if (info.exists()) {
                source.exists = true;
                if (info.isDir()) {
                    // Calculate directory size
                    qint64 dirSize = 0;
                    QDirIterator it(fullPath, QDir::Files | QDir::NoDotAndDotDot,
                                    QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        it.next();
                        dirSize += it.fileInfo().size();
                    }
                    source.size_bytes = dirSize;
                } else {
                    source.size_bytes = info.size();
                }
                allSources.append(source);
            }
        }
    }

    m_scanned = true;
    m_scanButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_appDataTree->setEnabled(true);
    m_scanProgress->setVisible(false);

    m_statusLabel->setText(tr("Found %1 application data source(s)").arg(allSources.size()));
    populateTree(allSources);
    updateNextButtonText();
}

void UserProfileBackupAppDataPage::populateTree(const QVector<AppDataSourceInfo>& sources) {
    m_appDataTree->blockSignals(true);
    m_appDataTree->clear();

    // Group by category
    QMap<QString, QVector<const AppDataSourceInfo*>> categories;
    for (const auto& source : sources) {
        categories[source.category].append(&source);
    }

    int totalSelected = 0;
    int total = 0;

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* categoryItem = new QTreeWidgetItem(m_appDataTree);
        categoryItem->setText(0, it.key());
        categoryItem->setFlags(categoryItem->flags() | Qt::ItemIsUserCheckable);

        int catSelected = 0;
        for (const auto* source : it.value()) {
            auto* item = new QTreeWidgetItem(categoryItem);
            item->setText(0, source->name);
            item->setText(1, source->relative_path);
            double sizeMB = source->size_bytes / sak::kBytesPerMBf;
            item->setText(2, QString("%1 MB").arg(sizeMB, 0, 'f', 1));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, source->selected ? Qt::Checked : Qt::Unchecked);
            if (source->selected) catSelected++;
            total++;
        }

        totalSelected += catSelected;
        categoryItem->setCheckState(0, catSelected == it.value().size() ? Qt::Checked :
                                       catSelected > 0 ? Qt::PartiallyChecked : Qt::Unchecked);
        categoryItem->setExpanded(true);
    }

    m_appDataTree->blockSignals(false);
    m_summaryLabel->setText(tr("%1 of %2 application data source(s) selected")
                                .arg(totalSelected).arg(total));
}

void UserProfileBackupAppDataPage::updateParentCheckState(QTreeWidgetItem* parent) {
    int checkedCount = 0;
    int totalCount = parent->childCount();

    for (int i = 0; i < totalCount; ++i) {
        if (parent->child(i)->checkState(0) == Qt::Checked) checkedCount++;
    }

    if (checkedCount == 0) parent->setCheckState(0, Qt::Unchecked);
    else if (checkedCount == totalCount) parent->setCheckState(0, Qt::Checked);
    else parent->setCheckState(0, Qt::PartiallyChecked);
}

void UserProfileBackupAppDataPage::updateNextButtonText() {
    auto* wiz = wizard();
    if (!wiz) return;

    bool hasSelection = false;
    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            if (category->child(j)->checkState(0) == Qt::Checked) {
                hasSelection = true;
                break;
            }
        }
        if (hasSelection) break;
    }

    wiz->setButtonText(QWizard::NextButton, hasSelection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupAppDataPage::onItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;

    m_appDataTree->blockSignals(true);

    if (item->childCount() > 0) {
        Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
        }
    } else if (item->parent()) {
        updateParentCheckState(item->parent());
    }

    m_appDataTree->blockSignals(false);

    // Update summary and save to wizard
    int total = 0;
    int selected = 0;
    QVector<AppDataSourceInfo> selectedSources;

    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            auto* appItem = category->child(j);
            total++;
            if (appItem->checkState(0) == Qt::Checked) {
                selected++;
                AppDataSourceInfo info;
                info.name = appItem->text(0);
                info.category = category->text(0);
                info.relative_path = appItem->text(1);
                info.selected = true;
                selectedSources.append(info);
            }
        }
    }

    m_summaryLabel->setText(tr("%1 of %2 application data source(s) selected")
                                .arg(selected).arg(total));

    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz) {
        wiz->setAppDataSources(selectedSources);
    }

    updateNextButtonText();
}

void UserProfileBackupAppDataPage::onSelectAll() {
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

void UserProfileBackupAppDataPage::onSelectNone() {
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
// UserProfileBackupKnownNetworksPage
// ============================================================================

UserProfileBackupKnownNetworksPage::UserProfileBackupKnownNetworksPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Known WiFi Networks"));
    setSubTitle(tr("Scan and select WiFi network profiles to include in the backup"));

    setupUi();
}

void UserProfileBackupKnownNetworksPage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    auto* instructionLabel = new QLabel(tr(
        "Click <b>Scan Networks</b> to detect saved WiFi profiles. "
        "Selected profiles will be exported and included in the backup "
        "for easy restoration on the destination machine."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);

    auto* scanLayout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Networks"), this);
    connect(m_scanButton, &QPushButton::clicked, this,
        &UserProfileBackupKnownNetworksPage::onScanNetworks);
    scanLayout->addWidget(m_scanButton);
    m_statusLabel = new QLabel(tr("Click Scan Networks to begin"), this);
    scanLayout->addWidget(m_statusLabel, 1);
    layout->addLayout(scanLayout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_networkTree = new QTreeWidget(this);
    m_networkTree->setHeaderLabels({tr("Network Name (SSID)"), tr("Security Type")});
    m_networkTree->setAlternatingRowColors(true);
    m_networkTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_networkTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_networkTree->setEnabled(false);
    connect(m_networkTree, &QTreeWidget::itemChanged,
            this, &UserProfileBackupKnownNetworksPage::onItemChanged);
    layout->addWidget(m_networkTree);

    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this,
        &UserProfileBackupKnownNetworksPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this,
        &UserProfileBackupKnownNetworksPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No WiFi networks scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupKnownNetworksPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupKnownNetworksPage::isComplete() const {
    return true; // Always complete — network backup is optional
}

void UserProfileBackupKnownNetworksPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupKnownNetworksPage::onScanNetworks() {
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning WiFi profiles..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_networkTree->clear();

    const QVector<WifiProfileInfo> profiles = sak::scanAllWifiProfiles();

    m_scanned = true;
    m_scanButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_networkTree->setEnabled(true);
    m_scanProgress->setVisible(false);

    m_statusLabel->setText(tr("Found %1 WiFi profile(s)").arg(profiles.size()));
    populateTree(profiles);
    updateNextButtonText();
}

void UserProfileBackupKnownNetworksPage::populateTree(const QVector<WifiProfileInfo>& profiles) {
    m_networkTree->blockSignals(true);
    m_networkTree->clear();

    for (const auto& profile : profiles) {
        auto* item = new QTreeWidgetItem(m_networkTree);
        item->setText(0, profile.profile_name);
        item->setText(1, profile.security_type.isEmpty() ? tr("Unknown") : profile.security_type);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, profile.selected ? Qt::Checked : Qt::Unchecked);
    }

    m_networkTree->blockSignals(false);
    int selectedCount = 0;
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        if (m_networkTree->topLevelItem(i)->checkState(0) == Qt::Checked) selectedCount++;
    }
    m_summaryLabel->setText(tr("%1 of %2 WiFi profile(s) selected")
                                .arg(selectedCount).arg(profiles.size()));
}

void UserProfileBackupKnownNetworksPage::updateNextButtonText() {
    auto* wiz = wizard();
    if (!wiz) return;

    bool hasSelection = false;
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        if (m_networkTree->topLevelItem(i)->checkState(0) == Qt::Checked) {
            hasSelection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, hasSelection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupKnownNetworksPage::onItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0) return;

    int selected = 0;
    int total = m_networkTree->topLevelItemCount();
    QVector<WifiProfileInfo> selectedProfiles;

    for (int i = 0; i < total; ++i) {
        auto* treeItem = m_networkTree->topLevelItem(i);
        if (treeItem->checkState(0) == Qt::Checked) {
            selected++;
            WifiProfileInfo info;
            info.profile_name = treeItem->text(0);
            info.security_type = treeItem->text(1);
            info.selected = true;
            selectedProfiles.append(info);
        }
    }

    Q_UNUSED(item)
    m_summaryLabel->setText(tr("%1 of %2 WiFi profile(s) selected").arg(selected).arg(total));

    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz) {
        wiz->setWifiProfiles(selectedProfiles);
    }

    updateNextButtonText();
}

void UserProfileBackupKnownNetworksPage::onSelectAll() {
    m_networkTree->blockSignals(true);
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        m_networkTree->topLevelItem(i)->setCheckState(0, Qt::Checked);
    }
    m_networkTree->blockSignals(false);
    if (m_networkTree->topLevelItemCount() > 0) {
        onItemChanged(m_networkTree->topLevelItem(0), 0);
    }
}

void UserProfileBackupKnownNetworksPage::onSelectNone() {
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
// UserProfileBackupEthernetSettingsPage
// ============================================================================

UserProfileBackupEthernetSettingsPage::UserProfileBackupEthernetSettingsPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Ethernet Settings"));
    setSubTitle(tr("Scan and select ethernet adapter configurations to include in the backup"));

    setupUi();
}

void UserProfileBackupEthernetSettingsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    auto* instructionLabel = new QLabel(tr(
        "Click <b>Scan Ethernet</b> to detect network adapter configurations. "
        "Selected adapter settings (IP, DNS, gateway) will be saved to the backup. "
        "This is especially useful for static IP configurations."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);

    auto* scanLayout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Ethernet"), this);
    connect(m_scanButton, &QPushButton::clicked, this,
        &UserProfileBackupEthernetSettingsPage::onScanEthernet);
    scanLayout->addWidget(m_scanButton);
    m_statusLabel = new QLabel(tr("Click Scan Ethernet to begin"), this);
    scanLayout->addWidget(m_statusLabel, 1);
    layout->addLayout(scanLayout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_ethernetTable = new QTableWidget(0, 7, this);
    m_ethernetTable->setHorizontalHeaderLabels({
        tr("Select"), tr("Adapter"), tr("DHCP"), tr("IP Address"),
        tr("Subnet"), tr("Gateway"), tr("DNS")
    });
    m_ethernetTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_ethernetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ethernetTable->verticalHeader()->setVisible(false);
    m_ethernetTable->setEnabled(false);
    layout->addWidget(m_ethernetTable);

    auto* buttonLayout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    connect(m_selectAllButton, &QPushButton::clicked, this,
        &UserProfileBackupEthernetSettingsPage::onSelectAll);
    buttonLayout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    connect(m_selectNoneButton, &QPushButton::clicked, this,
        &UserProfileBackupEthernetSettingsPage::onSelectNone);
    buttonLayout->addWidget(m_selectNoneButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(QString("QLabel { padding: 10px; background-color: %1; "
                                          "border-radius: 10px; }")
                                              .arg(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No ethernet adapters scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupEthernetSettingsPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupEthernetSettingsPage::isComplete() const {
    return true; // Always complete — ethernet backup is optional
}

void UserProfileBackupEthernetSettingsPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupEthernetSettingsPage::onScanEthernet() {
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning ethernet adapters..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_ethernetTable->setRowCount(0);

    QVector<EthernetConfigInfo> configs;

    // Use netsh to get interface configurations
    QProcess process;
    process.start("netsh", {"interface", "ipv4", "show", "config"});
    if (!process.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logError("netsh failed to start for ethernet adapter scan");
        m_scanButton->setEnabled(true);
        m_scanProgress->setVisible(false);
        m_statusLabel->setText(tr("Failed to start ethernet scan"));
        return;
    }
    if (!process.waitForFinished(sak::kTimeoutNetworkReadMs)) {
        sak::logError("Timed out scanning ethernet adapters for backup");
        process.kill();
        process.waitForFinished(2000);
        m_scanButton->setEnabled(true);
        m_scanProgress->setVisible(false);
        m_statusLabel->setText(tr("Ethernet scan timed out"));
        return;
    }
    QString output = QString::fromLocal8Bit(process.readAllStandardOutput());

    // Parse adapter blocks
    EthernetConfigInfo current;
    bool inAdapter = false;

    for (const QString& line : output.split('\n')) {
        QString trimmed = line.trimmed();

        if (trimmed.startsWith("Configuration for interface")) {
            if (inAdapter && !current.adapter_name.isEmpty()) {
                configs.append(current);
            }
            current = EthernetConfigInfo();
            // Extract adapter name between quotes
            int firstQuote = trimmed.indexOf('"');
            int lastQuote = trimmed.lastIndexOf('"');
            if (firstQuote >= 0 && lastQuote > firstQuote) {
                current.adapter_name = trimmed.mid(firstQuote + 1,
                                                    lastQuote - firstQuote - 1);
            }
            inAdapter = true;
        } else if (inAdapter) {
            if (trimmed.startsWith("DHCP enabled:", Qt::CaseInsensitive)) {
                current.dhcp_enabled = trimmed.contains("Yes", Qt::CaseInsensitive);
            } else if (trimmed.startsWith("IP Address:", Qt::CaseInsensitive)) {
                int colonIdx = trimmed.indexOf(':');
                if (colonIdx >= 0) current.ip_address = trimmed.mid(colonIdx + 1).trimmed();
            } else if (trimmed.startsWith("Subnet Prefix:", Qt::CaseInsensitive) ||
                       trimmed.startsWith("SubnetMask:", Qt::CaseInsensitive)) {
                int colonIdx = trimmed.indexOf(':');
                if (colonIdx >= 0) current.subnet_mask = trimmed.mid(colonIdx + 1).trimmed();
            } else if (trimmed.startsWith("Default Gateway:", Qt::CaseInsensitive)) {
                int colonIdx = trimmed.indexOf(':');
                if (colonIdx >= 0) current.default_gateway = trimmed.mid(colonIdx + 1).trimmed();
            } else if (trimmed.contains("DNS Server", Qt::CaseInsensitive) ||
                       trimmed.startsWith("Statically Configured DNS", Qt::CaseInsensitive)) {
                int colonIdx = trimmed.indexOf(':');
                if (colonIdx >= 0) {
                    QString dns = trimmed.mid(colonIdx + 1).trimmed();
                    if (!dns.isEmpty()) {
                        if (current.dns_primary.isEmpty()) current.dns_primary = dns;
                        else if (current.dns_secondary.isEmpty()) current.dns_secondary = dns;
                    }
                }
            }
        }
    }
    // Don't forget the last adapter
    if (inAdapter && !current.adapter_name.isEmpty()) {
        configs.append(current);
    }

    m_scanned = true;
    m_scanButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_ethernetTable->setEnabled(true);
    m_scanProgress->setVisible(false);

    m_statusLabel->setText(tr("Found %1 ethernet adapter(s)").arg(configs.size()));
    populateTable(configs);
    updateNextButtonText();
}

void UserProfileBackupEthernetSettingsPage::populateTable(
    const QVector<EthernetConfigInfo>& configs) {
    m_ethernetTable->setRowCount(0);

    for (const auto& config : configs) {
        int row = m_ethernetTable->rowCount();
        m_ethernetTable->insertRow(row);

        auto* checkItem = new QTableWidgetItem();
        checkItem->setCheckState(Qt::Checked);
        m_ethernetTable->setItem(row, 0, checkItem);

        auto* nameItem = new QTableWidgetItem(config.adapter_name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 1, nameItem);

        auto* dhcpItem = new QTableWidgetItem(config.dhcp_enabled ? tr("Yes") : tr("No"));
        dhcpItem->setFlags(dhcpItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 2, dhcpItem);

        auto* ipItem = new QTableWidgetItem(config.ip_address);
        ipItem->setFlags(ipItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 3, ipItem);

        auto* subnetItem = new QTableWidgetItem(config.subnet_mask);
        subnetItem->setFlags(subnetItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 4, subnetItem);

        auto* gwItem = new QTableWidgetItem(config.default_gateway);
        gwItem->setFlags(gwItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 5, gwItem);

        QString dns = config.dns_primary;
        if (!config.dns_secondary.isEmpty()) dns += ", " + config.dns_secondary;
        auto* dnsItem = new QTableWidgetItem(dns);
        dnsItem->setFlags(dnsItem->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, 6, dnsItem);
    }

    int total = m_ethernetTable->rowCount();
    m_summaryLabel->setText(tr("%1 ethernet adapter(s) selected").arg(total));
}

void UserProfileBackupEthernetSettingsPage::updateNextButtonText() {
    auto* wiz = wizard();
    if (!wiz) return;

    bool hasSelection = false;
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        if (m_ethernetTable->item(i, 0)->checkState() == Qt::Checked) {
            hasSelection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, hasSelection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupEthernetSettingsPage::onSelectAll() {
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, 0)->setCheckState(Qt::Checked);
    }

    int total = m_ethernetTable->rowCount();
    QVector<EthernetConfigInfo> configs;
    for (int i = 0; i < total; ++i) {
        EthernetConfigInfo info;
        info.adapter_name = m_ethernetTable->item(i, 1)->text();
        info.dhcp_enabled = m_ethernetTable->item(i, 2)->text() == tr("Yes");
        info.ip_address = m_ethernetTable->item(i, 3)->text();
        info.subnet_mask = m_ethernetTable->item(i, 4)->text();
        info.default_gateway = m_ethernetTable->item(i, 5)->text();
        info.selected = true;
        configs.append(info);
    }
    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz) wiz->setEthernetConfigs(configs);

    m_summaryLabel->setText(tr("%1 ethernet adapter(s) selected").arg(total));
    updateNextButtonText();
}

void UserProfileBackupEthernetSettingsPage::onSelectNone() {
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, 0)->setCheckState(Qt::Unchecked);
    }

    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz) wiz->setEthernetConfigs({});

    m_summaryLabel->setText(tr("0 ethernet adapter(s) selected"));
    updateNextButtonText();
}

} // namespace sak
