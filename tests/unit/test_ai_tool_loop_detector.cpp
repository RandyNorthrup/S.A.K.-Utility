// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_loop_detector.h"

#include <QtTest/QtTest>

class AiToolLoopDetectorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void identicalCallsTripAtThreshold();
    void distinctArgumentsNeverLoop();
    void distinctToolsNeverLoop();
    void toolNameIsCaseInsensitive();
    void emptyToolNameIgnored();
    void thresholdClampedToTwo();
    void resetClearsState();
    void loopingSummaryDescribesWorstOffender();
};

void AiToolLoopDetectorTests::identicalCallsTripAtThreshold() {
    sak::ai::AiToolLoopDetector detector(3);
    const QString args = QStringLiteral("{\"query\":\"disk\"}");
    QVERIFY(!detector.observe(QStringLiteral("sak_session_search"), args));  // 1
    QVERIFY(!detector.isLooping());
    QVERIFY(!detector.observe(QStringLiteral("sak_session_search"), args));  // 2
    QVERIFY(detector.observe(QStringLiteral("sak_session_search"), args));   // 3 -> trips
    QVERIFY(detector.isLooping());
    QCOMPARE(detector.maxRepeatCount(), 3);
    QCOMPARE(detector.repeatCountFor(QStringLiteral("sak_session_search"), args), 3);
}

void AiToolLoopDetectorTests::distinctArgumentsNeverLoop() {
    sak::ai::AiToolLoopDetector detector(3);
    for (int i = 0; i < 6; ++i) {
        const QString args = QStringLiteral("{\"page\":%1}").arg(i);
        QVERIFY(!detector.observe(QStringLiteral("sak_package_manager"), args));
    }
    QVERIFY(!detector.isLooping());
    QCOMPARE(detector.maxRepeatCount(), 1);
}

void AiToolLoopDetectorTests::distinctToolsNeverLoop() {
    sak::ai::AiToolLoopDetector detector(3);
    QVERIFY(!detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}")));
    QVERIFY(!detector.observe(QStringLiteral("sak_session_search"), QStringLiteral("{}")));
    QVERIFY(!detector.observe(QStringLiteral("sak_package_manager"), QStringLiteral("{}")));
    QVERIFY(!detector.isLooping());
}

void AiToolLoopDetectorTests::toolNameIsCaseInsensitive() {
    sak::ai::AiToolLoopDetector detector(2);
    QVERIFY(!detector.observe(QStringLiteral("Sak_Skill"), QStringLiteral("{}")));
    QVERIFY(detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}")));  // same signature
    QVERIFY(detector.isLooping());
}

void AiToolLoopDetectorTests::emptyToolNameIgnored() {
    sak::ai::AiToolLoopDetector detector(2);
    QVERIFY(!detector.observe(QString(), QStringLiteral("{}")));
    QVERIFY(!detector.observe(QStringLiteral("   "), QStringLiteral("{}")));
    QVERIFY(!detector.isLooping());
    QCOMPARE(detector.maxRepeatCount(), 0);
}

void AiToolLoopDetectorTests::thresholdClampedToTwo() {
    // A threshold below 2 would flag a single legitimate call; it is clamped.
    sak::ai::AiToolLoopDetector detector(0);
    QCOMPARE(detector.repeatThreshold(), 2);
    QVERIFY(!detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}")));
    QVERIFY(detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}")));
}

void AiToolLoopDetectorTests::resetClearsState() {
    sak::ai::AiToolLoopDetector detector(2);
    detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}"));
    detector.observe(QStringLiteral("sak_skill"), QStringLiteral("{}"));
    QVERIFY(detector.isLooping());
    detector.reset();
    QVERIFY(!detector.isLooping());
    QCOMPARE(detector.maxRepeatCount(), 0);
    QVERIFY(detector.loopingSummary().isEmpty());
}

void AiToolLoopDetectorTests::loopingSummaryDescribesWorstOffender() {
    sak::ai::AiToolLoopDetector detector(2);
    detector.observe(QStringLiteral("sak_session_search"), QStringLiteral("{\"query\":\"x\"}"));
    detector.observe(QStringLiteral("sak_session_search"), QStringLiteral("{\"query\":\"x\"}"));
    const QString summary = detector.loopingSummary();
    QVERIFY(summary.contains(QStringLiteral("sak_session_search")));
    QVERIFY(summary.contains(QStringLiteral("x2")));
}

QTEST_GUILESS_MAIN(AiToolLoopDetectorTests)
#include "test_ai_tool_loop_detector.moc"
