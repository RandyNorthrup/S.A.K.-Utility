// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/follow_scroll_controller.h"

#include <QPushButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QtTest/QtTest>

class FollowScrollControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendedContentFollowsNewestByDefault();
    void restoreAndJumpUseSameStateMachine();
};

void FollowScrollControllerTests::appendedContentFollowsNewestByDefault() {
    QTextEdit edit;
    edit.resize(320, 140);
    sak::FollowScrollController controller(&edit);
    edit.show();

    for (int i = 0; i < 80; ++i) {
        edit.append(QStringLiteral("line %1").arg(i));
    }

    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(),
                              edit.verticalScrollBar()->maximum(),
                              1000);
    QVERIFY2(edit.verticalScrollBar()->maximum() > 0,
             "80 appended lines must overflow the viewport, otherwise "
             "isScrolledToBottom() is vacuously true at maximum == 0");
    QCOMPARE(controller.scrollValue(), edit.verticalScrollBar()->maximum());
    QVERIFY(controller.isScrolledToBottom());
    QVERIFY(controller.autoScroll());
}

void FollowScrollControllerTests::restoreAndJumpUseSameStateMachine() {
    QTextEdit edit;
    edit.resize(320, 140);
    QPushButton jump;
    sak::FollowScrollController controller(&edit);
    controller.setJumpToNewestButton(&jump);
    edit.show();

    for (int i = 0; i < 80; ++i) {
        edit.append(QStringLiteral("line %1").arg(i));
    }
    controller.scrollToBottomLater(true);
    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(),
                              edit.verticalScrollBar()->maximum(),
                              1000);
    QVERIFY2(edit.verticalScrollBar()->maximum() > 0,
             "80 appended lines must overflow the viewport for the bottom checks to bind");
    QCOMPARE(controller.scrollValue(), edit.verticalScrollBar()->maximum());

    controller.setAutoScroll(false);
    QVERIFY2(jump.isHidden(),
             "auto-scroll off while still parked at the bottom must keep the jump button "
             "hidden: both terms of the visibility guard have to be evaluated");
    controller.restoreScrollPositionLater(0);
    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(), 0, 1000);
    QCOMPARE(controller.scrollValue(), 0);
    QVERIFY(!controller.autoScroll());
    QVERIFY(!controller.isScrolledToBottom());
    QVERIFY(!jump.isHidden());
    controller.restoreScrollPositionLater(0);
    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(), 0, 1000);
    QVERIFY(!controller.isScrolledToBottom());
    QVERIFY(!jump.isHidden());

    controller.jumpToNewest();
    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(),
                              edit.verticalScrollBar()->maximum(),
                              1000);
    QCOMPARE(controller.scrollValue(), edit.verticalScrollBar()->maximum());
    QVERIFY(controller.isScrolledToBottom());
    QVERIFY(controller.autoScroll());
    QVERIFY(jump.isHidden());
}

QTEST_MAIN(FollowScrollControllerTests)
#include "test_follow_scroll_controller.moc"
