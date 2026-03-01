// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QStackedWidget>
#include <QListWidget>
#include <QCheckBox>
#include <memory>

// Forward declarations
class DriveScanner;
class FlashCoordinator;
class WindowsISODownloader;
class LinuxISODownloader;
class QVBoxLayout;
class QGroupBox;
class QThread;
class WindowsUSBCreator;

namespace sak {
    struct FlashResult;
    struct FlashProgress;
    enum class FlashState;
    class LogToggleSwitch;

/**
 * @brief Image Flasher Panel
 * 
 * Provides comprehensive UI for flashing disk images to USB drives and SD cards.
 * Based on balena.io Etcher functionality with Windows-specific optimizations.
 * 
 * Features:
 * - Select image files (ISO, IMG, WIC, ZIP, GZ, BZ2, XZ, DMG, DSK)
 * - Automatic image decompression
 * - Multi-drive selection with safety checks
 * - Parallel writing to multiple drives
 * - SHA-512 verification
 * - Download Windows 11 ISOs directly
 * - Download Linux distribution ISOs
 * - Real-time progress tracking
 * - System drive protection
 * 
 * Workflow:
 * 1. Select Image - Choose file or download Windows 11
 * 2. Select Target(s) - Choose one or more drives
 * 3. Flash! - Write with progress and verification
 * 
 * Thread-Safety: UI updates occur on main thread.
 * Flash operations use separate threads with signal/slot communication.
 */
class ImageFlasherPanel : public QWidget {
    Q_OBJECT

public:
    explicit ImageFlasherPanel(QWidget* parent = nullptr);
    ~ImageFlasherPanel() override;

    // Disable copy and move
    ImageFlasherPanel(const ImageFlasherPanel&) = delete;
    ImageFlasherPanel& operator=(const ImageFlasherPanel&) = delete;
    ImageFlasherPanel(ImageFlasherPanel&&) = delete;
    ImageFlasherPanel& operator=(ImageFlasherPanel&&) = delete;

    /**
     * @brief Load an image file directly (for drag-drop or command line)
     * @param filePath Path to image file
     * @return true if image loaded successfully
     */
    bool loadImageFile(const QString& filePath);

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void flashCompleted(int driveCount, qint64 totalBytes);
    void flashFailed(const QString& error);
    void flashCancelled();
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Step 1: Image Selection
    /** @brief Open a file dialog to choose a local image file */
    void onSelectImageClicked();
    /** @brief Launch the Windows 11 ISO download dialog */
    void onDownloadWindowsClicked();
    /** @brief Open the Microsoft Windows download page in a browser */
    void onOpenMicrosoftWindowsDownloadClicked();
    /** @brief Launch the Linux distribution ISO download dialog */
    void onDownloadLinuxClicked();
    /** @brief Handle a successfully chosen image path */
    void onImageSelected(const QString& imagePath);
    /** @brief Handle a completed Windows ISO download */
    void onWindowsISODownloaded(const QString& isoPath);

    // Step 2: Drive Selection
    /** @brief Refresh the drive list after a scan completes */
    void onDriveListUpdated();
    /** @brief Update UI state when drive selection changes */
    void onDriveSelectionChanged();

    // Step 3: Flash
    /** @brief Start writing the selected image to the selected drives */
    void onFlashClicked();
    /** @brief Update progress indicators from the flash coordinator */
    void onFlashProgress(const FlashProgress& progress);
    /** @brief Update the UI when the flash state machine transitions */
    void onFlashStateChanged(FlashState newState, const QString& message);
    /** @brief Handle final flash result (success or failure details) */
    void onFlashCompleted(const FlashResult& result);
    /** @brief Display a flash error and transition to error state */
    void onFlashError(const QString& error);
    /** @brief Request cancellation of the in-progress flash operation */
    void onCancelClicked();

private:
    /** @brief Build the panel layout and stacked-widget pages */
    void setupUi();
    /** @brief Create and wire up navigation buttons (Back/Next/Flash/Settings) */
    void setupNavigationButtons(QVBoxLayout* mainLayout);
    /** @brief Build the image-selection wizard page */
    void createImageSelectionPage();
    /** @brief Create download/select buttons within the image selection group */
    void createDownloadButtons(QVBoxLayout* groupLayout, QGroupBox* groupBox);
    /** @brief Build the drive-selection wizard page */
    void createDriveSelectionPage();
    /** @brief Build the flash-progress wizard page */
    void createFlashProgressPage();
    /** @brief Build the completion/summary wizard page */
    void createCompletionPage();

    /** @brief Show or hide back/next/flash buttons per current page */
    void updateNavigationButtons();
    /** @brief Validate the selected image file format and readability */
    void validateImageFile(const QString& filePath);
    /** @brief Ask the user to confirm before destructive write */
    void showConfirmationDialog();
    /** @brief Build a formatted list of selected drives and detect system drives */
    QStringList buildDriveDetailsList(bool& hasSystemDrive) const;
    /** @brief Build the destructive-operation confirmation message */
    QString buildFlashConfirmationMessage(const QStringList& driveDetails, bool isWindowsISO) const;

    /** @brief Return true if the device path belongs to the OS drive */
    bool isSystemDrive(const QString& devicePath) const;
    /** @brief Return true if the ISO is a Windows installer (needs special handling) */
    bool isWindowsInstallISO(const QString& isoPath) const;
    /** @brief Create a bootable Windows USB instead of raw flash */
    void createWindowsUSB();
    /** @brief Wire signal/slot connections for the Windows USB creator worker */
    void connectWindowsUSBCreatorSignals(WindowsUSBCreator* creator, QThread* thread);
    /** @brief Parse disk number from PhysicalDrive device path; returns empty on failure */
    QString parseDiskNumberFromDevicePath(const QString& devicePath);
    /** @brief Format a byte count as a human-readable string */
    QString formatFileSize(qint64 bytes) const;
    /** @brief Format a transfer speed in MB/s */
    QString formatSpeed(double mbps) const;

    // UI Components
    QStackedWidget* m_stackedWidget;
    
    // Step 1: Image Selection
    QWidget* m_imageSelectionPage;
    QPushButton* m_selectImageButton;
    QPushButton* m_downloadWindowsButton;
    QPushButton* m_microsoftWindowsDownloadButton;
    QPushButton* m_downloadLinuxButton;
    QLabel* m_imagePathLabel;
    QLabel* m_imageSizeLabel;
    QLabel* m_imageFormatLabel;
    
    // Step 2: Drive Selection
    QWidget* m_driveSelectionPage;
    QListWidget* m_driveListWidget;
    QLabel* m_driveCountLabel;
    QCheckBox* m_showAllDrivesCheckBox;
    
    // Step 3: Flash Progress
    QWidget* m_flashProgressPage;
    QLabel* m_flashStateLabel;
    QLabel* m_flashDetailsLabel;
    QLabel* m_flashSpeedLabel;
    QPushButton* m_cancelButton;
    
    // Step 4: Completion
    QWidget* m_completionPage;
    QLabel* m_completionMessageLabel;
    QLabel* m_completionDetailsLabel;
    QPushButton* m_flashAnotherButton;
    
    // Navigation
    QPushButton* m_backButton;
    QPushButton* m_nextButton;
    QPushButton* m_flashButton;
    
    // Core components
    std::unique_ptr<DriveScanner> m_driveScanner;
    std::unique_ptr<FlashCoordinator> m_flashCoordinator;
    std::unique_ptr<WindowsISODownloader> m_isoDownloader;
    std::unique_ptr<LinuxISODownloader> m_linuxIsoDownloader;
    
    // State
    QString m_selectedImagePath;
    qint64 m_imageSize;
    QStringList m_selectedDrives;
    bool m_isFlashing;
    int m_currentPage;
    LogToggleSwitch* m_logToggle{nullptr};
};

} // namespace sak
