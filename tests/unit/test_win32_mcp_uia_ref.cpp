// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_uia_ref.h"

#include <QtTest/QtTest>

using sak::win32mcp::uiaRefDrifted;
using sak::win32mcp::UiaRefNode;

namespace {

UiaRefNode node(const QString& role, const QString& name, long left = 0, long top = 0) {
    return UiaRefNode{role, name, left, top};
}

}  // namespace

class Win32McpUiaRefTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void identicalTreeDoesNotDrift();
    void refOutOfRangeIsDrift();
    void roleChangeIsDrift();
    void nameChangeIsDrift();
    void boundsChangeIsDrift();
    void namelessPositionSwapIsCaughtByBounds();
};

void Win32McpUiaRefTests::identicalTreeDoesNotDrift() {
    const QVector<UiaRefNode> snap{node(QStringLiteral("window"), QStringLiteral("App")),
                                   node(QStringLiteral("button"), QStringLiteral("OK"), 10, 20)};
    const QVector<UiaRefNode> live = snap;
    QVERIFY(!uiaRefDrifted(snap, live, 0));
    QVERIFY(!uiaRefDrifted(snap, live, 1));
    // Only the node AT ref is compared: an unrelated sibling changing (a title bar repainting)
    // must NOT invalidate a stored ref, and the ref'd node's own change must. Without this, a
    // whole-tree "anything changed" implementation stays green on every case in this file.
    const QVector<UiaRefNode> siblingChanged{
        node(QStringLiteral("window"), QStringLiteral("App Renamed")),
        node(QStringLiteral("button"), QStringLiteral("OK"), 10, 20)};
    QVERIFY(!uiaRefDrifted(snap, siblingChanged, 1));
    QVERIFY(uiaRefDrifted(snap, siblingChanged, 0));
}

void Win32McpUiaRefTests::refOutOfRangeIsDrift() {
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QStringLiteral("OK"))};
    const QVector<UiaRefNode> live = snap;
    QVERIFY(uiaRefDrifted(snap, live, -1));  // negative
    QVERIFY(uiaRefDrifted(snap, live, 1));   // past both
    // Live walk shrank below the ref -> fail closed even though the snapshot still has it.
    QVERIFY(uiaRefDrifted(snap, QVector<UiaRefNode>{}, 0));
    // An EMPTY live walk is also caught by an isEmpty()-shaped guard, so it does not isolate the
    // live-length bound. A walk that shrank but is still non-empty can only be caught by
    // `ref >= live.size()` (the snapshot is still long enough), so pin that shape too.
    const QVector<UiaRefNode> snapTwo{node(QStringLiteral("button"), QStringLiteral("OK")),
                                      node(QStringLiteral("button"), QStringLiteral("Cancel"))};
    const QVector<UiaRefNode> shrunk{node(QStringLiteral("button"), QStringLiteral("OK"))};
    QVERIFY(uiaRefDrifted(snapTwo, shrunk, 1));
    QVERIFY(!uiaRefDrifted(snapTwo, snapTwo, 1));  // same ref is fine while live still has it
    // Snapshot shorter than the ref -> also drift.
    const QVector<UiaRefNode> live2{node(QStringLiteral("button"), QStringLiteral("OK")),
                                    node(QStringLiteral("button"), QStringLiteral("Cancel"))};
    QVERIFY(uiaRefDrifted(snap, live2, 1));
}

void Win32McpUiaRefTests::roleChangeIsDrift() {
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QStringLiteral("OK"))};
    const QVector<UiaRefNode> live{node(QStringLiteral("checkbox"), QStringLiteral("OK"))};
    QVERIFY(uiaRefDrifted(snap, live, 0));
}

void Win32McpUiaRefTests::nameChangeIsDrift() {
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QStringLiteral("OK"))};
    const QVector<UiaRefNode> live{node(QStringLiteral("button"), QStringLiteral("Cancel"))};
    QVERIFY(uiaRefDrifted(snap, live, 0));
}

void Win32McpUiaRefTests::boundsChangeIsDrift() {
    // Same role+name but the control moved -> treat as drift.
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QStringLiteral("OK"), 10, 20)};
    const QVector<UiaRefNode> live{node(QStringLiteral("button"), QStringLiteral("OK"), 300, 20)};
    QVERIFY(uiaRefDrifted(snap, live, 0));
    // Only `left` differs above, so `a.left != b.left` alone satisfies that assertion and the
    // `a.top != b.top` term is dead in this entire file. Pin top on its own: a control that moved
    // only vertically (a toolbar row pushed down by an inserted item) must also report drift.
    const QVector<UiaRefNode> movedDown{
        node(QStringLiteral("button"), QStringLiteral("OK"), 10, 300)};
    QVERIFY(uiaRefDrifted(snap, movedDown, 0));
}

void Win32McpUiaRefTests::namelessPositionSwapIsCaughtByBounds() {
    // Two nameless, same-role buttons swap order between inspect and act. Role+name alone would
    // report "no drift" and invoke the wrong control; the bounding-rect origin catches it.
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QString(), 10, 10),
                                   node(QStringLiteral("button"), QString(), 100, 10)};
    const QVector<UiaRefNode> live{node(QStringLiteral("button"), QString(), 100, 10),
                                   node(QStringLiteral("button"), QString(), 10, 10)};
    QVERIFY(uiaRefDrifted(snap, live, 0));
    QVERIFY(uiaRefDrifted(snap, live, 1));
}

QTEST_GUILESS_MAIN(Win32McpUiaRefTests)
#include "test_win32_mcp_uia_ref.moc"
