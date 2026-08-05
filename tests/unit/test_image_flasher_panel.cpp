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
    QVERIFY(!flash->isEnabled());
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

    // Image + drive is.
    selectOneDrive();
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
    armMessageBoxDismisser();
    QVERIFY(!m_panel->loadImageFile(m_dir.filePath("does_not_exist.img")));
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
}

QTEST_MAIN(ImageFlasherPanelTests)
#include "test_image_flasher_panel.moc"
