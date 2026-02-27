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

namespace sak {
    struct FlashResult;
    struct FlashProgress;
    enum class FlashState;
    class LogToggleSwitch;
}

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
    sak::LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void flashCompleted(int driveCount, qint64 totalBytes);
    void flashFailed(const QString& error);
    void flashCancelled();
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Step 1: Image Selection
    void onSelectImageClicked();
    void onDownloadWindowsClicked();
    void onDownloadLinuxClicked();
    void onImageSelected(const QString& imagePath);
    void onWindowsISODownloaded(const QString& isoPath);

    // Step 2: Drive Selection
    void onDriveListUpdated();
    void onDriveSelectionChanged();

    // Step 3: Flash
    void onFlashClicked();
    void onFlashProgress(const sak::FlashProgress& progress);
    void onFlashStateChanged(sak::FlashState newState, const QString& message);
    void onFlashCompleted(const sak::FlashResult& result);
    void onFlashError(const QString& error);
    void onCancelClicked();

private:
    void setupUI();
    void createImageSelectionPage();
    void createDriveSelectionPage();
    void createFlashProgressPage();
    void createCompletionPage();
    
    void updateNavigationButtons();
    void validateImageFile(const QString& filePath);
    void showConfirmationDialog();
    
    bool isSystemDrive(const QString& devicePath) const;
    bool isWindowsInstallISO(const QString& isoPath) const;
    void createWindowsUSB();
    QString formatFileSize(qint64 bytes) const;
    QString formatSpeed(double mbps) const;

    // UI Components
    QStackedWidget* m_stackedWidget;
    
    // Step 1: Image Selection
    QWidget* m_imageSelectionPage;
    QPushButton* m_selectImageButton;
    QPushButton* m_downloadWindowsButton;
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
    sak::LogToggleSwitch* m_logToggle{nullptr};
};
