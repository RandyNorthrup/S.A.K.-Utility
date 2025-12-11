#include "sak/user_profile_backup_wizard.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/user_profile_backup_worker.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDateTime>
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
        "<li><b>Custom Folders</b> - Add any additional folders you need</li>"
        "</ul>"
        
        "<h3>Smart Filtering</h3>"
        "<p>The wizard automatically excludes dangerous files that can corrupt profiles:</p>"
        "<ul>"
        "<li>Registry hives (NTUSER.DAT, UsrClass.dat)</li>"
        "<li>Lock files and cache directories</li>"
        "<li>Temporary files and system folders</li>"
        "</ul>"
        
        "<h3>Safe Restoration</h3>"
        "<p>Backups include metadata for intelligent restoration:</p>"
        "<ul>"
        "<li>User mapping (source user → destination user)</li>"
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
    m_userTable->setHorizontalHeaderLabels({tr("✓"), tr("Username"), tr("Profile Path"), tr("Est. Size")});
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
    m_summaryLabel->setStyleSheet("QLabel { padding: 8px; background-color: #e8f4fd; border-radius: 4px; }");
    layout->addWidget(m_summaryLabel);
    updateSummary();
}

void UserProfileBackupSelectUsersPage::initializePage() {
    // Auto-scan on first entry
    if (!m_scanned) {
        QTimer::singleShot(200, this, &UserProfileBackupSelectUsersPage::onScanUsers);
    }
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
    
    // User table (3 columns: username, folders selected, customize button)
    m_userTable = new QTableWidget(0, 3, this);
    m_userTable->setHorizontalHeaderLabels({tr("Username"), tr("Folders Selected"), tr("Actions")});
    m_userTable->horizontalHeader()->setStretchLastSection(false);
    m_userTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_userTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
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
    m_summaryLabel->setStyleSheet("QLabel { padding: 8px; background-color: #e8f4fd; border-radius: 4px; }");
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
        
        // Customize button
        auto* customizeBtn = new QPushButton(tr("Customize..."), this);
        customizeBtn->setProperty("userIndex", row);
        connect(customizeBtn, &QPushButton::clicked, this, [this, customizeBtn]() {
            m_userTable->selectRow(customizeBtn->property("userIndex").toInt());
            onCustomizeUser();
        });
        m_userTable->setCellWidget(row, 2, customizeBtn);
        
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
    
    double totalGB = totalSize / (1024.0 * 1024.0 * 1024.0);
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
    
    // Filter settings grid
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
        "⚠ <b>Always excluded:</b> Registry hives (NTUSER.DAT, UsrClass.dat), system folders"
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
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("QLabel { padding: 8px; background-color: #e8f4fd; border-radius: 4px; }");
    layout->addWidget(m_summaryLabel);
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
    qint64 fileSizeMB = m_filter.max_single_file_size / (1024 * 1024);
    m_maxFileSizeSpinBox->setValue(static_cast<int>(fileSizeMB));
    m_maxFileSizeSpinBox->setEnabled(m_filter.enable_file_size_limit);
    
    qint64 folderSizeGB = m_filter.max_folder_size / (1024 * 1024 * 1024);
    m_maxFolderSizeSpinBox->setValue(static_cast<int>(folderSizeGB));
    m_maxFolderSizeSpinBox->setEnabled(m_filter.enable_folder_size_limit);
    
    // These checkboxes just indicate if patterns exist (always enabled for now)
    m_excludeCacheCheck->setChecked(!m_filter.exclude_folders.isEmpty());
    m_excludeTempCheck->setChecked(!m_filter.exclude_patterns.isEmpty());
    m_excludeLockCheck->setChecked(!m_filter.dangerous_files.isEmpty());
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
    m_filter.max_single_file_size = static_cast<qint64>(m_maxFileSizeSpinBox->value()) * 1024 * 1024;
    m_filter.max_folder_size = static_cast<qint64>(m_maxFolderSizeSpinBox->value()) * 1024 * 1024 * 1024;
    
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

void UserProfileBackupSettingsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Instructions
    auto* instructionLabel = new QLabel(tr(
        "Choose where to save the backup and configure additional options."
    ), this);
    instructionLabel->setWordWrap(true);
    layout->addWidget(instructionLabel);
    
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
    
    // Compression (future feature)
    auto* compressionLayout = new QHBoxLayout();
    compressionLayout->addWidget(new QLabel(tr("Compression:"), this));
    m_compressionCombo = new QComboBox(this);
    m_compressionCombo->addItems({tr("None"), tr("Fast"), tr("Balanced"), tr("Maximum")});
    m_compressionCombo->setCurrentIndex(0);
    m_compressionCombo->setEnabled(false); // Not implemented yet
    compressionLayout->addWidget(m_compressionCombo);
    compressionLayout->addWidget(new QLabel(tr("(Coming soon)"), this));
    compressionLayout->addStretch();
    layout->addLayout(compressionLayout);
    
    // Encryption (future feature)
    auto* encryptionLayout = new QHBoxLayout();
    m_encryptionCheck = new QCheckBox(tr("Encrypt backup"), this);
    m_encryptionCheck->setEnabled(false); // Not implemented yet
    connect(m_encryptionCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_passwordEdit->setEnabled(checked);
    });
    encryptionLayout->addWidget(m_encryptionCheck);
    encryptionLayout->addWidget(new QLabel(tr("(Coming soon)"), this));
    encryptionLayout->addStretch();
    layout->addLayout(encryptionLayout);
    
    auto* passwordLayout = new QHBoxLayout();
    passwordLayout->addWidget(new QLabel(tr("Password:"), this));
    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setEnabled(false);
    passwordLayout->addWidget(m_passwordEdit);
    layout->addLayout(passwordLayout);
    
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
    permExplainLabel->setStyleSheet("QLabel { padding: 4px; color: #555; }");
    layout->addWidget(permExplainLabel);
    
    // Verification
    m_verifyCheck = new QCheckBox(tr("Verify files after backup (MD5 checksums)"), this);
    m_verifyCheck->setChecked(true);
    layout->addWidget(m_verifyCheck);
    
    layout->addStretch();
    
    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet("QLabel { padding: 8px; background-color: #e8f4fd; border-radius: 4px; }");
    layout->addWidget(m_summaryLabel);
    
    // Register destination field for validation
    registerField("destination*", m_destinationEdit);
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
// UserProfileBackupExecutePage
// ============================================================================

UserProfileBackupExecutePage::UserProfileBackupExecutePage(BackupManifest& manifest,
                                     const QVector<UserProfile>& users,
                                     const QString& destinationPath,
                                     QWidget* parent)
    : QWizardPage(parent)
    , m_manifest(manifest)
    , m_users(users)
    , m_destinationPath(destinationPath)
{
    setTitle(tr("Execute Backup"));
    setSubTitle(tr("Backup in progress..."));
    
    setupUi();
}

void UserProfileBackupExecutePage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Status label
    m_statusLabel = new QLabel(tr("Ready to start backup"), this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: bold; }");
    layout->addWidget(m_statusLabel);
    
    // Current user being backed up
    m_currentUserLabel = new QLabel(this);
    layout->addWidget(m_currentUserLabel);
    
    // Overall progress
    layout->addWidget(new QLabel(tr("Overall Progress:"), this));
    m_overallProgress = new QProgressBar(this);
    m_overallProgress->setFormat("%v / %m (%p%)");
    layout->addWidget(m_overallProgress);
    
    // Current file/folder progress
    layout->addWidget(new QLabel(tr("Current Operation:"), this));
    m_currentProgress = new QProgressBar(this);
    m_currentProgress->setFormat("%v / %m (%p%)");
    layout->addWidget(m_currentProgress);
    
    // Log output
    layout->addWidget(new QLabel(tr("Log:"), this));
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(150);
    layout->addWidget(m_logEdit);
    
    // Start button
    m_startButton = new QPushButton(tr("Start Backup"), this);
    m_startButton->setIcon(QIcon::fromTheme("media-playback-start"));
    connect(m_startButton, &QPushButton::clicked, this, &UserProfileBackupExecutePage::onStartBackup);
    layout->addWidget(m_startButton);
    
    layout->addStretch();
}

void UserProfileBackupExecutePage::initializePage() {
    m_statusLabel->setText(tr("Ready to start backup. Click Start Backup to begin."));
    m_startButton->setEnabled(true);
}

bool UserProfileBackupExecutePage::isComplete() const {
    return m_completed;
}

void UserProfileBackupExecutePage::onStartBackup() {
    if (m_started) return;
    
    m_started = true;
    m_startButton->setEnabled(false);
    m_statusLabel->setText(tr("Backup in progress..."));
    
    appendLog(tr("=== Backup Started ==="));
    appendLog(tr("Destination: %1").arg(m_destinationPath));
    appendLog(tr("Users to backup: %1").arg(m_users.size()));
    
    // Get smart filter and permission mode from wizard
    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (!wiz) {
        appendLog(tr("ERROR: Could not access wizard"));
        return;
    }
    
    SmartFilter smartFilter = wiz->getSmartFilter();
    
    // Get permission mode from settings page (through wizard field)
    // Default to StripAll if not found
    PermissionMode permissionMode = PermissionMode::StripAll;
    
    // Create and configure backup worker
    auto worker = new UserProfileBackupWorker(this);
    
    // Connect signals for progress tracking
    connect(worker, &UserProfileBackupWorker::overallProgress, 
            this, &UserProfileBackupExecutePage::onBackupProgress);
    connect(worker, &UserProfileBackupWorker::logMessage,
            this, [this](const QString& message, bool isWarning) {
                appendLog(isWarning ? QString("[WARNING] %1").arg(message) : message);
            });
    connect(worker, &UserProfileBackupWorker::statusUpdate,
            this, [this](const QString& username, const QString& operation) {
                m_statusLabel->setText(tr("Backing up %1: %2").arg(username, operation));
            });
    connect(worker, &UserProfileBackupWorker::backupComplete,
            this, [this, worker](bool success, const QString& message, const BackupManifest&) {
                onBackupComplete(success, message);
                worker->deleteLater();
            });
    
    // Start backup operation
    worker->startBackup(m_manifest, m_users, m_destinationPath, smartFilter, permissionMode);
    
    // Configure progress bars
    m_overallProgress->setRange(0, m_users.size());
    m_currentProgress->setRange(0, 0); // Indeterminate for file operations
}

void UserProfileBackupExecutePage::onBackupProgress(int current, int total, qint64 bytes, qint64 totalBytes) {
    (void)bytes;
    (void)totalBytes;
    m_overallProgress->setMaximum(total);
    m_overallProgress->setValue(current);
}

void UserProfileBackupExecutePage::onBackupComplete(bool success, const QString& message) {
    m_completed = true;
    m_statusLabel->setText(success ? tr("Backup completed successfully!") : tr("Backup failed!"));
    appendLog(success ? tr("=== Backup Complete ===") : tr("=== Backup Failed ==="));
    appendLog(message);
    Q_EMIT completeChanged();
}

void UserProfileBackupExecutePage::onLogMessage(const QString& message) {
    appendLog(message);
}

void UserProfileBackupExecutePage::appendLog(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logEdit->append(QString("[%1] %2").arg(timestamp, message));
}

} // namespace sak
