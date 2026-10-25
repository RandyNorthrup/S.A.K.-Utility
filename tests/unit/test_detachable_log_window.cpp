// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_detachable_log_window.cpp
/// @brief The detachable log opens on-screen, without taking focus, and is not rebuilt hidden.

#include "sak/detachable_log_window.h"

#include <QApplication>
#include <QMainWindow>
#include <QScreen>
#include <QTextDocument>
#include <QtTest/QtTest>

namespace {
constexpr int kLogWidth = 420;
const QMargins kLogFrame(8, 31, 8, 8);

/// The frame a window placed by anchoredGeometry() ends up with.
QRect frameOf(const QRect& anchored) {
    return QRect(anchored.topLeft(), anchored.size().grownBy(kLogFrame));
}
}  // namespace

class DetachableLogWindowTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void anchoredGeometry_snapsBesideWindowWithRoom();
    void anchoredGeometry_staysOnScreenBesideMaximizedWindow();
    void openingBesideFullScreenWindowLandsOnScreen();
    void openingDoesNotTakeFocus();
    void hiddenWindowIsNotRebuilt();
    void shownWindowShowsLinesLiterally();
};

void DetachableLogWindowTests::anchoredGeometry_snapsBesideWindowWithRoom() {
    const QRect available(0, 0, 1920, 1040);
    const QRect main_frame(100, 50, 800, 600);
    const QRect main_geometry(108, 81, 784, 561);
    const QRect anchored = sak::DetachableLogWindow::anchoredGeometry(
        main_frame, main_geometry, kLogWidth, kLogFrame, available);
    QCOMPARE(anchored.topLeft(), QPoint(main_frame.right() + 1, main_frame.top()));
    QCOMPARE(anchored.size(), QSize(kLogWidth, main_geometry.height()));
}

// Beside a maximized main window the plain snap put the whole log past the right edge of the
// screen, where nobody could see or grab it.
void DetachableLogWindowTests::anchoredGeometry_staysOnScreenBesideMaximizedWindow() {
    const QRect available(0, 0, 1920, 1040);
    // Windows reports a maximized frame overhanging the work area by its resize border.
    const QRect main_frame(-8, -8, 1936, 1056);
    const QRect main_geometry(0, 23, 1920, 1009);
    const QRect anchored = sak::DetachableLogWindow::anchoredGeometry(
        main_frame, main_geometry, kLogWidth, kLogFrame, available);
    const QRect frame = frameOf(anchored);
    QVERIFY2(available.contains(frame),
             qPrintable(QStringLiteral("log frame (%1,%2 %3x%4) leaves the work area")
                            .arg(frame.x())
                            .arg(frame.y())
                            .arg(frame.width())
                            .arg(frame.height())));
    QCOMPARE(frame.right(), available.right());
    QCOMPARE(anchored.width(), kLogWidth);
}

void DetachableLogWindowTests::openingBesideFullScreenWindowLandsOnScreen() {
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect available = screen->availableGeometry();
    QMainWindow main;
    main.setGeometry(available);
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    sak::DetachableLogWindow log(QStringLiteral("Log"), &main);
    log.setLogVisible(true);
    QVERIFY(QTest::qWaitForWindowExposed(&log));
    QTRY_VERIFY2(available.contains(log.frameGeometry()),
                 qPrintable(QStringLiteral("log frame (%1,%2 %3x%4) is off screen (%5x%6)")
                                .arg(log.frameGeometry().x())
                                .arg(log.frameGeometry().y())
                                .arg(log.frameGeometry().width())
                                .arg(log.frameGeometry().height())
                                .arg(available.width())
                                .arg(available.height())));
}

// Toggling the log on used to activateWindow() it, pulling keyboard focus out of the panel the
// user was typing in.
void DetachableLogWindowTests::openingDoesNotTakeFocus() {
    QMainWindow main;
    main.resize(640, 480);
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));

    sak::DetachableLogWindow log(QStringLiteral("Log"), &main);
    // The Windows platform shows a window carrying this without activating it
    // (SW_SHOWNOACTIVATE); QWidget hands it to the platform as a window property.
    QVERIFY(log.testAttribute(Qt::WA_ShowWithoutActivating));
    log.setLogVisible(true);
    QVERIFY(QTest::qWaitForWindowExposed(&log));
    QVERIFY(log.windowHandle() != nullptr);
    QVERIFY(log.windowHandle()->property("_q_showWithoutActivating").toBool());

    // The offscreen test platform activates every newly shown window whatever it carries, so the
    // other half -- no explicit activateWindow() -- is probed on a log that is already shown:
    // asking for it again must leave the user's window active.
    main.activateWindow();
    QTRY_COMPARE(QApplication::activeWindow(), &main);
    log.setLogVisible(true);
    QTest::qWait(50);
    QCOMPARE(QApplication::activeWindow(), &main);
}

// MainWindow refilled the whole document (up to 5000 lines) on every tab switch, even with the
// log window hidden. A hidden window now ignores the refill; showing it refills it once.
void DetachableLogWindowTests::hiddenWindowIsNotRebuilt() {
    sak::DetachableLogWindow log(QStringLiteral("Log"));
    log.logTextEdit()->setPlainText(QStringLiteral("kept"));
    QVERIFY(!log.isVisible());
    QVERIFY(!log.replaceLog({QStringLiteral("a"), QStringLiteral("b")}));
    QCOMPARE(log.logTextEdit()->toPlainText(), QStringLiteral("kept"));
}

void DetachableLogWindowTests::shownWindowShowsLinesLiterally() {
    sak::DetachableLogWindow log(QStringLiteral("Log"));
    log.show();
    QVERIFY(QTest::qWaitForWindowExposed(&log));
    const QStringList lines{QStringLiteral("<b>bold?</b>"),
                            QStringLiteral("AT&amp;T"),
                            QStringLiteral("two\nlines")};
    QVERIFY(log.replaceLog(lines));
    QTextDocument* document = log.logTextEdit()->document();
    QCOMPARE(document->blockCount(), 3);
    QCOMPARE(log.logTextEdit()->toPlainText(),
             QStringLiteral("<b>bold?</b>\nAT&amp;T\ntwo\nlines"));

    QVERIFY(log.replaceLog({}));
    QVERIFY(log.logTextEdit()->toPlainText().isEmpty());
}

QTEST_MAIN(DetachableLogWindowTests)
#include "test_detachable_log_window.moc"
