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

    // The widget size above is set unconditionally by setFixedSize() and says
    // nothing about the asset, so pin the rendered content instead: a 1024x512
    // source must be expanded to cover and then centre-cropped, leaving the whole
    // square opaque -- not a letterboxed strip and not a dropped (null) pixmap.
    // Sampled relative to the grabbed image so the check is device-pixel-ratio
    // independent, and well inside the corner radius / shadow padding.
    const QImage rendered = splash.grab().toImage();
    QCOMPARE(rendered.pixelColor(rendered.width() / 2, rendered.height() / 6), QColor(Qt::black));
    QCOMPARE(rendered.pixelColor(rendered.width() / 2, rendered.height() / 2), QColor(Qt::black));
    QCOMPARE(rendered.pixelColor(rendered.width() / 2, (rendered.height() * 5) / 6),
             QColor(Qt::black));
}

QTEST_MAIN(SplashScreenTests)

#include "test_splash_screen.moc"
