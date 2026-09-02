// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_loop_detector.h"

#include <QCryptographicHash>
#include <QtTest/QtTest>

namespace {
constexpr int kOverlongToolNameChars = 257;
constexpr char16_t kUnitSeparator = 0x1F;
constexpr int kTrackedCapacity = 10'000;
constexpr int kOversizedArgumentChars = 5000;
constexpr qsizetype kSha256HexChars = 64;
constexpr qsizetype kRetainedPreviewChars = 81;
}  // namespace

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
    void overlongToolNameRejected();
    void separatorCannotAliasDistinctCalls();
    void saturationPreservesRepeatProgress();
    void oversizedArgumentsRemainDistinct();
    void oversizedSignatureUsesSha256Digest();
    void oversizedArgumentsRetainBoundedPreview();
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
    // loopingSummary() is byte-deterministic: "%1(%2) x%3" with the worst tool, its (untruncated)
    // args, and the repeat count. Pin the whole rendered string.
    QCOMPARE(summary, QStringLiteral("sak_session_search({\"query\":\"x\"}) x2"));
}

void AiToolLoopDetectorTests::overlongToolNameRejected() {
    sak::ai::AiToolLoopDetector detector(2);
    const QString overlong_name(kOverlongToolNameChars, QLatin1Char('x'));
    QVERIFY(!detector.observe(overlong_name, QStringLiteral("{}")));
    QCOMPARE(detector.maxRepeatCount(), 0);
    QCOMPARE(detector.repeatCountFor(overlong_name, QStringLiteral("{}")), 0);
}

void AiToolLoopDetectorTests::separatorCannotAliasDistinctCalls() {
    const QString separator = QChar(kUnitSeparator);
    sak::ai::AiToolLoopDetector detector(2);
    QVERIFY(!detector.observe(QStringLiteral("a"), QStringLiteral("b") + separator + "c"));
    QVERIFY(!detector.observe(QStringLiteral("a") + separator + "b", QStringLiteral("c")));
    QVERIFY(!detector.isLooping());
}

void AiToolLoopDetectorTests::saturationPreservesRepeatProgress() {
    sak::ai::AiToolLoopDetector detector(4);
    for (int repeat = 0; repeat < 3; ++repeat) {
        QVERIFY(!detector.observe(QStringLiteral("repeat"), QStringLiteral("{}")));
    }
    for (int index = 0; index < kTrackedCapacity - 1; ++index) {
        QVERIFY(!detector.observe(QStringLiteral("unique-%1").arg(index), QStringLiteral("{}")));
    }
    QVERIFY(!detector.observe(QStringLiteral("overflow"), QStringLiteral("{}")));
    QVERIFY(detector.observe(QStringLiteral("repeat"), QStringLiteral("{}")));
    QCOMPARE(detector.repeatCountFor(QStringLiteral("repeat"), QStringLiteral("{}")), 4);
}

void AiToolLoopDetectorTests::oversizedArgumentsRemainDistinct() {
    sak::ai::AiToolLoopDetector detector(2);
    const QString first(kOversizedArgumentChars, QLatin1Char('x'));
    const QString second(kOversizedArgumentChars, QLatin1Char('y'));
    QVERIFY(!detector.observe(QStringLiteral("tool"), first));
    QVERIFY(!detector.observe(QStringLiteral("tool"), second));
    QVERIFY(detector.observe(QStringLiteral("tool"), first));
    QCOMPARE(detector.repeatCountFor(QStringLiteral("tool"), second), 1);
}

void AiToolLoopDetectorTests::oversizedSignatureUsesSha256Digest() {
    const QString arguments(kOversizedArgumentChars, QLatin1Char('x'));
    const QByteArray digest =
        QCryptographicHash::hash(arguments.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString signature = sak::ai::AiToolLoopDetector::signatureKey(QStringLiteral("tool"),
                                                                        arguments);
    QVERIFY(signature.endsWith(QString::fromLatin1(digest)));
    QCOMPARE(digest.size(), kSha256HexChars);
}

void AiToolLoopDetectorTests::oversizedArgumentsRetainBoundedPreview() {
    sak::ai::AiToolLoopDetector detector(2);
    detector.observe(QStringLiteral("tool"), QString(kOversizedArgumentChars, QLatin1Char('x')));
    QCOMPARE(detector.m_worst_args.size(), kRetainedPreviewChars);
    QVERIFY(detector.loopingSummary().contains(QStringLiteral("...")));
}

QTEST_GUILESS_MAIN(AiToolLoopDetectorTests)
#include "test_ai_tool_loop_detector.moc"
