// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_selection.h"

#include <QtTest/QtTest>

namespace {

QJsonObject packageObject(const QString& package_id,
                          const QString& title,
                          const QString& version = QStringLiteral("1.0")) {
    QJsonObject object;
    object[QStringLiteral("package_id")] = package_id;
    object[QStringLiteral("title")] = title;
    object[QStringLiteral("version")] = version;
    return object;
}

}  // namespace

class AiPackageSelectionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalizesHumanPackageNames();
    void selectsExactIdFromMultipleCandidates();
    void selectsExactTitleAlias();
    void asksHumanForAmbiguousMatches();
    void reportsNoCandidatesWithoutGuessing();
};

void AiPackageSelectionTests::normalizesHumanPackageNames() {
    QCOMPARE(sak::ai::normalizePackageQueryKey(QStringLiteral("Notepad++")),
             QStringLiteral("notepadplusplus"));
    QCOMPARE(sak::ai::normalizePackageQueryKey(QStringLiteral("Visual Studio Code")),
             QStringLiteral("visualstudiocode"));
    QCOMPARE(sak::ai::normalizePackageQueryKey(QStringLiteral("C# Tools")),
             QStringLiteral("csharptools"));
}

void AiPackageSelectionTests::selectsExactIdFromMultipleCandidates() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("firefox"), QStringLiteral("Firefox")));
    packages.append(
        packageObject(QStringLiteral("firefox-dev"), QStringLiteral("Firefox Developer Edition")));

    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages);

    QVERIFY(result.success);
    QVERIFY(!result.ambiguous);
    QCOMPARE(result.selected.package_id, QStringLiteral("firefox"));
}

void AiPackageSelectionTests::selectsExactTitleAlias() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("vscode"), QStringLiteral("Visual Studio Code")));
    packages.append(packageObject(QStringLiteral("vscode-insiders"),
                                  QStringLiteral("Visual Studio Code Insiders")));

    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("visual studio code"),
                                                          packages);

    QVERIFY(result.success);
    QCOMPARE(result.selected.package_id, QStringLiteral("vscode"));
}

void AiPackageSelectionTests::asksHumanForAmbiguousMatches() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("googlechrome"), QStringLiteral("Google Chrome")));
    packages.append(packageObject(QStringLiteral("chromium"), QStringLiteral("Chromium")));
    packages.append(packageObject(QStringLiteral("chrome-remote-desktop"),
                                  QStringLiteral("Chrome Remote Desktop")));

    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("chrome"), packages);

    QVERIFY(!result.success);
    QVERIFY(result.ambiguous);
    QVERIFY(result.requires_human);
    QVERIFY(result.error_message.contains(QStringLiteral("Ambiguous package match")));
    QVERIFY(result.question_for_human.contains(QStringLiteral("googlechrome")));
    QVERIFY(result.question_for_human.contains(QStringLiteral("chromium")));
}

void AiPackageSelectionTests::reportsNoCandidatesWithoutGuessing() {
    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("definitely-not-real"),
                                                          {});

    QVERIFY(!result.success);
    QVERIFY(!result.ambiguous);
    QVERIFY(!result.requires_human);
    QVERIFY(result.error_message.contains(QStringLiteral("no candidates")));
    QVERIFY(result.selected.package_id.isEmpty());
}

QTEST_GUILESS_MAIN(AiPackageSelectionTests)
#include "test_ai_package_selection.moc"
