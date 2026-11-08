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
#include "sak/detachable_log_window.h"
#include "sak/drive_scanner.h"
#include "sak/flash_coordinator.h"
#include "sak/image_flasher_panel.h"
#include "sak/image_flasher_settings_dialog.h"
#include "sak/layout_constants.h"
#include "sak/linux_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"
#include "sak/style_constants.h"
#include "sak/windows11_theme.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"
#include "sak/windows_usb_creator.h"

#include "../support/test_hardware_guard.h"

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include <algorithm>
#include <cmath>

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

/// The colour a phase line's style sheet gives its text ("color: #rrggbb;").
QString phaseLineColour(const QString& sheet) {
    static const QRegularExpression kColour(
        QStringLiteral(R"((?<![-\w])color:\s*(#[0-9a-fA-F]{6}))"));
    return kColour.match(sheet).captured(1);
}

double linearChannel(int value) {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// WCAG 2.x contrast of two opaque colours.
double contrastRatio(const QColor& a, const QColor& b) {
    const auto luminance = [](const QColor& c) {
        return (0.2126 * linearChannel(c.red())) + (0.7152 * linearChannel(c.green())) +
               (0.0722 * linearChannel(c.blue()));
    };
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// The Step 1 -> Step 2 navigation button. Every assertion in this file watched the Flash
/// button only, so the Next gate -- which decides whether a user can reach drive selection at
/// all -- had no coverage in either direction.
/// Answers any message box that opens while it is alive, and keeps what the box said. Goes away
/// with the test, so nothing is left running to answer a later test's dialog.
class ModalAnswerer {
public:
    QString seen;
    /// Every box answered, in order.
    QStringList titles;

    /// @p answer is clicked when the box has it; otherwise the box is accepted.
    explicit ModalAnswerer(QMessageBox::StandardButton answer = QMessageBox::NoButton) {
        m_timer.setInterval(kModalPollIntervalMs);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this, answer]() {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                if (seen.isEmpty()) {
                    seen = box->text();
                }
                titles.append(box->windowTitle());
                if (QAbstractButton* button = box->button(answer)) {
                    button->click();
                } else {
                    box->accept();
                }
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
    // Review 4 (Y13): Settings, Back and Flash! sat in the Log switch's row, and the target list
    // ran on under them with no bottom edge, its page as tall as the tallest page. The steps have
    // a row of their own, and the list ends inside the window.
    void theTargetListEndsAboveItsOwnActionRow() {
        sak::ImageFlasherPanel& panel = *m_panel;
        panel.resize(1240, 760);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        panel.m_stackedWidget->setCurrentIndex(1);  // Step 2: Select Target Drives
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QWidget* list = panel.m_driveListWidget;
        QVERIFY(list != nullptr && list->isVisible());
        QCOMPARE(list->visibleRegion().boundingRect().height(), list->height());
        const int toggle_top = panel.m_logToggle->mapTo(&panel, QPoint(0, 0)).y();
        const int flash_bottom =
            panel.m_flashButton->mapTo(&panel, QPoint(0, panel.m_flashButton->height())).y();
        QVERIFY2(flash_bottom <= toggle_top,
                 qPrintable(QStringLiteral("Flash! ends at y=%1, the Log switch starts at y=%2")
                                .arg(flash_bottom)
                                .arg(toggle_top)));
        QCOMPARE(panel.m_backButton->styleSheet(), sak::ui::kSecondaryButtonStyle);
        QCOMPARE(panel.m_settingsButton->styleSheet(), sak::ui::kSecondaryButtonStyle);
        // The stack asks for the room of the page it shows, not of its tallest page: as tall as
        // the tallest, the target list's page ran on below the window.
        QStackedWidget* stack = panel.m_stackedWidget;
        const int shown = stack->currentIndex();
        const auto back = qScopeGuard([stack, shown]() { stack->setCurrentIndex(shown); });
        for (int index = 0; index < stack->count(); ++index) {
            stack->setCurrentIndex(index);
            QCoreApplication::processEvents();
            QVERIFY2(stack->sizeHint().height() == stack->currentWidget()->sizeHint().height(),
                     qPrintable(QStringLiteral("page %1: the stack asks for %2 px, the page %3")
                                    .arg(index)
                                    .arg(stack->sizeHint().height())
                                    .arg(stack->currentWidget()->sizeHint().height())));
        }
    }
    // Review 3 (S22): a raw flash read "Ready" in the status bar beside its bar at 95-100%, and a
    // finished flash left the bar at 95%.
    void aRawFlashSaysItsStateAndEndsItsBar() {
        sak::ImageFlasherPanel& panel = *m_panel;
        QSignalSpy status(&panel, &sak::ImageFlasherPanel::statusMessage);
        QSignalSpy progress(&panel, &sak::ImageFlasherPanel::progressUpdate);
        panel.onFlashStateChanged(sak::FlashState::Flashing, QStringLiteral("Writing image"));
        QVERIFY(!status.isEmpty());
        QCOMPARE(status.last().at(0).toString(), QStringLiteral("Image Flasher: Writing image"));
        sak::FlashResult result;
        result.success = true;
        result.successfulDrives = {QStringLiteral("PhysicalDrive5")};
        panel.onFlashCompleted(result);
        QVERIFY(!progress.isEmpty());
        QCOMPARE(progress.last().at(0).toInt(), progress.last().at(1).toInt());
    }
    // Review 3 (X10): Restore Defaults was a second primary button beside OK.
    void flasherSettingsHaveOnePrimaryButton() {
        ::ImageFlasherSettingsDialog dialog;
        auto* box = dialog.findChild<QDialogButtonBox*>();
        QVERIFY(box != nullptr);
        QCOMPARE(box->button(QDialogButtonBox::RestoreDefaults)->styleSheet(),
                 sak::ui::kSecondaryButtonStyle);
    }
    // Review 4 (Y18): the settings' fields started in two columns (x=297 for the spin boxes,
    // x=210 for the threshold, x=191 for the combo), and "Buffer Size (MB): 64 MB" gave the unit
    // twice.
    void flasherSettingsHaveOneFieldColumn() {
        ::ImageFlasherSettingsDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QHash<QString, int> left;
        for (auto* field : dialog.findChildren<QWidget*>()) {
            if (!field->accessibleName().isEmpty()) {
                left.insert(field->accessibleName(), field->mapTo(&dialog, QPoint(0, 0)).x());
            }
        }
        for (const char* name :
             {"Verification mode", "Buffer size", "Concurrent writes", "Large drive threshold"}) {
            QVERIFY2(left.contains(QString::fromLatin1(name)), name);
            QVERIFY2(left.value(QString::fromLatin1(name)) ==
                         left.value(QStringLiteral("Buffer size")),
                     qPrintable(QStringLiteral("%1 at x=%2, Buffer size at x=%3")
                                    .arg(QLatin1String(name))
                                    .arg(left.value(QString::fromLatin1(name)))
                                    .arg(left.value(QStringLiteral("Buffer size")))));
        }
        for (const auto* label : dialog.findChildren<QLabel*>()) {
            QVERIFY2(!label->text().contains(QStringLiteral("(MB)")), qPrintable(label->text()));
        }
    }
    // Review 4 (Y9): before a build was picked, the Builds list showed one and a half rows, the
    // second cut through its glyphs, and it grew once one was picked. Four whole rows always.
    void theBuildListShowsWholeRowsInEveryState() {
        // In the theme, whose item padding makes a row as tall as it is on screen.
        sak::ui::applyWindows11Theme(*qApp, sak::ui::AppThemeMode::Light);
        WindowsISODownloader downloader;
        WindowsISODownloadDialog dialog(&downloader);
        QListWidget* builds = nullptr;
        for (auto* list : dialog.findChildren<QListWidget*>()) {
            if (list->accessibleName() == QStringLiteral("Builds")) {
                builds = list;
            }
        }
        QVERIFY(builds != nullptr);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // As short as the dialog can be made, as on a small screen.
        dialog.resize(dialog.width(), 200);
        QCoreApplication::processEvents();
        const int empty_height = builds->height();
        for (int row = 0; row < 12; ++row) {
            builds->addItem(QStringLiteral("Windows 11, version 25H2 (26200.%1)").arg(row));
        }
        builds->setCurrentRow(0);
        QCoreApplication::processEvents();
        QCOMPARE(builds->height(), empty_height);
        const int rows_shown = builds->viewport()->height() / builds->sizeHintForRow(0);
        QVERIFY2(rows_shown >= 3,
                 qPrintable(QStringLiteral("%1 whole rows in %2 px")
                                .arg(rows_shown)
                                .arg(builds->viewport()->height())));
    }
    // Review 5 (RG9): with four whole rows of builds the dialog was 933 px tall, taller than a
    // 1600x900 or 1366x768 screen, and its buttons ran off the foot of the screen. It fits the
    // screen it opens on (the offscreen platform's is 800x600), its groups scroll, and its
    // buttons are whole inside it.
    void theIsoDialogFitsItsScreenWithItsButtons() {
        sak::ui::applyWindows11Theme(*qApp, sak::ui::AppThemeMode::Light);
        WindowsISODownloader downloader;
        WindowsISODownloadDialog dialog(&downloader);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCoreApplication::processEvents();
        const QRect room = dialog.screen()->availableGeometry();
        QVERIFY2(dialog.height() + sak::kDialogWindowFrameAllowance <= room.height(),
                 qPrintable(QStringLiteral("%1 px tall on a screen with %2 px free")
                                .arg(dialog.height())
                                .arg(room.height())));
        int buttons = 0;
        for (const auto* button : dialog.findChildren<QPushButton*>()) {
            const QString text = button->text();
            if (text != QStringLiteral("Close") && text != QStringLiteral("Cancel") &&
                !text.startsWith(QStringLiteral("Download"))) {
                continue;
            }
            ++buttons;
            const int bottom = button->mapTo(&dialog, QPoint(0, button->height())).y();
            QVERIFY2(bottom <= dialog.height(),
                     qPrintable(QStringLiteral("%1 ends at y=%2 in a dialog %3 px tall")
                                    .arg(text)
                                    .arg(bottom)
                                    .arg(dialog.height())));
        }
        QCOMPARE(buttons, 3);
        // It opens as tall as its groups need, or as tall as the screen lets it: left at its own
        // size hint, the groups scrolled in a short window on a screen with room to spare.
        auto* form = dialog.findChild<QScrollArea*>();
        QVERIFY(form != nullptr);
        const int screen_room = room.height() - sak::kDialogWindowFrameAllowance;
        QVERIFY2(form->verticalScrollBar()->maximum() == 0 || dialog.height() == screen_room,
                 qPrintable(QStringLiteral("%1 px tall and scrolling, with %2 px of room")
                                .arg(dialog.height())
                                .arg(screen_room)));
        // With room for all of it, as on a 1080-line screen, nothing scrolls.
        constexpr int kTallRoom = 1040;
        dialog.fitToHeight(kTallRoom);
        QCoreApplication::processEvents();
        QCOMPARE(form->verticalScrollBar()->maximum(), 0);
    }
    // Review 4 (Y12): the raw flash's "ALL DATA ... PERMANENTLY ERASED" confirmation offered a
    // blue Yes, while Batch Uninstall's destructive action is red. Yes is the danger tone, and No
    // stays the default.
    void theEraseConfirmationsYesIsTheDangerTone() {
        const auto box = sak::ImageFlasherPanel::eraseConfirmation(
            nullptr, QStringLiteral("ALL DATA on the target drive will be PERMANENTLY ERASED."));
        QVERIFY(box != nullptr);
        QCOMPARE(box->windowTitle(), QStringLiteral("Confirm Flash - Data Loss Warning"));
        QCOMPARE(box->button(QMessageBox::Yes)->styleSheet(), sak::ui::kDangerButtonStyle);
        QCOMPARE(box->defaultButton(), box->button(QMessageBox::No));
        // Review 5 asked to see No as the default: Enter, as a technician presses it without
        // reading, answers No and erases nothing.
        box->show();
        QVERIFY(QTest::qWaitForWindowExposed(box.get()));
        QTest::keyClick(box.get(), Qt::Key_Return);
        QCOMPARE(box->clickedButton(), box->button(QMessageBox::No));
    }
    // Review 3 (X19): the Windows ISO download bar read "0%" with 0.09 of 8.70 GB down.
    void theWindowsIsoDownloadBarShowsTenthsBelowOnePercent() {
        WindowsISODownloader downloader;
        WindowsISODownloadDialog dialog(&downloader);
        QProgressBar* bar = nullptr;
        for (auto* candidate : dialog.findChildren<QProgressBar*>()) {
            if (candidate->accessibleName() == QStringLiteral("Download progress")) {
                bar = candidate;
            }
        }
        QVERIFY(bar != nullptr);
        constexpr qint64 kMegabyte = 1024LL * 1024;
        constexpr qint64 kGigabyte = 1024LL * kMegabyte;
        Q_EMIT downloader.downloadBytesUpdated(50 * kMegabyte, (87 * kGigabyte) / 10);
        QCOMPARE(bar->text(), QStringLiteral("0.5%"));
    }
    // Review 4 (Y2): "Downloading Windows UUP files..." was emerald #059669 in both themes, 3.60:1
    // on the light dialog and 3.89:1 on the dark one. Every phase line of both ISO dialogs reaches
    // AA on its dialog's surfaces in both themes.
    void everyIsoPhaseLineReadsInBothThemes() {
        const auto restore = qScopeGuard(
            []() { sak::ui::applyWindows11Theme(*qApp, sak::ui::AppThemeMode::Light); });
        for (const auto mode : {sak::ui::AppThemeMode::Light, sak::ui::AppThemeMode::Dark}) {
            sak::ui::applyWindows11Theme(*qApp, mode);
            const bool dark = mode == sak::ui::AppThemeMode::Dark;
            const QList<const char*> surfaces =
                dark ? QList<const char*>{sak::ui::kColorDarkBgSurface, sak::ui::kColorDarkBgPanel}
                     : QList<const char*>{sak::ui::kColorBgSurface, sak::ui::kColorBgWhite};
            const auto check = [&](QWidget& dialog, const QString& description) {
                QLabel* line = nullptr;
                for (auto* label : dialog.findChildren<QLabel*>()) {
                    if (label->text().endsWith(description)) {
                        line = label;
                    }
                }
                QVERIFY2(line != nullptr, qPrintable(description));
                const QColor ink(phaseLineColour(line->styleSheet()));
                QVERIFY2(ink.isValid(), qPrintable(line->styleSheet()));
                for (const char* surface : surfaces) {
                    const double ratio = contrastRatio(ink, QColor(QString::fromLatin1(surface)));
                    QVERIFY2(ratio >= 4.5,
                             qPrintable(QStringLiteral("%1 in %2 on %3: %4:1")
                                            .arg(description, ink.name(), QLatin1String(surface))
                                            .arg(ratio, 0, 'f', 2)));
                }
            };
            WindowsISODownloader windows_downloader;
            WindowsISODownloadDialog windows_dialog(&windows_downloader);
            for (const auto phase : {UupIsoBuilder::Phase::PreparingDownload,
                                     UupIsoBuilder::Phase::DownloadingFiles,
                                     UupIsoBuilder::Phase::ConvertingToISO,
                                     UupIsoBuilder::Phase::Completed,
                                     UupIsoBuilder::Phase::Failed}) {
                const QString description =
                    QStringLiteral("Windows phase %1").arg(static_cast<int>(phase));
                Q_EMIT windows_downloader.phaseChanged(phase, description);
                check(windows_dialog, description);
            }
            LinuxISODownloader linux_downloader;
            LinuxISODownloadDialog linux_dialog(&linux_downloader);
            for (const auto phase : {LinuxISODownloader::Phase::ResolvingVersion,
                                     LinuxISODownloader::Phase::Downloading,
                                     LinuxISODownloader::Phase::VerifyingChecksum,
                                     LinuxISODownloader::Phase::Completed,
                                     LinuxISODownloader::Phase::Failed}) {
                const QString description =
                    QStringLiteral("Linux phase %1").arg(static_cast<int>(phase));
                Q_EMIT linux_downloader.phaseChanged(phase, description);
                check(linux_dialog, description);
            }
        }
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
    void aWriteOffersCancelAndNoBack();
    void cancelWaitsForTheWriterAndSaysWhatTheDriveHolds();
    void aCancelledRawRunSaysWhetherItHadWritten();

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

// Review 5 (Z1): a raw flash and the Windows USB wait showed no Cancel and a disabled Back. The
// buttons were chosen for drive selection, before the switch to the progress page, so a technician
// who had picked the wrong drive could not stop the write from the page. Z3: the bar showed no work
// when the first step's message went out, so the message left the status bar after 5 s.
void ImageFlasherPanelTests::aWriteOffersCancelAndNoBack() {
    const QString image = writeImage("payload.img", QByteArray(4096, '\x5A'));
    QVERIFY(m_panel->loadImageFile(image));
    // Two rows naming one disk: the coordinator says it is validating, then refuses them as one
    // disk named twice, before it opens anything.
    m_panel->m_selectedDrives = {QString::fromLatin1(kFakeDevicePath),
                                 QString::fromLatin1(kFakeDevicePath)};
    bool seen = false;
    bool on_progress_page = false;
    bool cancel_shown = false;
    bool cancel_enabled = false;
    bool back_shown = true;
    connect(m_panel->m_flashCoordinator.get(),
            &FlashCoordinator::stateChanged,
            this,
            [&](FlashState /*state*/, const QString& /*message*/) {
                if (seen) {
                    return;
                }
                seen = true;
                on_progress_page = m_panel->m_stackedWidget->currentWidget() ==
                                   m_panel->m_flashProgressPage;
                cancel_shown = m_panel->m_cancelButton->isVisibleTo(m_panel.get());
                cancel_enabled = m_panel->m_cancelButton->isEnabled();
                back_shown = m_panel->m_backButton->isVisibleTo(m_panel.get());
            });
    QStringList order;
    connect(m_panel.get(), &ImageFlasherPanel::progressUpdate, this, [&order](int value, int max) {
        order.append(QStringLiteral("bar %1/%2").arg(value).arg(max));
    });
    connect(m_panel.get(),
            &ImageFlasherPanel::statusMessage,
            this,
            [&order](const QString& message, int /*timeout*/) { order.append(message); });
    ModalAnswerer refusal;

    m_panel->beginConfirmedFlash(false);

    QVERIFY2(seen, "the coordinator never reported a state");
    QVERIFY(on_progress_page);
    QVERIFY2(cancel_shown, "no Cancel on the progress page while the write starts");
    QVERIFY(cancel_enabled);
    QVERIFY2(!back_shown, "Back is shown on the progress page while the write starts");
    QVERIFY2(!order.isEmpty() && order.first() == QStringLiteral("bar 0/100"),
             qPrintable(order.join(QStringLiteral(" | "))));
}

// Z1: Cancel stops the writer; the page says the write is stopping until the writer reports, then
// what the drive holds. A Windows USB creation stopped before it erased anything left the drive as
// it was, and is not a failure dialog.
void ImageFlasherPanelTests::cancelWaitsForTheWriterAndSaysWhatTheDriveHolds() {
    const QString image = writeImage("payload.img", QByteArray(4096, '\x5A'));
    QVERIFY(m_panel->loadImageFile(image));
    selectOneDrive();
    auto* creator = new WindowsUSBCreator();
    auto* thread = new QThread();
    m_panel->m_windowsUsbCreator = creator;
    m_panel->m_windowsUsbThread = thread;
    m_panel->connectWindowsUSBCreatorSignals(creator, thread);
    // An idle thread stands in for a creator still inside a step that has not reached a cancel
    // point.
    thread->start();
    const auto stop = qScopeGuard([thread]() {
        thread->quit();
        thread->wait();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    });
    m_panel->m_isFlashing = true;
    m_panel->m_stackedWidget->setCurrentWidget(m_panel->m_flashProgressPage);
    m_panel->updateNavigationButtons();
    QVERIFY(m_panel->m_cancelButton->isEnabled());
    ModalAnswerer yes(QMessageBox::Yes);

    m_panel->onCancelClicked();

    QCOMPARE(yes.titles, QStringList{QStringLiteral("Cancel Flash")});
    QVERIFY2(m_panel->m_stackedWidget->currentWidget() == m_panel->m_flashProgressPage,
             "the page left the write before the writer stopped");
    QCOMPARE(m_panel->m_flashStateLabel->text(), QStringLiteral("Stopping the write..."));
    QVERIFY2(!m_panel->m_cancelButton->isEnabled(), "Cancel can be asked twice");
    QVERIFY(m_panel->m_isFlashing);

    Q_EMIT creator->failed(QStringLiteral("Operation cancelled"));

    QCOMPARE(yes.titles, QStringList{QStringLiteral("Cancel Flash")});
    QVERIFY(m_panel->m_stackedWidget->currentWidget() == m_panel->m_completionPage);
    QCOMPARE(m_panel->m_completionGroup->title(), QStringLiteral("Flash Cancelled"));
    QVERIFY2(m_panel->m_completionMessageLabel->text().endsWith(QStringLiteral("Flash cancelled")),
             qPrintable(m_panel->m_completionMessageLabel->text()));
    QCOMPARE(m_panel->m_completionDetailsLabel->text(),
             QStringLiteral("Nothing was written: the target drive is as it was."));
    QVERIFY(!m_panel->m_isFlashing);
}

// Z1: a raw run stopped mid-write reports the bytes its writer wrote, and the page says the drive
// is not usable as it is; one stopped before its first block says the drive is as it was.
void ImageFlasherPanelTests::aCancelledRawRunSaysWhetherItHadWritten() {
    for (const qint64 written : {qint64{0}, qint64{64} * 1024 * 1024}) {
        m_panel->m_isFlashing = true;
        m_panel->m_cancelRequested = true;
        FlashResult result;
        result.success = false;
        result.bytesWritten = written;
        result.failedDrives = {QString::fromLatin1(kFakeDevicePath)};
        m_panel->onFlashCompleted(result);
        QCOMPARE(m_panel->m_completionGroup->title(), QStringLiteral("Flash Cancelled"));
        const QString details = m_panel->m_completionDetailsLabel->text();
        const QString expected = written > 0 ? QStringLiteral("Writing had started")
                                             : QStringLiteral("Nothing was written");
        QVERIFY2(details.startsWith(expected), qPrintable(details));
        QVERIFY2(details.contains(QStringLiteral("Failed: 1")), qPrintable(details));
    }
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
