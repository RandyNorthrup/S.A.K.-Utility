#include "gui/settings_dialog.h"
#include "sak/config_manager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QPushButton>
#include <QDialogButtonBox>

namespace sak {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUI();
    loadSettings();
    
    // Connect signals
    connect(m_okButton, &QPushButton::clicked, this, &SettingsDialog::onOkClicked);
    connect(m_cancelButton, &QPushButton::clicked, this, &SettingsDialog::onCancelClicked);
    connect(m_applyButton, &QPushButton::clicked, this, &SettingsDialog::onApplyClicked);
    connect(m_resetButton, &QPushButton::clicked, this, &SettingsDialog::onResetToDefaultsClicked);
}

void SettingsDialog::setupUI() {
    setWindowTitle(tr("Settings"));
    setMinimumSize(600, 500);

    auto* mainLayout = new QVBoxLayout(this);

    // Create tab widget
    m_tabWidget = new QTabWidget(this);
    createGeneralTab();
    createBackupTab();
    createOrganizerTab();
    createDuplicateFinderTab();
    createLicenseScannerTab();
    createAdvancedTab();

    mainLayout->addWidget(m_tabWidget);

    // Button layout
    auto* buttonLayout = new QHBoxLayout();
    
    m_resetButton = new QPushButton(tr("Reset to Defaults"), this);
    buttonLayout->addWidget(m_resetButton);
    
    buttonLayout->addStretch();
    
    m_okButton = new QPushButton(tr("OK"), this);
    m_okButton->setDefault(true);
    buttonLayout->addWidget(m_okButton);
    
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    buttonLayout->addWidget(m_cancelButton);
    
    m_applyButton = new QPushButton(tr("Apply"), this);
    m_applyButton->setEnabled(false);
    buttonLayout->addWidget(m_applyButton);

    mainLayout->addLayout(buttonLayout);
}

void SettingsDialog::createGeneralTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Window Settings Group
    auto* windowGroup = new QGroupBox(tr("Window Settings"));
    auto* windowLayout = new QFormLayout();

    m_restoreWindowGeometry = new QCheckBox(tr("Restore window size and position on startup"));
    windowLayout->addRow(tr("Window Geometry:"), m_restoreWindowGeometry);

    windowGroup->setLayout(windowLayout);
    layout->addWidget(windowGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("General"));

    // Connect change signals
    connect(m_restoreWindowGeometry, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createBackupTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Backup Settings Group
    auto* backupGroup = new QGroupBox(tr("Backup Settings"));
    auto* backupLayout = new QFormLayout();

    m_backupThreadCount = new QSpinBox();
    m_backupThreadCount->setRange(1, 16);
    m_backupThreadCount->setToolTip(tr("Number of concurrent threads for backup operations"));
    backupLayout->addRow(tr("Thread Count:"), m_backupThreadCount);

    m_backupVerifyMD5 = new QCheckBox(tr("Verify files using MD5 hash after backup"));
    backupLayout->addRow(tr("Verify MD5:"), m_backupVerifyMD5);

    auto* locationLayout = new QHBoxLayout();
    m_lastBackupLocation = new QLineEdit();
    m_lastBackupLocation->setReadOnly(true);
    auto* browseButton = new QPushButton(tr("Browse..."));
    connect(browseButton, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(
            this,
            tr("Select Backup Location"),
            m_lastBackupLocation->text()
        );
        if (!dir.isEmpty()) {
            m_lastBackupLocation->setText(dir);
            onSettingChanged();
        }
    });
    locationLayout->addWidget(m_lastBackupLocation);
    locationLayout->addWidget(browseButton);
    backupLayout->addRow(tr("Last Location:"), locationLayout);

    backupGroup->setLayout(backupLayout);
    layout->addWidget(backupGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Backup"));

    // Connect change signals
    connect(m_backupThreadCount, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_backupVerifyMD5, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createOrganizerTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Organizer Settings Group
    auto* organizerGroup = new QGroupBox(tr("Organizer Settings"));
    auto* organizerLayout = new QFormLayout();

    m_organizerPreviewMode = new QCheckBox(tr("Enable preview mode (dry run) by default"));
    m_organizerPreviewMode->setToolTip(tr("When enabled, organizer will show what it would do without actually moving files"));
    organizerLayout->addRow(tr("Preview Mode:"), m_organizerPreviewMode);

    organizerGroup->setLayout(organizerLayout);
    layout->addWidget(organizerGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Organizer"));

    // Connect change signals
    connect(m_organizerPreviewMode, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createDuplicateFinderTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Duplicate Finder Settings Group
    auto* duplicateGroup = new QGroupBox(tr("Duplicate Finder Settings"));
    auto* duplicateLayout = new QFormLayout();

    m_duplicateMinFileSize = new QSpinBox();
    m_duplicateMinFileSize->setRange(0, 1024);
    m_duplicateMinFileSize->setSuffix(tr(" KB"));
    m_duplicateMinFileSize->setToolTip(tr("Minimum file size to consider for duplicate detection (0 = all files)"));
    duplicateLayout->addRow(tr("Minimum File Size:"), m_duplicateMinFileSize);

    m_duplicateKeepStrategy = new QComboBox();
    m_duplicateKeepStrategy->addItems({tr("Oldest"), tr("Newest"), tr("First Found")});
    m_duplicateKeepStrategy->setToolTip(tr("Strategy for selecting which duplicate to keep"));
    duplicateLayout->addRow(tr("Keep Strategy:"), m_duplicateKeepStrategy);

    duplicateGroup->setLayout(duplicateLayout);
    layout->addWidget(duplicateGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Duplicate Finder"));

    // Connect change signals
    connect(m_duplicateMinFileSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_duplicateKeepStrategy, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createLicenseScannerTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // License Scanner Settings Group
    auto* licenseGroup = new QGroupBox(tr("License Scanner Settings"));
    auto* licenseLayout = new QFormLayout();

    m_licenseScanRegistry = new QCheckBox(tr("Scan Windows Registry for licenses"));
    m_licenseScanRegistry->setToolTip(tr("Search registry keys for software license information"));
    licenseLayout->addRow(tr("Scan Registry:"), m_licenseScanRegistry);

    m_licenseScanFilesystem = new QCheckBox(tr("Scan filesystem for license files"));
    m_licenseScanFilesystem->setToolTip(tr("Search common locations for license key files"));
    licenseLayout->addRow(tr("Scan Filesystem:"), m_licenseScanFilesystem);

    licenseGroup->setLayout(licenseLayout);
    layout->addWidget(licenseGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("License Scanner"));

    // Connect change signals
    connect(m_licenseScanRegistry, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_licenseScanFilesystem, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createAdvancedTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    auto* infoLabel = new QLabel(tr(
        "<b>Advanced Settings</b><br><br>"
        "Advanced configuration options will be added here in future versions.<br>"
        "This may include:<br>"
        "- Custom file type associations<br>"
        "- Performance tuning<br>"
        "- Logging levels<br>"
        "- Network settings"
    ));
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Advanced"));
}

void SettingsDialog::loadSettings() {
    auto& config = ConfigManager::instance();

    // General
    m_restoreWindowGeometry->setChecked(config.getRestoreWindowGeometry());

    // Backup
    m_backupThreadCount->setValue(config.getBackupThreadCount());
    m_backupVerifyMD5->setChecked(config.getBackupVerifyMD5());
    m_lastBackupLocation->setText(config.getLastBackupLocation());

    // Organizer
    m_organizerPreviewMode->setChecked(config.getOrganizerPreviewMode());

    // Duplicate Finder
    m_duplicateMinFileSize->setValue(static_cast<int>(config.getDuplicateMinimumFileSize() / 1024));
    
    QString strategy = config.getDuplicateKeepStrategy();
    if (strategy == "oldest") {
        m_duplicateKeepStrategy->setCurrentIndex(0);
    } else if (strategy == "newest") {
        m_duplicateKeepStrategy->setCurrentIndex(1);
    } else {
        m_duplicateKeepStrategy->setCurrentIndex(2); // first
    }

    // License Scanner
    m_licenseScanRegistry->setChecked(config.getLicenseScanRegistry());
    m_licenseScanFilesystem->setChecked(config.getLicenseScanFilesystem());

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

void SettingsDialog::saveSettings() {
    auto& config = ConfigManager::instance();

    // General
    config.setRestoreWindowGeometry(m_restoreWindowGeometry->isChecked());

    // Backup
    config.setBackupThreadCount(m_backupThreadCount->value());
    config.setBackupVerifyMD5(m_backupVerifyMD5->isChecked());
    config.setLastBackupLocation(m_lastBackupLocation->text());

    // Organizer
    config.setOrganizerPreviewMode(m_organizerPreviewMode->isChecked());

    // Duplicate Finder
    config.setDuplicateMinimumFileSize(static_cast<qint64>(m_duplicateMinFileSize->value()) * 1024);
    
    QString strategy;
    switch (m_duplicateKeepStrategy->currentIndex()) {
        case 0: strategy = "oldest"; break;
        case 1: strategy = "newest"; break;
        case 2: strategy = "first"; break;
        default: strategy = "first"; break;
    }
    config.setDuplicateKeepStrategy(strategy);

    // License Scanner
    config.setLicenseScanRegistry(m_licenseScanRegistry->isChecked());
    config.setLicenseScanFilesystem(m_licenseScanFilesystem->isChecked());

    // Sync to disk
    config.sync();

    m_settingsModified = false;
    m_applyButton->setEnabled(false);
}

void SettingsDialog::applySettings() {
    if (!validateSettings()) {
        return;
    }
    saveSettings();
}

bool SettingsDialog::validateSettings() {
    // Validate thread count
    if (m_backupThreadCount->value() < 1) {
        QMessageBox::warning(this, tr("Invalid Setting"), 
                           tr("Thread count must be at least 1."));
        m_tabWidget->setCurrentIndex(1); // Switch to Backup tab
        m_backupThreadCount->setFocus();
        return false;
    }

    // Validate minimum file size
    if (m_duplicateMinFileSize->value() < 0) {
        QMessageBox::warning(this, tr("Invalid Setting"), 
                           tr("Minimum file size cannot be negative."));
        m_tabWidget->setCurrentIndex(3); // Switch to Duplicate Finder tab
        m_duplicateMinFileSize->setFocus();
        return false;
    }

    return true;
}

void SettingsDialog::onApplyClicked() {
    applySettings();
}

void SettingsDialog::onOkClicked() {
    if (m_settingsModified) {
        applySettings();
    }
    accept();
}

void SettingsDialog::onCancelClicked() {
    if (m_settingsModified) {
        auto reply = QMessageBox::question(
            this,
            tr("Unsaved Changes"),
            tr("You have unsaved changes. Are you sure you want to discard them?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        
        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    reject();
}

void SettingsDialog::onResetToDefaultsClicked() {
    auto reply = QMessageBox::question(
        this,
        tr("Reset to Defaults"),
        tr("Are you sure you want to reset all settings to their default values?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        ConfigManager::instance().resetToDefaults();
        loadSettings();
        QMessageBox::information(this, tr("Reset Complete"), 
                               tr("All settings have been reset to defaults."));
    }
}

void SettingsDialog::onSettingChanged() {
    m_settingsModified = true;
    m_applyButton->setEnabled(true);
}

} // namespace sak
