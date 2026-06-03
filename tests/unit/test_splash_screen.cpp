// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/splash_screen.h"

#include <QPixmap>
#include <QSize>
#include <QtTest/QtTest>

class SplashScreenTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void splashWindowIsFixedAtRequestedSize();
    void splashAcceptsNonSquareAssets();
};

void SplashScreenTests::splashWindowIsFixedAtRequestedSize() {
    QPixmap pixmap(QSize(1024, 1024));
    pixmap.fill(Qt::black);
    sak::ui::SplashScreen splash(pixmap);

    QCOMPARE(splash.size(), QSize(sak::ui::kSplashSizePx, sak::ui::kSplashSizePx));
    QCOMPARE(splash.minimumSize(), QSize(sak::ui::kSplashSizePx, sak::ui::kSplashSizePx));
    QCOMPARE(splash.maximumSize(), QSize(sak::ui::kSplashSizePx, sak::ui::kSplashSizePx));
}

void SplashScreenTests::splashAcceptsNonSquareAssets() {
    QPixmap pixmap(QSize(1024, 512));
    pixmap.fill(Qt::black);
    sak::ui::SplashScreen splash(pixmap);

    QCOMPARE(splash.size(), QSize(sak::ui::kSplashSizePx, sak::ui::kSplashSizePx));
}

QTEST_MAIN(SplashScreenTests)

#include "test_splash_screen.moc"
