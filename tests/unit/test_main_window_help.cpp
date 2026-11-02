// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_main_window_help.cpp
/// @brief What the Help and About tabs send people to and tell them.

#include "sak/main_window.h"

#include <QApplication>
#include <QDesktopServices>
#include <QPushButton>
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

QTEST_MAIN(MainWindowHelpTests)
#include "test_main_window_help.moc"
