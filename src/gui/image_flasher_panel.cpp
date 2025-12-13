// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/image_flasher_panel.h"
#include "sak/drive_scanner.h"
#include "sak/flash_coordinator.h"
#include "sak/windows_iso_downloader.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/image_flasher_settings_dialog.h"
#include "sak/windows_usb_creator.h"
#include "sak/config_manager.h"
#include "sak/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QGroupBox>

ImageFlasherPanel::ImageFlasherPanel(QWidget* parent)
    : QWidget(parent)
    , m_stackedWidget(new QStackedWidget(this))
    , m_driveScanner(std::make_unique<DriveScanner>(this))
    , m_flashCoordinator(std::make_unique<FlashCoordinator>(this))
    , m_isoDownloader(std::make_unique<WindowsISODownloader>(this))
    , m_imageSize(0)
    , m_isFlashing(false)
    , m_currentPage(0)
{
    setupUI();
    
    // Connect drive scanner signals
    connect(m_driveScanner.get(), &DriveScanner::drivesUpdated,
            this, &ImageFlasherPanel::onDriveListUpdated);
    
    // Connect flash coordinator signals
    connect(m_flashCoordinator.get(), &FlashCoordinator::stateChanged,
            this, &ImageFlasherPanel::onFlashStateChanged);
    connect(m_flashCoordinator.get(), &FlashCoordinator::progressUpdated,
            this, &ImageFlasherPanel::onFlashProgress);
    connect(m_flashCoordinator.get(), &FlashCoordinator::flashCompleted,
            this, &ImageFlasherPanel::onFlashCompleted);
    connect(m_flashCoordinator.get(), &FlashCoordinator::flashError,
            this, &ImageFlasherPanel::onFlashError);
    
    // Connect ISO downloader signals
    connect(m_isoDownloader.get(), &WindowsISODownloader::downloadComplete,
            this, &ImageFlasherPanel::onWindowsISODownloaded);
    
    // Start drive scanner
    m_driveScanner->start();
    
    sak::log_info("Image Flasher Panel initialized");
}

ImageFlasherPanel::~ImageFlasherPanel() {
    m_driveScanner->stop();
}

void ImageFlasherPanel::setupUI() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    
    // Title
    auto* titleLabel = new QLabel("Image Flasher", this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);
    
    auto* subtitleLabel = new QLabel("Flash disk images to USB drives and SD cards", this);
    subtitleLabel->setStyleSheet("color: gray;");
    mainLayout->addWidget(subtitleLabel);
    
    mainLayout->addSpacing(20);
    
    // Create pages
    createImageSelectionPage();
    createDriveSelectionPage();
    createFlashProgressPage();
    createCompletionPage();
    
    mainLayout->addWidget(m_stackedWidget);
    
    // Navigation buttons
    auto* buttonLayout = new QHBoxLayout();
    
    m_settingsButton = new QPushButton("Settings", this);
    buttonLayout->addWidget(m_settingsButton);
    connect(m_settingsButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onSettingsClicked);
    
    buttonLayout->addStretch();
    
    m_backButton = new QPushButton("Back", this);
    m_backButton->setEnabled(false);
    buttonLayout->addWidget(m_backButton);
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        int currentIndex = m_stackedWidget->currentIndex();
        if (currentIndex > 0) {
            m_stackedWidget->setCurrentIndex(currentIndex - 1);
            updateNavigationButtons();
        }
    });
    
    m_nextButton = new QPushButton("Next", this);
    m_nextButton->setEnabled(false);
    buttonLayout->addWidget(m_nextButton);
    connect(m_nextButton, &QPushButton::clicked, this, [this]() {
        int currentIndex = m_stackedWidget->currentIndex();
        if (currentIndex < m_stackedWidget->count() - 1) {
            m_stackedWidget->setCurrentIndex(currentIndex + 1);
            updateNavigationButtons();
        }
    });
    
    m_flashButton = new QPushButton("Flash!", this);
    m_flashButton->setEnabled(false);
    m_flashButton->setStyleSheet(
        "QPushButton { background-color: #0078d4; color: white; font-weight: bold; padding: 8px 20px; }"
        "QPushButton:disabled { background-color: #cccccc; color: #666666; }");
    buttonLayout->addWidget(m_flashButton);
    connect(m_flashButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onFlashClicked);
    
    mainLayout->addLayout(buttonLayout);
    
    // Show first page
    m_stackedWidget->setCurrentIndex(0);
    updateNavigationButtons();
}

void ImageFlasherPanel::createImageSelectionPage() {
    m_imageSelectionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_imageSelectionPage);
    
    auto* groupBox = new QGroupBox("Step 1: Select Image", m_imageSelectionPage);
    auto* groupLayout = new QVBoxLayout(groupBox);
    
    // Select image button
    m_selectImageButton = new QPushButton("Select Image File", groupBox);
    m_selectImageButton->setMinimumHeight(60);
    groupLayout->addWidget(m_selectImageButton);
    connect(m_selectImageButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onSelectImageClicked);
    
    groupLayout->addSpacing(10);
    
    // Download Windows button
    m_downloadWindowsButton = new QPushButton("Download Windows 11", groupBox);
    m_downloadWindowsButton->setMinimumHeight(60);
    groupLayout->addWidget(m_downloadWindowsButton);
    connect(m_downloadWindowsButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onDownloadWindowsClicked);
    
    groupLayout->addSpacing(20);
    
    // Image info
    m_imagePathLabel = new QLabel("No image selected", groupBox);
    m_imagePathLabel->setWordWrap(true);
    groupLayout->addWidget(m_imagePathLabel);
    
    m_imageSizeLabel = new QLabel("", groupBox);
    groupLayout->addWidget(m_imageSizeLabel);
    
    m_imageFormatLabel = new QLabel("", groupBox);
    groupLayout->addWidget(m_imageFormatLabel);
    
    groupLayout->addStretch();
    
    layout->addWidget(groupBox);
    m_stackedWidget->addWidget(m_imageSelectionPage);
}

void ImageFlasherPanel::createDriveSelectionPage() {
    m_driveSelectionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_driveSelectionPage);
    
    auto* groupBox = new QGroupBox("Step 2: Select Target Drive(s)", m_driveSelectionPage);
    auto* groupLayout = new QVBoxLayout(groupBox);
    
    m_driveCountLabel = new QLabel("No removable drives detected", groupBox);
    groupLayout->addWidget(m_driveCountLabel);
    
    m_driveListWidget = new QListWidget(groupBox);
    m_driveListWidget->setSelectionMode(QAbstractItemView::MultiSelection);
    groupLayout->addWidget(m_driveListWidget);
    connect(m_driveListWidget, &QListWidget::itemSelectionChanged,
            this, &ImageFlasherPanel::onDriveSelectionChanged);
    
    m_showAllDrivesCheckBox = new QCheckBox("Show all drives (including system drive)", groupBox);
    groupLayout->addWidget(m_showAllDrivesCheckBox);
    connect(m_showAllDrivesCheckBox, &QCheckBox::toggled, [this]() {
        onDriveListUpdated();
    });
    
    layout->addWidget(groupBox);
    m_stackedWidget->addWidget(m_driveSelectionPage);
}

void ImageFlasherPanel::createFlashProgressPage() {
    m_flashProgressPage = new QWidget();
    auto* layout = new QVBoxLayout(m_flashProgressPage);
    
    auto* groupBox = new QGroupBox("Step 3: Flashing...", m_flashProgressPage);
    auto* groupLayout = new QVBoxLayout(groupBox);
    
    m_flashStateLabel = new QLabel("Preparing...", groupBox);
    QFont stateFont = m_flashStateLabel->font();
    stateFont.setBold(true);
    m_flashStateLabel->setFont(stateFont);
    groupLayout->addWidget(m_flashStateLabel);
    
    m_flashProgressBar = new QProgressBar(groupBox);
    m_flashProgressBar->setMinimum(0);
    m_flashProgressBar->setMaximum(100);
    m_flashProgressBar->setValue(0);
    groupLayout->addWidget(m_flashProgressBar);
    
    m_flashDetailsLabel = new QLabel("", groupBox);
    groupLayout->addWidget(m_flashDetailsLabel);
    
    m_flashSpeedLabel = new QLabel("", groupBox);
    groupLayout->addWidget(m_flashSpeedLabel);
    
    groupLayout->addStretch();
    
    m_cancelButton = new QPushButton("Cancel", groupBox);
    groupLayout->addWidget(m_cancelButton);
    connect(m_cancelButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onCancelClicked);
    
    layout->addWidget(groupBox);
    m_stackedWidget->addWidget(m_flashProgressPage);
}

void ImageFlasherPanel::createCompletionPage() {
    m_completionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_completionPage);
    
    auto* groupBox = new QGroupBox("Flash Complete", m_completionPage);
    auto* groupLayout = new QVBoxLayout(groupBox);
    
    m_completionMessageLabel = new QLabel("", groupBox);
    QFont msgFont = m_completionMessageLabel->font();
    msgFont.setPointSize(14);
    msgFont.setBold(true);
    m_completionMessageLabel->setFont(msgFont);
    groupLayout->addWidget(m_completionMessageLabel);
    
    m_completionDetailsLabel = new QLabel("", groupBox);
    m_completionDetailsLabel->setWordWrap(true);
    groupLayout->addWidget(m_completionDetailsLabel);
    
    groupLayout->addStretch();
    
    m_flashAnotherButton = new QPushButton("Flash Another", groupBox);
    groupLayout->addWidget(m_flashAnotherButton);
    connect(m_flashAnotherButton, &QPushButton::clicked, [this]() {
        m_stackedWidget->setCurrentIndex(0);
        m_currentPage = 0;
        updateNavigationButtons();
    });
    
    layout->addWidget(groupBox);
    m_stackedWidget->addWidget(m_completionPage);
}

bool ImageFlasherPanel::loadImageFile(const QString& filePath) {
    if (!QFileInfo::exists(filePath)) {
        return false;
    }
    
    onImageSelected(filePath);
    return true;
}

void ImageFlasherPanel::onSelectImageClicked() {
    QString filter = "Disk Images (*.iso *.img *.wic *.dmg *.dsk *.gz *.bz2 *.xz *.zip);;All Files (*.*)";
    QString filePath = QFileDialog::getOpenFileName(this, "Select Image File", QString(), filter);
    
    if (!filePath.isEmpty()) {
        onImageSelected(filePath);
    }
}

void ImageFlasherPanel::onDownloadWindowsClicked() {
    auto* dialog = new WindowsISODownloadDialog(m_isoDownloader.get(), this);
    
    connect(dialog, &WindowsISODownloadDialog::downloadCompleted,
            this, [this](const QString& filePath) {
        onImageSelected(filePath);
        m_stackedWidget->setCurrentIndex(1); // Move to drive selection page
    });
    
    dialog->exec();
    dialog->deleteLater();
}

void ImageFlasherPanel::onImageSelected(const QString& imagePath) {
    m_selectedImagePath = imagePath;
    
    QFileInfo fileInfo(imagePath);
    m_imageSize = fileInfo.size();
    
    m_imagePathLabel->setText(QString("Selected: %1").arg(fileInfo.fileName()));
    m_imageSizeLabel->setText(QString("Size: %1").arg(formatFileSize(m_imageSize)));
    
    sak::ImageFormat format = FileImageSource::detectFormat(imagePath);
    QString formatStr = "Unknown";
    switch (format) {
        case sak::ImageFormat::ISO: formatStr = "ISO 9660"; break;
        case sak::ImageFormat::IMG: formatStr = "Raw Image"; break;
        case sak::ImageFormat::WIC: formatStr = "Windows Imaging"; break;
        case sak::ImageFormat::GZIP: formatStr = "GZIP Compressed"; break;
        case sak::ImageFormat::BZIP2: formatStr = "BZIP2 Compressed"; break;
        case sak::ImageFormat::XZ: formatStr = "XZ Compressed"; break;
        case sak::ImageFormat::ZIP: formatStr = "ZIP Archive"; break;
        case sak::ImageFormat::DMG: formatStr = "Apple Disk Image"; break;
        case sak::ImageFormat::DSK: formatStr = "Disk Image"; break;
        default: break;
    }
    m_imageFormatLabel->setText(QString("Format: %1").arg(formatStr));
    
    m_nextButton->setEnabled(true);
    updateNavigationButtons();
    
    sak::log_info(QString("Image selected: %1").arg(imagePath).toStdString());
}

void ImageFlasherPanel::onWindowsISODownloaded(const QString& isoPath) {
    onImageSelected(isoPath);
    
    QMessageBox::information(this, "Download Complete",
        QString("Windows 11 ISO downloaded successfully!\n\n%1").arg(isoPath));
}

void ImageFlasherPanel::onDriveListUpdated() {
    m_driveListWidget->clear();
    
    QList<sak::DriveInfo> drives;
    if (m_showAllDrivesCheckBox->isChecked()) {
        drives = m_driveScanner->getDrives();
    } else {
        drives = m_driveScanner->getRemovableDrives();
    }
    
    for (const auto& drive : drives) {
        QString text = QString("%1 - %2 (%3)")
            .arg(drive.name)
            .arg(formatFileSize(drive.size))
            .arg(drive.devicePath);
        
        if (drive.isSystem) {
            text += " [SYSTEM DRIVE]";
        }
        
        auto* item = new QListWidgetItem(text, m_driveListWidget);
        item->setData(Qt::UserRole, drive.devicePath);
        
        if (drive.isSystem) {
            item->setForeground(QBrush(Qt::red));
            item->setToolTip("WARNING: This is your system drive!");
        }
    }
    
    m_driveCountLabel->setText(QString("%1 drive(s) available").arg(drives.size()));
}

void ImageFlasherPanel::onDriveSelectionChanged() {
    m_selectedDrives.clear();
    
    auto selectedItems = m_driveListWidget->selectedItems();
    for (auto* item : selectedItems) {
        m_selectedDrives.append(item->data(Qt::UserRole).toString());
    }
    
    updateNavigationButtons();
}

void ImageFlasherPanel::onFlashClicked() {
    // Show confirmation dialog
    showConfirmationDialog();
}

void ImageFlasherPanel::onFlashProgress(const sak::FlashProgress& progress) {
    m_flashProgressBar->setValue(static_cast<int>(progress.percentage));
    m_flashDetailsLabel->setText(QString("Written: %1 / %2")
        .arg(formatFileSize(progress.bytesWritten))
        .arg(formatFileSize(progress.totalBytes)));
    m_flashSpeedLabel->setText(QString("Speed: %1").arg(formatSpeed(progress.speedMBps)));
}

void ImageFlasherPanel::onFlashStateChanged(sak::FlashState newState, const QString& message) {
    (void)newState;
    m_flashStateLabel->setText(message);
    sak::log_info(QString("Flash state: %1").arg(message).toStdString());
}

void ImageFlasherPanel::onFlashCompleted(const sak::FlashResult& result) {
    if (result.success) {
        m_completionMessageLabel->setText("✓ Flash Completed Successfully!");
        m_completionMessageLabel->setStyleSheet("color: green;");
    } else {
        m_completionMessageLabel->setText("✗ Flash Completed with Errors");
        m_completionMessageLabel->setStyleSheet("color: red;");
    }
    
    QString details = QString("Successful: %1\nFailed: %2")
        .arg(result.successfulDrives.size())
        .arg(result.failedDrives.size());
    m_completionDetailsLabel->setText(details);
    
    m_stackedWidget->setCurrentWidget(m_completionPage);
    m_isFlashing = false;
    
    Q_EMIT flashCompleted(result.totalDrives(), result.bytesWritten);
}

void ImageFlasherPanel::onFlashError(const QString& error) {
    QMessageBox::critical(this, "Flash Error", error);
    m_isFlashing = false;
    Q_EMIT flashFailed(error);
}

void ImageFlasherPanel::onCancelClicked() {
    auto reply = QMessageBox::question(this, "Cancel Flash",
        "Are you sure you want to cancel the flash operation?",
        QMessageBox::Yes | QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        m_flashCoordinator->cancel();
        Q_EMIT flashCancelled();
    }
}

void ImageFlasherPanel::onSettingsClicked() {
    auto& configManager = sak::ConfigManager::instance();
    auto* dialog = new ImageFlasherSettingsDialog(&configManager, this);
    
    if (dialog->exec() == QDialog::Accepted) {
        sak::log_info("Image Flasher settings updated");
    }
    
    dialog->deleteLater();
}

void ImageFlasherPanel::updateNavigationButtons() {
    int currentIndex = m_stackedWidget->currentIndex();
    
    // Back button: enabled on pages after the first
    m_backButton->setEnabled(currentIndex > 0 && !m_isFlashing);
    
    // Next button: enabled based on page state
    switch (currentIndex) {
        case 0: // Image selection page
            m_nextButton->setEnabled(!m_selectedImagePath.isEmpty());
            break;
        case 1: // Drive selection page
            m_nextButton->setEnabled(false); // Use Flash button instead
            break;
        case 2: // Progress page
            m_nextButton->setEnabled(false);
            break;
        case 3: // Completion page
            m_nextButton->setEnabled(false);
            break;
        default:
            m_nextButton->setEnabled(false);
            break;
    }
    
    // Flash button: only visible and enabled on drive selection page when not flashing
    m_flashButton->setVisible(currentIndex == 1 && !m_isFlashing);
    m_flashButton->setEnabled(!m_selectedDrives.isEmpty() && !m_isFlashing);
    
    // Cancel button: only visible during flashing
    m_cancelButton->setVisible(currentIndex == 2 && m_isFlashing);
}

void ImageFlasherPanel::validateImageFile(const QString& filePath) {
    QFileInfo fileInfo(filePath);
    
    // Check if file exists
    if (!fileInfo.exists()) {
        QMessageBox::warning(this, "Invalid Image",
            QString("File does not exist: %1").arg(filePath));
        return;
    }
    
    // Check if file is readable
    if (!fileInfo.isReadable()) {
        QMessageBox::warning(this, "Invalid Image",
            QString("File is not readable: %1").arg(filePath));
        return;
    }
    
    // Check if file is not empty
    if (fileInfo.size() == 0) {
        QMessageBox::warning(this, "Invalid Image",
            "Image file is empty");
        return;
    }
    
    // Detect and validate format
    sak::ImageFormat format = FileImageSource::detectFormat(filePath);
    if (format == sak::ImageFormat::Unknown) {
        auto reply = QMessageBox::question(this, "Unknown Format",
            "Unable to detect image format. Continue anyway?",
            QMessageBox::Yes | QMessageBox::No);
        
        if (reply != QMessageBox::Yes) {
            return;
        }
    }
    
    // File is valid
    sak::log_info(QString("Image file validated: %1").arg(filePath).toStdString());
}

void ImageFlasherPanel::showConfirmationDialog() {
    // Check if this is a Windows ISO
    bool isWindowsISO = isWindowsInstallISO(m_selectedImagePath);
    
    QString methodInfo;
    if (isWindowsISO) {
        methodInfo = "\n\nMethod: Extract ISO to NTFS-formatted drive\n"
                    "(Proper Windows installation USB)";
    } else {
        methodInfo = "\n\nMethod: Raw disk imaging\n"
                    "(Bootable for Linux, other ISOs)";
    }
    
    QString message = QString("Are you sure you want to flash this image?\n\n"
                              "Image: %1\n"
                              "Target Drives: %2%3\n\n"
                              "WARNING: All data on the target drives will be erased!")
        .arg(QFileInfo(m_selectedImagePath).fileName())
        .arg(m_selectedDrives.size())
        .arg(methodInfo);
    
    auto reply = QMessageBox::warning(this, "Confirm Flash", message,
        QMessageBox::Yes | QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        m_isFlashing = true;
        m_flashButton->setEnabled(false); // Disable flash button during operation
        m_stackedWidget->setCurrentWidget(m_flashProgressPage);
        
        if (isWindowsISO) {
            // Use Windows USB Creator for proper NTFS extraction
            createWindowsUSB();
        } else {
            // Use raw disk imaging for other ISOs
            m_flashCoordinator->startFlash(m_selectedImagePath, m_selectedDrives);
        }
    }
}

bool ImageFlasherPanel::isWindowsInstallISO(const QString& isoPath) const {
    // Check if ISO filename suggests it's a Windows ISO
    QString fileName = QFileInfo(isoPath).fileName().toLower();
    
    if (fileName.contains("windows") || 
        fileName.contains("win10") || 
        fileName.contains("win11") ||
        fileName.contains("win_") ||
        fileName.contains("server")) {
        return true;
    }
    
    // Could also check ISO contents for sources/install.wim
    // but that requires mounting, so filename heuristic is faster
    
    return false;
}

void ImageFlasherPanel::createWindowsUSB() {
    if (m_selectedDrives.isEmpty()) {
        QMessageBox::critical(this, "Error", "No target drives selected");
        m_isFlashing = false;
        return;
    }
    
    // For Windows USB creation, we can only handle one drive at a time
    if (m_selectedDrives.size() > 1) {
        QMessageBox::information(this, "Multiple Drives", 
            "Windows USB creation will process drives one at a time.\n"
            "This may take longer than raw imaging.");
    }
    
    // Create Windows USB creator
    auto* creator = new WindowsUSBCreator(this);
    
    connect(creator, &WindowsUSBCreator::statusChanged, this, [this](const QString& status) {
        m_flashStateLabel->setText(status);
    });
    
    connect(creator, &WindowsUSBCreator::progressUpdated, this, [this](int percentage) {
        m_flashProgressBar->setValue(percentage);
    });
    
    connect(creator, &WindowsUSBCreator::completed, this, [this, creator]() {
        sak::FlashResult result;
        result.success = true;
        result.successfulDrives = m_selectedDrives;
        result.bytesWritten = m_imageSize * m_selectedDrives.size();
        
        onFlashCompleted(result);
        creator->deleteLater();
    });
    
    connect(creator, &WindowsUSBCreator::failed, this, [this, creator](const QString& error) {
        onFlashError(error);
        creator->deleteLater();
    });
    
    // Extract drive letter from device path (e.g., "\\.\PhysicalDrive1" -> find volume letter)
    QString devicePath = m_selectedDrives.first();
    QString driveLetter;
    
    // Extract disk number from PhysicalDrive path
    QRegularExpression regex(R"(PhysicalDrive(\d+))");
    QRegularExpressionMatch match = regex.match(devicePath);
    
    if (match.hasMatch()) {
        QString diskNumber = match.captured(1);
        
        // Query for existing drive letter on this disk
        QProcess process;
        QString cmd = QString("(Get-Partition -DiskNumber %1 | Get-Volume | Where-Object {$_.DriveLetter -ne $null} | Select-Object -First 1).DriveLetter").arg(diskNumber);
        process.start("powershell", QStringList() << "-NoProfile" << "-Command" << cmd);
        process.waitForFinished(5000);
        
        driveLetter = QString(process.readAllStandardOutput()).trimmed();
        
        if (driveLetter.isEmpty()) {
            // No existing drive letter - the format operation will assign one
            // Use a default letter for now (will be reassigned during format)
            driveLetter = "E";
            sak::log_info(QString("No drive letter found for disk %1, will assign during format").arg(diskNumber).toStdString());
        } else {
            sak::log_info(QString("Found drive letter %1 for disk %2").arg(driveLetter, diskNumber).toStdString());
        }
    } else {
        // Fallback if path format is unexpected
        driveLetter = "E";
        sak::log_warning(QString("Could not parse disk number from %1, using default E:").arg(devicePath).toStdString());
    }
    
    // Start the creation process
    creator->createBootableUSB(m_selectedImagePath, driveLetter);
}

bool ImageFlasherPanel::isSystemDrive(const QString& devicePath) const {
    return m_driveScanner->isSystemDrive(devicePath);
}

QString ImageFlasherPanel::formatFileSize(qint64 bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024 * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString ImageFlasherPanel::formatSpeed(double mbps) const {
    if (mbps < 1.0) return QString("%1 KB/s").arg(mbps * 1024.0, 0, 'f', 1);
    return QString("%1 MB/s").arg(mbps, 0, 'f', 1);
}



