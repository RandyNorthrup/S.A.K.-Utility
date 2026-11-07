// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_image_flasher_panel.cpp
/// @brief Guards on the Image Flasher's entry into a destructive write.
///
/// The panel had no test target at all, which is how the Flash button came to be
/// gated on the target drive list alone: an ISO download whose image FAILED
/// validation still navigated to drive selection, and picking a drive there
/// enabled Flash with no image selected. Nothing was written -- the mid-flow
/// revalidation refuses -- but the user was told the image had "changed or is no
/// longer available", which was false.
///
/// These tests drive the real panel through its widget surface (the same buttons
/// and list a technician clicks), because the defect lived in the wiring between
/// selection state and button state, not in any single function.

#include "sak/color_constants.h"
#include "sak/drive_scanner.h"
#include "sak/flash_coordinator.h"
#include "sak/image_flasher_panel.h"
#include "sak/linux_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"
#include "sak/windows_usb_creator.h"

#include "../support/test_hardware_guard.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

using namespace sak;

namespace {

constexpr int kModalPollIntervalMs = 50;
// A real, read-only enumeration of this host's disks; it has measured 8-10 s under a loaded
// parallel suite (see test_drive_scanner), so the bound is generous but finite.
constexpr int kStartupScanTimeoutMs = 30'000;

/// A device path plus the identity signature the panel stores alongside it. The
/// panel builds these from DriveScanner results; the tests place this row directly once the
/// real start-up scan has landed.
constexpr auto kFakeDevicePath = "\\\\.\\PhysicalDrive99";

/// The Step 1 -> Step 2 navigation button. Every assertion in this file watched the Flash
/// button only, so the Next gate -- which decides whether a user can reach drive selection at
/// all -- had no coverage in either direction.
/// Answers any message box that opens while it is alive, and keeps what the box said. Goes away
/// with the test, so nothing is left running to answer a later test's dialog.
class ModalAnswerer {
public:
    QString seen;

    ModalAnswerer() {
        m_timer.setInterval(kModalPollIntervalMs);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (seen.isEmpty()) {
                    seen = box->text();
                }
                box->accept();
            }
        });
        m_timer.start();
    }

private:
    QTimer m_timer;
};

QPushButton* findNextButton(QWidget* panel) {
    const auto buttons = panel->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == QLatin1String("Next Step")) {
            return button;
        }
    }
    return nullptr;
}

}  // namespace

class ImageFlasherPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Review 2 (S17): Build Configuration's fields started near 250 px, Language and Edition's
    // near 361: two group boxes, two label columns.
    void theIsoFormHasOneLabelColumn() {
        WindowsISODownloader downloader;
        WindowsISODownloadDialog dialog(&downloader);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QHash<QString, int> left;
        for (auto* combo : dialog.findChildren<QComboBox*>()) {
            left.insert(combo->accessibleName(), combo->mapTo(&dialog, QPoint(0, 0)).x());
        }
        for (const char* name : {"Architecture", "Release channel", "Language", "Edition"}) {
            QVERIFY2(left.contains(QString::fromLatin1(name)), name);
            QCOMPARE(left.value(QString::fromLatin1(name)), left.value(QStringLiteral("Language")));
        }
        // Review 2 (F35): the Available Builds list was empty with nothing to say why.
        QListWidget* builds = nullptr;
        for (auto* list : dialog.findChildren<QListWidget*>()) {
            if (list->accessibleName() == QStringLiteral("Builds")) {
                builds = list;
            }
        }
        QVERIFY(builds != nullptr);
        const QLabel* empty = builds->viewport()->findChild<QLabel*>();
        QVERIFY(empty != nullptr);
        QVERIFY2(empty->text().startsWith(QStringLiteral("Press Fetch Builds")),
                 qPrintable(empty->text()));
    }
    // UI review S5: success read "[x] Flash Completed Successfully!", a failure mark.
    // The page said only that the stick "could NOT be ejected"; it was still offline and showed
    // up with no drive letter afterwards.
    void aStickLeftOfflineIsNamedWithTheWayBack() {
        // The fixture's panel: init() makes one for every test, and a second panel beside it is a
        // second DriveScanner, which the scanner's one-instance assertion stops in Debug.
        sak::ImageFlasherPanel& panel = *m_panel;
        sak::FlashResult result;
        result.success = true;
        result.successfulDrives = {QStringLiteral("\\\\.\\PhysicalDrive5")};
        result.reonlineFailedDrives = result.successfulDrives;
        panel.onFlashCompleted(result);
        const QString text = panel.m_completionDetailsLabel->text();
        QVERIFY2(text.contains(QStringLiteral("Still offline (1)")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("online disk")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("PhysicalDrive5")), qPrintable(text));
    }
    // Dark re-check DN3, both themes: "Flash Another" sat at the foot of a page as tall as the
    // tallest page, and the fixed footer cut it to a 9 px strip.
    void flashAnotherIsWholeAboveTheFooter() {
        // Shorter than the pages' content, as the capture's window was: the page then scrolls,
        // and a button at the foot of the tallest page's height is below the fold.
        // The fixture's panel: init() makes one for every test, and a second panel beside it is a
        // second DriveScanner, which the scanner's one-instance assertion stops in Debug.
        sak::ImageFlasherPanel& panel = *m_panel;
        panel.resize(1240, 520);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        sak::FlashResult result;
        result.success = true;
        result.successfulDrives = {QStringLiteral("\\\\.\\PhysicalDrive5")};
        panel.onFlashCompleted(result);
        QCoreApplication::processEvents();
        QPushButton* button = panel.m_flashAnotherButton;
        QVERIFY(button->isVisible());
        const int bottom = button->mapTo(&panel, QPoint(0, button->height())).y();
        const int footer_top = panel.m_settingsButton->mapTo(&panel, QPoint(0, 0)).y();
        QVERIFY2(bottom <= footer_top,
                 qPrintable(QStringLiteral("button ends at %1, footer starts at %2")
                                .arg(bottom)
                                .arg(footer_top)));
        QCOMPARE(button->visibleRegion().boundingRect().height(), button->height());
    }
    void aSuccessfulFlashSaysSoWithATick() {
        // The fixture's panel: init() makes one for every test, and a second panel beside it is a
        // second DriveScanner, which the scanner's one-instance assertion stops in Debug.
        sak::ImageFlasherPanel& panel = *m_panel;
        sak::FlashResult result;
        result.success = true;
        panel.onFlashCompleted(result);
        const QString text = panel.m_completionMessageLabel->text();
        QVERIFY2(text.startsWith(QString::fromUtf8(sak::ui::kStatusPrefixSuccess)),
                 qPrintable(text));
        QVERIFY2(!text.contains(QLatin1Char('[')), qPrintable(text));
    }
    // UI review F19: Image Details' values started half way across the group, ~590 px from
    // their labels.
    void imageDetailsValuesSitNextToTheirLabels() {
        // The fixture's panel: init() makes one for every test, and a second panel beside it is a
        // second DriveScanner, which the scanner's one-instance assertion stops in Debug.
        sak::ImageFlasherPanel& panel = *m_panel;
        panel.resize(1280, 800);
        panel.m_isoInfoGroup->setVisible(true);
        panel.m_isoInfoGroup->resize(1200, 300);
        panel.m_isoInfoGrid->activate();
        panel.m_isoInfoGrid->setGeometry(QRect(0, 0, 1200, 300));
        const QRect labels = panel.m_isoInfoGrid->cellRect(0, 0);
        const QRect values = panel.m_isoInfoGrid->cellRect(0, 1);
        QVERIFY2(values.left() - labels.left() < 400,
                 qPrintable(QStringLiteral("labels at %1, values at %2")
                                .arg(labels.left())
                                .arg(values.left())));
    }
    void initTestCase();
    void init();
    void cleanup();
    void windowsIsoEditionStartsOnAnEdition();

    void smokeAndAuditModesRunTheStartupScanUsersGet();
    void flashIsDisabledWithNothingSelected();
    void flashStaysDisabledWithADriveButNoImage();
    void flashEnablesOnlyWithBothAnImageAndADrive();
    void loadImageFileRefusesAnEmptyPath();
    void loadImageFileRefusesAMissingFile();
    void loadImageFileRefusesAnEmptyFile();
    void aRejectedImageLeavesFlashDisabled();
    void loadImageFileRefusesAnUnknownFormatWhenDeclined();
    void loadImageFileRefusesAZipArchive();
    void loadImageFileRefusesAnAppleDiskImage();
    void imagePickerOffersOnlyWhatFlashes();
    void readingACompressedImageIsNotShownAsWriting();
    void aRefusalBeforeWritingSaysNothingWasWritten();
    void aWindowsUsbStepReachesTheStatusBar();
    void aBuiltIsoIsNotAnnouncedTwice();
    void shredOsStillGetsASaveLocation();
    void aWicImageIsNotCalledWindowsImaging();
    void anImageThatCannotBeLookedInsideStopsTheFlash();
    void aDriveThatChangedSaysToReselectIt();
    void categoryListShowsOneAmpersand();

private:
    QPushButton* flashButton() const;
    QListWidget* driveList() const;
    /// Put one selectable drive row in the list, as onDriveListUpdated would.
    void selectOneDrive();
    /// Close whatever modal QMessageBox appears; returns after it has been closed.
    void armMessageBoxDismisser(QString* seen_text = nullptr);
    /// Write @p bytes to a .img under m_dir and return its path.
    QString writeImage(const QString& name, const QByteArray& bytes);

    std::unique_ptr<ImageFlasherPanel> m_panel;
    QTemporaryDir m_dir;
};

// Every coordinator error comes before a writer starts. Telling the technician to expect a ruined
// drive at that point is false, and on a raw-disk tool it is alarming.
void ImageFlasherPanelTests::aRefusalBeforeWritingSaysNothingWasWritten() {
    auto* coordinator = m_panel->findChild<FlashCoordinator*>();
    QVERIFY(coordinator != nullptr);
    QString dialog_text;
    QTimer::singleShot(0, this, [&dialog_text]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr) {
            dialog_text = box->text();
            box->accept();
        }
    });
    Q_EMIT coordinator->flashError(QStringLiteral("Refusing to flash: too small"));
    QVERIFY2(dialog_text.contains(QStringLiteral("Nothing was written")), qPrintable(dialog_text));
    QVERIFY2(!dialog_text.contains(QStringLiteral("unusable")), qPrintable(dialog_text));
}

// testpc1: through a whole Windows USB run the status bar read "Ready" beside its progress bar, as
// the creator's steps reached only the page. Each step reaches the status bar as well.
void ImageFlasherPanelTests::aWindowsUsbStepReachesTheStatusBar() {
    WindowsUSBCreator creator;
    QThread thread;  // never started: the panel only wires its end
    m_panel->connectWindowsUSBCreatorSignals(&creator, &thread);
    QSignalSpy status(m_panel.get(), &ImageFlasherPanel::statusMessage);
    Q_EMIT creator.statusChanged(
        QStringLiteral("Step 2/3: Splitting install.wim for FAT32 (DISM)..."));
    QCOMPARE(status.count(), 1);
    const QString shown = status.first().first().toString();
    QVERIFY2(shown.contains(QStringLiteral("Splitting install.wim")), qPrintable(shown));
}

// The build dialog says it finished and the panel offers to flash. The downloader's own signal used
// to add a third box on top, always naming Windows 11 whatever was built.
void ImageFlasherPanelTests::aBuiltIsoIsNotAnnouncedTwice() {
    auto* downloader = m_panel->findChild<WindowsISODownloader*>();
    QVERIFY(downloader != nullptr);
    // Answers a dialog if one opens, so a second announcement fails this test rather than leaving
    // the suite stuck in a modal loop until Qt's watchdog kills the whole run.
    ModalAnswerer announced;
    Q_EMIT downloader->downloadComplete(QStringLiteral("C:/does-not-exist.iso"), 1);
    QTest::qWait(2 * kModalPollIntervalMs);
    QVERIFY2(announced.seen.isEmpty(), qPrintable(announced.seen));
}

// If the bundled 7-Zip cannot run, nothing is known about the image, and guessing the wrong writer
// erases the drive and then fails. Nothing further may be asked or started.
void ImageFlasherPanelTests::anImageThatCannotBeLookedInsideStopsTheFlash() {
    const QString image = writeImage("payload.img", QByteArray(4096, '\x5A'));
    QVERIFY(m_panel->loadImageFile(image));
    WindowsUSBCreator::setInstallImageCheckForTesting(WindowsUSBCreator::InstallImageCheck::Failed);
    const auto reset = qScopeGuard([]() { WindowsUSBCreator::setInstallImageCheckForTesting({}); });
    ModalAnswerer told;

    bool is_windows_iso = true;
    QVERIFY(!m_panel->decideImageWriter(&is_windows_iso));

    QVERIFY2(told.seen.contains(QStringLiteral("Could not look inside")), qPrintable(told.seen));
    QVERIFY2(told.seen.contains(QStringLiteral("nothing was changed")), qPrintable(told.seen));
}

// A row this suite planted is on no scan, so it reads as changed. There is no Rescan button.
void ImageFlasherPanelTests::aDriveThatChangedSaysToReselectIt() {
    const QString image = writeImage("payload.img", QByteArray(4096, '\x5A'));
    QVERIFY(m_panel->loadImageFile(image));
    selectOneDrive();
    QVERIFY(flashButton()->isEnabled());
    ModalAnswerer told;

    QVERIFY(!m_panel->confirmSelectionStillValid());

    QVERIFY2(told.seen.contains(QStringLiteral("Reselect the target")), qPrintable(told.seen));
    QVERIFY2(!told.seen.contains(QStringLiteral("Rescan")), qPrintable(told.seen));
}

// A combo box shows text as it is, so a doubled ampersand there was shown doubled.
void ImageFlasherPanelTests::categoryListShowsOneAmpersand() {
    LinuxISODownloader downloader;
    LinuxISODownloadDialog dialog(&downloader);
    auto* categories = dialog.findChild<QComboBox*>();
    QVERIFY(categories != nullptr);
    QVERIFY(categories->findText(QStringLiteral("Security & Pen-Testing")) >= 0);
    for (int row = 0; row < categories->count(); ++row) {
        QVERIFY2(!categories->itemText(row).contains(QStringLiteral("&&")),
                 qPrintable(categories->itemText(row)));
    }
}

// .wic is the raw disk image OpenEmbedded's wic tool produces. It has nothing to do with Windows.
void ImageFlasherPanelTests::aWicImageIsNotCalledWindowsImaging() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile image(dir.filePath(QStringLiteral("board.wic")));
    QVERIFY(image.open(QIODevice::WriteOnly));
    image.write(QByteArray(4096, 'w'));
    image.close();
    QVERIFY(m_panel->loadImageFile(image.fileName()));

    const auto labels = m_panel->findChildren<QLabel*>();
    QVERIFY(std::ranges::any_of(labels, [](const QLabel* label) {
        return label->text() == QStringLiteral("WIC Disk Image");
    }));
    QVERIFY(std::ranges::none_of(labels, [](const QLabel* label) {
        return label->text().contains(QStringLiteral("Windows Imaging"));
    }));
}

// ShredOS's release name is only known once the download has started, and an empty Save Location
// left the technician to invent a name, which then failed the checksum after the whole download.
void ImageFlasherPanelTests::shredOsStillGetsASaveLocation() {
    LinuxISODownloader downloader;
    LinuxISODownloadDialog dialog(&downloader);
    auto* list = dialog.findChild<QListWidget*>();
    QVERIFY(list != nullptr);
    int shredos_row = -1;
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->text().startsWith(QStringLiteral("ShredOS"))) {
            shredos_row = row;
        }
    }
    QVERIFY(shredos_row >= 0);
    list->setCurrentRow(shredos_row);

    QString save_path;
    for (const auto* edit : dialog.findChildren<QLineEdit*>()) {
        if (edit->placeholderText().startsWith(QStringLiteral("Select a distribution"))) {
            save_path = edit->text();
        }
    }
    QVERIFY2(save_path.endsWith(QStringLiteral("shredos.iso")), qPrintable(save_path));
}

// The first stage of a compressed image reads the file; nothing has gone to any drive yet.
void ImageFlasherPanelTests::readingACompressedImageIsNotShownAsWriting() {
    auto* coordinator = m_panel->findChild<FlashCoordinator*>();
    QVERIFY(coordinator != nullptr);
    FlashProgress progress;
    progress.state = FlashState::Decompressing;
    progress.bytesWritten = 512;
    progress.totalBytes = 1024;
    progress.percentage = 50.0;
    Q_EMIT coordinator->progressUpdated(progress);

    const auto labels = m_panel->findChildren<QLabel*>();
    const auto shows = [&labels](const QString& start) {
        return std::ranges::any_of(labels, [&start](const QLabel* label) {
            return label->text().startsWith(start);
        });
    };
    QVERIFY(shows(QStringLiteral("Read: ")));
    QVERIFY(!shows(QStringLiteral("Written: ")));
}

void ImageFlasherPanelTests::initTestCase() {
    // The panel always starts its DriveScanner -- smoke and audit modes included -- so tests
    // that place their own row wait for that read-only start-up scan to land first (see
    // selectOneDrive). With no periodic rescan, nothing else repopulates the list afterwards.
    QVERIFY2(QFile::exists(QStringLiteral(":/icons/icons/panel_image_flasher.svg")),
             "Image Flasher panel test must link its production icon resources");
    QVERIFY(m_dir.isValid());
}

void ImageFlasherPanelTests::init() {
    m_panel = std::make_unique<ImageFlasherPanel>();
}

void ImageFlasherPanelTests::cleanup() {
    m_panel.reset();
    QCoreApplication::instance()->setProperty("sakAccessibilityAudit", QVariant());
    QCoreApplication::instance()->setProperty("sakStartupSmokeTest", QVariant());
}

QPushButton* ImageFlasherPanelTests::flashButton() const {
    const auto buttons = m_panel->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == QLatin1String("Flash Drive")) {
            return button;
        }
    }
    return nullptr;
}

QListWidget* ImageFlasherPanelTests::driveList() const {
    const auto lists = m_panel->findChildren<QListWidget*>();
    for (QListWidget* list : lists) {
        if (list->accessibleName() == QLatin1String("Target Drive List")) {
            return list;
        }
    }
    return nullptr;
}

void ImageFlasherPanelTests::selectOneDrive() {
    // Let the panel's real start-up scan land before planting a row: its result rebuilds the
    // list, which would otherwise wipe the row (and its selection) mid-test.
    auto* scanner = m_panel->findChild<DriveScanner*>();
    QVERIFY(scanner);
    QTRY_VERIFY_WITH_TIMEOUT(!scanner->isScanning(), kStartupScanTimeoutMs);
    QListWidget* list = driveList();
    QVERIFY(list);
    // Drop the host's real drives the scan listed: the only selectable row in this suite is the
    // fake one below, so no test can ever arm a flash at a real disk.
    list->clear();
    auto* item = new QListWidgetItem(QStringLiteral("Test Drive - 8 GB (%1)").arg(kFakeDevicePath),
                                     list);
    item->setData(Qt::UserRole, QString::fromLatin1(kFakeDevicePath));
    item->setData(Qt::UserRole + 1, QStringLiteral("Test Drive|8000000000|USB|512"));
    // Selecting emits itemSelectionChanged, which is what refreshes the buttons.
    item->setSelected(true);
}

void ImageFlasherPanelTests::armMessageBoxDismisser(QString* seen_text) {
    auto* dismiss = new QTimer(this);
    dismiss->setInterval(kModalPollIntervalMs);
    connect(dismiss, &QTimer::timeout, this, [dismiss, seen_text]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            dismiss->stop();
            dismiss->deleteLater();
            if (seen_text != nullptr) {
                *seen_text = box->text();
            }
            box->accept();
        }
    });
    dismiss->start();
}

QString ImageFlasherPanelTests::writeImage(const QString& name, const QByteArray& bytes) {
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    const qint64 written = file.write(bytes);
    file.close();
    return written == bytes.size() ? path : QString();
}

// ============================================================================
// Flash button gating
// ============================================================================

// Smoke and audit modes exist to exercise the start-up work users get. They used to skip the
// drive scanner, so neither gate ever ran it -- and the scanner then rescanned every 5 s.
void ImageFlasherPanelTests::smokeAndAuditModesRunTheStartupScanUsersGet() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    for (const char* mode : {"sakStartupSmokeTest", "sakAccessibilityAudit"}) {
        m_panel.reset();
        QCoreApplication::instance()->setProperty("sakAccessibilityAudit", QVariant());
        QCoreApplication::instance()->setProperty("sakStartupSmokeTest", QVariant());
        QCoreApplication::instance()->setProperty(mode, true);
        m_panel = std::make_unique<ImageFlasherPanel>();

        auto* scanner = m_panel->findChild<DriveScanner*>();
        QVERIFY2(scanner, mode);
        // Every Windows host has at least its system disk, so a scan that ran finds one.
        QTRY_VERIFY2_WITH_TIMEOUT(!scanner->getDrives().isEmpty(), mode, kStartupScanTimeoutMs);
        for (const auto* timer : scanner->findChildren<QTimer*>()) {
            QVERIFY2(!timer->isActive() || timer->isSingleShot(), mode);
        }
    }
}

void ImageFlasherPanelTests::flashIsDisabledWithNothingSelected() {
    QPushButton* flash = flashButton();
    QVERIFY(flash);
    QVERIFY2(!flash->isEnabled(), "nothing selected must leave Flash disabled");
    // The one Step 1 fact only updateNavigationButtons establishes: the destructive button
    // is not on the image-selection page at all (image_flasher_panel.cpp:1037). The button is
    // constructed disabled (:285), so isEnabled() alone survives deleting the gate call at :229.
    // isVisibleTo() reads the explicit hide flag without show()-ing the panel.
    QVERIFY2(!flash->isVisibleTo(m_panel.get()),
             "Flash must not be shown on the image-selection page");
}

// THE REGRESSION. The gate used to read the drive list alone, so this passed with
// no image and the next click opened the destructive confirmation.
void ImageFlasherPanelTests::flashStaysDisabledWithADriveButNoImage() {
    selectOneDrive();
    QPushButton* flash = flashButton();
    QVERIFY(flash);
    QVERIFY2(!flash->isEnabled(), "Flash must require an image, not just a target drive");
}

void ImageFlasherPanelTests::flashEnablesOnlyWithBothAnImageAndADrive() {
    const QString image = writeImage("payload.img", QByteArray(4096, '\x5A'));
    QVERIFY(!image.isEmpty());

    // An image on its own is not enough.
    QVERIFY(m_panel->loadImageFile(image));
    QPushButton* flash = flashButton();
    QVERIFY(flash);
    QVERIFY(!flash->isEnabled());

    // ...but it must still open the road to drive selection. Next is gated on the selected
    // image path alone, and nothing in this suite touched it -- so hard-wiring that gate to
    // disabled, which kills Step 1 -> Step 2 for every user, would ship green.
    QPushButton* next = findNextButton(m_panel.get());
    QVERIFY(next);
    QVERIFY2(next->isEnabled(), "a selected image must enable Next into drive selection");

    // Image + drive is.
    selectOneDrive();
    QVERIFY(flash->isEnabled());

    // The gate reads the SELECTION, not the presence of a row. Deselecting the only target
    // must shut Flash again: nothing else in this file changes a selection twice, so gating on
    // m_driveListWidget->count() -- the original defect -- or dropping the m_selectedDrives
    // clear in onDriveSelectionChanged would leave a deselected drive a live write target.
    QListWidget* list = driveList();
    QVERIFY(list);
    QCOMPARE(list->count(), 1);
    list->item(0)->setSelected(false);
    QVERIFY2(!flash->isEnabled(), "a deselected drive is not a target drive");

    // Reachability is the other half of the same gate, and no assertion here ever looked at it:
    // Flash is deliberately hidden on Step 1 and must actually appear on drive selection.
    QVERIFY2(!flash->isVisibleTo(m_panel.get()), "Flash must not be offered on the image step");
    list->item(0)->setSelected(true);
    next->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(flash->isVisibleTo(m_panel.get()), "Flash must be reachable on drive selection");
    QVERIFY(flash->isEnabled());
}

// ============================================================================
// loadImageFile refusals
//
// loadImageFile is public and documented for drag-drop and the command line, so
// it takes whatever it is handed. It used to Q_ASSERT a non-empty path, which
// aborts a Debug build on input a Release build handles.
// ============================================================================

void ImageFlasherPanelTests::loadImageFileRefusesAnEmptyPath() {
    armMessageBoxDismisser();
    QVERIFY(!m_panel->loadImageFile(QString()));
    QVERIFY(!flashButton()->isEnabled());
}

void ImageFlasherPanelTests::loadImageFileRefusesAMissingFile() {
    // loadImageFile is the drag-drop and command-line entry point, so "returned false" and
    // "told the user why" are two different behaviours: a silent refusal is a drop that
    // appears to do nothing. Capture the refusal the user actually sees.
    QString seenText;
    std::unique_ptr<QTimer> dismiss;
    const auto armCapturingDismisser = [&seenText, &dismiss]() {
        seenText.clear();
        dismiss = std::make_unique<QTimer>();
        dismiss->setInterval(kModalPollIntervalMs);
        QTimer* poll = dismiss.get();
        connect(poll, &QTimer::timeout, poll, [poll, &seenText]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                poll->stop();
                seenText = box->text();
                box->accept();
            }
        });
        dismiss->start();
    };

    const QString missing = m_dir.filePath("does_not_exist.img");
    armCapturingDismisser();
    QVERIFY(!m_panel->loadImageFile(missing));
    QVERIFY2(seenText.contains(QStringLiteral("Not a readable image file")),
             "a missing path must be refused out loud, not silently");
    QVERIFY2(seenText.contains(missing), "the refusal must name the path it refused");
    QVERIFY(!flashButton()->isEnabled());
    QPushButton* next = findNextButton(m_panel.get());
    QVERIFY(next);
    QVERIFY2(!next->isEnabled(), "a refused image must not open drive selection");

    // The other arm of the same guard: a directory exists but is not a file. Its size is 0
    // on Windows, so the empty-file guard would refuse it too -- with the wrong reason --
    // which makes the message the only witness that the isFile() arm is still there.
    QVERIFY(QDir(m_dir.path()).mkpath(QStringLiteral("a_directory.img")));
    const QString directory = m_dir.filePath("a_directory.img");
    armCapturingDismisser();
    QVERIFY(!m_panel->loadImageFile(directory));
    QVERIFY2(seenText.contains(QStringLiteral("Not a readable image file")),
             "a directory must be refused as not-a-file, not as an empty file");
    QVERIFY(!flashButton()->isEnabled());
}

void ImageFlasherPanelTests::loadImageFileRefusesAnEmptyFile() {
    const QString image = writeImage("empty.img", QByteArray());
    QVERIFY(!image.isEmpty());
    armMessageBoxDismisser();
    QVERIFY(!m_panel->loadImageFile(image));
    QVERIFY(!flashButton()->isEnabled());
}

// The picker listed these and the flash then refused them after a drive had been chosen. The
// refusal now comes at selection and says what to do, whatever the file's name claims.
void ImageFlasherPanelTests::loadImageFileRefusesAZipArchive() {
    const QString image = writeImage("archive.img", QByteArray("PK\x03\x04 not a disk", 16));
    QVERIFY(!image.isEmpty());
    QString seen;
    armMessageBoxDismisser(&seen);
    QVERIFY(!m_panel->loadImageFile(image));
    QVERIFY2(seen.contains(QStringLiteral("extract the disk image")), qPrintable(seen));
    QVERIFY(!flashButton()->isEnabled());
}

void ImageFlasherPanelTests::loadImageFileRefusesAnAppleDiskImage() {
    QByteArray bytes(2048, '\0');
    QByteArray trailer(512, '\0');
    trailer.replace(0, 4, "koly");
    trailer[11] = 0;
    trailer[10] = 2;  // big-endian header size 512
    bytes.replace(bytes.size() - trailer.size(), trailer.size(), trailer);
    const QString image = writeImage("installer.dmg", bytes);
    QVERIFY(!image.isEmpty());
    QString seen;
    armMessageBoxDismisser(&seen);
    QVERIFY(!m_panel->loadImageFile(image));
    QVERIFY2(seen.contains(QStringLiteral(".dmg")), qPrintable(seen));
    QVERIFY(!flashButton()->isEnabled());
}

void ImageFlasherPanelTests::imagePickerOffersOnlyWhatFlashes() {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    QPushButton* select = nullptr;
    const auto buttons = m_panel->findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Select Image File")) {
            select = button;
        }
    }
    QVERIFY(select != nullptr);

    QStringList filters;
    QTimer poll;
    poll.setInterval(kModalPollIntervalMs);
    connect(&poll, &QTimer::timeout, this, [&filters, &poll]() {
        if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
            filters = dialog->nameFilters();
            poll.stop();
            dialog->reject();
        }
    });
    poll.start();
    select->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);

    QVERIFY(!filters.isEmpty());
    const QString disk_images = filters.first();
    QVERIFY2(disk_images.contains(QStringLiteral("*.iso")), qPrintable(disk_images));
    QVERIFY2(!disk_images.contains(QStringLiteral("*.zip")), qPrintable(disk_images));
    QVERIFY2(!disk_images.contains(QStringLiteral("*.dmg")), qPrintable(disk_images));
}

// The shape of the download-flow bug: an image that fails validation selects
// nothing, so a drive picked afterwards must not enable Flash.
void ImageFlasherPanelTests::aRejectedImageLeavesFlashDisabled() {
    const QString image = writeImage("rejected.img", QByteArray());
    QVERIFY(!image.isEmpty());
    armMessageBoxDismisser();
    QVERIFY(!m_panel->loadImageFile(image));

    selectOneDrive();
    QVERIFY2(!flashButton()->isEnabled(), "a rejected image is not a selected image");
    // The other half of the same fail-closed contract, and the half the download-flow bug this
    // test is named for actually broke: navigation must stay shut too.
    QPushButton* next = findNextButton(m_panel.get());
    QVERIFY(next);
    QVERIFY2(!next->isEnabled(), "a rejected image must not open drive selection");
}

// The fourth refusal guard, and the only one no test in this file reached. Every image the
// fixture writes is a .img, which the extension table resolves to ImageFormat::IMG, so the
// unknown-format confirmation at image_flasher_panel.cpp:1074-1083 was never executed: deleting
// that block outright -- letting an unrecognised file become a raw-disk write source with no
// prompt at all -- left all seven other tests green.
//
// The technician is asked here, and anything short of Yes refuses (fail closed). The No button is
// clicked explicitly rather than closing the box with accept(): QMessageBox::done() maps a
// programmatic result code back to a button in Qt 6.5+, so a bare accept() is not a dependable
// way to express "declined".
void ImageFlasherPanelTests::loadImageFileRefusesAnUnknownFormatWhenDeclined() {
    // .bin is in neither the extension table nor the compound-suffix table, so detectFormat
    // reports Unknown and the confirmation is reached. If .bin is ever added to that table this
    // test fails loudly on the first assertion rather than silently passing.
    const QString image = writeImage("mystery.bin", QByteArray(4096, '\x5A'));
    QVERIFY(!image.isEmpty());

    auto* decline = new QTimer(this);
    decline->setInterval(kModalPollIntervalMs);
    connect(decline, &QTimer::timeout, this, [decline]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box == nullptr) {
            return;
        }
        QAbstractButton* no = box->button(QMessageBox::No);
        if (no == nullptr) {
            // Not the Unknown Format question; close it so the test cannot hang.
            decline->stop();
            decline->deleteLater();
            box->reject();
            return;
        }
        decline->stop();
        decline->deleteLater();
        no->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    });
    decline->start();

    QVERIFY2(!m_panel->loadImageFile(image),
             "an unrecognised image format must be refused unless the technician confirms");

    selectOneDrive();
    QVERIFY2(!flashButton()->isEnabled(),
             "a declined unknown-format image is not a selected image");
    QPushButton* next = findNextButton(m_panel.get());
    QVERIFY(next);
    QVERIFY2(!next->isEnabled(), "a declined unknown-format image must not open drive selection");
}


// Review 3 (X18): with the first build picked, Edition started on "Microsoft Store Inbox Apps",
// which is not an edition. The list starts on Pro, else Home, else on nothing.
void ImageFlasherPanelTests::windowsIsoEditionStartsOnAnEdition() {
    QCOMPARE(WindowsISODownloadDialog::defaultEditionIndex(
                 {QStringLiteral("APP"), QStringLiteral("CORE"), QStringLiteral("PROFESSIONAL")}),
             2);
    QCOMPARE(WindowsISODownloadDialog::defaultEditionIndex(
                 {QStringLiteral("APP"), QStringLiteral("CORE")}),
             1);
    QCOMPARE(WindowsISODownloadDialog::defaultEditionIndex({QStringLiteral("APP")}), -1);
    QCOMPARE(WindowsISODownloadDialog::defaultEditionIndex({}), -1);
}

QTEST_MAIN(ImageFlasherPanelTests)
#include "test_image_flasher_panel.moc"
