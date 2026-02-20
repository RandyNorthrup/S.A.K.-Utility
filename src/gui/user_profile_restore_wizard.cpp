#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_user_scanner.h"
#include "sak/user_profile_restore_worker.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QScrollBar>

namespace sak {

// ============================================================================
// Page 1: Welcome and Select Backup
// ============================================================================

UserProfileRestoreWelcomePage::UserProfileRestoreWelcomePage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle(tr("Restore User Profiles"));
    setSubTitle(tr("Select a backup to restore user profile data"));
    
    setupUi();
    
    registerField("backupPath*", m_backupPathEdit);
}

void UserProfileRestoreWelcomePage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Info section
    QString infoHtml = QString(
        "<h3>%1</h3>"
        "<p>%2</p>"
        "<ul>"
        "<li>%3</li>"
        "<li>%4</li>"
        "<li>%5</li>"
        "<li>%6</li>"
        "<li>%7</li>"
        "</ul>"
    ).arg(
        tr("Welcome to User Profile Restore"),
        tr("This wizard will guide you through restoring user profile data from a backup."),
        tr("<b>User Mapping</b>: Map backup users to destination users"),
        tr("<b>Merge Options</b>: Choose how to handle existing files"),
        tr("<b>Folder Selection</b>: Select which folders to restore"),
        tr("<b>Conflict Resolution</b>: Configure how to handle file conflicts"),
        tr("<b>Permissions</b>: Set permission strategies for restored files")
    );
    
    m_infoLabel = new QLabel(infoHtml, this);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setTextFormat(Qt::RichText);
    layout->addWidget(m_infoLabel);
    
    layout->addSpacing(20);
    
    // Backup selection
    auto* selectGroup = new QWidget(this);
    auto* selectLayout = new QHBoxLayout(selectGroup);
    selectLayout->setContentsMargins(0, 0, 0, 0);
    
    auto* backupLabel = new QLabel(tr("Backup Location:"), selectGroup);
    m_backupPathEdit = new QLineEdit(selectGroup);
    m_backupPathEdit->setPlaceholderText(tr("Select backup directory or manifest.json file..."));
    
    m_browseButton = new QPushButton(tr("Browse..."), selectGroup);
    
    selectLayout->addWidget(backupLabel);
    selectLayout->addWidget(m_backupPathEdit, 1);
    selectLayout->addWidget(m_browseButton);
    
    layout->addWidget(selectGroup);
    
    // Manifest info
    m_manifestInfoLabel = new QLabel(this);
    m_manifestInfoLabel->setWordWrap(true);
    m_manifestInfoLabel->setStyleSheet("QLabel { background-color: #f8fafc; padding: 12px; border-radius: 10px; }");
    m_manifestInfoLabel->hide();
    layout->addWidget(m_manifestInfoLabel);
    
    layout->addStretch(1);
    
    // Connections
    connect(m_browseButton, &QPushButton::clicked, this, &UserProfileRestoreWelcomePage::onBrowseBackup);
    connect(m_backupPathEdit, &QLineEdit::textChanged, this, &UserProfileRestoreWelcomePage::onBackupPathChanged);
}

void UserProfileRestoreWelcomePage::onBrowseBackup() {
    QString path = QFileDialog::getExistingDirectory(
        this,
        tr("Select Backup Directory"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );
    
    if (!path.isEmpty()) {
        m_backupPathEdit->setText(path);
    }
}

void UserProfileRestoreWelcomePage::onBackupPathChanged() {
    QString path = m_backupPathEdit->text();
    
    if (path.isEmpty()) {
        m_manifestInfoLabel->hide();
        Q_EMIT completeChanged();
        return;
    }
    
    // Try to load manifest
    QFileInfo fileInfo(path);
    QString manifestPath;
    
    if (fileInfo.isDir()) {
        manifestPath = path + "/manifest.json";
    } else if (fileInfo.fileName() == "manifest.json") {
        manifestPath = path;
        m_backupPathEdit->setText(fileInfo.absolutePath());
    } else {
        m_manifestInfoLabel->setText(tr("❌ Invalid backup path. Please select a backup directory or manifest.json file."));
        m_manifestInfoLabel->show();
        Q_EMIT completeChanged();
        return;
    }
    
    // Load manifest
    BackupManifest manifest = BackupManifest::loadFromFile(manifestPath);
    if (manifest.version.isEmpty()) {
        m_manifestInfoLabel->setText(tr("❌ Failed to load backup manifest. The backup may be corrupted."));
        m_manifestInfoLabel->show();
        Q_EMIT completeChanged();
        return;
    }
    
    // Display manifest info
    QString info = QString(
        "<b>✅ Valid Backup Found</b><br>"
        "<b>Version:</b> %1<br>"
        "<b>Created:</b> %2<br>"
        "<b>Source Machine:</b> %3<br>"
        "<b>Users:</b> %4<br>"
        "<b>Total Size:</b> %5 GB"
    ).arg(
        manifest.version,
        manifest.created.toString("yyyy-MM-dd hh:mm:ss"),
        manifest.source_machine,
        QString::number(manifest.users.size()),
        QString::number(manifest.total_backup_size_bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2)
    );
    
    m_manifestInfoLabel->setText(info);
    m_manifestInfoLabel->show();
    
    // Store manifest in wizard
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz) {
        wiz->setBackupPath(m_backupPathEdit->text());
        wiz->setManifest(manifest);
    }
    
    Q_EMIT completeChanged();
}

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
    m_summaryLabel->setStyleSheet("QLabel { background-color: #e0f2fe; padding: 10px; border-radius: 10px; }");
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
        QString sourceUsername = m_mappingTable->item(row, 1)->text();
        auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
        
        if (destCombo) {
            // Find matching destination user
            int matchIndex = destCombo->findText(sourceUsername);
            if (matchIndex >= 0) {
                destCombo->setCurrentIndex(matchIndex);
            }
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
        if (m_mappingTable->item(row, 0)->checkState() == Qt::Checked) {
            selectedMappings++;
            
            auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
            if (destCombo && destCombo->currentData().toString().isEmpty()) {
                newUsers++;
            } else {
                merges++;
            }
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

bool UserProfileRestoreUserMappingPage::validatePage() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) return false;
    
    BackupManifest manifest = wiz->manifest();
    QVector<UserMapping> mappings;
    
    for (int row = 0; row < m_mappingTable->rowCount(); ++row) {
        if (m_mappingTable->item(row, 0)->checkState() != Qt::Checked) {
            continue;
        }
        
        UserMapping mapping;
        mapping.source_username = m_mappingTable->item(row, 1)->text();
        mapping.selected = true;
        
        // Find source user data
        for (const auto& user : manifest.users) {
            if (user.username == mapping.source_username) {
                mapping.source_sid = user.sid;
                break;
            }
        }
        
        // Get destination user
        auto* destCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 3));
        if (destCombo) {
            mapping.destination_username = destCombo->currentData().toString();
            
            // Find destination SID
            if (!mapping.destination_username.isEmpty()) {
                for (const auto& user : m_destinationUsers) {
                    if (user.username == mapping.destination_username) {
                        mapping.destination_sid = user.sid;
                        break;
                    }
                }
            }
        }
        
        // Get merge mode
        auto* modeCombo = qobject_cast<QComboBox*>(m_mappingTable->cellWidget(row, 4));
        if (modeCombo) {
            mapping.mode = static_cast<MergeMode>(modeCombo->currentData().toInt());
        }
        
        mappings.append(mapping);
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
    m_summaryLabel->setStyleSheet("QLabel { background-color: #e0f2fe; padding: 10px; border-radius: 10px; }");
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
    m_summaryLabel->setStyleSheet("QLabel { background-color: #e0f2fe; padding: 10px; border-radius: 10px; }");
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
        // Find backup user data
        for (const auto& backupUser : manifest.users) {
            if (backupUser.username != mapping.source_username) continue;
            
            // Add each folder
            for (const auto& folder : backupUser.backed_up_folders) {
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
                double sizeMB = folder.size_bytes / (1024.0 * 1024.0);
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
            totalSize += static_cast<qint64>(sizeText.toDouble() * 1024 * 1024);
            
            totalFiles += m_folderTable->item(row, 4)->text().toInt();
        }
    }
    
    double totalGB = totalSize / (1024.0 * 1024.0 * 1024.0);
    
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
    m_summaryLabel->setStyleSheet("QLabel { background-color: #fef3c7; padding: 12px; border-radius: 10px; }");
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
// Page 6: Execute Restore
// ============================================================================

UserProfileRestoreExecutePage::UserProfileRestoreExecutePage(QWidget* parent)
    : QWizardPage(parent)
    , m_worker(nullptr)
    , m_restoreComplete(false)
    , m_restoreSuccess(false)
{
    setTitle(tr("Restore in Progress"));
    setSubTitle(tr("Restoring user profile data..."));
    
    setupUi();
}

void UserProfileRestoreExecutePage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Status
    m_statusLabel = new QLabel(tr("Ready to restore..."), this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: 600; font-size: 11pt; color: #1e293b; }");
    layout->addWidget(m_statusLabel);
    
    // Overall progress
    auto* overallLabel = new QLabel(tr("Overall Progress:"), this);
    layout->addWidget(overallLabel);
    m_overallProgressBar = new QProgressBar(this);
    m_overallProgressBar->setTextVisible(true);
    layout->addWidget(m_overallProgressBar);
    
    // Current operation progress
    m_currentOperationLabel = new QLabel(tr("Current: -"), this);
    layout->addWidget(m_currentOperationLabel);
    m_currentProgressBar = new QProgressBar(this);
    m_currentProgressBar->setTextVisible(true);
    layout->addWidget(m_currentProgressBar);
    
    layout->addSpacing(10);
    
    // Log viewer
    auto* logLabel = new QLabel(tr("Operation Log:"), this);
    layout->addWidget(logLabel);
    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(200);
    layout->addWidget(m_logText);
    
    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    m_cancelButton = new QPushButton(tr("Cancel Restore"), this);
    m_viewLogButton = new QPushButton(tr("View Full Log"), this);
    m_viewLogButton->setEnabled(false);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(m_viewLogButton);
    layout->addLayout(buttonLayout);
    
    // Connections
    connect(m_cancelButton, &QPushButton::clicked, this, &UserProfileRestoreExecutePage::onCancelRestore);
    connect(m_viewLogButton, &QPushButton::clicked, this, &UserProfileRestoreExecutePage::onViewLog);
}

void UserProfileRestoreExecutePage::initializePage() {
    // Reset state
    m_restoreComplete = false;
    m_restoreSuccess = false;
    m_overallProgressBar->setValue(0);
    m_currentProgressBar->setValue(0);
    m_logText->clear();
    m_statusLabel->setText(tr("Preparing to restore..."));
    
    // Start restore after UI initializes
    QTimer::singleShot(200, this, &UserProfileRestoreExecutePage::onStartRestore);
}

void UserProfileRestoreExecutePage::onStartRestore() {
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (!wiz) {
        m_statusLabel->setText(tr("Error: Could not access wizard data"));
        m_restoreComplete = true;
        Q_EMIT completeChanged();
        return;
    }
    
    m_statusLabel->setText(tr("Restore in progress..."));
    m_logText->append(tr("[INFO] Restore started..."));
    
    // Get restore configuration from wizard
    QString backupPath = wiz->backupPath();
    BackupManifest manifest = wiz->manifest();
    QVector<UserMapping> mappings = wiz->userMappings();
    ConflictResolution conflictMode = wiz->conflictResolution();
    PermissionMode permMode = wiz->permissionMode();
    bool verify = wiz->verifyFiles();
    
    // Create and configure restore worker
    auto worker = new UserProfileRestoreWorker(this);
    
    // Connect signals for progress tracking
    connect(worker, &UserProfileRestoreWorker::overallProgress,
            this, [this](int current, int total, qint64 bytes, qint64 totalBytes) {
                m_overallProgressBar->setMaximum(total);
                m_overallProgressBar->setValue(current);
                (void)bytes; (void)totalBytes; // Currently unused
            });
    connect(worker, &UserProfileRestoreWorker::fileProgress,
            this, [this](int current, int total) {
                m_currentProgressBar->setMaximum(total);
                m_currentProgressBar->setValue(current);
            });
    connect(worker, &UserProfileRestoreWorker::statusUpdate,
            this, [this](const QString& username, const QString& operation) {
                m_statusLabel->setText(tr("Restoring %1: %2").arg(username, operation));
            });
    connect(worker, &UserProfileRestoreWorker::logMessage,
            this, [this](const QString& message, bool isWarning) {
                QString prefix = isWarning ? "[WARNING]" : "[INFO]";
                m_logText->append(QString("%1 %2").arg(prefix, message));
            });
    connect(worker, &UserProfileRestoreWorker::restoreComplete,
            this, [this, worker](bool success, const QString& message) {
                m_statusLabel->setText(success ? tr("Restore complete!") : tr("Restore failed"));
                m_logText->append(success ? tr("[INFO] Restore completed successfully") : tr("[ERROR] Restore failed"));
                m_logText->append(QString("[INFO] %1").arg(message));
                m_restoreComplete = true;
                m_restoreSuccess = success;
                m_cancelButton->setEnabled(false);
                m_viewLogButton->setEnabled(true);
                Q_EMIT completeChanged();
                worker->deleteLater();
            });
    
    // Start restore operation
    worker->startRestore(backupPath, manifest, mappings, conflictMode, permMode, verify);
    
    // Configure progress bars
    m_overallProgressBar->setRange(0, mappings.size());
    m_currentProgressBar->setRange(0, 0); // Indeterminate
}

void UserProfileRestoreExecutePage::onCancelRestore() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
        m_logText->append(tr("[WARNING] Canceling restore..."));
    }
}

void UserProfileRestoreExecutePage::onOverallProgress(int current, int total, qint64 bytes, qint64 totalBytes) {
    if (total > 0) {
        int percent = (current * 100) / total;
        m_overallProgressBar->setValue(percent);
        
        double gbCopied = bytes / (1024.0 * 1024.0 * 1024.0);
        double gbTotal = totalBytes / (1024.0 * 1024.0 * 1024.0);
        m_overallProgressBar->setFormat(QString("%1% - %2 / %3 GB").arg(percent).arg(gbCopied, 0, 'f', 2).arg(gbTotal, 0, 'f', 2));
    }
}

void UserProfileRestoreExecutePage::onFileProgress(int current, int total) {
    if (total > 0) {
        int percent = (current * 100) / total;
        m_currentProgressBar->setValue(percent);
        m_currentProgressBar->setFormat(QString("%1% - %2 / %3 files").arg(percent).arg(current).arg(total));
    }
}

void UserProfileRestoreExecutePage::onStatusUpdate(const QString& username, const QString& operation) {
    m_currentOperationLabel->setText(tr("Current: %1 - %2").arg(username, operation));
}

void UserProfileRestoreExecutePage::onLogMessage(const QString& message, bool isWarning) {
    QString prefix = isWarning ? "[WARNING]" : "[INFO]";
    m_logText->append(QString("%1 %2").arg(prefix, message));
    
    // Auto-scroll to bottom
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
}

void UserProfileRestoreExecutePage::onRestoreComplete(bool success, const QString& message) {
    m_restoreComplete = true;
    m_restoreSuccess = success;
    
    if (success) {
        m_statusLabel->setText(tr("✅ Restore completed successfully!"));
    } else {
        m_statusLabel->setText(tr("❌ Restore failed"));
    }
    
    m_logText->append(QString("\n=== RESTORE COMPLETE ===\n%1").arg(message));
    m_cancelButton->setEnabled(false);
    m_viewLogButton->setEnabled(true);
    
    Q_EMIT completeChanged();
}

void UserProfileRestoreExecutePage::onViewLog() {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Restore Log"));
    msgBox.setText(tr("Complete restore operation log:"));
    msgBox.setDetailedText(m_logText->toPlainText());
    msgBox.setIcon(m_restoreSuccess ? QMessageBox::Information : QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Save);
    
    int ret = msgBox.exec();
    if (ret == QMessageBox::Save) {
        QString fileName = QFileDialog::getSaveFileName(this, tr("Save Log"), 
            QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/restore_log.txt",
            tr("Text Files (*.txt);;All Files (*.*)"));
        if (!fileName.isEmpty()) {
            QFile file(fileName);
            if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&file);
                out << m_logText->toPlainText();
                file.close();
            }
        }
    }
}

bool UserProfileRestoreExecutePage::isComplete() const {
    return m_restoreComplete;
}

// ============================================================================
// Main Wizard
// ============================================================================

UserProfileRestoreWizard::UserProfileRestoreWizard(QWidget* parent)
    : QWizard(parent)
    , m_conflictResolution(ConflictResolution::RenameWithSuffix)
    , m_permissionMode(PermissionMode::StripAll)
    , m_verifyFiles(true)
    , m_createBackup(false)
{
    setWindowTitle(tr("Restore User Profiles"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::HaveHelpButton, false);
    setOption(QWizard::NoCancelButton, false);
    
    // Add pages
    addPage(new UserProfileRestoreWelcomePage(this));
    addPage(new UserProfileRestoreUserMappingPage(this));
    addPage(new UserProfileRestoreMergeConfigPage(this));
    addPage(new UserProfileRestoreFolderSelectionPage(this));
    addPage(new UserProfileRestorePermissionSettingsPage(this));
    addPage(new UserProfileRestoreExecutePage(this));
    
    resize(900, 700);
}

} // namespace sak
