// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_ux.cpp
/// @brief Unit tests for Phase 4 elevation UX components
///
///  - Shield icon extraction (getShieldIcon)
///  - Elevation banner creation (createElevationBanner)
///  - Banner conditional visibility (admin vs. standard)

#include "sak/elevation_banner.h"
#include "sak/elevation_manager.h"
#include "sak/shield_icon.h"
#include "sak/style_constants.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QTest>
#include <QWidget>

class TestElevationUx : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ======================================================================
    // Shield Icon
    // ======================================================================

    void testGetShieldIconReturnsNonNull() {
        QIcon icon = sak::getShieldIcon();
        // On Windows with shell32.dll the icon should be valid
        QVERIFY(!icon.isNull());
    }

    void testGetShieldIconIsCached() {
        QIcon first = sak::getShieldIcon();
        QIcon second = sak::getShieldIcon();
        // Both calls should return the same cached icon data
        QCOMPARE(first.cacheKey(), second.cacheKey());
    }

    void testGetShieldIconHasPixmap() {
        QIcon icon = sak::getShieldIcon();
        if (icon.isNull()) {
            QSKIP("Shield icon extraction not available on this system");
        }
        // Should produce a pixmap at small icon size (typically 16x16)
        QPixmap pm = icon.pixmap(16, 16);
        QVERIFY(!pm.isNull());
        QVERIFY(pm.width() > 0);
        QVERIFY(pm.height() > 0);
    }

    void testGetShieldIconMultipleSizesValid() {
        QIcon icon = sak::getShieldIcon();
        if (icon.isNull()) {
            QSKIP("Shield icon extraction not available");
        }
        // Icon should scale to various requested sizes
        QPixmap small_pm = icon.pixmap(16, 16);
        QPixmap large_pm = icon.pixmap(32, 32);
        QVERIFY(!small_pm.isNull());
        QVERIFY(!large_pm.isNull());
    }

    // ======================================================================
    // Elevation Banner
    // ======================================================================

    void testCreateElevationBannerWhenNotAdmin() {
        if (sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner != nullptr);
        QCOMPARE(banner->objectName(), QStringLiteral("elevationBanner"));
    }

    void testCreateElevationBannerHasLayout() {
        if (sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner != nullptr);

        auto* layout = qobject_cast<QHBoxLayout*>(banner->layout());
        QVERIFY(layout != nullptr);
        // Should have icon label + text label = 2 items
        QCOMPARE(layout->count(), 2);
    }

    void testCreateElevationBannerTextContent() {
        if (sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner != nullptr);

        // Find the text label (second widget in layout)
        auto* layout = banner->layout();
        QVERIFY(layout->count() >= 2);
        auto* text_label = qobject_cast<QLabel*>(layout->itemAt(1)->widget());
        QVERIFY(text_label != nullptr);
        QVERIFY(text_label->text().contains("administrator"));
    }

    void testCreateElevationBannerParentship() {
        if (sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner != nullptr);
        QCOMPARE(banner->parent(), &parent);
    }

    void testCreateElevationBannerReturnsNullWhenAdmin() {
        if (!sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner == nullptr);
    }

    // ======================================================================
    // Style Constants Sanity
    // ======================================================================

    void testInfoPanelColorDefined() {
        // Verify the info panel color token has a valid hex color format
        QString color = QLatin1String(sak::ui::kColorBgInfoPanel);
        QVERIFY(color.startsWith('#'));
        QCOMPARE(color.length(), 7);  // #RRGGBB
    }

    void testPrimaryColorDefined() {
        QString color = QLatin1String(sak::ui::kColorPrimary);
        QVERIFY(color.startsWith('#'));
        QCOMPARE(color.length(), 7);
    }
};

QTEST_MAIN(TestElevationUx)

#include "test_elevation_ux.moc"
