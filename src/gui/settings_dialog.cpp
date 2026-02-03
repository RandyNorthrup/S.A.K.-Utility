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
    createImageFlasherTab();
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

void SettingsDialog::createImageFlasherTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Validation Settings Group
    auto* validationGroup = new QGroupBox(tr("Validation Settings"));
    auto* validationLayout = new QFormLayout();

    m_imageFlasherValidationMode = new QComboBox();
    m_imageFlasherValidationMode->addItem(tr("Full Verification"), "full");
    m_imageFlasherValidationMode->addItem(tr("Quick Check"), "quick");
    m_imageFlasherValidationMode->addItem(tr("No Verification"), "none");
    m_imageFlasherValidationMode->setToolTip(tr("Choose verification method after writing"));
    validationLayout->addRow(tr("Verification Mode:"), m_imageFlasherValidationMode);

    validationGroup->setLayout(validationLayout);
    layout->addWidget(validationGroup);

    // Performance Settings Group
    auto* perfGroup = new QGroupBox(tr("Performance Settings"));
    auto* perfLayout = new QFormLayout();

    m_imageFlasherBufferSize = new QSpinBox();
    m_imageFlasherBufferSize->setRange(16, 1024);
    m_imageFlasherBufferSize->setSuffix(tr(" MB"));
    m_imageFlasherBufferSize->setToolTip(tr("Buffer size for reading/writing (higher = faster but more memory)"));
    perfLayout->addRow(tr("Buffer Size:"), m_imageFlasherBufferSize);

    m_imageFlasherMaxConcurrentWrites = new QSpinBox();
    m_imageFlasherMaxConcurrentWrites->setRange(1, 32);
    m_imageFlasherMaxConcurrentWrites->setToolTip(tr("Maximum number of simultaneous writes"));
    perfLayout->addRow(tr("Max Concurrent Writes:"), m_imageFlasherMaxConcurrentWrites);

    perfGroup->setLayout(perfLayout);
    layout->addWidget(perfGroup);

    // Safety Settings Group
    auto* safetyGroup = new QGroupBox(tr("Safety Settings"));
    auto* safetyLayout = new QVBoxLayout();

    m_imageFlasherShowSystemDriveWarning = new QCheckBox(tr("Warn when system drive selected"));
    safetyLayout->addWidget(m_imageFlasherShowSystemDriveWarning);

    m_imageFlasherShowLargeDriveWarning = new QCheckBox(tr("Warn when large drive selected"));
    safetyLayout->addWidget(m_imageFlasherShowLargeDriveWarning);

    auto* thresholdLayout = new QHBoxLayout();
    thresholdLayout->addWidget(new QLabel(tr("Large drive threshold:")));
    m_imageFlasherLargeDriveThreshold = new QSpinBox();
    m_imageFlasherLargeDriveThreshold->setRange(32, 10000);
    m_imageFlasherLargeDriveThreshold->setSuffix(tr(" GB"));
    m_imageFlasherLargeDriveThreshold->setToolTip(tr("Drives larger than this will trigger warning"));
    thresholdLayout->addWidget(m_imageFlasherLargeDriveThreshold);
    thresholdLayout->addStretch();
    safetyLayout->addLayout(thresholdLayout);

    safetyGroup->setLayout(safetyLayout);
    layout->addWidget(safetyGroup);

    // Behavior Settings Group
    auto* behaviorGroup = new QGroupBox(tr("Behavior Settings"));
    auto* behaviorLayout = new QVBoxLayout();

    m_imageFlasherUnmountOnCompletion = new QCheckBox(tr("Auto-unmount on completion"));
    m_imageFlasherUnmountOnCompletion->setToolTip(tr("Automatically unmount drives after successful flash"));
    behaviorLayout->addWidget(m_imageFlasherUnmountOnCompletion);

    m_imageFlasherEnableNotifications = new QCheckBox(tr("Enable desktop notifications"));
    m_imageFlasherEnableNotifications->setToolTip(tr("Show system notifications when flash completes"));
    behaviorLayout->addWidget(m_imageFlasherEnableNotifications);

    behaviorGroup->setLayout(behaviorLayout);
    layout->addWidget(behaviorGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Image Flasher"));

    // Connect change signals
    connect(m_imageFlasherValidationMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherBufferSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherUnmountOnCompletion, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherShowSystemDriveWarning, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherShowLargeDriveWarning, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherLargeDriveThreshold, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherMaxConcurrentWrites, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_imageFlasherEnableNotifications, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
}

void SettingsDialog::createAdvancedTab() {
    auto* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);

    // Network Transfer Settings
    auto* networkGroup = new QGroupBox(tr("Network Transfer"));
    auto* networkLayout = new QFormLayout();

    m_networkTransferEnabled = new QCheckBox(tr("Enable Network Transfer"));
    networkLayout->addRow(tr("Enabled:"), m_networkTransferEnabled);

    m_networkTransferAutoDiscovery = new QCheckBox(tr("Enable auto discovery"));
    networkLayout->addRow(tr("Auto Discovery:"), m_networkTransferAutoDiscovery);

    m_networkTransferEncryption = new QCheckBox(tr("Encrypt data in transit (AES-256-GCM)"));
    networkLayout->addRow(tr("Encryption:"), m_networkTransferEncryption);

    m_networkTransferCompression = new QCheckBox(tr("Enable compression"));
    networkLayout->addRow(tr("Compression:"), m_networkTransferCompression);

    m_networkTransferResume = new QCheckBox(tr("Enable resume capability"));
    networkLayout->addRow(tr("Resume:"), m_networkTransferResume);

    m_networkTransferDiscoveryPort = new QSpinBox();
    m_networkTransferDiscoveryPort->setRange(1024, 65535);
    networkLayout->addRow(tr("Discovery Port:"), m_networkTransferDiscoveryPort);

    m_networkTransferControlPort = new QSpinBox();
    m_networkTransferControlPort->setRange(1024, 65535);
    networkLayout->addRow(tr("Control Port:"), m_networkTransferControlPort);

    m_networkTransferDataPort = new QSpinBox();
    m_networkTransferDataPort->setRange(1024, 65535);
    networkLayout->addRow(tr("Data Port:"), m_networkTransferDataPort);

    m_networkTransferChunkSize = new QSpinBox();
    m_networkTransferChunkSize->setRange(16, 1024 * 4);
    m_networkTransferChunkSize->setSuffix(tr(" KB"));
    networkLayout->addRow(tr("Chunk Size:"), m_networkTransferChunkSize);

    m_networkTransferMaxBandwidth = new QSpinBox();
    m_networkTransferMaxBandwidth->setRange(0, 1024 * 1024);
    m_networkTransferMaxBandwidth->setSuffix(tr(" KB/s (0 = unlimited)"));
    networkLayout->addRow(tr("Max Bandwidth:"), m_networkTransferMaxBandwidth);

    m_networkTransferRelayServer = new QLineEdit();
    m_networkTransferRelayServer->setPlaceholderText(tr("https://relay.example.com"));
    networkLayout->addRow(tr("Relay Server:"), m_networkTransferRelayServer);

    networkGroup->setLayout(networkLayout);
    layout->addWidget(networkGroup);

    layout->addStretch();
    m_tabWidget->addTab(widget, tr("Advanced"));

    connect(m_networkTransferEnabled, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferAutoDiscovery, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferEncryption, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferCompression, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferResume, &QCheckBox::stateChanged, this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferDiscoveryPort, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferControlPort, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferDataPort, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferChunkSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferMaxBandwidth, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsDialog::onSettingChanged);
    connect(m_networkTransferRelayServer, &QLineEdit::textChanged, this, &SettingsDialog::onSettingChanged);
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

    // Image Flasher
    QString validationMode = config.getImageFlasherValidationMode();
    if (validationMode == "full") {
        m_imageFlasherValidationMode->setCurrentIndex(0);
    } else if (validationMode == "quick") {
        m_imageFlasherValidationMode->setCurrentIndex(1);
    } else {
        m_imageFlasherValidationMode->setCurrentIndex(2); // none
    }
    m_imageFlasherBufferSize->setValue(config.getImageFlasherBufferSize());
    m_imageFlasherUnmountOnCompletion->setChecked(config.getImageFlasherUnmountOnCompletion());
    m_imageFlasherShowSystemDriveWarning->setChecked(config.getImageFlasherShowSystemDriveWarning());
    m_imageFlasherShowLargeDriveWarning->setChecked(config.getImageFlasherShowLargeDriveWarning());
    m_imageFlasherLargeDriveThreshold->setValue(config.getImageFlasherLargeDriveThreshold());
    m_imageFlasherMaxConcurrentWrites->setValue(config.getImageFlasherMaxConcurrentWrites());
    m_imageFlasherEnableNotifications->setChecked(config.getImageFlasherEnableNotifications());

    // Network Transfer
    if (m_networkTransferEnabled) {
        m_networkTransferEnabled->setChecked(config.getNetworkTransferEnabled());
        m_networkTransferAutoDiscovery->setChecked(config.getNetworkTransferAutoDiscoveryEnabled());
        m_networkTransferEncryption->setChecked(config.getNetworkTransferEncryptionEnabled());
        m_networkTransferCompression->setChecked(config.getNetworkTransferCompressionEnabled());
        m_networkTransferResume->setChecked(config.getNetworkTransferResumeEnabled());
        m_networkTransferDiscoveryPort->setValue(config.getNetworkTransferDiscoveryPort());
        m_networkTransferControlPort->setValue(config.getNetworkTransferControlPort());
        m_networkTransferDataPort->setValue(config.getNetworkTransferDataPort());
        m_networkTransferChunkSize->setValue(config.getNetworkTransferChunkSize() / 1024);
        m_networkTransferMaxBandwidth->setValue(config.getNetworkTransferMaxBandwidth());
        m_networkTransferRelayServer->setText(config.getNetworkTransferRelayServer());
    }

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

    // Image Flasher
    QString validationMode;
    switch (m_imageFlasherValidationMode->currentIndex()) {
        case 0: validationMode = "full"; break;
        case 1: validationMode = "quick"; break;
        case 2: validationMode = "none"; break;
        default: validationMode = "full"; break;
    }
    config.setImageFlasherValidationMode(validationMode);
    config.setImageFlasherBufferSize(m_imageFlasherBufferSize->value());
    config.setImageFlasherUnmountOnCompletion(m_imageFlasherUnmountOnCompletion->isChecked());
    config.setImageFlasherShowSystemDriveWarning(m_imageFlasherShowSystemDriveWarning->isChecked());
    config.setImageFlasherShowLargeDriveWarning(m_imageFlasherShowLargeDriveWarning->isChecked());
    config.setImageFlasherLargeDriveThreshold(m_imageFlasherLargeDriveThreshold->value());
    config.setImageFlasherMaxConcurrentWrites(m_imageFlasherMaxConcurrentWrites->value());
    config.setImageFlasherEnableNotifications(m_imageFlasherEnableNotifications->isChecked());

    // Network Transfer
    if (m_networkTransferEnabled) {
        config.setNetworkTransferEnabled(m_networkTransferEnabled->isChecked());
        config.setNetworkTransferAutoDiscoveryEnabled(m_networkTransferAutoDiscovery->isChecked());
        config.setNetworkTransferEncryptionEnabled(m_networkTransferEncryption->isChecked());
        config.setNetworkTransferCompressionEnabled(m_networkTransferCompression->isChecked());
        config.setNetworkTransferResumeEnabled(m_networkTransferResume->isChecked());
        config.setNetworkTransferDiscoveryPort(m_networkTransferDiscoveryPort->value());
        config.setNetworkTransferControlPort(m_networkTransferControlPort->value());
        config.setNetworkTransferDataPort(m_networkTransferDataPort->value());
        config.setNetworkTransferChunkSize(m_networkTransferChunkSize->value() * 1024);
        config.setNetworkTransferMaxBandwidth(m_networkTransferMaxBandwidth->value());
        config.setNetworkTransferRelayServer(m_networkTransferRelayServer->text());
    }

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

    if (m_networkTransferControlPort && m_networkTransferControlPort->value() == m_networkTransferDataPort->value()) {
        QMessageBox::warning(this, tr("Invalid Setting"),
                           tr("Control port and data port must be different."));
        m_tabWidget->setCurrentIndex(m_tabWidget->count() - 1);
        m_networkTransferControlPort->setFocus();
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
