// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_token_usage_tracker.h"

#include <QJsonObject>
#include <QtTest/QtTest>

class AiTokenUsageTrackerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fromJson_parsesNestedUsage();
    void addTurn_accumulatesSession();
    void reset_clearsUsage();
};

void AiTokenUsageTrackerTests::fromJson_parsesNestedUsage() {
    QJsonObject input_details;
    input_details["cached_tokens"] = 12;
    QJsonObject output_details;
    output_details["reasoning_tokens"] = 34;

    QJsonObject usage_json;
    usage_json["input_tokens"] = 100;
    usage_json["input_tokens_details"] = input_details;
    usage_json["output_tokens"] = 50;
    usage_json["output_tokens_details"] = output_details;
    usage_json["total_tokens"] = 150;

    const auto usage = sak::ai::TokenUsageTracker::fromJson(usage_json);
    QCOMPARE(usage.input_tokens, qint64{100});
    QCOMPARE(usage.cached_input_tokens, qint64{12});
    QCOMPARE(usage.output_tokens, qint64{50});
    QCOMPARE(usage.reasoning_tokens, qint64{34});
    QCOMPARE(usage.total_tokens, qint64{150});
}

void AiTokenUsageTrackerTests::addTurn_accumulatesSession() {
    sak::ai::TokenUsageTracker tracker;
    tracker.addTurn({10, 1, 5, 2, 15});
    tracker.addTurn({20, 3, 7, 4, 27});

    QCOMPARE(tracker.lastTurn().input_tokens, qint64{20});
    QCOMPARE(tracker.sessionTotal().input_tokens, qint64{30});
    QCOMPARE(tracker.sessionTotal().cached_input_tokens, qint64{4});
    QCOMPARE(tracker.sessionTotal().output_tokens, qint64{12});
    QCOMPARE(tracker.sessionTotal().reasoning_tokens, qint64{6});
    QCOMPARE(tracker.sessionTotal().total_tokens, qint64{42});
}

void AiTokenUsageTrackerTests::reset_clearsUsage() {
    sak::ai::TokenUsageTracker tracker;
    tracker.addTurn({10, 1, 5, 2, 15});
    tracker.reset();

    QVERIFY(tracker.lastTurn().isEmpty());
    QVERIFY(tracker.sessionTotal().isEmpty());
}

QTEST_GUILESS_MAIN(AiTokenUsageTrackerTests)
#include "test_ai_token_usage_tracker.moc"
