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
}

void Win32McpUiaRefTests::refOutOfRangeIsDrift() {
    const QVector<UiaRefNode> snap{node(QStringLiteral("button"), QStringLiteral("OK"))};
    const QVector<UiaRefNode> live = snap;
    QVERIFY(uiaRefDrifted(snap, live, -1));  // negative
    QVERIFY(uiaRefDrifted(snap, live, 1));   // past both
    // Live walk shrank below the ref -> fail closed even though the snapshot still has it.
    QVERIFY(uiaRefDrifted(snap, QVector<UiaRefNode>{}, 0));
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
