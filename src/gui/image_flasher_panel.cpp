// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/image_flasher_panel.h"
#include "sak/drive_scanner.h"
#include "sak/flash_coordinator.h"
#include "sak/windows_iso_downloader.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"
#include "sak/linux_iso_download_dialog.h"
#include "sak/windows_usb_creator.h"
#include "sak/logger.h"
#include <QCoreApplication>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QGroupBox>
#include <QRegularExpression>

ImageFlasherPanel::ImageFlasherPanel(QWidget* parent)
    : QWidget(parent)
    , m_stackedWidget(new QStackedWidget(this))
    , m_driveScanner(std::make_unique<DriveScanner>(this))
    , m_flashCoordinator(std::make_unique<FlashCoordinator>(this))
    , m_isoDownloader(std::make_unique<WindowsISODownloader>(this))
    , m_linuxIsoDownloader(std::make_unique<LinuxISODownloader>(this))
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
    subtitleLabel->setStyleSheet("color: #64748b;");
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
    
    buttonLayout->addStretch();
    
    m_backButton = new QPushButton("Back", this);
    m_backButton->setEnabled(false);
    buttonLayout->addWidget(m_backButton);
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        int currentIndex = m_stackedWidget->currentIndex();
        if (m_isFlashing) return; // Never navigate away during flash

        if (currentIndex == 3) {
            // From completion page, go back to image selection (skip progress page)
            m_stackedWidget->setCurrentIndex(0);
        } else if (currentIndex > 0) {
            m_stackedWidget->setCurrentIndex(currentIndex - 1);
        }
        updateNavigationButtons();
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
        "QPushButton { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #3b82f6, stop:1 #2563eb); color: white; font-weight: 600; padding: 8px 20px; border-radius: 12px; }"
        "QPushButton:hover { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #4f8efc, stop:1 #3b82f6); }"
        "QPushButton:pressed { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2563eb, stop:1 #1d4ed8); }"
        "QPushButton:disabled { background-color: #cbd5e1; color: #64748b; }");
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
    
    // Download Linux button
    m_downloadLinuxButton = new QPushButton("Download Linux ISO", groupBox);
    m_downloadLinuxButton->setMinimumHeight(60);
    groupLayout->addWidget(m_downloadLinuxButton);
    connect(m_downloadLinuxButton, &QPushButton::clicked,
            this, &ImageFlasherPanel::onDownloadLinuxClicked);
    
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

        auto reply = QMessageBox::question(this, "ISO Downloaded",
            QString("Windows ISO downloaded successfully!\n\n%1\n\n"
                    "Would you like to flash this image to a USB drive now?")
                .arg(QFileInfo(filePath).fileName()),
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            m_stackedWidget->setCurrentIndex(1);
            updateNavigationButtons();
        }
    });
    
    dialog->exec();
    dialog->deleteLater();
}

void ImageFlasherPanel::onDownloadLinuxClicked() {
    auto* dialog = new LinuxISODownloadDialog(m_linuxIsoDownloader.get(), this);
    
    connect(dialog, &LinuxISODownloadDialog::downloadCompleted,
            this, [this](const QString& filePath) {
        onImageSelected(filePath);

        auto reply = QMessageBox::question(this, "ISO Downloaded",
            QString("Linux ISO downloaded successfully!\n\n%1\n\n"
                    "Would you like to flash this image to a USB drive now?")
                .arg(QFileInfo(filePath).fileName()),
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
            m_stackedWidget->setCurrentIndex(1);
            updateNavigationButtons();
        }
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
    m_isFlashing = false;

    if (result.success) {
        m_completionMessageLabel->setText("✓ Flash Completed Successfully!");
        m_completionMessageLabel->setStyleSheet("color: #16a34a;");
    } else {
        m_completionMessageLabel->setText("✗ Flash Completed with Errors");
        m_completionMessageLabel->setStyleSheet("color: #dc2626;");
    }
    
    QString details = QString("Successful: %1\nFailed: %2")
        .arg(result.successfulDrives.size())
        .arg(result.failedDrives.size());
    m_completionDetailsLabel->setText(details);
    
    m_stackedWidget->setCurrentWidget(m_completionPage);
    updateNavigationButtons();
    
    Q_EMIT flashCompleted(result.totalDrives(), result.bytesWritten);
}

void ImageFlasherPanel::onFlashError(const QString& error) {
    m_isFlashing = false;

    QMessageBox::critical(this, "Flash Error",
        QString("The flash operation failed:\n\n%1\n\n"
                "The target drive(s) may be in an unusable state. "
                "You may need to reformat them before they can be used again.")
            .arg(error));

    // Return to drive selection so the user can see the state
    m_stackedWidget->setCurrentIndex(1);
    updateNavigationButtons();

    Q_EMIT flashFailed(error);
}

void ImageFlasherPanel::onCancelClicked() {
    auto reply = QMessageBox::warning(this, "Cancel Flash",
        "Are you sure you want to cancel the flash operation?\n\n"
        "WARNING: Cancelling mid-write may leave the target drive(s) in an \n"
        "unusable state. You may need to reformat them before they can be used again.",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        m_flashCoordinator->cancel();
        m_isFlashing = false;

        m_flashStateLabel->setText("Flash cancelled by user");
        m_flashProgressBar->setValue(0);
        m_flashDetailsLabel->clear();
        m_flashSpeedLabel->clear();

        // Return to drive selection so the user can retry
        m_stackedWidget->setCurrentIndex(1);
        updateNavigationButtons();

        Q_EMIT flashCancelled();
    }
}

void ImageFlasherPanel::updateNavigationButtons() {
    int currentIndex = m_stackedWidget->currentIndex();
    
    // Back button: enabled on pages 1 (drive selection) and 3 (completion)
    // Disabled during flashing (page 2) and on page 0 (nothing to go back to)
    bool canGoBack = !m_isFlashing && (currentIndex == 1 || currentIndex == 3);
    m_backButton->setEnabled(canGoBack);
    m_backButton->setVisible(currentIndex != 2); // Hide entirely during flash
    
    // Next button: enabled based on page state
    switch (currentIndex) {
        case 0: // Image selection page
            m_nextButton->setEnabled(!m_selectedImagePath.isEmpty());
            m_nextButton->setVisible(true);
            break;
        case 1: // Drive selection page — use Flash button instead
        case 2: // Progress page
        case 3: // Completion page
        default:
            m_nextButton->setEnabled(false);
            m_nextButton->setVisible(currentIndex == 0);
            break;
    }
    
    // Flash button: only visible and enabled on drive selection page when not flashing
    m_flashButton->setVisible(currentIndex == 1 && !m_isFlashing);
    m_flashButton->setEnabled(!m_selectedDrives.isEmpty() && !m_isFlashing);
    
    // Cancel button: visible during flashing
    m_cancelButton->setVisible(currentIndex == 2);
    m_cancelButton->setEnabled(m_isFlashing);
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

    // Build drive list for display
    QStringList driveDetails;
    bool hasSystemDrive = false;
    for (const auto& drivePath : m_selectedDrives) {
        QString label = drivePath;
        // Find the display text from the list widget
        for (int i = 0; i < m_driveListWidget->count(); ++i) {
            auto* item = m_driveListWidget->item(i);
            if (item->data(Qt::UserRole).toString() == drivePath) {
                label = item->text();
                break;
            }
        }
        driveDetails << QString("  • %1").arg(label);
        if (isSystemDrive(drivePath)) {
            hasSystemDrive = true;
        }
    }

    // Block system drive flashing with a hard error
    if (hasSystemDrive) {
        QMessageBox::critical(this, "Operation Blocked",
            "One or more selected drives is your SYSTEM DRIVE.\n\n"
            "Flashing to the system drive would destroy your Windows installation "
            "and render this computer unbootable.\n\n"
            "Please deselect the system drive and try again.");
        return;
    }
    
    QString methodInfo;
    if (isWindowsISO) {
        methodInfo = "\nMethod: Extract ISO to NTFS-formatted drive\n"
                    "(Proper Windows installation USB)";
    } else {
        methodInfo = "\nMethod: Raw disk imaging\n"
                    "(Bootable for Linux, other ISOs)";
    }
    
    QString message = QString(
        "⚠  DESTRUCTIVE OPERATION  ⚠\n\n"
        "Image: %1\n\n"
        "Target Drive(s):\n%2\n"
        "%3\n\n"
        "ALL DATA on the target drive(s) will be PERMANENTLY ERASED.\n"
        "This action CANNOT be undone.\n\n"
        "Are you absolutely sure you want to continue?")
        .arg(QFileInfo(m_selectedImagePath).fileName())
        .arg(driveDetails.join("\n"))
        .arg(methodInfo);
    
    auto reply = QMessageBox::warning(this, "Confirm Flash — Data Loss Warning", message,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    
    if (reply == QMessageBox::Yes) {
        // Disable UI immediately upon confirmation
        m_isFlashing = true;
        m_flashButton->setEnabled(false);
        m_flashButton->setVisible(false);
        updateNavigationButtons(); // Update all navigation state
        
        // Switch to progress page
        m_stackedWidget->setCurrentWidget(m_flashProgressPage);
        
        if (isWindowsISO) {
            // Use Windows USB Creator for proper NTFS extraction
            createWindowsUSB();
        } else {
            // Use raw disk imaging for other ISOs
            if (!m_flashCoordinator->startFlash(m_selectedImagePath, m_selectedDrives)) {
                m_isFlashing = false;
                onFlashError("Failed to start flash operation - flash coordinator returned error");
                return;
            }
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
    
    // Initialize progress display
    m_flashStateLabel->setText("Initializing...");
    m_flashProgressBar->setValue(0);
    
    // Force UI update before starting blocking operation
    QCoreApplication::processEvents();
    
    // Create Windows USB creator
    auto* creator = new WindowsUSBCreator(this);
    
    connect(creator, &WindowsUSBCreator::statusChanged, this, [this](const QString& status) {
        m_flashStateLabel->setText(status);
        QCoreApplication::processEvents(); // Force UI update
    });
    
    connect(creator, &WindowsUSBCreator::progressUpdated, this, [this](int percentage) {
        m_flashProgressBar->setValue(percentage);
        QCoreApplication::processEvents(); // Force UI update
    });
    
    connect(creator, &WindowsUSBCreator::completed, this, [this, creator]() {
        m_isFlashing = false;
        sak::FlashResult result;
        result.success = true;
        result.successfulDrives = m_selectedDrives;
        result.bytesWritten = m_imageSize * m_selectedDrives.size();
        
        onFlashCompleted(result);
        creator->deleteLater();
    });
    
    connect(creator, &WindowsUSBCreator::failed, this, [this, creator](const QString& error) {
        m_isFlashing = false;
        m_flashButton->setEnabled(true);
        m_flashButton->setVisible(true);
        updateNavigationButtons();
        onFlashError(error);
        creator->deleteLater();
    });
    
    // Extract disk number (hardware ID) from device path
    QString devicePath = m_selectedDrives.first();
    QString diskNumber;
    
    // Extract disk number from PhysicalDrive path (e.g., "\\.\PhysicalDrive1" -> "1")
    QRegularExpression regex(R"(PhysicalDrive(\d+))");
    QRegularExpressionMatch match = regex.match(devicePath);
    
    if (match.hasMatch()) {
        diskNumber = match.captured(1);
        sak::log_info(QString("Using disk number %1 (PhysicalDrive%1)").arg(diskNumber).toStdString());
    } else {
        // Could not parse disk number - fail immediately
        m_isFlashing = false;
        m_flashButton->setEnabled(true);
        m_flashButton->setVisible(true);
        updateNavigationButtons();
        onFlashError(QString("Could not identify disk number from %1").arg(devicePath));
        creator->deleteLater();
        return;
    }
    
    // Start the creation process with disk number (hardware ID)
    creator->createBootableUSB(m_selectedImagePath, diskNumber);
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



