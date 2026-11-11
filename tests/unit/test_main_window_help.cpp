// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_main_window_help.cpp
/// @brief What the Help and About tabs send people to and tell them.

#include "sak/main_window.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTextBrowser>
#include <QtTest/QtTest>
#include <QUrl>

#include <algorithm>

/// Catches the URL a click would have opened in the browser.
class UrlCatcher : public QObject {
    Q_OBJECT

public Q_SLOTS:
    void open(const QUrl& url) { urls << url; }

public:
    QList<QUrl> urls;
};

class MainWindowHelpTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void wikiButtonOpensTheSiteWiki();
    void aboutTextDescribesTheAppAsItIs_data();
    void aboutTextDescribesTheAppAsItIs();
    void ostConverterHeaderNamesItsOneFormat();
    void aboutVersionLineReadsAsADate();
    void aboutTestingTextQuotesTheCertification();
};

// The button opened a GitHub wiki that does not exist, and its name and tooltip said GitHub.
void MainWindowHelpTests::wikiButtonOpensTheSiteWiki() {
    sak::MainWindow window;
    QPushButton* wiki = nullptr;
    const auto buttons = window.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Open Help Wiki")) {
            wiki = button;
        }
    }
    QVERIFY(wiki != nullptr);
    QVERIFY2(!wiki->accessibleName().contains(QStringLiteral("GitHub")),
             qPrintable(wiki->accessibleName()));
    QVERIFY2(!wiki->toolTip().contains(QStringLiteral("GitHub")), qPrintable(wiki->toolTip()));

    UrlCatcher catcher;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &catcher, "open");
    wiki->click();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));

    QCOMPARE(catcher.urls, QList<QUrl>{QUrl(QStringLiteral("https://sakutility.com/wiki"))});
}

void MainWindowHelpTests::aboutTextDescribesTheAppAsItIs_data() {
    QTest::addColumn<QString>("phrase");
    QTest::addColumn<bool>("present");

    // Each of these described a feature the app does not have.
    QTest::newRow("reboot-scheduled cleanup") << "reboot scheduling" << false;
    QTest::newRow("converter formats") << "conversion to EML" << false;
    QTest::newRow("scan on the install tab") << "Scan installed apps, match" << false;

    QTest::newRow("converter output") << "bulk conversion to MBOX" << true;
    QTest::newRow("backup records the match") << "known Chocolatey package" << true;
    QTest::newRow("network report") << "HTML or JSON reports" << true;
}

void MainWindowHelpTests::aboutTextDescribesTheAppAsItIs() {
    QFETCH(QString, phrase);
    QFETCH(bool, present);

    sak::MainWindow window;
    QTextBrowser* about = nullptr;
    const auto browsers = window.findChildren<QTextBrowser*>();
    for (QTextBrowser* browser : browsers) {
        if (browser->accessibleName() == QStringLiteral("About S.A.K. Utility")) {
            about = browser;
        }
    }
    QVERIFY(about != nullptr);
    QCOMPARE(about->toPlainText().contains(phrase), present);
}

// UI review F37: the compiler's raw date ("Nov  4 2026") after an em dash.
void MainWindowHelpTests::aboutVersionLineReadsAsADate() {
    sak::MainWindow window;
    QStringList lines;
    for (const QLabel* label : window.findChildren<QLabel*>()) {
        if (label->text().startsWith(QStringLiteral("Version "))) {
            lines << label->text();
        }
    }
    QCOMPARE(lines.size(), 1);
    static const QRegularExpression kLine(
        QStringLiteral(R"(^Version \S+ - [A-Z][a-z]+ \d{1,2}, \d{4}$)"));
    QVERIFY2(kLine.match(lines.first()).hasMatch(), qPrintable(lines.first()));
}

void MainWindowHelpTests::ostConverterHeaderNamesItsOneFormat() {
    sak::MainWindow window;

    // The tool tabs are built the first time they are opened.
    const auto main_tabs = window.findChildren<QTabWidget*>();
    for (QTabWidget* tabs : main_tabs) {
        for (int index = 0; index < tabs->count(); ++index) {
            if (tabs->tabText(index) == QStringLiteral("Email Tools")) {
                tabs->setCurrentIndex(index);
            }
        }
    }
    const auto findEmailTabs = [&window]() -> QTabWidget* {
        const auto tab_widgets = window.findChildren<QTabWidget*>();
        for (QTabWidget* tabs : tab_widgets) {
            if (tabs->accessibleName() == QStringLiteral("Email tools tabs")) {
                return tabs;
            }
        }
        return nullptr;
    };
    QTRY_VERIFY(findEmailTabs() != nullptr);
    findEmailTabs()->setCurrentIndex(1);

    QStringList subtitles;
    const auto labels = window.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text().startsWith(QStringLiteral("Bulk OST/PST"))) {
            subtitles << label->text();
        }
    }
    QCOMPARE(subtitles, QStringList{QStringLiteral("Bulk OST/PST file conversion to MBOX")});
}

namespace {

/// @p text split into its sentences: each ends at a full stop followed by a space and a capital,
/// so "v0.9.3" and "512 GB" stay inside theirs.
QStringList sentencesOf(const QString& text) {
    static const QRegularExpression kSentenceEnd(QStringLiteral(R"((?<=\.)\s+(?=[A-Z]))"));
    QStringList sentences = text.simplified().split(kSentenceEnd, Qt::SkipEmptyParts);
    for (QString& sentence : sentences) {
        sentence = sentence.trimmed();
    }
    return sentences;
}

/// The sentences docs/PARTITION_MANAGER_CERTIFICATION.md says the site may say for the evidence
/// it records: the quoted line after "What the site may say for this evidence:", which
/// scripts/check_partition_manager_release_claims.ps1 generates from that evidence.
QStringList certificationSiteSentences(QString* error) {
    QFile doc(QStringLiteral(SAK_CERTIFICATION_DOC));
    if (!doc.open(QIODevice::ReadOnly | QIODevice::Text)) {
        *error = QStringLiteral("cannot read %1").arg(doc.fileName());
        return {};
    }
    const QStringList lines = QString::fromUtf8(doc.readAll()).split(QLatin1Char('\n'));
    const auto heading = std::ranges::find_if(lines, [](const QString& line) {
        return line.trimmed() == QStringLiteral("What the site may say for this evidence:");
    });
    const auto quote = std::find_if(heading, lines.end(), [](const QString& line) {
        return line.startsWith(QStringLiteral("> "));
    });
    if (heading == lines.end() || quote == lines.end()) {
        *error = QStringLiteral("no \"What the site may say\" quote in %1").arg(doc.fileName());
        return {};
    }
    return sentencesOf(quote->mid(2));
}

}  // namespace

// The owner, 2026-11-11: the About tab said "Partition Manager has not yet been tested through the
// app on real hardware" after the certification had tested it on real disks. Its Testing line
// now quotes what the certification's generated block says the site may say, read from the
// document at test time: every sentence is one of the block's, in its order (a sentence may be
// left out, never reworded), and every limit the block states (a sentence with "not") and its
// "Back up" sentence are kept. Nothing says "Apple-certified".
void MainWindowHelpTests::aboutTestingTextQuotesTheCertification() {
    QString error;
    const QStringList site = certificationSiteSentences(&error);
    QVERIFY2(!site.isEmpty(), qPrintable(error));

    sak::MainWindow window;
    QTextBrowser* about = nullptr;
    const auto browsers = window.findChildren<QTextBrowser*>();
    for (QTextBrowser* browser : browsers) {
        if (browser->accessibleName() == QStringLiteral("About S.A.K. Utility")) {
            about = browser;
        }
    }
    QVERIFY(about != nullptr);
    const QString plain = about->toPlainText();
    QVERIFY2(!plain.contains(QStringLiteral("Apple-certified"), Qt::CaseInsensitive),
             qPrintable(plain));
    const QString prefix = QStringLiteral("Testing - ");
    const QStringList lines = plain.split(QLatin1Char('\n'));
    const auto testing = std::ranges::find_if(lines, [&prefix](const QString& line) {
        return line.trimmed().startsWith(prefix);
    });
    QVERIFY2(testing != lines.end(), "the About tab has no \"Testing - \" line");
    const QStringList said = sentencesOf(testing->trimmed().mid(prefix.size()));
    QVERIFY(!said.isEmpty());

    qsizetype next = 0;
    for (const QString& sentence : said) {
        const qsizetype at = site.indexOf(sentence, next);
        QVERIFY2(at >= 0,
                 qPrintable(QStringLiteral("About says \"%1\", which is not one of the "
                                           "certification's sentences in their order:\n%2")
                                .arg(sentence, site.join(QLatin1Char('\n')))));
        next = at + 1;
    }
    static const QRegularExpression kLimit(QStringLiteral(R"(\bnot\b)"));
    for (const QString& sentence : site) {
        if (sentence.contains(kLimit) || sentence.startsWith(QStringLiteral("Back up"))) {
            QVERIFY2(said.contains(sentence),
                     qPrintable(QStringLiteral("About leaves out the limit \"%1\"").arg(sentence)));
        }
    }
}

QTEST_MAIN(MainWindowHelpTests)
#include "test_main_window_help.moc"
