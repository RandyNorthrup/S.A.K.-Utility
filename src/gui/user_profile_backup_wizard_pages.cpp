// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_scanner.h"
#include "sak/backup_destination_guard.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/process_runner.h"
#include "sak/style_constants.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/widget_helpers.h"
#include "sak/wifi_profile_scanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QtConcurrent>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <atomic>
#include <iterator>
#include <memory>

namespace sak {

namespace {

/// Short enough not to be a nuisance on a technician's bench, long enough that the
/// PBKDF2 work factor is doing real work rather than covering a 4-character password.
constexpr int kMinBackupPasswordLength = 8;
/// zlib's ends of the range; the middle entry uses the codec's own default.
constexpr int kCompressionLevelFastest = 1;
constexpr int kCompressionLevelSmallest = 9;

constexpr int kUserFolderColumnName = 0;
constexpr int kUserFolderColumnFolders = 1;
constexpr int kUserFolderColumnCount = 2;
constexpr int kSizeDisplayPrecision = 2;
constexpr int kAppDataSizeDisplayPrecision = 1;
constexpr int kMaxFileSizeMinimumMb = 1;
constexpr int kMaxFileSizeMaximumMb = 10'000;
constexpr int kDefaultMaxSingleFileMb = 2048;
constexpr int kMaxFolderSizeMinimumGb = 1;
constexpr int kMaxFolderSizeMaximumGb = 1000;
constexpr int kDefaultMaxFolderSizeGb = 50;
constexpr int kInstalledAppColumnName = 0;
constexpr int kInstalledAppColumnVersion = 1;
constexpr int kInstalledAppColumnPublisher = 2;
constexpr int kAppDataColumnName = 0;
constexpr int kAppDataColumnPath = 1;
constexpr int kAppDataColumnSize = 2;
constexpr int kEthernetColumnSelect = 0;
constexpr int kEthernetColumnAdapter = 1;
constexpr int kEthernetColumnDhcp = 2;
constexpr int kEthernetColumnIp = 3;
constexpr int kEthernetColumnSubnet = 4;
constexpr int kEthernetColumnGateway = 5;
constexpr int kEthernetColumnDns = 6;
constexpr int kEthernetColumnCount = 7;

/// The source trees a backup run will read, paired with the subfolder name each one is written to
/// under the destination ("<destination>/<username>"). Only selected users are copied, so only
/// those constrain where the destination may live.
struct SelectedProfileRoots {
    QStringList paths;
    QStringList folder_names;
};

SelectedProfileRoots selectedProfileRoots(const QVector<UserProfile>& users) {
    SelectedProfileRoots roots;
    for (const auto& user : users) {
        if (user.is_selected && !user.profile_path.trimmed().isEmpty()) {
            roots.paths.append(user.profile_path);
            roots.folder_names.append(user.username);
        }
    }
    return roots;
}

}  // namespace

// ============================================================================
// UserProfileBackupCustomizeDataPage
// ============================================================================

UserProfileBackupCustomizeDataPage::UserProfileBackupCustomizeDataPage(QVector<UserProfile>& users,
                                                                       QWidget* parent)
    : QWizardPage(parent), m_users(users) {
    setTitle(tr("Customize Per-User Data"));
    setSubTitle(tr("Customize which folders and application data to backup for each user"));

    setupUi();
}

void UserProfileBackupCustomizeDataPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Instructions
    m_instructionLabel = new QLabel(this);
    m_instructionLabel->setWordWrap(true);
    layout->addWidget(m_instructionLabel);

    // User table (2 columns: username, folders selected)
    m_userTable = new QTableWidget(0, kUserFolderColumnCount, this);
    m_userTable->setHorizontalHeaderLabels({tr("Username"), tr("Folders Selected")});
    m_userTable->horizontalHeader()->setStretchLastSection(true);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserFolderColumnName,
                                                          QHeaderView::ResizeToContents);
    m_userTable->horizontalHeader()->setSectionResizeMode(kUserFolderColumnFolders,
                                                          QHeaderView::Stretch);
    m_userTable->verticalHeader()->setVisible(false);
    m_userTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_userTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_userTable);

    // Customize button (for selected row)
    auto* button_layout = new QHBoxLayout();
    m_customizeButton = new QPushButton(tr("Customize Selected User"), this);
    m_customizeButton->setIcon(QIcon::fromTheme("configure"));
    m_customizeButton->setEnabled(false);
    m_customizeButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_customizeButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupCustomizeDataPage::onCustomizeUser);
    connect(m_userTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_customizeButton->setEnabled(!m_userTable->selectedItems().isEmpty());
    });
    button_layout->addWidget(m_customizeButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);

    Q_ASSERT(m_instructionLabel);
}

void UserProfileBackupCustomizeDataPage::initializePage() {
    Q_ASSERT(m_instructionLabel);
    m_instructionLabel->setText(
        tr("By default, common folders (Documents, Desktop, Pictures, Downloads) are selected for "
           "each user. "
           "Click <b>Customize</b> to change folder selections, add custom folders, or select "
           "specific application data."));

    populateUserList();
    updateSummary();
}

bool UserProfileBackupCustomizeDataPage::isComplete() const {
    // Always complete - customization is optional
    return true;
}

void UserProfileBackupCustomizeDataPage::populateUserList() {
    Q_ASSERT(m_userTable);
    m_userTable->setRowCount(0);

    int row = 0;
    for (auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }

        m_userTable->insertRow(row);

        // Username
        auto* name_item = new QTableWidgetItem(user.username);
        m_userTable->setItem(row, kUserFolderColumnName, name_item);

        // Folder count
        int selected_count = 0;
        for (const auto& folder : user.folder_selections) {
            if (folder.selected) {
                selected_count++;
            }
        }
        auto* folder_item = new QTableWidgetItem(tr("%1 folders selected").arg(selected_count));
        m_userTable->setItem(row, kUserFolderColumnFolders, folder_item);

        row++;
    }
}

UserProfile* UserProfileBackupCustomizeDataPage::findSelectedUserByRow(int selected_row) {
    Q_ASSERT(selected_row >= 0);
    int current_row = 0;
    for (auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }
        if (current_row == selected_row) {
            return &user;
        }
        current_row++;
    }
    return nullptr;
}

void UserProfileBackupCustomizeDataPage::onCustomizeUser() {
    Q_ASSERT(m_userTable);
    const int selected_row = m_userTable->currentRow();
    if (selected_row < 0) {
        return;
    }

    auto* user = findSelectedUserByRow(selected_row);
    if (user == nullptr) {
        return;
    }

    PerUserCustomizationDialog dialog(*user, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const int selected_count =
        static_cast<int>(std::count_if(user->folder_selections.begin(),
                                       user->folder_selections.end(),
                                       [](const auto& f) { return f.selected; }));
    m_userTable->item(selected_row, 1)->setText(tr("%1 folders selected").arg(selected_count));
    updateSummary();
}

void UserProfileBackupCustomizeDataPage::updateSummary() {
    Q_ASSERT(m_summaryLabel);
    int total_users = 0;
    int total_folders = 0;
    qint64 total_size = 0;

    for (const auto& user : m_users) {
        if (!user.is_selected) {
            continue;
        }
        total_users++;

        for (const auto& folder : user.folder_selections) {
            if (!folder.selected) {
                continue;
            }
            total_folders++;
            total_size += folder.size_bytes;
        }
    }

    const double total_gb = static_cast<double>(total_size) / sak::kBytesPerGBf;
    m_summaryLabel->setText(tr("%1 user(s), %2 total folders | Estimated: %3 GB")
                                .arg(total_users)
                                .arg(total_folders)
                                .arg(total_gb, 0, 'f', kSizeDisplayPrecision));
}

// ============================================================================
// UserProfileBackupSmartFiltersPage
// ============================================================================

UserProfileBackupSmartFiltersPage::UserProfileBackupSmartFiltersPage(SmartFilter& filter,
                                                                     QWidget* parent)
    : QWizardPage(parent), m_filter(filter) {
    setTitle(tr("Smart Filter Configuration"));
    setSubTitle(tr("Configure automatic file and folder exclusions to prevent corruption"));

    setupUi();
}

void UserProfileBackupSmartFiltersPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("Smart filters automatically exclude files that can corrupt user profiles or "
                      "waste space. "
                      "You can adjust these settings or keep the recommended defaults."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    setupUi_filterSettings(layout);
    setupUi_exclusionsAndControls(layout);

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupSmartFiltersPage::setupUi_filterSettings(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* grid_layout = new QGridLayout();
    int row = 0;

    // File size limit
    m_enableFileSizeLimitCheck = new QCheckBox(tr("Limit single file size:"), this);
    connect(m_enableFileSizeLimitCheck, &QCheckBox::toggled, [this](bool enabled) {
        m_maxFileSizeSpinBox->setEnabled(enabled);
        updateSummary();
    });
    grid_layout->addWidget(m_enableFileSizeLimitCheck, row, 0);

    m_maxFileSizeSpinBox = new QSpinBox(this);
    m_maxFileSizeSpinBox->setRange(kMaxFileSizeMinimumMb, kMaxFileSizeMaximumMb);
    m_maxFileSizeSpinBox->setSuffix(tr(" MB"));
    m_maxFileSizeSpinBox->setValue(kDefaultMaxSingleFileMb);
    connect(m_maxFileSizeSpinBox,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &UserProfileBackupSmartFiltersPage::updateSummary);
    grid_layout->addWidget(m_maxFileSizeSpinBox, row, 1);
    row++;

    // Folder size warning
    m_enableFolderSizeLimitCheck = new QCheckBox(tr("Warn if folder exceeds:"), this);
    connect(m_enableFolderSizeLimitCheck, &QCheckBox::toggled, [this](bool enabled) {
        m_maxFolderSizeSpinBox->setEnabled(enabled);
        updateSummary();
    });
    grid_layout->addWidget(m_enableFolderSizeLimitCheck, row, 0);

    m_maxFolderSizeSpinBox = new QSpinBox(this);
    m_maxFolderSizeSpinBox->setRange(kMaxFolderSizeMinimumGb, kMaxFolderSizeMaximumGb);
    m_maxFolderSizeSpinBox->setSuffix(tr(" GB"));
    m_maxFolderSizeSpinBox->setValue(kDefaultMaxFolderSizeGb);
    connect(m_maxFolderSizeSpinBox,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            &UserProfileBackupSmartFiltersPage::updateSummary);
    grid_layout->addWidget(m_maxFolderSizeSpinBox, row, 1);
    row++;

    layout->addLayout(grid_layout);
}

void UserProfileBackupSmartFiltersPage::setupUi_exclusionsAndControls(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    // Automatic exclusions
    auto* exclusions_group = new QWidget(this);
    auto* exclusions_layout = new QVBoxLayout(exclusions_group);
    exclusions_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* exclusions_label = new QLabel(tr("<b>Automatic Exclusions:</b>"), this);
    exclusions_layout->addWidget(exclusions_label);

    m_excludeCacheCheck = new QCheckBox(tr("Exclude cache directories (WebCache, GPUCache, etc.)"),
                                        this);
    connect(m_excludeCacheCheck,
            &QCheckBox::toggled,
            this,
            &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusions_layout->addWidget(m_excludeCacheCheck);

    m_excludeTempCheck = new QCheckBox(tr("Exclude temporary files (*.tmp, *.cache, *.temp)"),
                                       this);
    connect(m_excludeTempCheck,
            &QCheckBox::toggled,
            this,
            &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusions_layout->addWidget(m_excludeTempCheck);

    m_excludeLockCheck = new QCheckBox(tr("Exclude lock files (*.lock, *.lck)"), this);
    connect(m_excludeLockCheck,
            &QCheckBox::toggled,
            this,
            &UserProfileBackupSmartFiltersPage::updateSummary);
    exclusions_layout->addWidget(m_excludeLockCheck);

    layout->addWidget(exclusions_group);

    // Dangerous files info
    auto* dangerous_layout = new QHBoxLayout();
    auto* dangerous_label = new QLabel(tr("\u26a0 <b>Always excluded:</b> Registry hives "
                                          "(NTUSER.DAT, UsrClass.dat), system folders"),
                                       this);
    dangerous_label->setWordWrap(true);
    dangerous_layout->addWidget(dangerous_label, 1);

    m_viewDangerousButton = new QPushButton(tr("View Full List..."), this);
    m_viewDangerousButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_viewDangerousButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupSmartFiltersPage::onViewDangerousList);
    dangerous_layout->addWidget(m_viewDangerousButton);
    layout->addLayout(dangerous_layout);

    layout->addStretch();

    // Reset button
    auto* reset_layout = new QHBoxLayout();
    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    m_resetButton->setIcon(QIcon::fromTheme("edit-undo"));
    m_resetButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_resetButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupSmartFiltersPage::onResetToDefaults);
    reset_layout->addWidget(m_resetButton);
    reset_layout->addStretch();
    layout->addLayout(reset_layout);
}

void UserProfileBackupSmartFiltersPage::initializePage() {
    loadFilterSettings();
    updateSummary();
}

void UserProfileBackupSmartFiltersPage::loadFilterSettings() {
    Q_ASSERT(m_enableFileSizeLimitCheck);
    Q_ASSERT(m_enableFolderSizeLimitCheck);
    // Load enable flags
    m_enableFileSizeLimitCheck->setChecked(m_filter.enable_file_size_limit);
    m_enableFolderSizeLimitCheck->setChecked(m_filter.enable_folder_size_limit);

    // Load from m_filter (convert bytes to MB/GB for UI)
    const qint64 file_size_mb = m_filter.max_single_file_size_bytes / sak::kBytesPerMB;
    m_maxFileSizeSpinBox->setValue(static_cast<int>(file_size_mb));
    m_maxFileSizeSpinBox->setEnabled(m_filter.enable_file_size_limit);

    const qint64 folder_size_gb = m_filter.max_folder_size_bytes / sak::kBytesPerGB;
    m_maxFolderSizeSpinBox->setValue(static_cast<int>(folder_size_gb));
    m_maxFolderSizeSpinBox->setEnabled(m_filter.enable_folder_size_limit);

    // Enable checkboxes based on whether the filter category has any rules defined
    const bool has_cache_rules = !m_filter.exclude_folders.isEmpty();
    const bool has_temp_rules = !m_filter.exclude_patterns.isEmpty();
    const bool has_lock_rules = !m_filter.dangerous_files.isEmpty();

    m_excludeCacheCheck->setEnabled(has_cache_rules);
    m_excludeCacheCheck->setChecked(has_cache_rules);
    if (!has_cache_rules) {
        m_excludeCacheCheck->setToolTip(tr("No cache exclusion rules defined"));
    } else {
        m_excludeCacheCheck->setToolTip(QString());
    }

    m_excludeTempCheck->setEnabled(has_temp_rules);
    m_excludeTempCheck->setChecked(has_temp_rules);
    if (!has_temp_rules) {
        m_excludeTempCheck->setToolTip(tr("No temporary file exclusion patterns defined"));
    } else {
        m_excludeTempCheck->setToolTip(QString());
    }

    m_excludeLockCheck->setEnabled(has_lock_rules);
    m_excludeLockCheck->setChecked(has_lock_rules);
    if (!has_lock_rules) {
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
    auto* header_label = new QLabel(
        tr("<b>[shield] Files and patterns that are always excluded from backups</b>"), dialog);
    header_label->setWordWrap(true);
    header_label->setStyleSheet(sak::ui::notePanelStyleWithFontSize(sak::ui::kColorBgWarningPanel,
                                                                    sak::ui::kColorTextBody,
                                                                    sak::ui::kFontSizeStatus,
                                                                    sak::ui::kCssPaddingXLargePx,
                                                                    sak::ui::kCssRadiusLargePx));
    layout->addWidget(header_label);

    // Build rich content
    QString content;
    content += QString::fromLatin1(sak::ui::kHtmlHeading3Color)
                   .arg(sak::ui::htmlColor(sak::ui::kColorError), tr("Always Excluded Files"));
    content += "<ul>";
    for (const auto& file : m_filter.dangerous_files) {
        content += QString("<li><code>%1</code></li>").arg(file.toHtmlEscaped());
    }
    content += "</ul>";

    content += QString::fromLatin1(sak::ui::kHtmlHeading3Color)
                   .arg(sak::ui::htmlColor(sak::ui::kColorWarning), tr("Excluded Patterns"));
    content += "<ul>";
    for (const auto& pattern : m_filter.exclude_patterns) {
        content += QString("<li><code>%1</code></li>").arg(pattern.toHtmlEscaped());
    }
    content += "</ul>";

    content += QString::fromLatin1(sak::ui::kHtmlHeading3Color)
                   .arg(sak::ui::htmlColor(sak::ui::kColorTextSecondary), tr("Excluded Folders"));
    content += "<ul>";
    for (const auto& folder : m_filter.exclude_folders) {
        content += QString("<li><code>%1</code></li>").arg(folder.toHtmlEscaped());
    }
    content += "</ul>";

    auto* text_browser = new QTextEdit(dialog);
    text_browser->setReadOnly(true);
    text_browser->setHtml(content);
    text_browser->setStyleSheet(
        sak::ui::textBrowserSurfaceStyle(sak::ui::kColorBgSurface, sak::ui::kColorBorderDefault));
    layout->addWidget(text_browser);

    auto* close_button = new QPushButton(tr("Close"), dialog);
    close_button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(close_button, &QPushButton::clicked, dialog, &QDialog::accept);
    layout->addWidget(close_button, 0, Qt::AlignRight);

    dialog->exec();
}

void UserProfileBackupSmartFiltersPage::updateSummary() {
    Q_ASSERT(m_enableFileSizeLimitCheck);
    Q_ASSERT(m_enableFolderSizeLimitCheck);
    // Update filter enable flags
    m_filter.enable_file_size_limit = m_enableFileSizeLimitCheck->isChecked();
    m_filter.enable_folder_size_limit = m_enableFolderSizeLimitCheck->isChecked();

    // Update filter from UI (convert MB/GB back to bytes)
    m_filter.max_single_file_size_bytes = static_cast<qint64>(m_maxFileSizeSpinBox->value()) *
                                          sak::kBytesPerMB;
    m_filter.max_folder_size_bytes = static_cast<qint64>(m_maxFolderSizeSpinBox->value()) *
                                     sak::kBytesPerGB;

    // Count total exclusions
    const int exclusion_count = static_cast<int>(m_filter.dangerous_files.size() +
                                                 m_filter.exclude_patterns.size() +
                                                 m_filter.exclude_folders.size());

    QString limit_text;
    if (m_filter.enable_file_size_limit) {
        limit_text = tr("File limit: %1 MB").arg(m_maxFileSizeSpinBox->value());
    } else {
        limit_text = tr("No file size limit");
    }

    m_summaryLabel->setText(
        tr("[shield] %1 exclusion rules active | %2").arg(exclusion_count).arg(limit_text));
}

// ============================================================================
// UserProfileBackupSettingsPage
// ============================================================================

UserProfileBackupSettingsPage::UserProfileBackupSettingsPage(BackupManifest& manifest,
                                                             QWidget* parent)
    : QWizardPage(parent), m_manifest(manifest) {
    setTitle(tr("Backup Settings"));
    setSubTitle(tr("Configure backup destination and options"));

    setupUi();
}

void UserProfileBackupSettingsPage::setupUi_destination(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    // Destination path
    auto* dest_layout = new QHBoxLayout();
    dest_layout->addWidget(new QLabel(tr("Backup destination:"), this));
    m_destinationEdit = new QLineEdit(this);
    m_destinationEdit->setPlaceholderText(tr("Select backup folder..."));
    connect(m_destinationEdit,
            &QLineEdit::textChanged,
            this,
            &UserProfileBackupSettingsPage::updateSummary);
    dest_layout->addWidget(m_destinationEdit, 1);
    m_browseButton = new QPushButton(tr("Browse..."), this);
    m_browseButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_browseButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupSettingsPage::onBrowseDestination);
    dest_layout->addWidget(m_browseButton);
    layout->addLayout(dest_layout);

    // The compression combo that used to sit here is gone. It defaulted to Balanced, so
    // every default run logged "Compression: Balanced" and then produced an uncompressed
    // verbatim copy. UserProfileBackupWorker has never compressed anything. See R5-G19-1.
}

void UserProfileBackupSettingsPage::setupUi_transforms(QVBoxLayout* layout) {
    Q_ASSERT(layout);

    // Compression. OFF by default: the version of this page removed in R5-G19-1 defaulted
    // to "Balanced" and then wrote uncompressed copies, so every default run misreported
    // what it had done. An opt-in that is actually applied is the fix, not a better default.
    m_compressCheck = new QCheckBox(tr("Compress file contents"), this);
    m_compressCheck->setToolTip(
        tr("Stores each file compressed. Smaller backups, slower to write and restore."));
    layout->addWidget(m_compressCheck);

    auto* level_layout = new QHBoxLayout();
    level_layout->addWidget(new QLabel(tr("Compression level:"), this));
    m_compressionLevelCombo = new QComboBox(this);
    m_compressionLevelCombo->addItem(tr("Fastest"), kCompressionLevelFastest);
    m_compressionLevelCombo->addItem(tr("Balanced"), sak::kBackupDefaultCompressionLevel);
    m_compressionLevelCombo->addItem(tr("Smallest"), kCompressionLevelSmallest);
    m_compressionLevelCombo->setCurrentIndex(1);
    m_compressionLevelCombo->setEnabled(false);
    level_layout->addWidget(m_compressionLevelCombo, 1);
    layout->addLayout(level_layout);

    // Encryption. The controls are created BEFORE the toggle handler that enables them:
    // the removed version captured m_passwordEdit in a lambda ten lines before it existed.
    m_encryptCheck = new QCheckBox(tr("Encrypt file contents (AES-256)"), this);
    layout->addWidget(m_encryptCheck);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(
        tr("Password (at least %1 characters)").arg(kMinBackupPasswordLength));
    m_passwordEdit->setEnabled(false);
    layout->addWidget(m_passwordEdit);

    m_passwordConfirmEdit = new QLineEdit(this);
    m_passwordConfirmEdit->setEchoMode(QLineEdit::Password);
    m_passwordConfirmEdit->setPlaceholderText(tr("Confirm password"));
    m_passwordConfirmEdit->setEnabled(false);
    layout->addWidget(m_passwordConfirmEdit);

    // Say what encryption does NOT cover, where the choice is made. File names and sizes
    // stay readable because the tree is preserved for selective restore; a technician
    // deciding whether this is enough protection needs to know that here, not in a header.
    auto* encrypt_note = new QLabel(
        tr("(i) File <b>contents</b> are encrypted. File <b>names</b>, folder structure and "
           "file sizes remain visible. There is no password recovery - a lost password means "
           "the backup cannot be restored."),
        this);
    encrypt_note->setWordWrap(true);
    encrypt_note->setStyleSheet(
        sak::ui::paddedTextStyle(sak::ui::kColorTextMuted, sak::ui::kSpacingSmall));
    layout->addWidget(encrypt_note);

    connect(m_compressCheck, &QCheckBox::toggled, this, [this](const bool on) {
        m_compressionLevelCombo->setEnabled(on);
        updateSummary();
    });
    connect(m_encryptCheck, &QCheckBox::toggled, this, [this](const bool on) {
        m_passwordEdit->setEnabled(on);
        m_passwordConfirmEdit->setEnabled(on);
        if (!on) {
            m_passwordEdit->clear();
            m_passwordConfirmEdit->clear();
        }
        updateSummary();
    });
}

bool UserProfileBackupSettingsPage::compressionEnabled() const {
    return (m_compressCheck != nullptr) && m_compressCheck->isChecked();
}

int UserProfileBackupSettingsPage::compressionLevel() const {
    return (m_compressionLevelCombo != nullptr) ? m_compressionLevelCombo->currentData().toInt()
                                                : sak::kBackupDefaultCompressionLevel;
}

bool UserProfileBackupSettingsPage::encryptionEnabled() const {
    return (m_encryptCheck != nullptr) && m_encryptCheck->isChecked();
}

QString UserProfileBackupSettingsPage::encryptionPassword() const {
    // Never hand back a password for a run that is not encrypting: the worker treats a
    // non-empty password as intent and would otherwise write containers unexpectedly.
    return encryptionEnabled() && (m_passwordEdit != nullptr) ? m_passwordEdit->text() : QString();
}

bool UserProfileBackupSettingsPage::validateEncryptionPassword() {
    if (!encryptionEnabled()) {
        return true;
    }
    const QString password = m_passwordEdit->text();
    if (password.length() < kMinBackupPasswordLength) {
        sak::showWarningLogged(this,
                               tr("Password Too Short"),
                               tr("The backup password must be at least %1 characters.")
                                   .arg(kMinBackupPasswordLength));
        return false;
    }
    if (password != m_passwordConfirmEdit->text()) {
        sak::showWarningLogged(this,
                               tr("Passwords Do Not Match"),
                               tr("The password and its confirmation are different."));
        return false;
    }
    return true;
}

void UserProfileBackupSettingsPage::setupUi_permissions(QVBoxLayout* layout) {
    Q_ASSERT(layout);

    // Permission mode
    auto* perm_layout = new QHBoxLayout();
    perm_layout->addWidget(new QLabel(tr("Permission handling:"), this));
    m_permissionModeCombo = new QComboBox(this);
    m_permissionModeCombo->addItem(tr("Strip All (Recommended)"),
                                   static_cast<int>(PermissionMode::StripAll));
    m_permissionModeCombo->addItem(tr("Preserve Original"),
                                   static_cast<int>(PermissionMode::PreserveOriginal));
    m_permissionModeCombo->addItem(tr("Assign to Destination"),
                                   static_cast<int>(PermissionMode::AssignToDestination));
    m_permissionModeCombo->setCurrentIndex(0);  // StripAll by default
    connect(m_permissionModeCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &UserProfileBackupSettingsPage::updateSummary);
    perm_layout->addWidget(m_permissionModeCombo, 1);
    layout->addLayout(perm_layout);

    // Permission explanation
    auto* perm_explain_label = new QLabel(
        tr("(i) <b>Strip All:</b> Removes ACLs to prevent permission conflicts (safest). "
           "<b>Preserve:</b> Keeps original permissions (may cause errors). "
           "<b>Assign Standard:</b> Sets full control for destination user."),
        this);
    perm_explain_label->setWordWrap(true);
    perm_explain_label->setStyleSheet(
        sak::ui::paddedTextStyle(sak::ui::kColorTextMuted, sak::ui::kSpacingSmall));
    layout->addWidget(perm_explain_label);
}

void UserProfileBackupSettingsPage::setupUi_summaryAndRegistration(QVBoxLayout* layout) {
    // Verification
    m_verifyCheck = new QCheckBox(tr("Verify files after backup (MD5 checksums)"), this);
    m_verifyCheck->setChecked(true);
    layout->addWidget(m_verifyCheck);

    layout->addStretch();

    // Summary
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    layout->addWidget(m_summaryLabel);

    // Register wizard fields for validation
    registerField("destination*", m_destinationEdit);
    registerField("permissionMode", m_permissionModeCombo, "currentIndex");
}

void UserProfileBackupSettingsPage::setupUi() {
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* instruction_label =
        new QLabel(tr("Choose where to save the backup and configure additional options."), this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    setupUi_destination(layout);
    setupUi_transforms(layout);
    setupUi_permissions(layout);
    setupUi_summaryAndRegistration(layout);
}

void UserProfileBackupSettingsPage::initializePage() {
    // Suggest default backup location
    const QString default_path = QDir::homePath() + "/UserProfileBackup_" +
                                 QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_destinationEdit->setText(default_path);
    updateSummary();
}

bool UserProfileBackupSettingsPage::confirmExistingDestination(const QString& destination) {
    const QDir dest_dir(destination);
    if (!dest_dir.exists()) {
        return true;
    }
    auto reply =
        sak::showQuestionLogged(this,
                                tr("Folder Exists"),
                                tr("The destination folder already exists. Continue anyway?"),
                                QMessageBox::Yes | QMessageBox::No);
    return reply == QMessageBox::Yes;
}

bool UserProfileBackupSettingsPage::validateDestination() {
    // The destination is screened before anything is written: whitespace-only text, a relative
    // path (which QDir would resolve against the process working directory), and a folder that
    // overlaps a profile being backed up (the run would recurse into its own output) are all
    // refused outright -- never corrected to something "close enough".
    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (wiz == nullptr) {
        sak::logWarning("User profile backup: settings page has no owning wizard");
        sak::showWarningLogged(this,
                               tr("Backup Unavailable"),
                               tr("The backup wizard is not available. Close the wizard and "
                                  "start the backup again."));
        return false;
    }

    const SelectedProfileRoots roots = selectedProfileRoots(wiz->scannedUsers());
    const BackupDestinationCheck check =
        screenBackupDestination(m_destinationEdit->text(), roots.paths, roots.folder_names);
    if (!check.accepted) {
        sak::logWarning("User profile backup: destination refused: {}",
                        check.refusal.toStdString());
        sak::showWarningLogged(this, tr("Invalid Destination"), check.refusal);
        m_destinationEdit->setFocus();
        return false;
    }

    if (!confirmExistingDestination(check.path)) {
        return false;
    }
    // Store the screened form so the value that was checked is the value the run uses.
    m_destinationPath = check.path;
    return true;
}

void UserProfileBackupSettingsPage::installExecutePage() {
    auto* wizard = qobject_cast<UserProfileBackupWizard*>(this->wizard());
    if (wizard != nullptr) {
        // Use the wizard's real scanned-users list; the old "scannedUsers"
        // property was never set, so the execute page always backed up nothing.
        wizard->setPage(UserProfileBackupWizard::Page_Execute,
                        new UserProfileBackupExecutePage(
                            m_manifest, wizard->scannedUsers(), m_destinationPath, wizard));
    }
}

bool UserProfileBackupSettingsPage::validatePage() {
    Q_ASSERT(m_destinationEdit);
    // validateDestination() stores the screened destination in m_destinationPath.
    if (!validateDestination()) {
        return false;
    }
    if (!validateEncryptionPassword()) {
        return false;
    }

    m_manifest.version = "1.0";
    m_manifest.created = QDateTime::currentDateTime();
    m_manifest.source_machine = QSysInfo::machineHostName();
    installExecutePage();

    return true;
}

void UserProfileBackupSettingsPage::onBrowseDestination() {
    const QString dir = QFileDialog::getExistingDirectory(this,
                                                          tr("Select Backup Destination"),
                                                          m_destinationEdit->text().isEmpty()
                                                              ? QDir::homePath()
                                                              : m_destinationEdit->text());
    if (!dir.isEmpty()) {
        m_destinationEdit->setText(dir);
    }
}

void UserProfileBackupSettingsPage::updateSummary() {
    Q_ASSERT(m_destinationEdit);
    Q_ASSERT(m_permissionModeCombo);
    const QString dest = m_destinationEdit->text().isEmpty()
                             ? tr("Not selected")
                             : QDir::toNativeSeparators(m_destinationEdit->text());

    QString perm_mode;
    auto current_mode = static_cast<PermissionMode>(m_permissionModeCombo->currentData().toInt());
    if (current_mode == PermissionMode::StripAll) {
        perm_mode = tr("Strip ACLs");
    } else if (current_mode == PermissionMode::PreserveOriginal) {
        perm_mode = tr("Preserve");
    } else if (current_mode == PermissionMode::AssignToDestination) {
        perm_mode = tr("Assign Destination");
    }

    m_summaryLabel->setText(tr("[save] Destination: %1 | Permissions: %2 | Verify: %3")
                                .arg(dest)
                                .arg(perm_mode)
                                .arg(m_verifyCheck->isChecked() ? tr("Yes") : tr("No")));
}

// ============================================================================
// UserProfileBackupInstalledAppsPage
// ============================================================================

UserProfileBackupInstalledAppsPage::UserProfileBackupInstalledAppsPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Installed Applications"));
    setSubTitle(tr("Select installed applications to include in the backup for later restoration"));

    setupUi();
}

void UserProfileBackupInstalledAppsPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Instructions
    auto* instruction_label =
        new QLabel(tr("Click <b>Scan Applications</b> to detect all installed programs. "
                      "Selected applications will be saved to the backup so they can be "
                      "reinstalled via Chocolatey on the destination machine."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    setupScanControls(layout);
    setupAppsTree(layout);
    setupSelectionControls(layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No applications scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupInstalledAppsPage::setupScanControls(QVBoxLayout* layout) {
    auto* scan_layout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Applications"), this);
    m_scanButton->setIcon(QIcon::fromTheme("view-refresh"));
    m_scanButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(
        m_scanButton, &QPushButton::clicked, this, &UserProfileBackupInstalledAppsPage::onScanApps);
    scan_layout->addWidget(m_scanButton);

    m_statusLabel = new QLabel(tr("Click Scan Applications to begin"), this);
    scan_layout->addWidget(m_statusLabel, 1);
    layout->addLayout(scan_layout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);
}

void UserProfileBackupInstalledAppsPage::setupAppsTree(QVBoxLayout* layout) {
    m_appTree = new QTreeWidget(this);
    m_appTree->setHeaderLabels({tr("Application"), tr("Version"), tr("Publisher")});
    m_appTree->setAlternatingRowColors(true);
    m_appTree->setRootIsDecorated(true);
    m_appTree->header()->setSectionResizeMode(kInstalledAppColumnName, QHeaderView::Stretch);
    m_appTree->header()->setSectionResizeMode(kInstalledAppColumnVersion,
                                              QHeaderView::ResizeToContents);
    m_appTree->header()->setSectionResizeMode(kInstalledAppColumnPublisher,
                                              QHeaderView::ResizeToContents);
    m_appTree->setEnabled(false);
    connect(m_appTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileBackupInstalledAppsPage::onItemChanged);
    layout->addWidget(m_appTree);
}

void UserProfileBackupInstalledAppsPage::setupSelectionControls(QVBoxLayout* layout) {
    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupInstalledAppsPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupInstalledAppsPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);
}

void UserProfileBackupInstalledAppsPage::initializePage() {
    // Don't auto-scan -- let the user click Scan when ready
    updateNextButtonText();
}

void UserProfileBackupInstalledAppsPage::cleanupPage() {
    // Restore default button text when leaving the page
    auto* wiz = wizard();
    if (wiz != nullptr) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

/// @brief Check if any app in a category is checked
static bool categoryHasCheckedApp(QTreeWidgetItem* category) {
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        if (category->child(child_index)->checkState(0) == Qt::Checked) {
            return true;
        }
    }
    return false;
}

/// @brief Collect selected apps from one category into the output vector
static void collectCategoryApps(QTreeWidgetItem* category,
                                int& total,
                                int& selected,
                                QVector<InstalledAppInfo>& out) {
    for (int child_index = 0; child_index < category->childCount(); ++child_index) {
        auto* app_item = category->child(child_index);
        total++;
        if (app_item->checkState(0) != Qt::Checked) {
            continue;
        }
        selected++;
        InstalledAppInfo info;
        info.name = app_item->text(kInstalledAppColumnName);
        info.version = app_item->text(kInstalledAppColumnVersion);
        info.publisher = app_item->text(kInstalledAppColumnPublisher);
        info.category = category->text(kInstalledAppColumnName);
        info.selected = true;
        out.append(info);
    }
}

void UserProfileBackupInstalledAppsPage::updateNextButtonText() {
    Q_ASSERT(m_appTree);
    auto* wiz = wizard();
    if (wiz == nullptr) {
        return;
    }

    bool has_selection = false;
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        if (categoryHasCheckedApp(m_appTree->topLevelItem(category_index))) {
            has_selection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, has_selection ? tr("Next >") : tr("Skip >"));
}

bool UserProfileBackupInstalledAppsPage::isComplete() const {
    // Always complete -- app selection is optional
    return true;
}

struct AppCategoryRule {
    const char* category;
    std::initializer_list<const char*> keywords;
};

static const std::array<AppCategoryRule, 10> kAppCategoryRules = {{
    {.category = "Browsers",
     .keywords = {"chrome", "firefox", "edge", "opera", "brave", "vivaldi", "browser"}},
    {.category = "Development",
     .keywords = {"visual studio",
                  "vscode",
                  "jetbrains",
                  "intellij",
                  "pycharm",
                  "webstorm",
                  "rider",
                  "clion",
                  "android studio",
                  "eclipse",
                  "sublime",
                  "notepad++",
                  "atom",
                  "code"}},
    {.category = "Productivity",
     .keywords = {"office",
                  "word",
                  "excel",
                  "powerpoint",
                  "outlook",
                  "onenote",
                  "libreoffice",
                  "openoffice"}},
    {.category = "Communication",
     .keywords = {"discord", "slack", "teams", "zoom", "skype", "telegram", "signal", "whatsapp"}},
    {.category = "Gaming",
     .keywords = {"steam", "epic games", "origin", "battle.net", "gog", "ubisoft", "game"}},
    {.category = "Graphics & Design",
     .keywords = {"photoshop",
                  "illustrator",
                  "gimp",
                  "blender",
                  "inkscape",
                  "paint",
                  "krita",
                  "figma",
                  "canva"}},
    {.category = "Media",
     .keywords = {"vlc",
                  "spotify",
                  "itunes",
                  "audacity",
                  "obs",
                  "handbrake",
                  "media",
                  "player",
                  "foobar",
                  "winamp"}},
    {.category = "Utilities",
     .keywords = {"7-zip",
                  "winrar",
                  "peazip",
                  "ccleaner",
                  "everything",
                  "totalcommander",
                  "wiztree",
                  "treesize",
                  "windirstat",
                  "revo"}},
    {.category = "Security",
     .keywords = {"norton",
                  "kaspersky",
                  "malwarebytes",
                  "avast",
                  "avg",
                  "bitdefender",
                  "security",
                  "antivirus"}},
    {.category = "Drivers & Hardware",
     .keywords = {"nvidia", "amd", "realtek", "intel", "driver", "logitech", "corsair", "razer"}},
}};

QString UserProfileBackupInstalledAppsPage::categorizeApp(const QString& name,
                                                          const QString& publisher) {
    Q_UNUSED(publisher)

    const QString app_name_lower = name.toLower();
    for (const auto& rule : kAppCategoryRules) {
        for (const char* keyword : rule.keywords) {
            if (app_name_lower.contains(QLatin1String(keyword))) {
                return QCoreApplication::translate("UserProfileBackupInstalledAppsPage",
                                                   rule.category);
            }
        }
    }
    return QCoreApplication::translate("UserProfileBackupInstalledAppsPage", "Other");
}

void UserProfileBackupInstalledAppsPage::onScanApps() {
    Q_ASSERT(m_scanButton);
    Q_ASSERT(m_statusLabel);
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning installed applications..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);  // Indeterminate
    m_appTree->clear();

    // Use a QPointer so we can detect if the page was destroyed while scanning
    const QPointer<UserProfileBackupInstalledAppsPage> safe_this(this);

    // Run heavy scanning work on a background thread to avoid freezing the UI
    auto* watcher = new QFutureWatcher<QVector<InstalledAppInfo>>(this);

    connect(watcher,
            &QFutureWatcher<QVector<InstalledAppInfo>>::finished,
            this,
            [safe_this, watcher]() {
                watcher->deleteLater();
                if (!safe_this) {
                    return;  // Page was destroyed
                }

                auto app_infos = watcher->result();

                safe_this->m_scanned = true;
                safe_this->m_scanButton->setEnabled(true);
                safe_this->m_selectAllButton->setEnabled(true);
                safe_this->m_selectNoneButton->setEnabled(true);
                safe_this->m_appTree->setEnabled(true);
                safe_this->m_scanProgress->setVisible(false);

                safe_this->m_statusLabel->setText(
                    QCoreApplication::translate("UserProfileBackupInstalledAppsPage",
                                                "Found %1 application(s)")
                        .arg(app_infos.size()));
                safe_this->populateTree(app_infos);
                safe_this->updateNextButtonText();
                Q_EMIT safe_this->completeChanged();
            });

    watcher->setFuture(QtConcurrent::run([]() -> QVector<InstalledAppInfo> {
        // Scan installed apps from registry + AppX (no Chocolatey calls).
        // Backup only needs name/version/publisher -- restore handles Chocolatey matching.
        AppScanner scanner;
        auto apps = scanner.scanAll();

        sak::logDebug("onScanApps background: scanAll returned {} apps", apps.size());

        QVector<InstalledAppInfo> app_infos;
        app_infos.reserve(static_cast<qsizetype>(apps.size()));

        for (const auto& app : apps) {
            InstalledAppInfo info;
            info.name = app.name;
            info.version = app.version;
            info.publisher = app.publisher;
            info.selected = true;
            info.category = categorizeApp(app.name, app.publisher);
            app_infos.append(info);
        }

        sak::logDebug("onScanApps background: returning {} app infos", app_infos.size());
        return app_infos;
    }));
}

void UserProfileBackupInstalledAppsPage::populateTree(const QVector<InstalledAppInfo>& apps) {
    Q_ASSERT(m_appTree);
    Q_ASSERT(m_summaryLabel);
    m_appTree->blockSignals(true);
    m_appTree->clear();

    // Group apps by category
    QMap<QString, QVector<const InstalledAppInfo*>> categories;
    for (const auto& app : apps) {
        categories[app.category].append(&app);
    }

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* category_item = new QTreeWidgetItem(m_appTree);
        category_item->setText(kInstalledAppColumnName, it.key());
        category_item->setFlags(category_item->flags() | Qt::ItemIsUserCheckable);
        category_item->setCheckState(0, Qt::Checked);

        for (const auto* app : it.value()) {
            auto* app_item = new QTreeWidgetItem(category_item);
            app_item->setText(kInstalledAppColumnName, app->name);
            app_item->setText(kInstalledAppColumnVersion, app->version);
            app_item->setText(kInstalledAppColumnPublisher, app->publisher);
            app_item->setFlags(app_item->flags() | Qt::ItemIsUserCheckable);
            app_item->setCheckState(0, app->selected ? Qt::Checked : Qt::Unchecked);
        }

        category_item->setExpanded(true);
    }

    m_appTree->blockSignals(false);
    // Commit the default (all-checked) selection so it is not lost if the user
    // proceeds without toggling anything.
    commitAppSelection();
}

void UserProfileBackupInstalledAppsPage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_appTree);
    Q_ASSERT(m_summaryLabel);
    if (column != 0) {
        return;
    }

    m_appTree->blockSignals(true);

    // If it's a parent (category) item, propagate to children
    if (item->childCount() > 0) {
        const Qt::CheckState state = item->checkState(0);
        for (int i = 0; i < item->childCount(); ++i) {
            item->child(i)->setCheckState(0, state);
        }
    } else if (item->parent() != nullptr) {
        updateParentCheckState(item->parent());
    }

    m_appTree->blockSignals(false);

    commitAppSelection();
    updateNextButtonText();
}

void UserProfileBackupInstalledAppsPage::commitAppSelection() {
    Q_ASSERT(m_appTree);
    Q_ASSERT(m_summaryLabel);
    int total = 0;
    int selected = 0;
    QVector<InstalledAppInfo> selected_apps;

    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
        collectCategoryApps(
            m_appTree->topLevelItem(category_index), total, selected, selected_apps);
    }

    m_summaryLabel->setText(tr("%1 application(s) selected out of %2").arg(selected).arg(total));
    if (auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard())) {
        wiz->setInstalledApps(selected_apps);
    }
}

void UserProfileBackupInstalledAppsPage::updateParentCheckState(QTreeWidgetItem* parent) {
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

void UserProfileBackupInstalledAppsPage::onSelectAll() {
    Q_ASSERT(m_appTree);
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
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
    Q_ASSERT(m_appTree);
    m_appTree->blockSignals(true);
    for (int category_index = 0; category_index < m_appTree->topLevelItemCount();
         ++category_index) {
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

/// @brief Calculate size of a file or directory; returns -1 if path doesn't exist
static qint64 calculateSourceSize(const QString& path, const std::atomic_bool* cancel) {
    Q_ASSERT(!path.isEmpty());
    const QFileInfo info(path);
    if (!info.exists()) {
        return -1;
    }
    if (!info.isDir()) {
        return info.size();
    }
    qint64 size = 0;
    QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if ((cancel != nullptr) && cancel->load()) {
            return size;
        }
        it.next();
        size += it.fileInfo().size();
    }
    return size;
}

// PowerShell that reads each adapter's IPv4 config through Get-NetIPConfiguration +
// Get-NetIPInterface and emits it as JSON. It replaces a parse of
// `netsh interface ipv4 show config`, whose "Configuration for interface" / "DHCP enabled" /
// "IP Address" labels are LOCALIZED -- so the text parse silently captured NOTHING on a
// non-English Windows and the backup lost every adapter. The cmdlets, their
// Name/Dhcp/IPv4/Prefix/Gateway/Dns fields, and the Enabled/Disabled DHCP enum are all
// language-neutral. EthernetConfigInfo::parseNetIpConfigJson consumes the JSON (and converts the
// CIDR Prefix to the dotted-quad subnet mask the restore path feeds to `netsh set address`).
// The scan command now lives beside its parser in user_profile_types.h, because
// EthernetConfigManager needs the identical command -- see kNetIpConfigPowerShell.
constexpr auto kEthernetConfigPowerShell = sak::kNetIpConfigPowerShell;

static const auto kCommonAppDataSources = std::to_array<AppDataSourceInfo>({
    // Browsers
    {.name = "Chrome Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Local/Google/Chrome/User Data",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Firefox Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Roaming/Mozilla/Firefox/Profiles",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Edge Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Local/Microsoft/Edge/User Data",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Brave Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Local/BraveSoftware/Brave-Browser/User Data",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Opera Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Roaming/Opera Software/Opera Stable",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Vivaldi Profiles",
     .category = "Browsers",
     .relative_path = "AppData/Local/Vivaldi/User Data",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Email Clients
    {.name = "Thunderbird Profiles",
     .category = "Email",
     .relative_path = "AppData/Roaming/Thunderbird/Profiles",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Outlook Data",
     .category = "Email",
     .relative_path = "AppData/Local/Microsoft/Outlook",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // IDEs & Editors
    {.name = "VS Code Settings",
     .category = "Development",
     .relative_path = "AppData/Roaming/Code/User",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "VS Code Extensions",
     .category = "Development",
     .relative_path = ".vscode/extensions",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "JetBrains Settings",
     .category = "Development",
     .relative_path = "AppData/Roaming/JetBrains",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Sublime Text Settings",
     .category = "Development",
     .relative_path = "AppData/Roaming/Sublime Text 3",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Notepad++ Settings",
     .category = "Development",
     .relative_path = "AppData/Roaming/Notepad++",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Communication
    {.name = "Discord Data",
     .category = "Communication",
     .relative_path = "AppData/Roaming/discord",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Telegram Data",
     .category = "Communication",
     .relative_path = "AppData/Roaming/Telegram Desktop",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Signal Data",
     .category = "Communication",
     .relative_path = "AppData/Roaming/Signal",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Slack Data",
     .category = "Communication",
     .relative_path = "AppData/Roaming/Slack",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Gaming
    {.name = "Steam Config",
     .category = "Gaming",
     .relative_path = "AppData/Local/Steam",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Epic Games Settings",
     .category = "Gaming",
     .relative_path = "AppData/Local/EpicGamesLauncher",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Minecraft Data",
     .category = "Gaming",
     .relative_path = "AppData/Roaming/.minecraft",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Productivity
    {.name = "OneNote Cache",
     .category = "Productivity",
     .relative_path = "AppData/Local/Microsoft/OneNote",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Sticky Notes",
     .category = "Productivity",
     .relative_path = "AppData/Local/Packages/"
                      "Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/"
                      "LocalState",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Media
    {.name = "Spotify Data",
     .category = "Media",
     .relative_path = "AppData/Roaming/Spotify",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "VLC Settings",
     .category = "Media",
     .relative_path = "AppData/Roaming/vlc",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "OBS Studio",
     .category = "Media",
     .relative_path = "AppData/Roaming/obs-studio",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // Utilities
    {.name = "PuTTY Sessions",
     .category = "Utilities",
     .relative_path = "AppData/Roaming/PuTTY",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "WinSCP Settings",
     .category = "Utilities",
     .relative_path = "AppData/Roaming/WinSCP",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "FileZilla Settings",
     .category = "Utilities",
     .relative_path = "AppData/Roaming/FileZilla",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "PowerShell Profile",
     .category = "Utilities",
     .relative_path = "Documents/PowerShell",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Windows Terminal Settings",
     .category = "Utilities",
     .relative_path = "AppData/Local/Packages/"
                      "Microsoft.WindowsTerminal_8wekyb3d8bbwe/"
                      "LocalState",
     .size_bytes = 0,
     .exists = false,
     .selected = true},

    // System
    {.name = "SSH Keys",
     .category = "System",
     .relative_path = ".ssh",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "Git Config",
     .category = "System",
     .relative_path = ".gitconfig",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "npm Config",
     .category = "System",
     .relative_path = "AppData/Roaming/npm",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
    {.name = "pip Config",
     .category = "System",
     .relative_path = "AppData/Roaming/pip",
     .size_bytes = 0,
     .exists = false,
     .selected = true},
});

/// @brief Common application data sources to detect
static QVector<AppDataSourceInfo> getCommonAppDataSources() {
    return QVector<AppDataSourceInfo>(std::begin(kCommonAppDataSources),
                                      std::end(kCommonAppDataSources));
}

UserProfileBackupAppDataPage::UserProfileBackupAppDataPage(QVector<UserProfile>& users,
                                                           QWidget* parent)
    : QWizardPage(parent), m_users(users) {
    setTitle(tr("Application Data"));
    setSubTitle(tr("Select application data and settings to include in the backup"));

    m_scanWatcher = new QFutureWatcher<QVector<AppDataSourceInfo>>(this);
    connect(m_scanWatcher,
            &QFutureWatcher<QVector<AppDataSourceInfo>>::finished,
            this,
            &UserProfileBackupAppDataPage::onScanFinished);

    setupUi();
}

UserProfileBackupAppDataPage::~UserProfileBackupAppDataPage() {
    // Cooperatively cancel then join so the scan never outlives the page and
    // no slot fires on partially destroyed members.
    if (m_scanCancel) {
        m_scanCancel->store(true);
    }
    if ((m_scanWatcher != nullptr) && m_scanWatcher->isRunning()) {
        // SAK-ALLOW-BLOCKING: m_scanCancel was set above and the scan polls it, so the task
        // returns on its own. The watcher dies with this page, so abandoning the wait would
        // free it under a running scan.
        m_scanWatcher->waitForFinished();
    }
}

void UserProfileBackupAppDataPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label = new QLabel(
        tr("Click <b>Scan App Data</b> to detect application data on selected users. "
           "This backs up application settings, profiles, and configurations -- "
           "not the applications themselves (use the Installed Applications step for that)."),
        this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    auto* scan_layout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan App Data"), this);
    m_scanButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(
        m_scanButton, &QPushButton::clicked, this, &UserProfileBackupAppDataPage::onScanAppData);
    scan_layout->addWidget(m_scanButton);
    m_statusLabel = new QLabel(tr("Click Scan App Data to begin"), this);
    scan_layout->addWidget(m_statusLabel, 1);
    layout->addLayout(scan_layout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_appDataTree = new QTreeWidget(this);
    m_appDataTree->setHeaderLabels({tr("Application Data"), tr("Path"), tr("Size")});
    m_appDataTree->setAlternatingRowColors(true);
    m_appDataTree->setRootIsDecorated(true);
    m_appDataTree->header()->setSectionResizeMode(kAppDataColumnName, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(kAppDataColumnPath, QHeaderView::Stretch);
    m_appDataTree->header()->setSectionResizeMode(kAppDataColumnSize,
                                                  QHeaderView::ResizeToContents);
    m_appDataTree->setEnabled(false);
    connect(m_appDataTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileBackupAppDataPage::onItemChanged);
    layout->addWidget(m_appDataTree);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(
        m_selectAllButton, &QPushButton::clicked, this, &UserProfileBackupAppDataPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupAppDataPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No application data scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupAppDataPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupAppDataPage::isComplete() const {
    return true;  // Always complete -- app data selection is optional
}

void UserProfileBackupAppDataPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz != nullptr) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

// Worker-thread seam: size every common app-data source for the selected
// users. Read-only (no mutation), so it is safe to run off the GUI thread.
// Honors an optional cancel flag so an owner teardown can bound the wait.
static QVector<AppDataSourceInfo> scanSelectedAppDataSources(const QVector<UserProfile>& users,
                                                             const std::atomic_bool* cancel) {
    QVector<AppDataSourceInfo> all_sources;
    const auto common_sources = getCommonAppDataSources();
    for (const auto& user : users) {
        if (!user.is_selected) {
            continue;
        }
        for (auto source : common_sources) {
            if ((cancel != nullptr) && cancel->load()) {
                return all_sources;
            }
            const QString full_path = user.profile_path + "/" + source.relative_path;
            const qint64 path_size = calculateSourceSize(full_path, cancel);
            if (path_size < 0) {
                continue;
            }
            source.exists = true;
            source.size_bytes = path_size;
            all_sources.append(source);
        }
    }
    return all_sources;
}

void UserProfileBackupAppDataPage::onScanAppData() {
    Q_ASSERT(m_scanButton);
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_scanWatcher);
    if (m_scanWatcher->isRunning()) {
        return;  // A scan is already in flight; ignore re-entry.
    }
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning application data..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_appDataTree->clear();

    // Snapshot inputs so the worker never touches page-owned state, and share
    // a cancel flag the destructor can flip to bound the wait.
    m_scanCancel = std::make_shared<std::atomic_bool>(false);
    const QVector<UserProfile> users = m_users;
    const auto cancel = m_scanCancel;
    m_scanWatcher->setFuture(QtConcurrent::run(
        [users, cancel] { return scanSelectedAppDataSources(users, cancel.get()); }));
}

void UserProfileBackupAppDataPage::onScanFinished() {
    Q_ASSERT(m_scanWatcher);
    const QVector<AppDataSourceInfo> all_sources = m_scanWatcher->result();
    m_scanned = true;
    m_scanButton->setEnabled(true);
    m_selectAllButton->setEnabled(true);
    m_selectNoneButton->setEnabled(true);
    m_appDataTree->setEnabled(true);
    m_scanProgress->setVisible(false);

    m_statusLabel->setText(tr("Found %1 application data source(s)").arg(all_sources.size()));
    populateTree(all_sources);
    updateNextButtonText();
}

void UserProfileBackupAppDataPage::populateTree(const QVector<AppDataSourceInfo>& sources) {
    Q_ASSERT(m_appDataTree);
    Q_ASSERT(m_summaryLabel);
    m_appDataTree->blockSignals(true);
    m_appDataTree->clear();

    // Group by category
    QMap<QString, QVector<const AppDataSourceInfo*>> categories;
    for (const auto& source : sources) {
        categories[source.category].append(&source);
    }

    for (auto it = categories.constBegin(); it != categories.constEnd(); ++it) {
        auto* category_item = new QTreeWidgetItem(m_appDataTree);
        category_item->setText(kAppDataColumnName, it.key());
        category_item->setFlags(category_item->flags() | Qt::ItemIsUserCheckable);

        int cat_selected = 0;
        for (const auto* source : it.value()) {
            auto* item = new QTreeWidgetItem(category_item);
            item->setText(kAppDataColumnName, source->name);
            item->setText(kAppDataColumnPath, source->relative_path);
            const double size_mb = static_cast<double>(source->size_bytes) / sak::kBytesPerMBf;
            item->setText(kAppDataColumnSize,
                          QString("%1 MB").arg(size_mb, 0, 'f', kAppDataSizeDisplayPrecision));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(0, source->selected ? Qt::Checked : Qt::Unchecked);
            if (source->selected) {
                cat_selected++;
            }
        }

        category_item->setCheckState(0,
                                     cat_selected == it.value().size() ? Qt::Checked
                                     : cat_selected > 0                ? Qt::PartiallyChecked
                                                                       : Qt::Unchecked);
        category_item->setExpanded(true);
    }

    m_appDataTree->blockSignals(false);
    // Commit the default (all-checked) selection so it is not lost if the user
    // proceeds without toggling anything.
    commitAppDataSelection();
}

void UserProfileBackupAppDataPage::commitAppDataSelection() {
    Q_ASSERT(m_appDataTree);
    Q_ASSERT(m_summaryLabel);
    int total = 0;
    int selected = 0;
    QVector<AppDataSourceInfo> selected_sources;

    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            auto* app_item = category->child(j);
            total++;
            if (app_item->checkState(0) != Qt::Checked) {
                continue;
            }
            selected++;
            AppDataSourceInfo info;
            info.name = app_item->text(0);
            info.category = category->text(0);
            info.relative_path = app_item->text(1);
            info.selected = true;
            selected_sources.append(info);
        }
    }

    m_summaryLabel->setText(
        tr("%1 of %2 application data source(s) selected").arg(selected).arg(total));
    if (auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard())) {
        wiz->setAppDataSources(selected_sources);
    }
}

void UserProfileBackupAppDataPage::updateParentCheckState(QTreeWidgetItem* parent) {
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

void UserProfileBackupAppDataPage::updateNextButtonText() {
    Q_ASSERT(m_appDataTree);
    auto* wiz = wizard();
    if (wiz == nullptr) {
        return;
    }

    bool has_selection = false;
    for (int i = 0; i < m_appDataTree->topLevelItemCount(); ++i) {
        auto* category = m_appDataTree->topLevelItem(i);
        for (int j = 0; j < category->childCount(); ++j) {
            if (category->child(j)->checkState(0) == Qt::Checked) {
                has_selection = true;
                break;
            }
        }
        if (has_selection) {
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, has_selection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupAppDataPage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_appDataTree);
    Q_ASSERT(m_summaryLabel);
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

    commitAppDataSelection();

    updateNextButtonText();
}

void UserProfileBackupAppDataPage::onSelectAll() {
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

void UserProfileBackupAppDataPage::onSelectNone() {
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
// UserProfileBackupKnownNetworksPage
// ============================================================================

UserProfileBackupKnownNetworksPage::UserProfileBackupKnownNetworksPage(QWidget* parent)
    : QWizardPage(parent) {
    setTitle(tr("Known WiFi Networks"));
    setSubTitle(tr("Scan and select WiFi network profiles to include in the backup"));

    setupUi();
}

void UserProfileBackupKnownNetworksPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("Click <b>Scan Networks</b> to detect saved WiFi profiles. "
                      "Selected profiles will be exported and included in the backup "
                      "for easy restoration on the destination machine."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    auto* scan_layout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Networks"), this);
    m_scanButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_scanButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupKnownNetworksPage::onScanNetworks);
    scan_layout->addWidget(m_scanButton);
    m_statusLabel = new QLabel(tr("Click Scan Networks to begin"), this);
    scan_layout->addWidget(m_statusLabel, 1);
    layout->addLayout(scan_layout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_networkTree = new QTreeWidget(this);
    m_networkTree->setHeaderLabels({tr("Network Name (SSID)"), tr("Security Type")});
    m_networkTree->setAlternatingRowColors(true);
    m_networkTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_networkTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_networkTree->setEnabled(false);
    connect(m_networkTree,
            &QTreeWidget::itemChanged,
            this,
            &UserProfileBackupKnownNetworksPage::onItemChanged);
    layout->addWidget(m_networkTree);

    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupKnownNetworksPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupKnownNetworksPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No WiFi networks scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupKnownNetworksPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupKnownNetworksPage::isComplete() const {
    return true;  // Always complete -- network backup is optional
}

void UserProfileBackupKnownNetworksPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz != nullptr) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupKnownNetworksPage::onScanNetworks() {
    Q_ASSERT(m_scanButton);
    Q_ASSERT(m_statusLabel);
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
    Q_ASSERT(m_networkTree);
    Q_ASSERT(m_summaryLabel);
    m_networkTree->blockSignals(true);
    m_networkTree->clear();

    // Keep the full scanned profiles (incl. XML payload) so the committed
    // selection copies them intact rather than reconstructing from columns.
    m_scannedProfiles = profiles;
    for (int i = 0; i < profiles.size(); ++i) {
        const auto& profile = profiles[i];
        auto* item = new QTreeWidgetItem(m_networkTree);
        item->setText(0, profile.profile_name);
        item->setText(1, profile.security_type.isEmpty() ? tr("Unknown") : profile.security_type);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, profile.selected ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, i);  // Index into m_scannedProfiles
    }

    m_networkTree->blockSignals(false);
    // Commit the default (all-checked) selection so it is not lost if the user
    // proceeds without toggling anything.
    commitWifiSelection();
}

void UserProfileBackupKnownNetworksPage::commitWifiSelection() {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_networkTree);
    int selected = 0;
    const int total = m_networkTree->topLevelItemCount();
    QVector<WifiProfileInfo> selected_profiles;

    for (int i = 0; i < total; ++i) {
        auto* tree_item = m_networkTree->topLevelItem(i);
        if (tree_item->checkState(0) != Qt::Checked) {
            continue;
        }
        selected++;
        const int idx = tree_item->data(0, Qt::UserRole).toInt();
        WifiProfileInfo info = (idx >= 0 && idx < m_scannedProfiles.size()) ? m_scannedProfiles[idx]
                                                                            : WifiProfileInfo{};
        info.profile_name = tree_item->text(0);  // fall back to display text if unmapped
        info.selected = true;
        selected_profiles.append(info);
    }

    m_summaryLabel->setText(tr("%1 of %2 WiFi profile(s) selected").arg(selected).arg(total));
    if (auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard())) {
        wiz->setWifiProfiles(selected_profiles);
    }
}

void UserProfileBackupKnownNetworksPage::updateNextButtonText() {
    Q_ASSERT(m_networkTree);
    auto* wiz = wizard();
    if (wiz == nullptr) {
        return;
    }

    bool has_selection = false;
    for (int i = 0; i < m_networkTree->topLevelItemCount(); ++i) {
        if (m_networkTree->topLevelItem(i)->checkState(0) == Qt::Checked) {
            has_selection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, has_selection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupKnownNetworksPage::onItemChanged(QTreeWidgetItem* item, int column) {
    Q_ASSERT(m_summaryLabel);
    Q_ASSERT(m_networkTree);
    if (column != 0) {
        return;
    }
    Q_UNUSED(item)
    commitWifiSelection();
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
    : QWizardPage(parent) {
    setTitle(tr("Ethernet Settings"));
    setSubTitle(tr("Scan and select ethernet adapter configurations to include in the backup"));

    setupUi();
}

void UserProfileBackupEthernetSettingsPage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    auto* instruction_label =
        new QLabel(tr("Click <b>Scan Ethernet</b> to detect network adapter configurations. "
                      "Selected adapter settings (IP, DNS, gateway) will be saved to the backup. "
                      "This is especially useful for static IP configurations."),
                   this);
    instruction_label->setWordWrap(true);
    layout->addWidget(instruction_label);

    auto* scan_layout = new QHBoxLayout();
    m_scanButton = new QPushButton(tr("Scan Ethernet"), this);
    m_scanButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_scanButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupEthernetSettingsPage::onScanEthernet);
    scan_layout->addWidget(m_scanButton);
    // On a failed scan this label shows raw netsh output (adapter names and error text), so it
    // renders verbatim rather than letting QLabel auto-detect markup inside it.
    m_statusLabel = plainTextLabel(tr("Click Scan Ethernet to begin"), this);
    scan_layout->addWidget(m_statusLabel, 1);
    layout->addLayout(scan_layout);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setVisible(false);
    layout->addWidget(m_scanProgress);

    m_ethernetTable = new QTableWidget(0, kEthernetColumnCount, this);
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
    connect(m_ethernetTable,
            &QTableWidget::itemChanged,
            this,
            &UserProfileBackupEthernetSettingsPage::onEthernetItemChanged);
    layout->addWidget(m_ethernetTable);

    setupSelectionButtons(layout);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setStyleSheet(sak::ui::notePanelStyle(sak::ui::kColorBgInfoPanel));
    m_summaryLabel->setText(tr("No ethernet adapters scanned yet"));
    layout->addWidget(m_summaryLabel);
}

void UserProfileBackupEthernetSettingsPage::setupSelectionButtons(QVBoxLayout* layout) {
    auto* button_layout = new QHBoxLayout();
    m_selectAllButton = new QPushButton(tr("Select All"), this);
    m_selectAllButton->setEnabled(false);
    m_selectAllButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_selectAllButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupEthernetSettingsPage::onSelectAll);
    button_layout->addWidget(m_selectAllButton);

    m_selectNoneButton = new QPushButton(tr("Select None"), this);
    m_selectNoneButton->setEnabled(false);
    m_selectNoneButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_selectNoneButton,
            &QPushButton::clicked,
            this,
            &UserProfileBackupEthernetSettingsPage::onSelectNone);
    button_layout->addWidget(m_selectNoneButton);
    button_layout->addStretch();
    layout->addLayout(button_layout);
}

void UserProfileBackupEthernetSettingsPage::initializePage() {
    updateNextButtonText();
}

bool UserProfileBackupEthernetSettingsPage::isComplete() const {
    return true;  // Always complete -- ethernet backup is optional
}

void UserProfileBackupEthernetSettingsPage::cleanupPage() {
    auto* wiz = wizard();
    if (wiz != nullptr) {
        wiz->setButtonText(QWizard::NextButton, tr("Next >"));
    }
}

void UserProfileBackupEthernetSettingsPage::onScanEthernet() {
    Q_ASSERT(m_scanProgress);
    Q_ASSERT(m_ethernetTable);
    Q_ASSERT(m_scanButton);
    Q_ASSERT(m_statusLabel);
    m_scanButton->setEnabled(false);
    m_statusLabel->setText(tr("Scanning ethernet adapters..."));
    m_scanProgress->setVisible(true);
    m_scanProgress->setRange(0, 0);
    m_ethernetTable->setRowCount(0);

    auto* watcher = new QFutureWatcher<QPair<bool, QString>>(this);
    connect(watcher, &QFutureWatcher<QPair<bool, QString>>::finished, this, [this, watcher]() {
        const auto [success, output] = watcher->result();
        watcher->deleteLater();

        m_scanButton->setEnabled(true);
        m_scanProgress->setVisible(false);

        if (!success) {
            m_statusLabel->setText(output);
            return;
        }

        auto configs = EthernetConfigInfo::parseNetIpConfigJson(output);
        m_scanned = true;
        m_selectAllButton->setEnabled(true);
        m_selectNoneButton->setEnabled(true);
        m_ethernetTable->setEnabled(true);

        m_statusLabel->setText(tr("Found %1 ethernet adapter(s)").arg(configs.size()));
        populateTable(configs);
        updateNextButtonText();
    });

    watcher->setFuture(QtConcurrent::run([]() -> QPair<bool, QString> {
        // System32-qualified powershell, never the bare name: CreateProcess searches the current
        // directory ahead of System32, so a planted powershell could fabricate the adapter list
        // that a restore later applies. Unresolvable -> FAILED scan (fail closed).
        const QString powershell_exe = sak::systemPowerShellPath();
        if (powershell_exe.isEmpty()) {
            sak::logError("Cannot resolve the System32 powershell.exe path; ethernet scan aborted");
            return {false, QStringLiteral("Cannot resolve the System32 powershell.exe path")};
        }
        const auto process = runProcess(powershell_exe,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NonInteractive"),
                                         QStringLiteral("-Command"),
                                         QString::fromLatin1(kEthernetConfigPowerShell)},
                                        sak::kTimeoutNetworkReadMs);
        if (process.timed_out) {
            sak::logError("Timed out scanning ethernet adapters for backup");
            return {false, QStringLiteral("Ethernet scan timed out")};
        }
        if (process.exit_code != 0) {
            sak::logError("Get-NetIPConfiguration failed for ethernet adapter scan: {}",
                          process.std_err.toStdString());
            return {false, QStringLiteral("Failed to scan ethernet adapters")};
        }
        return {true, process.std_out};
    }));
}

void UserProfileBackupEthernetSettingsPage::populateTable(
    const QVector<EthernetConfigInfo>& configs) {
    // Keep the full scanned configs (incl. DNS) so the committed selection copies
    // them intact rather than reconstructing from the displayed columns.
    m_scannedConfigs = configs;
    m_ethernetTable->blockSignals(true);
    m_ethernetTable->setRowCount(0);

    for (const auto& config : configs) {
        const int row = m_ethernetTable->rowCount();
        m_ethernetTable->insertRow(row);

        auto* check_item = new QTableWidgetItem();
        check_item->setCheckState(Qt::Checked);
        check_item->setData(Qt::UserRole, row);  // Index into m_scannedConfigs
        m_ethernetTable->setItem(row, kEthernetColumnSelect, check_item);

        auto* name_item = new QTableWidgetItem(config.adapter_name);
        name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnAdapter, name_item);

        auto* dhcp_item = new QTableWidgetItem(config.dhcp_enabled ? tr("Yes") : tr("No"));
        dhcp_item->setFlags(dhcp_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnDhcp, dhcp_item);

        auto* ip_item = new QTableWidgetItem(config.ip_address);
        ip_item->setFlags(ip_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnIp, ip_item);

        auto* subnet_item = new QTableWidgetItem(config.subnet_mask);
        subnet_item->setFlags(subnet_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnSubnet, subnet_item);

        auto* gw_item = new QTableWidgetItem(config.default_gateway);
        gw_item->setFlags(gw_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnGateway, gw_item);

        QString dns = config.dns_primary;
        if (!config.dns_secondary.isEmpty()) {
            dns += ", " + config.dns_secondary;
        }
        auto* dns_item = new QTableWidgetItem(dns);
        dns_item->setFlags(dns_item->flags() & ~Qt::ItemIsEditable);
        m_ethernetTable->setItem(row, kEthernetColumnDns, dns_item);
    }

    m_ethernetTable->blockSignals(false);
    // Commit the default (all-checked) selection so it survives a Next without
    // any manual toggling.
    commitEthernetSelection();
}

void UserProfileBackupEthernetSettingsPage::commitEthernetSelection() {
    Q_ASSERT(m_ethernetTable);
    Q_ASSERT(m_summaryLabel);
    QVector<EthernetConfigInfo> configs;
    for (int row = 0; row < m_ethernetTable->rowCount(); ++row) {
        auto* check_item = m_ethernetTable->item(row, kEthernetColumnSelect);
        if ((check_item == nullptr) || check_item->checkState() != Qt::Checked) {
            continue;
        }
        const int idx = check_item->data(Qt::UserRole).toInt();
        EthernetConfigInfo info = (idx >= 0 && idx < m_scannedConfigs.size())
                                      ? m_scannedConfigs[idx]
                                      : EthernetConfigInfo{};
        info.selected = true;
        configs.append(info);
    }

    m_summaryLabel->setText(tr("%1 ethernet adapter(s) selected").arg(configs.size()));
    if (auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard())) {
        wiz->setEthernetConfigs(configs);
    }
}

void UserProfileBackupEthernetSettingsPage::onEthernetItemChanged(QTableWidgetItem* item) {
    if ((item != nullptr) && item->column() != kEthernetColumnSelect) {
        return;
    }
    commitEthernetSelection();
    updateNextButtonText();
}

void UserProfileBackupEthernetSettingsPage::updateNextButtonText() {
    Q_ASSERT(m_ethernetTable);
    auto* wiz = wizard();
    if (wiz == nullptr) {
        return;
    }

    bool has_selection = false;
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        if (m_ethernetTable->item(i, kEthernetColumnSelect)->checkState() == Qt::Checked) {
            has_selection = true;
            break;
        }
    }

    wiz->setButtonText(QWizard::NextButton, has_selection ? tr("Next >") : tr("Skip >"));
}

void UserProfileBackupEthernetSettingsPage::onSelectAll() {
    Q_ASSERT(m_ethernetTable);
    m_ethernetTable->blockSignals(true);
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, kEthernetColumnSelect)->setCheckState(Qt::Checked);
    }
    m_ethernetTable->blockSignals(false);
    // Copy the scanned configs intact (preserving DNS and other fields).
    commitEthernetSelection();
    updateNextButtonText();
}

void UserProfileBackupEthernetSettingsPage::onSelectNone() {
    Q_ASSERT(m_ethernetTable);
    m_ethernetTable->blockSignals(true);
    for (int i = 0; i < m_ethernetTable->rowCount(); ++i) {
        m_ethernetTable->item(i, kEthernetColumnSelect)->setCheckState(Qt::Unchecked);
    }
    m_ethernetTable->blockSignals(false);
    commitEthernetSelection();
    updateNextButtonText();
}

}  // namespace sak
