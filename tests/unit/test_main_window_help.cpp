// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_main_window_help.cpp
/// @brief What the Help and About tabs send people to and tell them.

#include "sak/main_window.h"

#include <QApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTextBrowser>
#include <QtTest/QtTest>
#include <QUrl>

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

QTEST_MAIN(MainWindowHelpTests)
#include "test_main_window_help.moc"
