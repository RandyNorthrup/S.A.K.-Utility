// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file image_flasher_panel.cpp
/// @brief Implements the USB image flasher panel UI with ISO downloading and flashing

#include "sak/image_flasher_panel.h"

#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/drive_scanner.h"
#include "sak/elevation_banner.h"
#include "sak/elevation_gate.h"
#include "sak/flash_coordinator.h"
// flash_worker.h, for ValidationMode and validationModeFromSetting. flash_coordinator.h
// only forward declares that enum so it does not drag <windows.h> into every GUI header;
// a translation unit that has to name the enumerators includes the real header itself.
#include "sak/flash_worker.h"
#include "sak/flasher_policy.h"
#include "sak/format_utils.h"
#include "sak/image_flasher_settings_dialog.h"
#include "sak/iso_analyzer.h"
#include "sak/layout_constants.h"
#include "sak/linux_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"
#include "sak/windows_usb_creator.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace sak {

namespace {

constexpr int kImageSelectionPageIndex = 0;
constexpr int kDriveSelectionPageIndex = 1;
constexpr int kFlashProgressPageIndex = 2;
constexpr int kCompletionPageIndex = 3;
constexpr int kIsoInfoLabelColumnMinWidth = 100;
constexpr int kCompletionMessageFontSize = 14;
constexpr int kImageFlasherStatusTimeoutMs = kTimerStatusDefaultMs;
// How long the flash-finished toast stays up, and how long the tray icon that
// carries it is kept alive before being hidden again.
constexpr int kNotificationTimeoutMs = 10'000;

// Stable identity signature for a physical drive. Binds the selection to the concrete device so a
// mid-flow swap (same PhysicalDriveN number, different hardware) or disk-number reuse is detected.
QString driveIdentitySignature(const DriveInfo& drive) {
    return QStringLiteral("%1|%2|%3|%4")
        .arg(drive.name,
             QString::number(drive.size),
             drive.busType,
             QString::number(drive.blockSize));
}

QString imageFormatLabel(ImageFormat format) {
    switch (format) {
    case ImageFormat::ISO:
        return QStringLiteral("ISO 9660");
    case ImageFormat::IMG:
        return QStringLiteral("Raw Image");
    case ImageFormat::WIC:
        return QStringLiteral("Windows Imaging");
    case ImageFormat::GZIP:
        return QStringLiteral("GZIP Compressed");
    case ImageFormat::BZIP2:
        return QStringLiteral("BZIP2 Compressed");
    case ImageFormat::XZ:
        return QStringLiteral("XZ Compressed");
    case ImageFormat::ZIP:
        return QStringLiteral("ZIP Archive");
    case ImageFormat::DMG:
        return QStringLiteral("Apple Disk Image");
    case ImageFormat::DSK:
        return QStringLiteral("Disk Image");
    default:
        return QStringLiteral("Unknown");
    }
}

}  // namespace

ImageFlasherPanel::ImageFlasherPanel(QWidget* parent)
    : QWidget(parent)
    , m_stackedWidget(new QStackedWidget(this))
    , m_driveScanner(std::make_unique<DriveScanner>(this))
    , m_flashCoordinator(std::make_unique<FlashCoordinator>(this))
    , m_isoDownloader(std::make_unique<WindowsISODownloader>(this))
    , m_linuxIsoDownloader(std::make_unique<LinuxISODownloader>(this))
    , m_imageSize(0)
    , m_isFlashing(false)
    , m_currentPage(0) {
    setupUi();

    // Connect drive scanner signals
    connect(m_driveScanner.get(),
            &DriveScanner::drivesUpdated,
            this,
            &ImageFlasherPanel::onDriveListUpdated);

    // Connect flash coordinator signals
    connect(m_flashCoordinator.get(),
            &FlashCoordinator::stateChanged,
            this,
            &ImageFlasherPanel::onFlashStateChanged);
    connect(m_flashCoordinator.get(),
            &FlashCoordinator::progressUpdated,
            this,
            &ImageFlasherPanel::onFlashProgress);
    connect(m_flashCoordinator.get(),
            &FlashCoordinator::flashCompleted,
            this,
            &ImageFlasherPanel::onFlashCompleted);
    connect(m_flashCoordinator.get(),
            &FlashCoordinator::flashError,
            this,
            &ImageFlasherPanel::onFlashError);

    // Connect ISO downloader signals
    connect(m_isoDownloader.get(),
            &WindowsISODownloader::downloadComplete,
            this,
            &ImageFlasherPanel::onWindowsISODownloaded);

    if (!QCoreApplication::instance()->property("sakAccessibilityAudit").toBool()) {
        m_driveScanner->start();
    }

    logInfo("Image Flasher Panel initialized");
}

ImageFlasherPanel::~ImageFlasherPanel() {
    m_driveScanner->stop();
    // A Windows USB creation may still be running on its worker thread (a child
    // of this panel). Destroying a running QThread aborts the process, so cancel
    // the creator -- it polls an atomic flag -- and wait for the worker to
    // unwind before the QObject child teardown deletes the thread.
    if ((m_windowsUsbThread != nullptr) && m_windowsUsbThread->isRunning()) {
        if (m_windowsUsbCreator != nullptr) {
            m_windowsUsbCreator->cancel();
        }
        m_windowsUsbThread->quit();
        // SAK-ALLOW-BLOCKING: the creator polls the cancel atomic set above, and destroying a
        // running QThread aborts the process. Deliberately NOT bounded: the fallback to a
        // bounded wait is terminate(), and killing a thread mid raw-disk-write is worse than
        // a slow exit.
        m_windowsUsbThread->wait();
    }
}

void ImageFlasherPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium);
    main_layout->setSpacing(sak::ui::kSpacingMedium);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);

    // Panel header -- consistent title + muted subtitle
    auto* title_row = new QHBoxLayout();
    auto* header_widget = new QWidget(this);
    auto* header_layout = new QVBoxLayout(header_widget);
    header_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    sak::createPanelHeader(header_widget,
                           QStringLiteral(":/icons/icons/panel_image_flasher.svg"),
                           tr("Image Flasher"),
                           tr("Flash disk images to USB drives and SD cards"),
                           header_layout);
    title_row->addWidget(header_widget);
    title_row->addStretch();
    main_layout->addLayout(title_row);

    // Elevation info banner (hidden when already admin)
    if (auto* banner = sak::createElevationBanner(content_widget)) {
        main_layout->addWidget(banner);
    }

    main_layout->addSpacing(sak::ui::kSpacingDefault);

    // Create pages
    createImageSelectionPage();
    createDriveSelectionPage();
    createFlashProgressPage();
    createCompletionPage();

    main_layout->addWidget(m_stackedWidget);

    // Navigation buttons
    setupNavigationButtons(main_layout);

    // Show first page
    m_stackedWidget->setCurrentIndex(kImageSelectionPageIndex);
    updateNavigationButtons();
}

void ImageFlasherPanel::setupNavigationButtons(QVBoxLayout* main_layout) {
    Q_ASSERT(main_layout);
    Q_ASSERT(m_stackedWidget);
    auto* button_layout = new QHBoxLayout();

    m_settingsButton = new QPushButton("Settings", this);
    m_settingsButton->setAccessibleName(QStringLiteral("Flasher Settings"));
    m_settingsButton->setToolTip(QStringLiteral("Configure image flasher settings"));
    m_settingsButton->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(m_settingsButton, &QPushButton::clicked, this, [this]() {
        ImageFlasherSettingsDialog dialog(this);
        dialog.exec();
    });
    button_layout->addWidget(m_settingsButton);

    button_layout->addStretch();

    m_backButton = new QPushButton("Back", this);
    m_backButton->setEnabled(false);
    m_backButton->setAccessibleName(QStringLiteral("Previous Step"));
    m_backButton->setToolTip(QStringLiteral("Go back to the previous step"));
    m_backButton->setStyleSheet(ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_backButton);
    connect(m_backButton, &QPushButton::clicked, this, [this]() {
        const int current_index = m_stackedWidget->currentIndex();
        if (m_isFlashing) {
            return;  // Never navigate away during flash
        }

        if (current_index == kCompletionPageIndex) {
            // From completion page, go back to image selection (skip progress page)
            m_stackedWidget->setCurrentIndex(kImageSelectionPageIndex);
        } else if (current_index > 0) {
            m_stackedWidget->setCurrentIndex(current_index - 1);
        }
        updateNavigationButtons();
    });

    m_nextButton = new QPushButton("Next", this);
    m_nextButton->setEnabled(false);
    m_nextButton->setAccessibleName(QStringLiteral("Next Step"));
    m_nextButton->setToolTip(QStringLiteral("Proceed to the next step"));
    m_nextButton->setStyleSheet(ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_nextButton);
    connect(m_nextButton, &QPushButton::clicked, this, [this]() {
        const int current_index = m_stackedWidget->currentIndex();
        if (current_index < m_stackedWidget->count() - 1) {
            m_stackedWidget->setCurrentIndex(current_index + 1);
            updateNavigationButtons();
        }
    });

    m_flashButton = new QPushButton("Flash!", this);
    m_flashButton->setEnabled(false);
    m_flashButton->setAccessibleName(QStringLiteral("Flash Drive"));
    m_flashButton->setToolTip(QStringLiteral("Write the selected image to the target drive"));
    m_flashButton->setStyleSheet(ui::kPrimaryButtonStyle);
    button_layout->addWidget(m_flashButton);
    connect(m_flashButton, &QPushButton::clicked, this, &ImageFlasherPanel::onFlashClicked);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    button_layout->insertWidget(1, m_logToggle);

    main_layout->addLayout(button_layout);
}

void ImageFlasherPanel::createImageSelectionPage() {
    Q_ASSERT(m_stackedWidget);
    m_imageSelectionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_imageSelectionPage);
    layout->setSpacing(sak::ui::kSpacingLarge);

    auto* step_label = new QLabel("Step 1: Select Image", m_imageSelectionPage);
    step_label->setStyleSheet(sak::ui::fontSizeWeightColorStyle(
        sak::ui::kFontSizeSection, sak::ui::kFontWeightBold, sak::ui::kColorTextHeading));
    layout->addWidget(step_label);

    createDownloadCards(layout);

    // Selected file indicator (later filled with the chosen image's file name).
    m_imagePathLabel = sak::plainTextLabel("No image selected", m_imageSelectionPage);
    m_imagePathLabel->setWordWrap(true);
    layout->addWidget(m_imagePathLabel);

    // ISO detailed info group (hidden until an image is analyzed)
    m_isoInfoGroup = new QGroupBox("Image Details", m_imageSelectionPage);
    m_isoInfoGroup->setStyleSheet(
        sak::ui::groupBoxPanelStyle(sak::ui::kColorTextHeading, sak::ui::kColorBorderDefault));
    m_isoInfoGrid = new QGridLayout(m_isoInfoGroup);
    m_isoInfoGrid->setSpacing(sak::ui::kSpacingSmall);
    m_isoInfoGrid->setColumnMinimumWidth(0, kIsoInfoLabelColumnMinWidth);

    auto add_info_row = [this](int row, const QString& label_text, QLabel*& value_out) {
        auto* key = new QLabel(label_text, m_isoInfoGroup);
        key->setStyleSheet(sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightSemibold,
                                                            sak::ui::kColorTextSecondary));
        // Every value here is parsed out of the selected image (volume label, publisher, OS name,
        // edition list), so it is author-controlled and is shown verbatim, never as markup.
        value_out = sak::plainTextLabel("-", m_isoInfoGroup);
        value_out->setWordWrap(true);
        value_out->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_isoInfoGrid->addWidget(key, row, 0, Qt::AlignTop);
        m_isoInfoGrid->addWidget(value_out, row, 1);
    };

    int info_row = 0;
    add_info_row(info_row++, "OS:", m_infoOsLabel);
    add_info_row(info_row++, "Architecture:", m_infoArchLabel);
    add_info_row(info_row++, "Size:", m_infoSizeLabel);
    add_info_row(info_row++, "Format:", m_infoFormatLabel);
    add_info_row(info_row++, "Boot Type:", m_infoBootLabel);
    add_info_row(info_row++, "File System:", m_infoFilesysLabel);
    add_info_row(info_row++, "Volume Label:", m_infoVolLabel);
    add_info_row(info_row++, "Publisher:", m_infoPublisherLabel);
    add_info_row(info_row++, "Created:", m_infoDateLabel);
    add_info_row(info_row++, "Editions:", m_infoEditionsLabel);

    m_isoInfoGroup->hide();
    layout->addWidget(m_isoInfoGroup);

    layout->addStretch();

    Q_ASSERT(m_imagePathLabel);
    m_stackedWidget->addWidget(m_imageSelectionPage);
}

// ============================================================================
// Download Card Builder
// ============================================================================

namespace {

constexpr int kCardLogoSize = 48;
constexpr int kSelectImageButtonWidth = 260;

}  // anonymous namespace

QFrame* ImageFlasherPanel::buildIsoDownloadCard(QWidget* parent,
                                                const IsoCardConfig& config,
                                                QPushButton*& button_out) {
    auto* card = new QFrame(parent);
    card->setProperty("sakCard", true);
    card->setStyleSheet(sak::ui::cardFrameStyle(sak::ui::kCssRadiusXLargePx));
    card->setCursor(Qt::PointingHandCursor);
    auto* layout = new QVBoxLayout(card);
    layout->setSpacing(sak::ui::kSpacingMedium);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* logo = new QLabel(card);
    logo->setPixmap(QIcon(config.icon_path).pixmap(kCardLogoSize, kCardLogoSize));
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(sak::ui::kTransparentWidgetStyle);
    layout->addWidget(logo, 0, Qt::AlignHCenter);

    auto* title_label = new QLabel(config.title, card);
    title_label->setAlignment(Qt::AlignCenter);
    title_label->setStyleSheet(sak::ui::cardTitleTextStyle());
    layout->addWidget(title_label);

    auto* desc_label = new QLabel(config.description, card);
    desc_label->setWordWrap(true);
    desc_label->setAlignment(Qt::AlignCenter);
    desc_label->setStyleSheet(sak::ui::cardDescriptionTextStyle());
    layout->addWidget(desc_label);

    layout->addStretch();

    button_out = new QPushButton(config.button_text, card);
    button_out->setMinimumHeight(sak::kButtonHeightTall);
    button_out->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_out->setAccessibleName(config.access_name);
    button_out->setToolTip(config.tip);
    layout->addWidget(button_out);

    return card;
}

void ImageFlasherPanel::addMicrosoftDownloadCard(QHBoxLayout* card_row) {
    auto* ms_card =
        buildIsoDownloadCard(m_imageSelectionPage,
                             {.icon_path = QStringLiteral(":/icons/icons/microsoft_logo.svg"),
                              .title = tr("Windows from Microsoft"),
                              .description = tr("Download an official Windows ISO directly"
                                                " from the Microsoft website."),
                              .button_text = tr("Open Microsoft Download"),
                              .access_name = QStringLiteral("Microsoft ISO Download"),
                              .tip = QStringLiteral("Open the official Microsoft Windows ISO"
                                                    " download page")},
                             m_microsoftWindowsDownloadButton);

    // Add a tip label to the MS card
    auto* ms_tip = new QLabel(tr("Tip: Downloading directly from Microsoft is often faster "
                                 "than building an ISO from UUP files."),
                              ms_card);
    ms_tip->setWordWrap(true);
    ms_tip->setAlignment(Qt::AlignCenter);
    ms_tip->setStyleSheet(sak::ui::cardDescriptionTextStyle());
    auto* ms_layout = ms_card->layout();
    ms_layout->removeWidget(m_microsoftWindowsDownloadButton);
    ms_layout->addWidget(ms_tip);
    static_cast<QVBoxLayout*>(ms_layout)->addStretch();
    ms_layout->addWidget(m_microsoftWindowsDownloadButton);

    connect(m_microsoftWindowsDownloadButton,
            &QPushButton::clicked,
            this,
            &ImageFlasherPanel::onOpenMicrosoftWindowsDownloadClicked);
    card_row->addWidget(ms_card);
}

void ImageFlasherPanel::addUupDownloadCard(QHBoxLayout* card_row) {
    card_row->addWidget(
        buildIsoDownloadCard(m_imageSelectionPage,
                             {.icon_path = QStringLiteral(":/icons/icons/uup_logo.svg"),
                              .title = tr("Windows via UUP"),
                              .description = tr("Build a custom Windows ISO from UUP update"
                                                " packages with edition selection."),
                              .button_text = tr("Download and Build ISO"),
                              .access_name = QStringLiteral("Download Windows ISO via UUP"),
                              .tip = QStringLiteral("Download a Windows 11 ISO using UUP dump")},
                             m_downloadWindowsButton));
    connect(m_downloadWindowsButton,
            &QPushButton::clicked,
            this,
            &ImageFlasherPanel::onDownloadWindowsClicked);
}

void ImageFlasherPanel::addLinuxDownloadCard(QHBoxLayout* card_row) {
    card_row->addWidget(
        buildIsoDownloadCard(m_imageSelectionPage,
                             {.icon_path = QStringLiteral(":/icons/icons/tux_logo.svg"),
                              .title = tr("Linux ISO"),
                              .description = tr("Download a Linux distribution ISO"
                                                " -- Ubuntu, Fedora, Debian, and more."),
                              .button_text = tr("Download Linux ISO"),
                              .access_name = QStringLiteral("Download Linux ISO"),
                              .tip = QStringLiteral("Download a Linux distribution ISO image")},
                             m_downloadLinuxButton));
    connect(m_downloadLinuxButton,
            &QPushButton::clicked,
            this,
            &ImageFlasherPanel::onDownloadLinuxClicked);
}

void ImageFlasherPanel::createDownloadCards(QVBoxLayout* page_layout) {
    Q_ASSERT(page_layout);
    auto* card_row = new QHBoxLayout();
    card_row->setSpacing(sak::ui::kSpacingLarge);

    addMicrosoftDownloadCard(card_row);
    addUupDownloadCard(card_row);
    addLinuxDownloadCard(card_row);

    page_layout->addLayout(card_row);

    createSelectImageButton(page_layout);
}

void ImageFlasherPanel::createSelectImageButton(QVBoxLayout* page_layout) {
    Q_ASSERT(page_layout);
    auto* select_row = new QHBoxLayout();
    select_row->addStretch();

    m_selectImageButton = new QPushButton(tr("Select Image File"), m_imageSelectionPage);
    m_selectImageButton->setMinimumHeight(sak::kButtonHeightTall);
    m_selectImageButton->setMinimumWidth(kSelectImageButtonWidth);
    m_selectImageButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_selectImageButton->setAccessibleName(QStringLiteral("Browse ISO File"));
    m_selectImageButton->setToolTip(QStringLiteral("Browse for a local disk image file to flash"));
    connect(
        m_selectImageButton, &QPushButton::clicked, this, &ImageFlasherPanel::onSelectImageClicked);

    select_row->addWidget(m_selectImageButton);
    select_row->addStretch();

    page_layout->addLayout(select_row);
}

void ImageFlasherPanel::createDriveSelectionPage() {
    m_driveSelectionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_driveSelectionPage);

    auto* group_box = new QGroupBox("Step 2: Select Target Drive(s)", m_driveSelectionPage);
    auto* group_layout = new QVBoxLayout(group_box);

    m_driveCountLabel = new QLabel("No removable drives detected", group_box);
    group_layout->addWidget(m_driveCountLabel);

    m_driveListWidget = new QListWidget(group_box);
    m_driveListWidget->setSelectionMode(QAbstractItemView::MultiSelection);
    m_driveListWidget->setAccessibleName(QStringLiteral("Target Drive List"));
    m_driveListWidget->setToolTip(QStringLiteral("Select one or more target drives to flash"));
    group_layout->addWidget(m_driveListWidget);
    connect(m_driveListWidget,
            &QListWidget::itemSelectionChanged,
            this,
            &ImageFlasherPanel::onDriveSelectionChanged);

    m_showAllDrivesCheckBox = new QCheckBox("Show all drives (including system drive)", group_box);
    m_showAllDrivesCheckBox->setAccessibleName(QStringLiteral("Show All Drives"));
    m_showAllDrivesCheckBox->setToolTip(
        QStringLiteral("Include non-removable and system drives in "
                       "the list"));
    group_layout->addWidget(m_showAllDrivesCheckBox);
    connect(m_showAllDrivesCheckBox, &QCheckBox::toggled, [this]() { onDriveListUpdated(); });

    layout->addWidget(group_box);
    m_stackedWidget->addWidget(m_driveSelectionPage);
}

void ImageFlasherPanel::createFlashProgressPage() {
    m_flashProgressPage = new QWidget();
    auto* layout = new QVBoxLayout(m_flashProgressPage);

    auto* group_box = new QGroupBox("Step 3: Flashing...", m_flashProgressPage);
    auto* group_layout = new QVBoxLayout(group_box);

    m_flashStateLabel = new QLabel("Preparing...", group_box);
    QFont state_font = m_flashStateLabel->font();
    state_font.setBold(true);
    m_flashStateLabel->setFont(state_font);
    group_layout->addWidget(m_flashStateLabel);

    m_flashDetailsLabel = new QLabel("", group_box);
    group_layout->addWidget(m_flashDetailsLabel);

    m_flashSpeedLabel = new QLabel("", group_box);
    group_layout->addWidget(m_flashSpeedLabel);

    group_layout->addStretch();

    m_cancelButton = new QPushButton("Cancel", group_box);
    m_cancelButton->setAccessibleName(QStringLiteral("Cancel Flash"));
    m_cancelButton->setToolTip(QStringLiteral("Cancel the current flash operation"));
    m_cancelButton->setStyleSheet(ui::kDangerButtonStyle);
    group_layout->addWidget(m_cancelButton);
    connect(m_cancelButton, &QPushButton::clicked, this, &ImageFlasherPanel::onCancelClicked);

    layout->addWidget(group_box);
    m_stackedWidget->addWidget(m_flashProgressPage);
}

void ImageFlasherPanel::createCompletionPage() {
    m_completionPage = new QWidget();
    auto* layout = new QVBoxLayout(m_completionPage);

    auto* group_box = new QGroupBox("Flash Complete", m_completionPage);
    auto* group_layout = new QVBoxLayout(group_box);

    m_completionMessageLabel = new QLabel("", group_box);
    QFont msg_font = m_completionMessageLabel->font();
    msg_font.setPointSize(kCompletionMessageFontSize);
    msg_font.setBold(true);
    m_completionMessageLabel->setFont(msg_font);
    group_layout->addWidget(m_completionMessageLabel);

    m_completionDetailsLabel = new QLabel("", group_box);
    m_completionDetailsLabel->setWordWrap(true);
    group_layout->addWidget(m_completionDetailsLabel);

    group_layout->addStretch();

    m_flashAnotherButton = new QPushButton("Flash Another", group_box);
    m_flashAnotherButton->setAccessibleName(QStringLiteral("Flash Another Drive"));
    m_flashAnotherButton->setToolTip(QStringLiteral("Start over and flash another drive"));
    m_flashAnotherButton->setStyleSheet(ui::kPrimaryButtonStyle);
    group_layout->addWidget(m_flashAnotherButton);
    connect(m_flashAnotherButton, &QPushButton::clicked, [this]() {
        m_stackedWidget->setCurrentIndex(kImageSelectionPageIndex);
        m_currentPage = kImageSelectionPageIndex;
        updateNavigationButtons();
    });

    layout->addWidget(group_box);
    m_stackedWidget->addWidget(m_completionPage);
}

bool ImageFlasherPanel::loadImageFile(const QString& file_path) {
    // Validation (existence, regular file, non-empty, format) lives in onImageSelected, which
    // returns false and makes no selection when the image is invalid.
    return onImageSelected(file_path);
}

void ImageFlasherPanel::onSelectImageClicked() {
    const QString filter =
        "Disk Images (*.iso *.img *.wic *.dmg *.dsk *.gz *.bz2 *.xz *.zip);;All Files "
        "(*.*)";
    const QString file_path =
        QFileDialog::getOpenFileName(this, "Select Image File", QString(), filter);

    if (!file_path.isEmpty()) {
        onImageSelected(file_path);
    }
}

void ImageFlasherPanel::onDownloadWindowsClicked() {
    Q_ASSERT(m_isoDownloader);
    Q_ASSERT(m_stackedWidget);
    auto* dialog = new WindowsISODownloadDialog(m_isoDownloader.get(), this);

    connect(dialog,
            &WindowsISODownloadDialog::downloadCompleted,
            this,
            [this](const QString& file_path) {
                // onImageSelected returns false when the downloaded file fails
                // validation, and has already said why. Offering to flash it then
                // -- and navigating to drive selection -- advertised an image that
                // was never selected, and left Flash reachable with no image at all.
                if (!onImageSelected(file_path)) {
                    return;
                }

                auto reply = sak::showQuestionLogged(
                    this,
                    "ISO Downloaded",
                    QString("Windows ISO downloaded successfully!\n\n%1\n\n"
                            "Would you like to flash this image to a USB drive now?")
                        .arg(QFileInfo(file_path).fileName()),
                    QMessageBox::Yes | QMessageBox::No);

                if (reply == QMessageBox::Yes) {
                    m_stackedWidget->setCurrentIndex(kDriveSelectionPageIndex);
                    updateNavigationButtons();
                }
            });

    dialog->exec();
    dialog->deleteLater();
}

void ImageFlasherPanel::onOpenMicrosoftWindowsDownloadClicked() {
    const QUrl microsoft_windows_iso_url("https://www.microsoft.com/software-download/windows11");
    if (!QDesktopServices::openUrl(microsoft_windows_iso_url)) {
        logWarning("Could not open the Microsoft download page in your default browser.");
        sak::showWarningLogged(
            this,
            "Unable to Open Browser",
            "Could not open the Microsoft download page in your default browser.");
    }
}

void ImageFlasherPanel::onDownloadLinuxClicked() {
    Q_ASSERT(m_linuxIsoDownloader);
    Q_ASSERT(m_stackedWidget);
    auto* dialog = new LinuxISODownloadDialog(m_linuxIsoDownloader.get(), this);

    connect(
        dialog, &LinuxISODownloadDialog::downloadCompleted, this, [this](const QString& file_path) {
            // Same as the Windows download flow: a downloaded image that fails
            // validation is not a selected image, so do not offer to flash it.
            if (!onImageSelected(file_path)) {
                return;
            }

            auto reply = sak::showQuestionLogged(
                this,
                "ISO Downloaded",
                QString("Linux ISO downloaded successfully!\n\n%1\n\n"
                        "Would you like to flash this image to a USB drive now?")
                    .arg(QFileInfo(file_path).fileName()),
                QMessageBox::Yes | QMessageBox::No);

            if (reply == QMessageBox::Yes) {
                m_stackedWidget->setCurrentIndex(kDriveSelectionPageIndex);
                updateNavigationButtons();
            }
        });

    dialog->exec();
    dialog->deleteLater();
}

bool ImageFlasherPanel::onImageSelected(const QString& image_path) {
    Q_ASSERT(m_imagePathLabel);
    // Fail closed: never select (or enable navigation for) an image that fails validation.
    if (!validateImageFile(image_path)) {
        return false;
    }
    m_selectedImagePath = image_path;

    const QFileInfo file_info(image_path);
    m_imageSize = file_info.size();
    m_imageLastModified = file_info.lastModified();

    m_imagePathLabel->setText(QString("Selected: %1").arg(file_info.fileName()));

    m_detectedFormat = imageFormatLabel(FileImageSource::detectFormat(image_path));

    // Run ISO analyzer for detailed info
    populateIsoInfo(image_path);

    m_nextButton->setEnabled(true);
    updateNavigationButtons();

    Q_EMIT statusMessage(QString("Image Flasher: %1 selected (%2)")
                             .arg(file_info.fileName(), formatFileSize(m_imageSize)),
                         kImageFlasherStatusTimeoutMs);
    logInfo(QString("Image selected: %1").arg(image_path).toStdString());
    return true;
}

void ImageFlasherPanel::onWindowsISODownloaded(const QString& iso_path) {
    // Do not report a successful download when the downloaded image fails validation
    // (validateImageFile has already surfaced the specific reason).
    if (!onImageSelected(iso_path)) {
        return;
    }

    sak::showInformationLogged(
        this,
        "Download Complete",
        QString("Windows 11 ISO downloaded successfully!\n\n%1").arg(iso_path));
}

void ImageFlasherPanel::onDriveListUpdated() {
    Q_ASSERT(m_driveListWidget);
    Q_ASSERT(m_driveScanner);
    m_driveListWidget->clear();

    QList<DriveInfo> drives;
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
        // Bind the concrete device identity to the row so a later swap can be detected.
        item->setData(Qt::UserRole + 1, driveIdentitySignature(drive));

        if (drive.isSystem) {
            item->setForeground(QBrush(Qt::red));
            item->setToolTip("WARNING: This is your system drive!");
        }
    }

    m_driveCountLabel->setText(QString("%1 drive(s) available").arg(drives.size()));
}

void ImageFlasherPanel::onDriveSelectionChanged() {
    m_selectedDrives.clear();
    m_selectedDriveSignatures.clear();

    auto selected_items = m_driveListWidget->selectedItems();
    for (auto* item : selected_items) {
        const QString path = item->data(Qt::UserRole).toString();
        m_selectedDrives.append(path);
        m_selectedDriveSignatures.insert(path, item->data(Qt::UserRole + 1).toString());
    }

    updateNavigationButtons();
}

void ImageFlasherPanel::onFlashClicked() {
    // Tier 2: raw disk I/O requires administrator privileges
    auto gate = sak::showElevationGate(
        this, tr("USB Flash"), tr("Writing to USB drives requires administrator privileges."));
    if (gate != sak::ElevationGateResult::AlreadyElevated) {
        return;
    }

    // Show confirmation dialog
    showConfirmationDialog();
}

void ImageFlasherPanel::onFlashProgress(const FlashProgress& progress) {
    // percentage is computed inside FlashCoordinator from worker-thread byte counts
    // against a totalBytes it derives itself. A bad total yields a nonsense
    // percentage; showing it would be a fabricated number on a destructive write.
    if (progress.percentage < 0.0 || progress.percentage > kPercentMaxF) {
        logError("Image flasher: ignoring out-of-range flash progress {}%", progress.percentage);
        return;
    }
    Q_EMIT progressUpdate(static_cast<int>(progress.percentage), kPercentMax);
    m_flashDetailsLabel->setText(QString("Written: %1 / %2")
                                     .arg(formatFileSize(progress.bytesWritten))
                                     .arg(formatFileSize(progress.totalBytes)));
    m_flashSpeedLabel->setText(QString("Speed: %1").arg(formatSpeed(progress.speedMBps)));
}

void ImageFlasherPanel::onFlashStateChanged(FlashState new_state, const QString& message) {
    (void)new_state;
    m_flashStateLabel->setText(message);
    logInfo(QString("Flash state: %1").arg(message).toStdString());
}

void ImageFlasherPanel::onFlashCompleted(const FlashResult& result) {
    Q_ASSERT(m_completionDetailsLabel);
    Q_ASSERT(m_completionMessageLabel);
    m_isFlashing = false;
    m_settingsButton->setEnabled(true);

    if (result.success) {
        m_completionMessageLabel->setText("[x] Flash Completed Successfully!");
        m_completionMessageLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorSuccess));
    } else {
        m_completionMessageLabel->setText("\u2717 Flash Completed with Errors");
        m_completionMessageLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorError));
    }

    m_completionDetailsLabel->setText(buildCompletionDetails(result));

    m_stackedWidget->setCurrentWidget(m_completionPage);
    updateNavigationButtons();

    notifyFlashFinished(result);

    Q_EMIT flashCompleted(result.totalDrives(), result.bytesWritten);
    Q_EMIT statusMessage(QString("Image Flasher: Flash %1 - %2 successful, %3 failed")
                             .arg(result.success ? "completed" : "completed with errors")
                             .arg(result.successfulDrives.size())
                             .arg(result.failedDrives.size()),
                         kImageFlasherStatusTimeoutMs);
}

QString ImageFlasherPanel::buildCompletionDetails(const FlashResult& result) {
    QString details = QString("Successful: %1\nFailed: %2")
                          .arg(result.successfulDrives.size())
                          .arg(result.failedDrives.size());

    if (!result.ejectedDrives.isEmpty()) {
        details += QString("\nEjected (safe to remove): %1").arg(result.ejectedDrives.size());
    }
    if (!result.ejectFailedDrives.isEmpty()) {
        // Never let a failed eject read as a successful one. The write is fine; the
        // drive is simply still mounted, and pulling it now can lose the last writes
        // Windows has cached.
        details += QString(
                       "\nCould NOT be ejected (%1) -- use Safely Remove Hardware before "
                       "unplugging:\n%2")
                       .arg(result.ejectFailedDrives.size())
                       .arg(result.ejectFailedDrives.join("\n"));
    }
    return details;
}

void ImageFlasherPanel::notifyFlashFinished(const FlashResult& result) {
    if (!sak::ConfigManager::instance().getImageFlasherEnableNotifications()) {
        return;
    }
    if (!QSystemTrayIcon::isSystemTrayAvailable() || !QSystemTrayIcon::supportsMessages()) {
        // Say so rather than silently doing nothing: the user turned notifications on
        // and is entitled to know why none arrived.
        logWarning(
            "Image flasher: desktop notifications are enabled but this session's system "
            "tray cannot display messages; no notification shown");
        return;
    }

    if (m_notificationTray == nullptr) {
        m_notificationTray = new QSystemTrayIcon(
            QIcon(QStringLiteral(":/icons/icons/panel_image_flasher.svg")), this);
    }
    m_notificationTray->show();
    m_notificationTray->showMessage(
        tr("Image Flasher"),
        QString("Flash %1: %2 successful, %3 failed.")
            .arg(result.success ? tr("completed") : tr("finished with errors"))
            .arg(result.successfulDrives.size())
            .arg(result.failedDrives.size()),
        result.success ? QSystemTrayIcon::Information : QSystemTrayIcon::Warning,
        kNotificationTimeoutMs);

    // Drop the tray icon again once the message has had its time on screen; it exists
    // only to carry the notification, not to give the app a permanent tray presence.
    QTimer::singleShot(kNotificationTimeoutMs, m_notificationTray, [this]() {
        if (m_notificationTray) {
            m_notificationTray->hide();
        }
    });
}

void ImageFlasherPanel::onFlashError(const QString& error) {
    Q_ASSERT(m_stackedWidget);
    m_isFlashing = false;
    m_settingsButton->setEnabled(true);

    // The message crosses a boundary from FlashCoordinator or from the Windows USB
    // creator's lastError(), which is default-empty. An empty reason must not turn
    // into a dialog with a blank gap where the cause should be -- name the missing
    // reason instead, the way FlashCoordinator::onWorkerFailed already does.
    const QString reported = error.isEmpty() ? tr("The writer reported no reason.") : error;

    logError(("Flash Error: " + reported).toStdString());
    sak::showCriticalLogged(this,
                            "Flash Error",
                            QString("The flash operation failed:\n\n%1\n\n"
                                    "The target drive(s) may be in an unusable state. "
                                    "You may need to reformat them before they can be used again.")
                                .arg(reported));

    // Return to drive selection so the user can see the state
    m_stackedWidget->setCurrentIndex(1);
    updateNavigationButtons();

    Q_EMIT flashFailed(reported);
    Q_EMIT statusMessage("Image Flasher: Flash operation failed", sak::kTimerStatusDefaultMs);
}

void ImageFlasherPanel::onCancelClicked() {
    Q_ASSERT(m_flashStateLabel);
    logWarning("Cancel Flash: User prompted to cancel flash operation.");
    auto reply = sak::showWarningLogged(
        this,
        "Cancel Flash",
        "Are you sure you want to cancel the flash operation?\n\n"
        "WARNING: Cancelling mid-write may leave the target drive(s) in an \n"
        "unusable state. You may need to reformat them before they can be used again.",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        // This panel drives two independent destructive writers: FlashCoordinator for raw
        // image writes, and a separate WindowsUSBCreator worker for Windows ISO media.
        // Cancel previously signalled only the coordinator, so cancelling a Windows USB
        // creation asked nothing to stop: the worker kept writing the disk while the UI
        // reported "Flash cancelled by user" and re-enabled the controls, letting the user
        // start another operation against a drive that was still being written.
        //
        // Signal every writer that currently exists. Both cancels set an atomic flag the
        // worker polls, and both are already invoked this way from the destructor.
        bool cancel_signalled = false;
        if (m_windowsUsbCreator != nullptr) {
            m_windowsUsbCreator->cancel();
            cancel_signalled = true;
        }
        if (m_flashCoordinator) {
            m_flashCoordinator->cancel();
            cancel_signalled = true;
        }
        if (!cancel_signalled) {
            // Nothing to cancel means the UI state and the worker state disagree. Refuse to
            // report a cancellation that never happened.
            logError("Cancel Flash: no active writer to cancel; refusing to report cancelled");
            return;
        }

        m_isFlashing = false;
        m_settingsButton->setEnabled(true);

        m_flashStateLabel->setText("Flash cancelled by user");
        m_flashDetailsLabel->clear();
        m_flashSpeedLabel->clear();

        // Return to drive selection so the user can retry
        m_stackedWidget->setCurrentIndex(1);
        updateNavigationButtons();

        Q_EMIT flashCancelled();
        Q_EMIT statusMessage("Image Flasher: Flash cancelled", sak::kTimerStatusDefaultMs);
    }
}

void ImageFlasherPanel::updateNavigationButtons() {
    Q_ASSERT(m_stackedWidget);
    Q_ASSERT(m_backButton);
    const int current_index = m_stackedWidget->currentIndex();

    // Back button: enabled on pages 1 (drive selection) and 3 (completion)
    // Disabled during flashing (page 2) and on page 0 (nothing to go back to)
    const bool can_go_back = !m_isFlashing && (current_index == kDriveSelectionPageIndex ||
                                               current_index == kCompletionPageIndex);
    m_backButton->setEnabled(can_go_back);
    m_backButton->setVisible(current_index != kFlashProgressPageIndex);

    // Next button: enabled based on page state
    switch (current_index) {
    case kImageSelectionPageIndex:
        m_nextButton->setEnabled(!m_selectedImagePath.isEmpty());
        m_nextButton->setVisible(true);
        break;
    case kDriveSelectionPageIndex:
    case kFlashProgressPageIndex:
    case kCompletionPageIndex:
    default:
        m_nextButton->setEnabled(false);
        m_nextButton->setVisible(current_index == kImageSelectionPageIndex);
        break;
    }

    // Flash button: only visible and enabled on drive selection page when not flashing
    m_flashButton->setVisible(current_index == kDriveSelectionPageIndex && !m_isFlashing);
    // An image is as necessary as a target. This used to gate on the drive list
    // alone, so a download whose image failed validation still left Flash enabled.
    m_flashButton->setEnabled(!m_selectedDrives.isEmpty() && !m_selectedImagePath.isEmpty() &&
                              !m_isFlashing);

    // Cancel button: visible during flashing
    m_cancelButton->setVisible(current_index == kFlashProgressPageIndex);
    m_cancelButton->setEnabled(m_isFlashing);
}

bool ImageFlasherPanel::validateImageFile(const QString& file_path) {
    const QFileInfo file_info(file_path);

    // A directory or a missing path is not a flashable image (fail closed).
    if (!file_info.exists() || !file_info.isFile()) {
        logWarning(QString("Invalid Image: not a file: %1").arg(file_path).toStdString());
        sak::showWarningLogged(this,
                               "Invalid Image",
                               QString("Not a readable image file: %1").arg(file_path));
        return false;
    }
    if (!file_info.isReadable()) {
        logWarning(QString("Invalid Image: File is not readable: %1").arg(file_path).toStdString());
        sak::showWarningLogged(this,
                               "Invalid Image",
                               QString("File is not readable: %1").arg(file_path));
        return false;
    }
    if (file_info.size() == 0) {
        logWarning("Invalid Image: Image file is empty");
        sak::showWarningLogged(this, "Invalid Image", "Image file is empty");
        return false;
    }

    // Unknown format is not a hard error (raw .img images are legitimate), but require explicit
    // user confirmation instead of silently proceeding.
    if (FileImageSource::detectFormat(file_path) == ImageFormat::Unknown) {
        const auto reply =
            sak::showQuestionLogged(this,
                                    "Unknown Format",
                                    "Unable to detect image format. Continue anyway?",
                                    QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return false;
        }
    }

    logInfo(QString("Image file validated: %1").arg(file_path).toStdString());
    return true;
}

bool ImageFlasherPanel::selectedImageUnchanged() const {
    const QFileInfo info(m_selectedImagePath);
    // Refuse if the source was swapped/truncated/removed since selection (identity == size + mtime
    // captured at selection). A same-size, same-mtime replacement is not detectable without a full
    // rehash, which is infeasible for multi-GB images on the UI thread.
    return info.exists() && info.isFile() && info.size() == m_imageSize && m_imageSize > 0 &&
           info.lastModified() == m_imageLastModified;
}

bool ImageFlasherPanel::selectedDrivesIdentityUnchanged() const {
    const QList<DriveInfo> current = m_driveScanner->getDrives();
    for (const QString& path : m_selectedDrives) {
        const auto match =
            std::ranges::find_if(current, [&](const DriveInfo& d) { return d.devicePath == path; });
        if (match == current.cend() ||
            driveIdentitySignature(*match) != m_selectedDriveSignatures.value(path)) {
            return false;
        }
    }
    return true;
}

bool ImageFlasherPanel::confirmSelectionStillValid() {
    if (!selectedImageUnchanged()) {
        sak::showCriticalLogged(this,
                                "Image Changed",
                                "The selected image changed or is no longer available. Reselect "
                                "the image before flashing.");
        return false;
    }
    if (!selectedDrivesIdentityUnchanged()) {
        sak::showCriticalLogged(this,
                                "Drive Changed",
                                "A selected drive changed or was removed since you selected it. "
                                "Rescan and reselect the target before flashing.");
        return false;
    }
    return true;
}

QStringList ImageFlasherPanel::buildDriveDetailsList(bool& has_system_drive) const {
    QStringList drive_details;
    has_system_drive = false;
    for (const auto& drive_path : m_selectedDrives) {
        const QString label = findDriveDisplayText(drive_path);
        drive_details << QString("  \u2022 %1").arg(label);
        if (isSystemDrive(drive_path)) {
            has_system_drive = true;
        }
    }
    return drive_details;
}

QStringList ImageFlasherPanel::selectedDrivesOverThreshold(qint64 threshold_bytes) const {
    const QList<DriveInfo> current = m_driveScanner->getDrives();
    QStringList oversized;
    for (const QString& drive_path : m_selectedDrives) {
        const auto match = std::ranges::find_if(current, [&](const DriveInfo& d) {
            return d.devicePath == drive_path;
        });
        // A drive that is no longer in the scan has no readable capacity; -1 makes
        // driveExceedsLargeThreshold warn instead of quietly passing it.
        const qint64 drive_bytes = (match == current.cend()) ? -1 : match->size;
        if (driveExceedsLargeThreshold(drive_bytes, threshold_bytes)) {
            oversized << QString("  \u2022 %1").arg(findDriveDisplayText(drive_path));
        }
    }
    return oversized;
}

bool ImageFlasherPanel::confirmLargeDrives() {
    const auto& config = sak::ConfigManager::instance();
    if (!config.getImageFlasherShowLargeDriveWarning()) {
        return true;
    }

    const int threshold_gb = config.getImageFlasherLargeDriveThreshold();
    const qint64 threshold_bytes = largeDriveThresholdBytes(threshold_gb);
    if (threshold_bytes < 0) {
        // Name the unusable value instead of skipping the check the user asked for.
        // Every selected drive is then reported as oversized (fail closed), so the
        // prompt still appears rather than silently disappearing.
        logError(
            "Image flasher: unusable large-drive threshold {} GB; warning about every "
            "selected drive",
            threshold_gb);
    }

    const QStringList oversized = selectedDrivesOverThreshold(threshold_bytes);
    if (oversized.isEmpty()) {
        return true;
    }

    // Say which of the two situations this is. Claiming the listed drives exceed a
    // threshold that could not be applied would be a lie about why they are listed.
    const QString lead =
        threshold_bytes < 0
            ? QString(
                  "The large-drive threshold (%1 GB) could not be applied, so every "
                  "selected target is listed:")
                  .arg(threshold_gb)
            : QString("The following target(s) are larger than the %1 GB threshold:")
                  .arg(threshold_gb);

    const auto reply = sak::showWarningLogged(
        this,
        "Large Drive Selected",
        QString("%1\n\n%2\n\n"
                "Drives this size are usually internal or external hard disks rather than "
                "USB sticks or SD cards. Flashing one ERASES ALL DATA on it.\n\n"
                "Continue with these targets?")
            .arg(lead, oversized.join("\n")),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return reply == QMessageBox::Yes;
}

QString ImageFlasherPanel::findDriveDisplayText(const QString& device_path) const {
    for (int i = 0; i < m_driveListWidget->count(); ++i) {
        auto* item = m_driveListWidget->item(i);
        if (item->data(Qt::UserRole).toString() == device_path) {
            return item->text();
        }
    }
    return device_path;
}

QString ImageFlasherPanel::buildFlashConfirmationMessage(const QStringList& drive_details,
                                                         bool is_windows_iso) const {
    QString method_info;
    if (is_windows_iso) {
        method_info =
            "\nMethod: Extract ISO to NTFS-formatted drive\n"
            "(Proper Windows installation USB)";
    } else {
        method_info =
            "\nMethod: Raw disk imaging\n"
            "(Bootable for Linux, other ISOs)";
    }

    return QString(
               "(!)  DESTRUCTIVE OPERATION  (!)\n\n"
               "Image: %1\n\n"
               "Target Drive(s):\n%2\n"
               "%3\n\n"
               "ALL DATA on the target drive(s) will be PERMANENTLY ERASED.\n"
               "This action CANNOT be undone.\n\n"
               "Are you absolutely sure you want to continue?")
        .arg(QFileInfo(m_selectedImagePath).fileName())
        .arg(drive_details.join("\n"))
        .arg(method_info);
}

void ImageFlasherPanel::showConfirmationDialog() {
    Q_ASSERT(m_flashButton);
    // Reaching here with no image selected is a real state, not an impossible one:
    // the Flash button's enabled state is derived from the drive list, and the two
    // download flows navigate to drive selection after offering "flash this now?".
    // Refuse with the accurate reason rather than continuing into
    // confirmSelectionStillValid(), which would report the image as CHANGED when in
    // fact none was ever selected.
    if (m_selectedImagePath.isEmpty()) {
        sak::showWarningLogged(this,
                               "No Image Selected",
                               "No image is selected. Go back to step 1 and choose an image "
                               "before flashing.");
        return;
    }

    // Check if this is a Windows ISO
    const bool is_windows_iso = isWindowsInstallISO(m_selectedImagePath);

    // Build drive list for display
    bool has_system_drive = false;
    const QStringList drive_details = buildDriveDetailsList(has_system_drive);

    // Block system drive flashing with a hard error
    if (has_system_drive) {
        sak::logError("Operation blocked: user attempted to flash system drive");
        sak::showCriticalLogged(
            this,
            "Operation Blocked",
            "One or more selected drives is your SYSTEM DRIVE.\n\n"
            "Flashing to the system drive would destroy your Windows installation "
            "and render this computer unbootable.\n\n"
            "Please deselect the system drive and try again.");
        return;
    }

    // Fail closed on a mid-flow swap of the source image or any target drive.
    if (!confirmSelectionStillValid()) {
        return;
    }

    // Large-drive warning, if the user left it on. It runs BEFORE the destructive
    // confirmation, so declining here costs nothing: it is a chance to notice an
    // internal disk in the list, not a second copy of the same prompt.
    if (!confirmLargeDrives()) {
        return;
    }

    const QString message = buildFlashConfirmationMessage(drive_details, is_windows_iso);

    auto reply = sak::showWarningLogged(this,
                                        "Confirm Flash -- Data Loss Warning",
                                        message,
                                        QMessageBox::Yes | QMessageBox::No,
                                        QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    beginConfirmedFlash(is_windows_iso);
}

void ImageFlasherPanel::beginConfirmedFlash(bool is_windows_iso) {
    // Disable UI immediately upon confirmation
    m_isFlashing = true;
    m_flashButton->setEnabled(false);
    m_flashButton->setVisible(false);
    m_settingsButton->setEnabled(false);
    updateNavigationButtons();

    // Switch to progress page
    m_stackedWidget->setCurrentWidget(m_flashProgressPage);

    if (is_windows_iso) {
        createWindowsUSB();
        return;
    }

    // Apply the persisted Image Flasher settings before starting. Nothing read them back
    // before this: the settings dialog wrote validation mode and buffer size to
    // ConfigManager and no code ever asked for them, so every flash ran with the worker's
    // built-in defaults and the user's choice was silently discarded.
    applyFlasherSettings();

    // Use raw disk imaging for other ISOs
    if (!m_flashCoordinator->startFlash(m_selectedImagePath, m_selectedDrives)) {
        m_isFlashing = false;
        onFlashError("Failed to start flash operation - flash coordinator returned error");
    }
}

bool ImageFlasherPanel::isWindowsInstallISO(const QString& iso_path) {
    // Classify from the image's own metadata (volume label / application ID via
    // IsoAnalyzer), not the filename. The old filename heuristic matched
    // "server" and routed Linux server ISOs into the Windows-installer path.
    return IsoAnalyzer::isWindowsInstallMedia(IsoAnalyzer::analyze(iso_path));
}

void ImageFlasherPanel::connectWindowsUSBCreatorSignals(WindowsUSBCreator* creator,
                                                        QThread* thread) {
    Q_ASSERT(creator);
    Q_ASSERT(thread);
    connect(creator, &WindowsUSBCreator::statusChanged, this, [this](const QString& status) {
        m_flashStateLabel->setText(status);
    });

    connect(creator, &WindowsUSBCreator::progressUpdated, this, [this](int percentage) {
        Q_EMIT progressUpdate(percentage, kPercentMax);
    });

    connect(creator, &WindowsUSBCreator::completed, this, [this]() {
        m_isFlashing = false;
        FlashResult result;
        result.success = true;
        result.successfulDrives = m_selectedDrives;
        result.bytesWritten = m_imageSize * m_selectedDrives.size();

        // Windows installation media is written by WindowsUSBCreator, not by
        // FlashCoordinator, so this path has to apply the eject-on-completion setting
        // itself. Both paths run the same coordinator routine, so a Windows USB and a
        // raw image report removal identically.
        if (sak::ConfigManager::instance().getImageFlasherUnmountOnCompletion()) {
            FlashCoordinator::ejectCompletedDrives(result);
        }

        onFlashCompleted(result);
    });

    connect(creator, &WindowsUSBCreator::failed, this, [this](const QString& error) {
        m_isFlashing = false;
        m_flashButton->setEnabled(true);
        m_flashButton->setVisible(true);
        updateNavigationButtons();
        onFlashError(error);
    });

    // Clean up thread and creator when thread finishes
    connect(creator, &WindowsUSBCreator::completed, thread, &QThread::quit);
    connect(creator, &WindowsUSBCreator::failed, thread, &QThread::quit);
    connect(thread, &QThread::finished, this, [this]() {
        // The operation ended; forget the tracked pointers so the destructor
        // does not touch objects that deleteLater is about to reclaim.
        m_windowsUsbCreator = nullptr;
        m_windowsUsbThread = nullptr;
    });
    connect(thread, &QThread::finished, creator, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
}

QString ImageFlasherPanel::parseDiskNumberFromDevicePath(const QString& device_path) {
    Q_ASSERT(!device_path.isEmpty());
    const QRegularExpression regex(R"(PhysicalDrive(\d+))");
    const QRegularExpressionMatch match = regex.match(device_path);
    if (match.hasMatch()) {
        logInfo(
            QString("Using disk number %1 (PhysicalDrive%1)").arg(match.captured(1)).toStdString());
        return match.captured(1);
    }
    return {};
}

void ImageFlasherPanel::createWindowsUSB() {
    Q_ASSERT(m_flashStateLabel);
    Q_ASSERT(m_flashButton);
    if (m_selectedDrives.isEmpty()) {
        sak::logError("Windows USB creation attempted with no target drives selected");
        sak::showCriticalLogged(this, "Error", "No target drives selected");
        m_isFlashing = false;
        return;
    }

    // Windows USB creation targets a single disk (diskpart). Extra drives were
    // silently dropped yet reported as written; reject instead of claiming
    // success for drives never touched.
    if (m_selectedDrives.size() > 1) {
        m_isFlashing = false;
        m_flashButton->setEnabled(true);
        m_flashButton->setVisible(true);
        updateNavigationButtons();
        onFlashError("Windows USB creation writes one drive at a time; select a single drive.");
        return;
    }

    // Initialize progress display
    m_flashStateLabel->setText("Initializing...");

    // Create Windows USB creator -- no parent, will be moved to worker thread
    auto* creator = new WindowsUSBCreator();
    auto* thread = new QThread(this);
    creator->moveToThread(thread);
    m_windowsUsbCreator = creator;
    m_windowsUsbThread = thread;

    connectWindowsUSBCreatorSignals(creator, thread);

    // Extract disk number (hardware ID) from device path
    const QString device_path = m_selectedDrives.first();
    const QString disk_number = parseDiskNumberFromDevicePath(device_path);

    if (disk_number.isEmpty()) {
        // Could not parse disk number - fail immediately
        m_isFlashing = false;
        m_flashButton->setEnabled(true);
        m_flashButton->setVisible(true);
        updateNavigationButtons();
        onFlashError(QString("Could not identify disk number from %1").arg(device_path));
        m_windowsUsbCreator = nullptr;
        m_windowsUsbThread = nullptr;
        creator->deleteLater();
        thread->deleteLater();
        return;
    }

    // Start the creation on the worker thread
    const QString iso_path = m_selectedImagePath;
    connect(thread, &QThread::started, creator, [creator, iso_path, disk_number]() {
        creator->createBootableUSB(iso_path, disk_number);
    });
    thread->start();
}

void ImageFlasherPanel::applyFlasherSettings() {
    const auto& config = sak::ConfigManager::instance();

    const QString mode_setting = config.getImageFlasherValidationMode();
    const auto mode = sak::validationModeFromSetting(mode_setting);
    if (mode.has_value()) {
        m_flashCoordinator->setValidationMode(*mode);
    } else {
        // Do not quietly pick one. Name the unrecognized value, then use the most thorough
        // mode rather than the least: a corrupted setting must not silently downgrade
        // verification on a destructive write.
        sak::logError("Image flasher: unrecognized validation mode '{}'; using full verification",
                      mode_setting.toStdString());
        m_flashCoordinator->setValidationMode(sak::ValidationMode::Full);
    }

    const int buffer_size_mb = config.getImageFlasherBufferSize();
    if (buffer_size_mb > 0) {
        m_flashCoordinator->setBufferSize(static_cast<qint64>(buffer_size_mb) * sak::kBytesPerMB);
    } else {
        sak::logError("Image flasher: non-positive buffer size {} MB in settings; keeping default",
                      buffer_size_mb);
    }

    // setMaxConcurrentWrites refuses a value below 1 and says so, so a corrupt
    // setting keeps the previous ceiling instead of stalling the run.
    const int max_concurrent = config.getImageFlasherMaxConcurrentWrites();
    m_flashCoordinator->setMaxConcurrentWrites(max_concurrent);

    const bool eject_on_completion = config.getImageFlasherUnmountOnCompletion();
    m_flashCoordinator->setEjectOnCompletion(eject_on_completion);

    sak::logInfo(
        "Image flasher settings applied: validation='{}', buffer={} MB, "
        "max concurrent writes={}, eject on completion={}",
        mode_setting.toStdString(),
        buffer_size_mb,
        m_flashCoordinator->maxConcurrentWrites(),
        eject_on_completion);
}

bool ImageFlasherPanel::isSystemDrive(const QString& device_path) const {
    return m_driveScanner->isSystemDrive(device_path);
}

QString ImageFlasherPanel::formatFileSize(qint64 bytes) {
    return formatBytes(bytes);
}

QString ImageFlasherPanel::formatSpeed(double mbps) const {
    if (mbps < 1.0) {
        return QString("%1 KB/s").arg(mbps * sak::kBytesPerKBf, 0, 'f', 1);
    }
    return QString("%1 MB/s").arg(mbps, 0, 'f', 1);
}

// ============================================================================
// ISO Info Population
// ============================================================================

void ImageFlasherPanel::populateIsoInfo(const QString& image_path) {
    Q_ASSERT(!image_path.isEmpty());
    Q_ASSERT(m_isoInfoGroup);

    const sak::IsoInfo iso = sak::IsoAnalyzer::analyze(image_path);

    // Always show size and format
    m_infoSizeLabel->setText(formatFileSize(m_imageSize));
    m_infoFormatLabel->setText(m_detectedFormat);

    // Populate all fields -- show dash for empty fields
    auto set_field = [](QLabel* label, const QString& value) {
        label->setText(value.isEmpty() ? QStringLiteral("-") : value);
    };

    set_field(m_infoOsLabel, iso.os_name);
    set_field(m_infoArchLabel, iso.architecture);

    QString boot_text;
    if (iso.is_bootable) {
        boot_text = iso.boot_type.isEmpty() ? QStringLiteral("Yes") : iso.boot_type;
    }
    set_field(m_infoBootLabel, boot_text);

    set_field(m_infoFilesysLabel, iso.filesystem);
    set_field(m_infoVolLabel, iso.volume_label);
    set_field(m_infoPublisherLabel, iso.publisher);
    set_field(m_infoDateLabel, iso.creation_date);
    set_field(m_infoEditionsLabel, iso.windows_editions.join(", "));

    m_isoInfoGroup->show();

    logInfo(QString("ISO analysis: %1 | Vol: %2 | Boot: %3")
                .arg(iso.os_name.isEmpty() ? "Unknown OS" : iso.os_name)
                .arg(iso.volume_label)
                .arg(iso.is_bootable ? iso.boot_type : "No")
                .toStdString());
}

}  // namespace sak
