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

#include "sak/image_flasher_panel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

using namespace sak;

namespace {

constexpr int kModalPollIntervalMs = 50;

/// A device path plus the identity signature the panel stores alongside it. The
/// panel builds these from DriveScanner results; the scanner is deliberately not
/// running here, so the row is placed directly.
constexpr auto kFakeDevicePath = "\\\\.\\PhysicalDrive99";

/// The Step 1 -> Step 2 navigation button. Every assertion in this file watched the Flash
/// button only, so the Next gate -- which decides whether a user can reach drive selection at
/// all -- had no coverage in either direction.
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
    void initTestCase();
    void init();
    void cleanup();

    void flashIsDisabledWithNothingSelected();
    void flashStaysDisabledWithADriveButNoImage();
    void flashEnablesOnlyWithBothAnImageAndADrive();
    void loadImageFileRefusesAnEmptyPath();
    void loadImageFileRefusesAMissingFile();
    void loadImageFileRefusesAnEmptyFile();
    void aRejectedImageLeavesFlashDisabled();
    void loadImageFileRefusesAnUnknownFormatWhenDeclined();

private:
    QPushButton* flashButton() const;
    QListWidget* driveList() const;
    /// Put one selectable drive row in the list, as onDriveListUpdated would.
    void selectOneDrive();
    /// Close whatever modal QMessageBox appears; returns after it has been closed.
    void armMessageBoxDismisser();
    /// Write @p bytes to a .img under m_dir and return its path.
    QString writeImage(const QString& name, const QByteArray& bytes);

    std::unique_ptr<ImageFlasherPanel> m_panel;
    QTemporaryDir m_dir;
};

void ImageFlasherPanelTests::initTestCase() {
    // The panel starts a DriveScanner thread unless this property is set. The
    // tests place their own row in the list, so a live scan would only race it.
    QCoreApplication::instance()->setProperty("sakAccessibilityAudit", true);
    QVERIFY(m_dir.isValid());
}

void ImageFlasherPanelTests::init() {
    m_panel = std::make_unique<ImageFlasherPanel>();
}

void ImageFlasherPanelTests::cleanup() {
    m_panel.reset();
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
    QListWidget* list = driveList();
    QVERIFY(list);
    auto* item = new QListWidgetItem(QStringLiteral("Test Drive - 8 GB (%1)").arg(kFakeDevicePath),
                                     list);
    item->setData(Qt::UserRole, QString::fromLatin1(kFakeDevicePath));
    item->setData(Qt::UserRole + 1, QStringLiteral("Test Drive|8000000000|USB|512"));
    // Selecting emits itemSelectionChanged, which is what refreshes the buttons.
    item->setSelected(true);
}

void ImageFlasherPanelTests::armMessageBoxDismisser() {
    auto* dismiss = new QTimer(this);
    dismiss->setInterval(kModalPollIntervalMs);
    connect(dismiss, &QTimer::timeout, this, [dismiss]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            dismiss->stop();
            dismiss->deleteLater();
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


QTEST_MAIN(ImageFlasherPanelTests)
#include "test_image_flasher_panel.moc"
