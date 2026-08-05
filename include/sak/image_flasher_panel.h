// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

#include <memory>

// Forward declarations
class DriveScanner;
class FlashCoordinator;
class WindowsISODownloader;
class LinuxISODownloader;
class QVBoxLayout;
class QThread;
class QSystemTrayIcon;
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
    /** @brief Handle a successfully chosen image path; returns false when the image fails
     *         validation (missing/empty/directory/declined-unknown-format) so no selection is made
     *         and navigation stays disabled (fail closed). */
    bool onImageSelected(const QString& imagePath);
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
    /** @brief Create download cards and select-file button */
    void createDownloadCards(QVBoxLayout* pageLayout);
    void addMicrosoftDownloadCard(QHBoxLayout* cardRow);
    void addUupDownloadCard(QHBoxLayout* cardRow);
    void addLinuxDownloadCard(QHBoxLayout* cardRow);
    void createSelectImageButton(QVBoxLayout* pageLayout);
    /// @brief Configuration for an ISO download card
    struct IsoCardConfig {
        QString icon_path;
        QString title;
        QString description;
        QString button_text;
        QString access_name;
        QString tip;
    };

    /** @brief Build a single ISO download card */
    QFrame* buildIsoDownloadCard(QWidget* parent,
                                 const IsoCardConfig& config,
                                 QPushButton*& buttonOut);
    /** @brief Build the drive-selection wizard page */
    void createDriveSelectionPage();
    /** @brief Build the flash-progress wizard page */
    void createFlashProgressPage();
    /** @brief Build the completion/summary wizard page */
    void createCompletionPage();

    /** @brief Show or hide back/next/flash buttons per current page */
    void updateNavigationButtons();
    /** @brief Validate the selected image file: returns true only when it exists, is a regular
     *         readable non-empty file and either has a known format or the user confirmed an
     *         unknown format. Fail closed: an invalid image returns false. */
    [[nodiscard]] bool validateImageFile(const QString& filePath);
    /** @brief Re-validate the selected image just before flashing: still exists as a regular file
     *         with the same size and modification time captured at selection (guards a source-file
     *         swap between selection and flash). */
    [[nodiscard]] bool selectedImageUnchanged() const;
    /** @brief Re-verify every selected drive still maps to the same physical device (identity
     *         signature captured at selection); guards a removable-drive swap / disk-number reuse
     *         between selection and the confirmed write. */
    [[nodiscard]] bool selectedDrivesIdentityUnchanged() const;
    /** @brief Fail-closed gate run at confirmation: image and every target drive must still match
     *         what was selected; warns and returns false otherwise. */
    [[nodiscard]] bool confirmSelectionStillValid();
    /** @brief Ask the user to confirm before destructive write */
    void showConfirmationDialog();
    /** @brief Take the UI into the flashing state and hand off to the right writer. Called only
     *         after every refusal gate has passed and the user has confirmed. */
    void beginConfirmedFlash(bool isWindowsISO);
    /** @brief Build a formatted list of selected drives and detect system drives */
    QStringList buildDriveDetailsList(bool& hasSystemDrive) const;
    /** @brief Selected drives at or above the configured large-drive threshold, formatted for
     *         display. A drive whose capacity cannot be read is included: an unreadable capacity
     *         is exactly what the warning guards against, so it is not silently passed. */
    [[nodiscard]] QStringList selectedDrivesOverThreshold(qint64 thresholdBytes) const;
    /** @brief Extra confirmation when a target exceeds the large-drive threshold and the warning
     *         is enabled. Returns true to continue; false when the user declined (or the setting
     *         is off but nothing is oversized). */
    [[nodiscard]] bool confirmLargeDrives();
    /** @brief Build the completion page's detail text, including the per-drive eject outcome when
     *         eject-on-completion was applied. */
    [[nodiscard]] static QString buildCompletionDetails(const FlashResult& result);
    /** @brief Show a desktop notification for a finished flash, when the setting is enabled and
     *         the system tray can actually display one. Logs and does nothing otherwise -- it never
     *         reports a notification it did not show. */
    void notifyFlashFinished(const FlashResult& result);
    /** @brief Build the destructive-operation confirmation message */
    QString buildFlashConfirmationMessage(const QStringList& driveDetails, bool isWindowsISO) const;

    /** @brief Return true if the device path belongs to the OS drive */
    bool isSystemDrive(const QString& devicePath) const;

    /// @brief Push the persisted Image Flasher settings into the flash coordinator.
    ///        Called immediately before each raw-image flash starts.
    void applyFlasherSettings();
    /** @brief Return true if the ISO is a Windows installer (needs special handling) */
    bool isWindowsInstallISO(const QString& isoPath) const;
    /** @brief Create a bootable Windows USB instead of raw flash */
    void createWindowsUSB();
    /** @brief Wire signal/slot connections for the Windows USB creator worker */
    void connectWindowsUSBCreatorSignals(WindowsUSBCreator* creator, QThread* thread);
    /** @brief Parse disk number from PhysicalDrive device path; returns empty on failure */
    QString parseDiskNumberFromDevicePath(const QString& devicePath);
    /** @brief Find the display text for a drive by matching its device path in the list widget */
    QString findDriveDisplayText(const QString& devicePath) const;
    /** @brief Format a byte count as a human-readable string */
    static QString formatFileSize(qint64 bytes);
    /** @brief Format a transfer speed in MB/s */
    QString formatSpeed(double mbps) const;
    /** @brief Run ISO analysis and populate the info group box */
    void populateIsoInfo(const QString& imagePath);

    // UI Components
    QStackedWidget* m_stackedWidget;

    // Step 1: Image Selection
    QWidget* m_imageSelectionPage;
    QPushButton* m_selectImageButton;
    QPushButton* m_downloadWindowsButton;
    QPushButton* m_microsoftWindowsDownloadButton;
    QPushButton* m_downloadLinuxButton;
    QLabel* m_imagePathLabel;
    QString m_detectedFormat;

    // ISO analysis info display
    QGroupBox* m_isoInfoGroup{nullptr};
    QGridLayout* m_isoInfoGrid{nullptr};
    QLabel* m_infoOsLabel{nullptr};
    QLabel* m_infoArchLabel{nullptr};
    QLabel* m_infoSizeLabel{nullptr};
    QLabel* m_infoFormatLabel{nullptr};
    QLabel* m_infoBootLabel{nullptr};
    QLabel* m_infoFilesysLabel{nullptr};
    QLabel* m_infoVolLabel{nullptr};
    QLabel* m_infoPublisherLabel{nullptr};
    QLabel* m_infoDateLabel{nullptr};
    QLabel* m_infoEditionsLabel{nullptr};

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
    QPushButton* m_settingsButton{nullptr};

    // Core components
    std::unique_ptr<DriveScanner> m_driveScanner;
    std::unique_ptr<FlashCoordinator> m_flashCoordinator;
    std::unique_ptr<WindowsISODownloader> m_isoDownloader;
    std::unique_ptr<LinuxISODownloader> m_linuxIsoDownloader;

    // State
    QString m_selectedImagePath;
    qint64 m_imageSize;
    // Modification time captured when the image was selected, so a source-file swap before flash
    // can be detected (size alone would miss a same-size replacement).
    QDateTime m_imageLastModified;
    QStringList m_selectedDrives;
    // devicePath -> identity signature (name|size|busType|blockSize) captured at selection, so a
    // removable-drive swap or disk-number reuse before the write can be detected and refused.
    QHash<QString, QString> m_selectedDriveSignatures;
    bool m_isFlashing;
    int m_currentPage;
    LogToggleSwitch* m_logToggle{nullptr};

    // In-flight Windows USB creation, tracked so the destructor can cancel and
    // wait for the worker instead of destroying a still-running QThread child.
    QThread* m_windowsUsbThread{nullptr};
    WindowsUSBCreator* m_windowsUsbCreator{nullptr};

    // Tray icon used only to raise the flash-finished desktop notification. Created
    // on first use and kept hidden between notifications, so an install with
    // notifications turned off never puts an icon in the tray at all.
    QSystemTrayIcon* m_notificationTray{nullptr};
};

}  // namespace sak
