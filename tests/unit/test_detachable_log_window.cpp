// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_detachable_log_window.cpp
/// @brief The detachable log opens on-screen, without taking focus, and is not rebuilt hidden.

#include "sak/detachable_log_window.h"
#include "sak/layout_constants.h"

#include <QApplication>
#include <QMainWindow>
#include <QScreen>
#include <QTextBlock>
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
    void anchoredGeometry_narrowsTheLogToFitBesideTheWindow();
    void anchoredGeometry_overlapsOnlyWhenTheMinimumWidthCannotFit();
    void mainWindowShift_isWhatTheLogNeedsAndTheScreenAllows();
    void openingMovesAMainWindowThatLeavesNoRoom();
    void openingLeavesAMaximizedMainWindowWhereItIs();
    void movingTheMainWindowAfterwardsIsNotFought();
    void openingBesideFullScreenWindowLandsOnScreen();
    void openingDoesNotTakeFocus();
    void hiddenWindowIsNotRebuilt();
    void shownWindowShowsLinesLiterally();
    void aProgramsReportKeepsItsColumns();
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

// 1280 wide on a 1680 display, moved fully left: 400 pixels are free, and the log needs 436.
void DetachableLogWindowTests::anchoredGeometry_narrowsTheLogToFitBesideTheWindow() {
    const QRect available(0, 0, 1680, 860);
    const QRect main_frame(0, 0, 1280, 800);
    const QRect main_geometry(8, 31, 1264, 761);
    const QRect anchored = sak::DetachableLogWindow::anchoredGeometry(
        main_frame, main_geometry, kLogWidth, kLogFrame, available);
    const QRect frame = frameOf(anchored);
    QVERIFY2(!frame.intersects(main_frame), "the log covers the main window");
    QVERIFY(available.contains(frame));
    QCOMPARE(frame.left(), main_frame.right() + 1);
    QVERIFY(anchored.width() >= sak::kDetachLogMinW);
}

void DetachableLogWindowTests::anchoredGeometry_overlapsOnlyWhenTheMinimumWidthCannotFit() {
    const QRect available(0, 0, 1680, 860);
    const QRect main_frame(0, 0, 1500, 800);
    const QRect main_geometry(8, 31, 1484, 761);
    const QRect anchored = sak::DetachableLogWindow::anchoredGeometry(
        main_frame, main_geometry, kLogWidth, kLogFrame, available);
    const QRect frame = frameOf(anchored);
    QVERIFY(available.contains(frame));
    // No shrinking below the minimum to no end: the log keeps its width and overlaps.
    QCOMPARE(anchored.width(), kLogWidth);
    QVERIFY(frame.intersects(main_frame));
}

void DetachableLogWindowTests::mainWindowShift_isWhatTheLogNeedsAndTheScreenAllows() {
    const QRect available(0, 0, 1680, 860);
    using Log = sak::DetachableLogWindow;
    // Room already: stays put.
    QCOMPARE(Log::mainWindowShiftForLog(QRect(0, 0, 1000, 800), 436, available), 0);
    // Exactly enough room.
    QCOMPARE(Log::mainWindowShiftForLog(QRect(244, 0, 1000, 800), 436, available), 0);
    // Short by 36 and room to move.
    QCOMPARE(Log::mainWindowShiftForLog(QRect(280, 0, 1000, 800), 436, available), 36);
    // Short by 236, with only 200 to the screen's edge.
    QCOMPARE(Log::mainWindowShiftForLog(QRect(200, 0, 1280, 800), 436, available), 200);
    // Hard against the left edge, or past it: nowhere to go.
    QCOMPARE(Log::mainWindowShiftForLog(QRect(0, 0, 1500, 800), 436, available), 0);
    QCOMPARE(Log::mainWindowShiftForLog(QRect(-8, 0, 1696, 800), 436, available), 0);
    // A second display that starts further right is measured from its own left edge.
    const QRect second(1680, 0, 1680, 860);
    QCOMPARE(Log::mainWindowShiftForLog(QRect(1880, 0, 1280, 800), 436, second), 200);
}

namespace {
/// A main window placed on the primary screen, with @p room_on_right left over on its right.
void placeWithRoomOnRight(QMainWindow& main, const QRect& available, int room_on_right) {
    constexpr int kWidth = 300;
    constexpr int kHeight = 400;
    main.resize(kWidth, kHeight);
    main.show();
    main.move(available.right() - room_on_right - main.frameGeometry().width() + 1,
              available.top());
}
}  // namespace

// Seen on a VM: a centred window on a 1680 display, with the log covering its right side.
void DetachableLogWindowTests::openingMovesAMainWindowThatLeavesNoRoom() {
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect available = screen->availableGeometry();
    QMainWindow main;
    placeWithRoomOnRight(main, available, 10);
    const int before = main.frameGeometry().left();
    QVERIFY(before > available.left());
    sak::DetachableLogWindow log(QStringLiteral("Log"), &main);

    log.setLogVisible(true);

    QVERIFY2(main.frameGeometry().left() < before, "the main window should have made room");
    QVERIFY(available.contains(main.frameGeometry()));
    QCOMPARE(log.frameGeometry().left(), main.frameGeometry().right() + 1);
}

void DetachableLogWindowTests::openingLeavesAMaximizedMainWindowWhereItIs() {
    QMainWindow main;
    main.showMaximized();
    QVERIFY(QTest::qWaitForWindowExposed(&main));
    QVERIFY(main.isMaximized());
    const QPoint before = main.pos();
    sak::DetachableLogWindow log(QStringLiteral("Log"), &main);

    log.setLogVisible(true);

    QCOMPARE(main.pos(), before);
}

// The log follows the main window around, and that must not turn into a tug of war.
void DetachableLogWindowTests::movingTheMainWindowAfterwardsIsNotFought() {
    QScreen* screen = QGuiApplication::primaryScreen();
    QVERIFY(screen != nullptr);
    const QRect available = screen->availableGeometry();
    QMainWindow main;
    placeWithRoomOnRight(main, available, 10);
    sak::DetachableLogWindow log(QStringLiteral("Log"), &main);
    log.setLogVisible(true);

    placeWithRoomOnRight(main, available, 10);
    const QPoint dragged_to = main.pos();
    log.repositionIfAnchored();

    QCOMPARE(main.pos(), dragged_to);
    QVERIFY(available.contains(log.frameGeometry()));
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
    // The second line of "two\nlines" is program output, in a block of its own (below).
    QCOMPARE(document->blockCount(), 4);
    QCOMPARE(log.logTextEdit()->toPlainText(),
             QStringLiteral("<b>bold?</b>\nAT&amp;T\ntwo\nlines"));

    QVERIFY(log.replaceLog({}));
    QVERIFY(log.logTextEdit()->toPlainText().isEmpty());
}

namespace {
// Where each block of @p document starts, what font it is in, and whether it may wrap.
QString describeBlocks(QTextDocument* document) {
    QStringList blocks;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QTextCharFormat format = block.begin().fragment().charFormat();
        blocks << QStringLiteral("%1|%2|%3")
                      .arg(block.text().left(12),
                           format.fontFamilies().toStringList().join(QLatin1Char(',')),
                           block.blockFormat().nonBreakableLines() ? QStringLiteral("nowrap")
                                                                   : QStringLiteral("wrap"));
    }
    return blocks.join(QLatin1Char('\n'));
}
}  // namespace

// Review 2 (F29, F31): the power plan's boxed report and the dry run's script came out in the
// log's proportional font, wrapped to the window, so the borders and columns fell apart. What a
// program wrote under a message is set in a fixed-width font that does not wrap; the message
// itself keeps the log's font. A refill and a line added live come out the same.
void DetachableLogWindowTests::aProgramsReportKeepsItsColumns() {
    const QString report = QStringLiteral(
        "Optimize Power Settings: Details:\n"
        "+============================+\n"
        "| Current Plan: Balanced     |\n"
        "+============================+\n");
    const QString expected = QStringLiteral(
        "Optimize Pow||wrap\n"
        "+===========|Consolas|nowrap\n"
        "next line||wrap");
    sak::DetachableLogWindow log(QStringLiteral("Log"));
    log.show();
    QVERIFY(QTest::qWaitForWindowExposed(&log));
    QVERIFY(log.replaceLog({report, QStringLiteral("next line")}));
    QTextDocument* document = log.logTextEdit()->document();
    QCOMPARE(describeBlocks(document), expected);
    QCOMPARE(document->begin().next().text().split(QChar::LineSeparator).size(), 3);

    QVERIFY(log.replaceLog({}));
    log.appendLine(report);
    log.appendLine(QStringLiteral("next line"));
    QCOMPARE(describeBlocks(document), expected);
}

QTEST_MAIN(DetachableLogWindowTests)
#include "test_detachable_log_window.moc"
