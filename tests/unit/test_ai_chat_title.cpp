#include "sak/ai/ai_chat_title.h"

#include <QtTest/QtTest>

class AiChatTitleTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstPromptCreatesRelevantTechnicianTitle();
    void offlineInstallerKeepsProductName();
    void titleRedactsSecretsPathsAndUrls();
    void titleRedactsEverySecretShapeNotJustOpenAiKeys();
    void titleRedactsAnySchemeAndUnclosedCode();
    void domainRulesMatchWholeWordsOnly();
    void offlineInstallerKeepsMultiWordProductName();
    void punctuationAndNonLatinWordHandling();
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

void AiChatTitleTests::titleRedactsAnySchemeAndUnclosedCode() {
    // URL redaction covered http(s) and www only, so any other scheme went into a PERSISTED
    // title verbatim -- hostname, path, and for ftp:// the embedded credentials too.
    //
    // THE URL GOES FIRST IN EVERY PROMPT HERE, DELIBERATELY. The first version of this test put
    // it mid-sentence and passed even with the fix reverted: the punctuation strip splits the URL
    // into ordinary words, and the six-word title cap then dropped them for a reason that has
    // nothing to do with redaction. The assertions were true by accident -- a vacuous test of
    // exactly the kind this campaign exists to find. Leading the prompt with the URL puts its
    // tokens inside the cap, so only real redaction can keep them out.
    const QString ftp = QStringLiteral("ftp://") + QStringLiteral("user:pw@files.example.test") +
                        QStringLiteral("/private/dump.sql");
    const QString ftp_title =
        sak::ai::chatTitleFromFirstPrompt(QStringLiteral("%1 keeps failing").arg(ftp));
    QVERIFY2(ftp_title.contains(QStringLiteral("Website")), qPrintable(ftp_title));
    for (const char* leaked : {"files", "example", "user", "dump", "private"}) {
        QVERIFY2(!ftp_title.contains(QLatin1String(leaked), Qt::CaseInsensitive),
                 qPrintable(ftp_title));
    }

    struct SchemeCase {
        QString url;
        QString leaked;
    };
    for (const SchemeCase& c :
         {SchemeCase{QStringLiteral("smb://server/share/secretfile.txt"),
                     QStringLiteral("secretfile")},
          SchemeCase{QStringLiteral("ssh://buildbox.internal/repo"), QStringLiteral("buildbox")},
          SchemeCase{QStringLiteral("ldap://directory.internal/ou=people"),
                     QStringLiteral("directory")}}) {
        const QString title =
            sak::ai::chatTitleFromFirstPrompt(QStringLiteral("%1 is unreachable").arg(c.url));
        QVERIFY2(title.contains(QStringLiteral("Website")), qPrintable(title));
        QVERIFY2(!title.contains(c.leaked, Qt::CaseInsensitive),
                 qPrintable(c.url + QStringLiteral(" -> ") + title));
    }

    // A drive letter is NOT a scheme -- "C:/temp" has one slash, not "://" -- so the path rule
    // must still be the one that handles it, and the URL rule must not swallow the sentence.
    const QString drive = sak::ai::chatTitleFromFirstPrompt(QStringLiteral("Clean C:") +
                                                            QStringLiteral("/temp and fix Chrome"));
    QVERIFY2(drive.contains(QStringLiteral("Chrome")), qPrintable(drive));

    // An UNCLOSED fence is not removed by a paired pattern, so the code body used to drive the
    // title. Both spellings now drop from the opener to the end.
    const QString fence = sak::ai::chatTitleFromFirstPrompt(
        QStringLiteral("Windows Update keeps failing ```powershell\nRemove-Item secretpath"));
    QVERIFY2(!fence.contains(QStringLiteral("secretpath")), qPrintable(fence));
    QVERIFY2(!fence.contains(QStringLiteral("powershell"), Qt::CaseInsensitive), qPrintable(fence));
    const QString inline_tick =
        sak::ai::chatTitleFromFirstPrompt(QStringLiteral("Printer is broken `Get-Secretthing"));
    QVERIFY2(!inline_tick.contains(QStringLiteral("Secretthing")), qPrintable(inline_tick));
}

void AiChatTitleTests::domainRulesMatchWholeWordsOnly() {
    // "virus" is a substring of "antivirus", so plain substring matching titled this prompt
    // "Malware Cleanup" -- the OPPOSITE of what the user said. It never reached the printer rule
    // either, because the malware rule sits earlier in the list.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(
                 QStringLiteral("my antivirus is blocking the printer driver")),
             QStringLiteral("Printer Troubleshooting"));

    // The rules themselves must still fire on the real words, or this is just a way of breaking
    // every title.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("remove this virus from the laptop")),
             QStringLiteral("Malware Cleanup"));
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("laptop has malware everywhere")),
             QStringLiteral("Malware Cleanup"));
    // A multi-word term still matches as a phrase.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("got a blue screen again today")),
             QStringLiteral("BSOD Investigation"));
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("windows update wont install")),
             QStringLiteral("Windows Update Repair"));
}

void AiChatTitleTests::offlineInstallerKeepsMultiWordProductName() {
    // Taking only the first non-skipped word named the WRONG product: Google ships several
    // installers, and Chrome is the one that was asked for.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(
                 QStringLiteral("download Google Chrome offline installer")),
             QStringLiteral("Google Chrome Offline Installer"));

    // Continuation stops at the first lowercase word, so ordinary prose after the product name
    // is not absorbed into it.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(
                 QStringLiteral("find an offline installer for Firefox but do not install it")),
             QStringLiteral("Firefox Offline Installer"));
    // ... and at a skip word regardless of case.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(
                 QStringLiteral("download Firefox offline installer for my tech USB")),
             QStringLiteral("Firefox Offline Installer"));
    // No product named at all still falls back rather than inventing one.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("make an offline installer bundle")),
             QStringLiteral("Offline Installer Download"));
}

void AiChatTitleTests::punctuationAndNonLatinWordHandling() {
    // '+', '-' and '.' are kept because they belong inside real tokens, but a token made only of
    // them spells nothing. "---" and "++" passed the length check and counted as meaningful,
    // which was enough to defeat the low-signal workflow fallback.
    QCOMPARE(sak::ai::chatTitleFromFirstPrompt(QStringLiteral("--- ++ ..."),
                                               QStringLiteral("Technician Service Report")),
             QStringLiteral("Technician Service Report"));
    // A token that merely CONTAINS punctuation is still a word.
    QVERIFY(
        sak::ai::chatTitleFromFirstPrompt(QStringLiteral("upgrade wi-fi driver on the laptop"))
            .contains(QStringLiteral("Wi-Fi"), Qt::CaseInsensitive) ||
        sak::ai::chatTitleFromFirstPrompt(QStringLiteral("upgrade wi-fi driver on the laptop")) ==
            QStringLiteral("Network Connectivity Repair"));

    // ASCII-only filtering erased every character of a non-Latin prompt, so an otherwise
    // descriptive request produced no meaningful words and fell back to the generic title. The
    // title was worst for exactly the users least able to work around it.
    const QString cyrillic =
        sak::ai::chatTitleFromFirstPrompt(QString::fromUtf8("\xD0\xBF\xD1\x80\xD0\xB8\xD0\xBD"
                                                            "\xD1\x82\xD0\xB5\xD1\x80 \xD1\x81"
                                                            "\xD0\xBB\xD0\xBE\xD0\xBC\xD0\xB0"
                                                            "\xD0\xBB\xD1\x81\xD1\x8F"));
    QVERIFY2(cyrillic != QStringLiteral("AI Chat"), qPrintable(cyrillic));
    QVERIFY(!cyrillic.isEmpty());
}

QTEST_GUILESS_MAIN(AiChatTitleTests)
#include "test_ai_chat_title.moc"
