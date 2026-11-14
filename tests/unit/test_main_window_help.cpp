// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_main_window_help.cpp
/// @brief What the Help and About tabs send people to and tell them.

#include "sak/bandwidth_tester.h"
#include "sak/main_window.h"
#include "sak/windows11_theme.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizeGrip>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QtTest/QtTest>
#include <QUrl>

#include <algorithm>
#include <string>

#include <windows.h>

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
    void iperfAdministratorMessageFitsTheStatusBar();
    void aiStatusLabelStandsAsideOnlyWhileAMessageShows();
};

namespace {

/// The width @p text takes in Segoe UI at @p pixel_size, as Windows itself draws it at 96 DPI. The
/// offscreen test platform draws every glyph as a box as wide as the font is tall, so its own
/// measure of a sentence is about twice a real font's.
int segoeUiTextWidth(const QString& text, int pixel_size) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-pixel_size,
                             0,
                             0,
                             0,
                             FW_NORMAL,
                             FALSE,
                             FALSE,
                             FALSE,
                             DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY,
                             DEFAULT_PITCH,
                             L"Segoe UI");
    HGDIOBJ previous = SelectObject(dc, font);
    const std::wstring wide = text.toStdWString();
    SIZE size{};
    const bool measured =
        GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &size) != FALSE;
    SelectObject(dc, previous);
    DeleteObject(font);
    DeleteDC(dc);
    return measured ? static_cast<int>(size.cx) : -1;
}

/// The width QStatusBar gives a timed message: from 6 px in to 2 px short of the first permanent
/// widget it shows (QStatusBarPrivate::messageRect). Its other widgets are hidden meanwhile.
int timedMessageRoom(const QStatusBar* bar) {
    int right = bar->width() - 12;
    for (const auto* child : bar->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        if (child->isVisible() && qobject_cast<const QSizeGrip*>(child) == nullptr) {
            right = (std::min)(right, child->x() - 2);
        }
    }
    return right - 6;
}

/// The AI status label in @p bar: the permanent label the AI Assistant tab fills.
QLabel* aiStatusLabel(const QStatusBar* bar) {
    for (auto* label : bar->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
        if (label->text().startsWith(QStringLiteral("AI:"))) {
            return label;
        }
    }
    return nullptr;
}

}  // namespace

// A timed status message is drawn only up to the status bar's first permanent widget. On the AI
// Assistant tab the AI status label, which takes all the free space, left it about 77 px at 1280
// px: every timed message there was cut after a dozen letters. The label now stands aside while a
// timed message shows. Measured with Segoe UI as Windows draws it at the theme's size, against
// the room the bar gives the message with the AI label present before it: the iPerf3 server's
// refusal (cut off at "S.A" in the capture of 1db87df4) and the longest other timed message fit
// whole at 1280 px, and at 1024 px their leading clause does. The room is measured with the
// offscreen platform's box glyphs, which make the permanent widgets wider than they are, so the
// real room is larger still.
void MainWindowHelpTests::iperfAdministratorMessageFitsTheStatusBar() {
    struct Message {
        QString text;
        QString leading;  ///< What must not be cut at 1024 px.
    };
    const QList<Message> messages{
        {sak::BandwidthTester::needsElevationMessage(),
         QStringLiteral("Run as Admin to start the iPerf3 server")},
        {QStringLiteral("Disk inventory changed while the operation was being reviewed. Refresh "
                        "and confirm it again."),
         QStringLiteral("Disk inventory changed while the operation was being reviewed.")}};
    sak::ui::applyWindows11Theme(*qApp, sak::ui::AppThemeMode::Light);
    for (const int width : {1280, 1024}) {
        sak::MainWindow window;
        window.resize(width, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QApplication::processEvents();
        QStatusBar* bar = window.statusBar();
        QLabel* ai = aiStatusLabel(bar);
        QVERIFY2(ai != nullptr && ai->isVisible(), "the AI Assistant tab opens with its label");
        const QFont font = bar->font();
        const int pixels = font.pixelSize() > 0 ? font.pixelSize()
                                                : qRound(font.pointSizeF() * 96.0 / 72.0);
        for (const Message& message : messages) {
            QVERIFY(message.text.startsWith(message.leading));
            QVERIFY(QMetaObject::invokeMethod(
                &window, "updateStatus", Q_ARG(QString, message.text), Q_ARG(int, 5000)));
            const int room = timedMessageRoom(bar);
            const int whole = segoeUiTextWidth(message.text, pixels);
            const int leading = segoeUiTextWidth(message.leading, pixels);
            qInfo().noquote() << width << "px window:" << room << "px of room;" << whole << "px for"
                              << message.text << ";" << leading << "px for its lead";
            QVERIFY2(leading > 0 && (width == 1280 ? whole : leading) <= room,
                     qPrintable(QStringLiteral("%1 px: %2 px (lead %3 px) in %4 px: %5")
                                    .arg(width)
                                    .arg(whole)
                                    .arg(leading)
                                    .arg(room)
                                    .arg(message.text)));
        }
        bar->clearMessage();
    }
}

// The AI status label's place is worked out afresh, never toggled: a message replacing a message
// keeps it aside, the bar clearing brings it back, and on another tab, where it is not shown, a
// message clearing does not show it.
void MainWindowHelpTests::aiStatusLabelStandsAsideOnlyWhileAMessageShows() {
    sak::ui::applyWindows11Theme(*qApp, sak::ui::AppThemeMode::Light);
    sak::MainWindow window;
    window.resize(1280, 800);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QApplication::processEvents();
    QStatusBar* bar = window.statusBar();
    QLabel* ai = aiStatusLabel(bar);
    QVERIFY2(ai != nullptr && ai->isVisible(), "the AI Assistant tab opens with its label");
    bar->showMessage(QStringLiteral("first"), 5000);
    bar->showMessage(QStringLiteral("second"), 5000);
    QVERIFY(!ai->isVisible());
    bar->clearMessage();
    QVERIFY(ai->isVisible());
    QTabWidget* tabs = nullptr;
    for (auto* candidate : window.findChildren<QTabWidget*>()) {
        if (candidate->count() > 1 &&
            candidate->tabText(candidate->currentIndex()) == QStringLiteral("AI Assistant")) {
            tabs = candidate;
        }
    }
    QVERIFY2(tabs != nullptr, "the main tabs, on the AI Assistant tab");
    const int ai_tab = tabs->currentIndex();
    tabs->setCurrentIndex(ai_tab == 0 ? 1 : 0);
    QApplication::processEvents();
    QVERIFY(!ai->isVisible());
    bar->showMessage(QStringLiteral("elsewhere"), 5000);
    bar->clearMessage();
    QVERIFY(!ai->isVisible());
    tabs->setCurrentIndex(ai_tab);
    QApplication::processEvents();
    QVERIFY(ai->isVisible());
}

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
