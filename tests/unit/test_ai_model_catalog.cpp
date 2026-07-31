// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_model_catalog.cpp
/// @brief Unit tests for the restricted OpenAI model catalog (GPT-5.6 only).

#include "sak/ai/ai_model_catalog.h"

#include <QtTest>

class TestAiModelCatalog : public QObject {
    Q_OBJECT

private slots:
    void catalogIsExactlyTheThreeGpt56Models();
    void defaultIsSol();
    void membershipAcceptsSupported();
    void membershipRejectsOldAndUnknownModels();
    void membershipTrimsWhitespace();
};

void TestAiModelCatalog::catalogIsExactlyTheThreeGpt56Models() {
    const QStringList models = sak::supportedOpenAiModels();
    QCOMPARE(models.size(), 3);
    QCOMPARE(models.at(0), QStringLiteral("gpt-5.6-sol"));
    QCOMPARE(models.at(1), QStringLiteral("gpt-5.6-terra"));
    QCOMPARE(models.at(2), QStringLiteral("gpt-5.6-luna"));
}

void TestAiModelCatalog::defaultIsSol() {
    QCOMPARE(sak::defaultOpenAiModel(), QStringLiteral("gpt-5.6-sol"));
    // The default must be a member of the catalog.
    QVERIFY(sak::isSupportedOpenAiModel(sak::defaultOpenAiModel()));
    // The default is the first offered item (drives the picker's initial value).
    QCOMPARE(sak::supportedOpenAiModels().first(), sak::defaultOpenAiModel());
}

void TestAiModelCatalog::membershipAcceptsSupported() {
    QVERIFY(sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.6-sol")));
    QVERIFY(sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.6-terra")));
    QVERIFY(sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.6-luna")));
}

void TestAiModelCatalog::membershipRejectsOldAndUnknownModels() {
    // The prior defaults must no longer be accepted.
    QVERIFY(!sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.5")));
    QVERIFY(!sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.4")));
    QVERIFY(!sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.4-mini")));
    QVERIFY(!sak::isSupportedOpenAiModel(QStringLiteral("gpt-4o")));
    QVERIFY(!sak::isSupportedOpenAiModel(QString()));
    // A near-miss (a 5.6 sibling that is not sol/terra/luna) is rejected.
    QVERIFY(!sak::isSupportedOpenAiModel(QStringLiteral("gpt-5.6")));
}

void TestAiModelCatalog::membershipTrimsWhitespace() {
    QVERIFY(sak::isSupportedOpenAiModel(QStringLiteral("  gpt-5.6-sol  ")));
}

QTEST_MAIN(TestAiModelCatalog)
#include "test_ai_model_catalog.moc"
