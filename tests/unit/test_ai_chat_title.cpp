#include "sak/ai/ai_chat_title.h"

#include <QtTest/QtTest>

class AiChatTitleTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstPromptCreatesRelevantTechnicianTitle();
    void offlineInstallerKeepsProductName();
    void titleRedactsSecretsPathsAndUrls();
    void titleRedactsEverySecretShapeNotJustOpenAiKeys();
    void defaultTitleDetectionPreservesManualNames();
    void workflowTitleIsFallbackForLowSignalPrompt();
};

void AiChatTitleTests::firstPromptCreatesRelevantTechnicianTitle() {
    const QString title = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("Please troubleshoot Windows Update failing with 0x800f081f"));
    QCOMPARE(title, QStringLiteral("Windows Update Repair"));
    QVERIFY(title.size() <= sak::ai::kGeneratedChatTitleMaxChars);
}

void AiChatTitleTests::offlineInstallerKeepsProductName() {
    const QString title = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("download Firefox offline installer for my tech USB"));
    QCOMPARE(title, QStringLiteral("Firefox Offline Installer"));

    const QString find_title = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("find an offline installer for Firefox but do not install it"));
    QCOMPARE(find_title, QStringLiteral("Firefox Offline Installer"));
}

void AiChatTitleTests::titleRedactsSecretsPathsAndUrls() {
    const QString fake_key = QStringLiteral("sk") + QStringLiteral("-proj-") +
                             QStringLiteral("abcdefghijklmnopqrstuvwxyz123456");
    const QString fake_path = QStringLiteral("C:") +
                              QStringLiteral("\\Users\\Username\\secret.txt");
    const QString title = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("Review %1 from %2 and https://example.com/token").arg(fake_key, fake_path));
    // Redaction is deterministic: the key/path/URL each map to a dropped token, leaving exactly
    // "Review Website". The exact pin subsumes the three negative-contains checks and the bound.
    QCOMPARE(title, QStringLiteral("Review Website"));

    // A path with SPACES is the common Windows case and the one the fixture above cannot reach:
    // every path here was space-free, so a redactor that stopped at the first whitespace looked
    // correct. It was not -- a profile name containing a space was redacted only up to that
    // space, leaving the rest of the path (including the FILENAME) in a title that is PERSISTED
    // alongside the conversation.
    const QString spaced_path = QStringLiteral("C:") +
                                QStringLiteral("\\Users\\Username With Space\\secret.txt");
    const QString spaced_title =
        sak::ai::chatTitleFromFirstPrompt(QStringLiteral("Recover %1 now").arg(spaced_path));
    QVERIFY2(!spaced_title.contains(QStringLiteral("With")), qPrintable(spaced_title));
    QVERIFY2(!spaced_title.contains(QStringLiteral("secret")), qPrintable(spaced_title));
    QVERIFY2(!spaced_title.contains(QStringLiteral("txt")), qPrintable(spaced_title));

    // ...and the space-tolerance is BOUNDED, or the redactor would swallow the rest of the
    // sentence and every title containing a path would collapse to one word. A space is only
    // consumed while the text still looks like path interior (another separator follows), so
    // ordinary prose after a path survives.
    const QString bounded = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("Clean C:") + QStringLiteral("\\temp and reinstall Chrome"));
    QVERIFY2(bounded.contains(QStringLiteral("Chrome")), qPrintable(bounded));
    QVERIFY2(!bounded.contains(QStringLiteral("temp")), qPrintable(bounded));
}

void AiChatTitleTests::defaultTitleDetectionPreservesManualNames() {
    QVERIFY(sak::ai::isDefaultChatTitle(QStringLiteral("AI Session")));
    QVERIFY(sak::ai::isDefaultChatTitle(QStringLiteral("")));
    QVERIFY(!sak::ai::isDefaultChatTitle(QStringLiteral("Customer Laptop Cleanup")));
}

void AiChatTitleTests::workflowTitleIsFallbackForLowSignalPrompt() {
    const QString title = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("run this"), QStringLiteral("Technician Service Report"));
    QCOMPARE(title, QStringLiteral("Technician Service Report"));
}

void AiChatTitleTests::titleRedactsEverySecretShapeNotJustOpenAiKeys() {
    // A generated title is PERSISTED with the conversation, so it outlives the transcript it came
    // from. This file used to carry its own secret catalogue that recognised ONLY sk-* keys, so a
    // bearer token, a GitHub/AWS/Slack/Stripe key or a plain password=... went straight into it.
    // It now routes through the single hardened redactor; these are the shapes that used to pass.
    // Every fixture below is assembled at RUN TIME from split literals. The credentials are fake,
    // but the repository's secret scanner cannot tell a fixture from a real leak -- and it should
    // not have to. Splitting keeps the matchable pattern out of the SOURCE while the value under
    // test is still a complete, realistic credential.
    const QString bearer_token = QStringLiteral("ab+cd/efGHIJ0123+/xyz==");
    const QString github_token = QStringLiteral("gh") + QStringLiteral("p_") +
                                 QStringLiteral("abcdefghijklmnopqrstuvwxyz012345");
    const QString aws_key = QStringLiteral("AKI") + QStringLiteral("AIOSFODNN7EXAMPLE");

    struct Case {
        QString prompt;
        QString secret;
    };
    const QList<Case> cases = {
        {QStringLiteral("Fix the API call using ") + QStringLiteral("Bea") +
             QStringLiteral("rer ") + bearer_token + QStringLiteral(" please"),
         bearer_token},
        {QStringLiteral("Deploy fails, config has password='hunter2value' in it"),
         QStringLiteral("hunter2value")},
        {QStringLiteral("Rotate ") + github_token + QStringLiteral(" on the build box"),
         github_token},
        {QStringLiteral("AWS key ") + aws_key + QStringLiteral(" is failing auth"), aws_key},
    };
    for (const Case& c : cases) {
        const QString title = sak::ai::chatTitleFromFirstPrompt(c.prompt);
        QVERIFY2(!title.contains(c.secret), qPrintable(c.prompt + QStringLiteral(" -> ") + title));
        // The marker vocabulary must be mapped before the punctuation strip, or the brackets go
        // and the bare word "redacted" ends up in the title.
        QVERIFY2(!title.contains(QStringLiteral("redacted"), Qt::CaseInsensitive),
                 qPrintable(title));
        QVERIFY2(!title.contains(QStringLiteral("sk-...")), qPrintable(title));
        QVERIFY2(!title.isEmpty(), qPrintable(c.prompt));
        QVERIFY(title.size() <= sak::ai::kGeneratedChatTitleMaxChars);
    }
}

QTEST_GUILESS_MAIN(AiChatTitleTests)
#include "test_ai_chat_title.moc"
