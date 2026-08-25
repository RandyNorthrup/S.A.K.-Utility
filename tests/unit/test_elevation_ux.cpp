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

#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSet>
#include <QTest>
#include <QWidget>

namespace {

/// True when at least one pixel is actually painted (alpha > 0).
///
/// iconColorBitmapToImage() pre-fills its QImage with Qt::transparent BEFORE the GetDIBits copy,
/// so that a partial copy never leaks garbage. A non-null pixmap with positive width and height
/// therefore proves only that a correctly shaped buffer exists -- never that any shield pixel was
/// copied out of the DIB. Mirrors the helper the file-management panel suite already uses for
/// exactly this trap.
bool hasVisiblePixel(const QPixmap& pixmap) {
    if (pixmap.isNull()) {
        return false;
    }
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

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
        // QIcon::cacheKey() returns 0 for a NULL icon, so comparing two cache keys is satisfied by
        // two FAILED extractions comparing equal -- the assertion held for "no icon data" while
        // its comment claimed "the same cached icon data". Require a real icon first, so a
        // refusal cannot score the same as a success.
        QVERIFY2(!first.isNull(),
                 "shield extraction produced no icon, so cacheKey() is a vacuous 0");
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
        // The extracted shield must carry REAL pixels, not merely a correctly shaped buffer. The
        // image is filled with Qt::transparent before the GetDIBits copy, so the three checks
        // above are all satisfied by a fully transparent image -- an invisible UAC cue on every
        // elevated-action button, with all four shield tests still green.
        QVERIFY2(hasVisiblePixel(pm), "shield icon pixmap must contain visible (alpha > 0) pixels");
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
        QVERIFY2(hasVisiblePixel(small_pm), "16x16 shield pixmap must have visible pixels");
        QVERIFY2(hasVisiblePixel(large_pm), "32x32 shield pixmap must have visible pixels");
        // This test is named for SIZES and asserted none. QIcon never upscales, so both requests
        // come from the one source pixmap the extractor produced, and the flag that decides its
        // dimensions -- SHGSI_SMALLICON -- is observed nowhere else in the file. A shield
        // extracted at 256x256 would satisfy every non-null check above while the banner and the
        // buttons scaled a large bitmap down.
        QCOMPARE(small_pm.size(), QSize(16, 16));
        QVERIFY2(large_pm.width() <= 32 && large_pm.height() <= 32,
                 qPrintable(QStringLiteral("a 32x32 request returned %1x%2")
                                .arg(large_pm.width())
                                .arg(large_pm.height())));
        // QIcon does not upscale, so the source really is the small icon: the 32px request
        // cannot come back larger than the 16px one.
        QCOMPARE(large_pm.size(), small_pm.size());
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
        // The objectName is only load-bearing BECAUSE the stylesheet selector keys on it. Pinning
        // the name while never observing the sheet proves the two halves of that pair
        // independently of each other: the banner can carry the right objectName and no
        // stylesheet at all -- an unstyled, invisible warning strip -- with every assertion in all
        // five banner tests still green. testInfoPanelColorDefined cannot stand in for this: it
        // pins the token in isolation, and that token never even reaches the banner.
        const QString sheet = banner->styleSheet();
        QVERIFY2(!sheet.isEmpty(), "the elevation banner must carry its stylesheet");
        QCOMPARE(sheet, sak::ui::elevationBannerStyle());
        QVERIFY2(sheet.contains(QStringLiteral("QFrame#elevationBanner")), qPrintable(sheet));
    }

    void testCreateElevationBannerHasLayout() {
        if (sak::ElevationManager::isElevated()) {
            QSKIP("Test only meaningful when not running as admin");
        }

        QWidget parent;
        QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner != nullptr);

        auto* layout = banner->layout();
        QVERIFY(layout != nullptr);
        // Contract: the banner visibly presents its warning ICON (a shield pixmap)
        // beside the message. Assert an icon-bearing QLabel is laid out -- not the exact
        // child-item count -- so adding a spacer or restructuring the layout stays green,
        // while dropping the icon (the user-visible cue) goes red. The message text itself
        // is pinned by testCreateElevationBannerTextContent.
        // Copy the two observables out INSIDE the loop, so no raw pointer outlives it.
        bool has_icon = false;
        QPixmap icon_pixmap;
        int icon_slot_width = 0;
        for (int i = 0; i < layout->count(); ++i) {
            auto* label = qobject_cast<QLabel*>(layout->itemAt(i)->widget());
            if (label != nullptr && !label->pixmap().isNull()) {
                has_icon = true;
                icon_pixmap = label->pixmap();
                icon_slot_width = label->width();
                break;
            }
        }
        QVERIFY2(has_icon, "elevation banner must present its shield icon");
        // "Some label carries some non-null pixmap" leaves both numbers that make the shield
        // legible unasserted: a 1x1 pixmap, or a 64px one overflowing a slot sized for 20,
        // satisfies the probe identically. kElevationBannerIconWidth exists for exactly one call
        // site and no test looked at it.
        QVERIFY2(hasVisiblePixel(icon_pixmap), "the banner shield must have real pixels");
        QCOMPARE(icon_pixmap.size(), QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
        QCOMPARE(icon_slot_width, sak::detail::kElevationBannerIconWidth);
        // The icon must fit the slot it is given, or it is clipped in the shipped banner.
        QVERIFY2(icon_pixmap.width() <= sak::detail::kElevationBannerIconWidth,
                 qPrintable(QStringLiteral("a %1px shield does not fit a %2px slot")
                                .arg(icon_pixmap.width())
                                .arg(sak::detail::kElevationBannerIconWidth)));
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
        // The banner is a narrow horizontal strip holding a full two-clause sentence, and it only
        // renders correctly because the label WRAPS and because it is given the layout's spare
        // width. QLabel::text() is identical whether the label wraps or clips and whether it gets
        // stretch or its bare size hint, so the exact-sentence compare below -- correct as far as
        // it goes -- can see neither, and nothing else in the file observes them.
        QVERIFY2(text_label->wordWrap(),
                 "the banner sentence must wrap, or it is clipped in a narrow panel");
        QCOMPARE(layout->itemAt(1)->widget(), text_label);
        auto* box = qobject_cast<QHBoxLayout*>(layout);
        QVERIFY(box != nullptr);
        QCOMPARE(box->stretch(1), 1);
        QCOMPARE(box->stretch(0), 0);
        // No QTranslator is installed, so tr() returns the source string verbatim -- pin the
        // whole sentence; contains("administrator") passes on any string mentioning the word.
        QCOMPARE(text_label->text(),
                 QStringLiteral("Some operations on this tab require administrator privileges. You "
                                "will be prompted when needed."));
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
        const QFrame* banner = sak::createElevationBanner(&parent);
        QVERIFY(banner == nullptr);
    }

    // ======================================================================
    // Style Constants Sanity
    // ======================================================================

    void testInfoPanelColorDefined() {
        // Verify the info panel color token has a valid hex color format
        QString color = QLatin1String(sak::ui::kColorBgInfoPanel);
        // Pin the exact token; length==7 alone passes for any '#RRGGBB' (e.g. "#000000").
        QCOMPARE(color, QStringLiteral("#e0f2fe"));  // sky-100
    }

    void testPrimaryColorDefined() {
        QString color = QLatin1String(sak::ui::kColorPrimary);
        QCOMPARE(color, QStringLiteral("#3b82f6"));  // blue-500
    }

    void testSharedIconResourceConstantsLoad() {
        const QStringList icons = {sak::ui::kIconPasswordEyeOpen,
                                   sak::ui::kIconPasswordEyeClosed,
                                   sak::ui::kIconPasswordEyeOpenOnTone,
                                   sak::ui::kIconPasswordEyeClosedOnTone,
                                   sak::ui::kIconSelectorChevronUpOnTone,
                                   sak::ui::kIconSelectorChevronDownOnTone,
                                   sak::ui::kIconSelectorChevronLeft,
                                   sak::ui::kIconSelectorChevronLeftDark,
                                   sak::ui::kIconSelectorChevronLeftOnTone,
                                   sak::ui::kIconSelectorChevronRight,
                                   sak::ui::kIconSelectorChevronRightDark,
                                   sak::ui::kIconSelectorChevronRightOnTone};
        for (const QString& icon_path : icons) {
            QVERIFY2(QFile::exists(icon_path), qPrintable(icon_path));
            const QPixmap pixmap = QIcon(icon_path).pixmap(sak::ui::kUiIconSmall,
                                                           sak::ui::kUiIconSmall);
            QVERIFY2(!pixmap.isNull(), qPrintable(icon_path));
            QVERIFY2(hasVisiblePixel(pixmap), qPrintable(icon_path));
        }
        // Twelve distinct design tokens were checked only for "names a file that exists and
        // renders". Any one of them could be repointed at ANOTHER token's asset -- the single
        // most likely copy-paste failure in a block of twelve near-identical lines differing by
        // one word -- and the loop stayed fully green, because the target file also exists and
        // also renders. The accessor loop below cannot catch it either: those accessors are
        // written in terms of these same constants, so both sides of any such compare move
        // together. Distinctness is the only assertion that separates the twelve.
        QCOMPARE(QSet<QString>(icons.begin(), icons.end()).size(), icons.size());

        for (const QIcon& icon : {sak::ui::passwordEyeOpenToolButtonIcon(),
                                  sak::ui::passwordEyeClosedToolButtonIcon(),
                                  sak::ui::selectorChevronUpToolButtonIcon(),
                                  sak::ui::selectorChevronDownToolButtonIcon(),
                                  sak::ui::selectorChevronLeftToolButtonIcon(),
                                  sak::ui::selectorChevronRightToolButtonIcon()}) {
            QVERIFY(!icon.pixmap(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall).isNull());
            // Each accessor builds a TWO-entry QIcon: the on-tone asset for Normal and the plain
            // (or dark-theme) asset for Disabled. Requesting only the default mode left the
            // Disabled arm of all six unrendered -- deleting it entirely was invisible -- so a
            // disabled toolbutton would fall back to a washed-out Normal asset instead of the
            // asset chosen for it.
            const QPixmap disabled =
                icon.pixmap(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall, QIcon::Disabled);
            QVERIFY(!disabled.isNull());
            QVERIFY(hasVisiblePixel(disabled));
        }

        // ... and the two eye accessors must not be interchangeable: "not null" is satisfied by
        // any SVG in the bundle, so open and closed could be swapped -- a password field whose
        // toggle shows the wrong state -- with every assertion above still green.
        const QPixmap eye_open = sak::ui::passwordEyeOpenToolButtonIcon().pixmap(
            sak::ui::kUiIconSmall, sak::ui::kUiIconSmall);
        const QPixmap eye_closed = sak::ui::passwordEyeClosedToolButtonIcon().pixmap(
            sak::ui::kUiIconSmall, sak::ui::kUiIconSmall);
        QVERIFY2(eye_open.toImage() != eye_closed.toImage(),
                 "the open-eye and closed-eye icons must render differently");
        const QPixmap chevron_left = sak::ui::selectorChevronLeftToolButtonIcon().pixmap(
            sak::ui::kUiIconSmall, sak::ui::kUiIconSmall);
        const QPixmap chevron_right = sak::ui::selectorChevronRightToolButtonIcon().pixmap(
            sak::ui::kUiIconSmall, sak::ui::kUiIconSmall);
        QVERIFY2(chevron_left.toImage() != chevron_right.toImage(),
                 "the left and right chevrons must render differently");
    }
};

QTEST_MAIN(TestElevationUx)

#include "test_elevation_ux.moc"
