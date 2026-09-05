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
    void selectsExactIdFromMultipleCandidates();
    void selectsExactTitleAlias();
    void refusesLossyIdAndTitleCollisions();
    void rejectsNonPositiveCandidateLimitWithoutExposure();
    void serializesOnlyCanonicalCandidateFields();
    void asksHumanForAmbiguousMatches();
    void reportsNoCandidatesWithoutGuessing();
    void doesNotAutoSelectLoneNonExactCandidate();
    void dropsCandidateWithDisallowedIdCharacters();
    void rejectsUnsafePackageIdDomain();
    void rejectsOversizedRawQueryBeforeTrimming();
    void boundsAndSanitizesCandidateMetadata();
    void rejectsMalformedLaterAlias();
    void exposesConflictingDuplicateVersions();
    void reportsRejectedEntriesOnSuccessfulSelection();
};

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

void AiPackageSelectionTests::refusesLossyIdAndTitleCollisions() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("foobar"), QStringLiteral("Foo Bar")));

    const auto collision = sak::ai::selectPackageForWorkflow(QStringLiteral("foo-bar"), packages);
    QVERIFY(!collision.success);
    QVERIFY(collision.requires_human);
    QVERIFY(collision.selected.package_id.isEmpty());

    const auto exact_id = sak::ai::selectPackageForWorkflow(QStringLiteral("FOOBAR"), packages);
    QVERIFY(exact_id.success);
    QCOMPARE(exact_id.selected.package_id, QStringLiteral("foobar"));

    const auto exact_title = sak::ai::selectPackageForWorkflow(QStringLiteral("fOo BaR"), packages);
    QVERIFY(exact_title.success);
    QCOMPARE(exact_title.selected.package_id, QStringLiteral("foobar"));
}

void AiPackageSelectionTests::rejectsNonPositiveCandidateLimitWithoutExposure() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("firefox"), QStringLiteral("Firefox")));

    for (const int invalid_limit : {0, -1}) {
        const auto result =
            sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages, invalid_limit);
        QVERIFY(!result.success);
        QVERIFY(!result.ambiguous);
        QVERIFY(!result.requires_human);
        QVERIFY(result.selected.package_id.isEmpty());
        QVERIFY(result.candidates.isEmpty());
        QCOMPARE(result.rejected_entries, 0);
        QCOMPARE(result.error_message,
                 QStringLiteral("Package candidate limit must be greater than zero (received %1)")
                     .arg(invalid_limit));
    }
}

void AiPackageSelectionTests::serializesOnlyCanonicalCandidateFields() {
    sak::ai::AiPackageCandidate candidate;
    candidate.package_id = QStringLiteral("tool");

    const QJsonObject minimal = candidate.toJson();
    QCOMPARE(minimal.value(QStringLiteral("package_id")).toString(), QStringLiteral("tool"));
    QVERIFY(!minimal.contains(QStringLiteral("version")));
    QVERIFY(!minimal.contains(QStringLiteral("title")));
    QVERIFY(!minimal.contains(QStringLiteral("description")));

    candidate.version = QStringLiteral("2.0");
    candidate.title = QStringLiteral("Tool Suite");
    candidate.description = QStringLiteral("Current description");
    const QJsonObject populated = candidate.toJson();
    QCOMPARE(populated.value(QStringLiteral("version")).toString(), QStringLiteral("2.0"));
    QCOMPARE(populated.value(QStringLiteral("title")).toString(), QStringLiteral("Tool Suite"));
    QCOMPARE(populated.value(QStringLiteral("description")).toString(),
             QStringLiteral("Current description"));
    QVERIFY(!populated.contains(QStringLiteral("source")));
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
    QCOMPARE(result.error_message,
             QStringLiteral("Ambiguous package match for 'chrome'. Choose an exact package_id."));
    QCOMPARE(result.question_for_human,
             QStringLiteral("Ambiguous package match for 'chrome'. Choose an exact package_id. "
                            "Candidates: googlechrome v1.0; chromium v1.0; chrome-remote-desktop "
                            "v1.0"));
}

void AiPackageSelectionTests::reportsNoCandidatesWithoutGuessing() {
    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("definitely-not-real"),
                                                          {});

    QVERIFY(!result.success);
    QVERIFY(!result.ambiguous);
    QVERIFY(!result.requires_human);
    QCOMPARE(result.error_message,
             QStringLiteral("Package search returned no candidates for 'definitely-not-real'"));
    QVERIFY(result.selected.package_id.isEmpty());
}

void AiPackageSelectionTests::doesNotAutoSelectLoneNonExactCandidate() {
    // A single search hit that does NOT exactly match the query must not be auto-installed:
    // fail closed to a human decision rather than guess a fuzzy match.
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("some-unrelated-tool"),
                                  QStringLiteral("Some Unrelated Tool")));

    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages);

    QVERIFY(!result.success);
    QVERIFY(result.requires_human);
    QVERIFY(result.selected.package_id.isEmpty());
}

void AiPackageSelectionTests::dropsCandidateWithDisallowedIdCharacters() {
    // An id with disallowed characters is REJECTED (not rewritten by deleting them, which
    // could turn "fire fox!" into the real "firefox" and install the wrong package). The
    // only candidate therefore has no usable id and the search reports no candidates.
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("fire fox!"), QStringLiteral("Bad Id")));

    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages);

    QVERIFY(!result.success);
    QVERIFY(result.selected.package_id.isEmpty());  // NOT rewritten to a valid "firefox"
    QCOMPARE(result.error_message,
             QStringLiteral("Package search returned no candidates for 'firefox' (1 malformed "
                            "search result(s) rejected)"));
}

void AiPackageSelectionTests::rejectsUnsafePackageIdDomain() {
    const QStringList invalidIds = {
        QStringLiteral(" padded"),
        QStringLiteral("-force"),
        QStringLiteral("."),
        QStringLiteral(".."),
        QStringLiteral("all"),
        QString(sak::ai::kMaxPackageIdChars + 1, QLatin1Char('a')),
    };
    for (const QString& id : invalidIds) {
        QJsonArray packages;
        packages.append(packageObject(id, QStringLiteral("Unsafe")));
        const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("unsafe"), packages);
        QVERIFY2(!result.success, qPrintable(id));
        QCOMPARE(result.rejected_entries, 1);
        QVERIFY(result.candidates.isEmpty());
    }
}

void AiPackageSelectionTests::rejectsOversizedRawQueryBeforeTrimming() {
    const QString query(sak::ai::kMaxPackageQueryChars + 1, QLatin1Char(' '));
    const auto result = sak::ai::selectPackageForWorkflow(query, {});
    QVERIFY(!result.success);
    QVERIFY(result.error_message.contains(QStringLiteral("too long")));
    QVERIFY(!result.error_message.contains(QStringLiteral("no usable candidates")));
}

void AiPackageSelectionTests::boundsAndSanitizesCandidateMetadata() {
    QJsonObject oversized = packageObject(QStringLiteral("oversized"), QStringLiteral("Title"));
    oversized[QStringLiteral("title")] = QString(sak::ai::kMaxPackageTitleChars + 1,
                                                 QLatin1Char('t'));
    QJsonArray rejected;
    rejected.append(oversized);
    const auto rejectedResult = sak::ai::selectPackageForWorkflow(QStringLiteral("oversized"),
                                                                  rejected);
    QVERIFY(!rejectedResult.success);
    QCOMPARE(rejectedResult.rejected_entries, 1);

    QJsonObject whitespace = packageObject(QStringLiteral("whitespace"), QStringLiteral("Title"));
    whitespace[QStringLiteral("description")] = QString(sak::ai::kMaxPackageDescriptionChars + 1,
                                                        QLatin1Char(' '));
    QJsonArray whitespacePackages;
    whitespacePackages.append(whitespace);
    QCOMPARE(sak::ai::selectPackageForWorkflow(QStringLiteral("whitespace"), whitespacePackages)
                 .rejected_entries,
             1);

    QJsonObject bounded = packageObject(QStringLiteral("bounded"), QStringLiteral("Bounded"));
    bounded[QStringLiteral("blob")] = QString(100'000, QLatin1Char('x'));
    QJsonArray accepted;
    accepted.append(bounded);
    const auto acceptedResult = sak::ai::selectPackageForWorkflow(QStringLiteral("bounded"),
                                                                  accepted);
    QVERIFY(acceptedResult.success);
    QVERIFY(!acceptedResult.selected.toJson().contains(QStringLiteral("blob")));
}

void AiPackageSelectionTests::rejectsMalformedLaterAlias() {
    QJsonObject malformed = packageObject(QStringLiteral("firefox"), QStringLiteral("Firefox"));
    malformed[QStringLiteral("id")] = 7;
    QJsonArray packages;
    packages.append(malformed);
    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages);
    QVERIFY(!result.success);
    QCOMPARE(result.rejected_entries, 1);
}

void AiPackageSelectionTests::exposesConflictingDuplicateVersions() {
    QJsonArray packages;
    packages.append(
        packageObject(QStringLiteral("tool"), QStringLiteral("Tool"), QStringLiteral("1.0")));
    packages.append(
        packageObject(QStringLiteral("tool"), QStringLiteral("Tool"), QStringLiteral("2.0")));
    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("tool"), packages);
    QVERIFY(!result.success);
    QVERIFY(result.ambiguous);
    QVERIFY(!result.requires_human);
    QVERIFY(result.error_message.contains(QStringLiteral("conflicting versions")));
    QVERIFY(!result.error_message.contains(QStringLiteral("Choose an exact package_id")));
    QCOMPARE(result.candidates.size(), 2);
    QCOMPARE(result.candidates.at(0).version, QStringLiteral("1.0"));
    QCOMPARE(result.candidates.at(1).version, QStringLiteral("2.0"));
}

void AiPackageSelectionTests::reportsRejectedEntriesOnSuccessfulSelection() {
    QJsonArray packages;
    packages.append(packageObject(QStringLiteral("firefox"), QStringLiteral("Firefox")));
    packages.append(QJsonObject{{QStringLiteral("package_id"), 7}});
    const auto result = sak::ai::selectPackageForWorkflow(QStringLiteral("firefox"), packages);
    QVERIFY(result.success);
    QCOMPARE(result.rejected_entries, 1);
    QCOMPARE(result.toJson().value(QStringLiteral("rejected_entries")).toInt(), 1);
}

QTEST_GUILESS_MAIN(AiPackageSelectionTests)
#include "test_ai_package_selection.moc"
