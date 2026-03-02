// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_restore_wizard.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QDir>
#include <QFileDialog>

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
        tr("<b>App Restore</b>: Reinstall saved applications via Chocolatey (optional)"),
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
    m_manifestInfoLabel->setStyleSheet(QString("QLabel { background-color: %1; padding: 12px; "
                                               "border-radius: 10px; }")
                                                   .arg(sak::ui::kColorBgSurface));
    m_manifestInfoLabel->hide();
    layout->addWidget(m_manifestInfoLabel);

    layout->addStretch(1);

    // Connections
    connect(m_browseButton, &QPushButton::clicked, this,
        &UserProfileRestoreWelcomePage::onBrowseBackup);
    connect(m_backupPathEdit, &QLineEdit::textChanged, this,
        &UserProfileRestoreWelcomePage::onBackupPathChanged);
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
        m_manifestInfoLabel->setText(tr("❌ Invalid backup path. Please select a backup directory "
                                        "or manifest.json file."));
        m_manifestInfoLabel->show();
        Q_EMIT completeChanged();
        return;
    }

    // Load manifest
    BackupManifest manifest = BackupManifest::loadFromFile(manifestPath);
    if (manifest.version.isEmpty()) {
        m_manifestInfoLabel->setText(tr("❌ Failed to load backup manifest. The backup may be "
                                        "corrupted."));
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
        QString::number(manifest.total_backup_size_bytes / sak::kBytesPerGBf, 'f', 2)
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
    addPage(new UserProfileRestoreAppRestorePage(this));
    addPage(new UserProfileRestorePermissionSettingsPage(this));
    addPage(new UserProfileRestoreExecutePage(this));

    resize(sak::kWizardLargeWidth, sak::kWizardLargeHeight);
}

} // namespace sak
