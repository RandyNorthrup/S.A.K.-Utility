#include "sak/ai/ai_chat_title.h"

#include <QtTest/QtTest>

class AiChatTitleTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void firstPromptCreatesRelevantTechnicianTitle();
    void offlineInstallerKeepsProductName();
    void titleRedactsSecretsPathsAndUrls();
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
    QVERIFY(!title.contains(QStringLiteral("sk") + QStringLiteral("-proj"), Qt::CaseInsensitive));
    QVERIFY(!title.contains(QStringLiteral("C:"), Qt::CaseInsensitive));
    QVERIFY(!title.contains(QStringLiteral("example.com"), Qt::CaseInsensitive));
    QVERIFY(title.size() <= sak::ai::kGeneratedChatTitleMaxChars);
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

QTEST_GUILESS_MAIN(AiChatTitleTests)
#include "test_ai_chat_title.moc"
