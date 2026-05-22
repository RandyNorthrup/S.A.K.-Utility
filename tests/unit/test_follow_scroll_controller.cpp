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

    QTRY_VERIFY_WITH_TIMEOUT(controller.isScrolledToBottom(), 1000);
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
    QTRY_VERIFY_WITH_TIMEOUT(controller.isScrolledToBottom(), 1000);
    QVERIFY(controller.scrollValue() > 0);

    controller.setAutoScroll(false);
    controller.restoreScrollPositionLater(0);
    QTRY_COMPARE_WITH_TIMEOUT(edit.verticalScrollBar()->value(), 0, 1000);
    QVERIFY(!controller.isScrolledToBottom());
    QVERIFY(!jump.isHidden());

    controller.jumpToNewest();
    QTRY_VERIFY_WITH_TIMEOUT(controller.isScrolledToBottom(), 1000);
    QVERIFY(controller.autoScroll());
    QVERIFY(jump.isHidden());
}

QTEST_MAIN(FollowScrollControllerTests)
#include "test_follow_scroll_controller.moc"
