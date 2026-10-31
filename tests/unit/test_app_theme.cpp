// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_theme.cpp
/// @brief The main window's appearance: the theme follows Windows (Light / Dark / System), every
/// tab renders in both, and surfaces are flat with 1px borders instead of drop shadows.

#include "sak/config_manager.h"
#include "sak/info_button.h"
#include "sak/main_window.h"
#include "sak/style_constants.h"
#include "sak/windows11_theme.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGroupBox>
#include <QImage>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStyleHints>
#include <QTabWidget>
#include <QtTest/QtTest>

#include <memory>

namespace {

constexpr int kBorderProbeInsetPx = 4;
constexpr int kReviewWindowWidth = 1280;
constexpr int kReviewWindowHeight = 860;

/// Shows every tab and inner tab of the real main window once (each tool panel is built when
/// its tab is first shown), so the widget tree a user can reach exists and is polished.
void showEveryTab(QWidget& window) {
    bool more = true;
    while (more) {
        more = false;
        const auto tab_widgets = window.findChildren<QTabWidget*>();
        for (QTabWidget* tabs : tab_widgets) {
            if (tabs->property("sakThemeTestVisited").toBool()) {
                continue;
            }
            tabs->setProperty("sakThemeTestVisited", true);
            more = true;
            for (int index = 0; index < tabs->count(); ++index) {
                tabs->setCurrentIndex(index);
                QCoreApplication::processEvents();
            }
            tabs->setCurrentIndex(0);
        }
    }
}

/// The surfaces the retired shadow pass used to decorate: group boxes and card frames.
QList<QWidget*> borderedSurfaces(const QWidget& window) {
    QList<QWidget*> surfaces;
    const auto widgets = window.findChildren<QWidget*>();
    for (QWidget* widget : widgets) {
        if ((qobject_cast<QGroupBox*>(widget) != nullptr) || widget->property("sakCard").toBool()) {
            surfaces.append(widget);
        }
    }
    return surfaces;
}

using sak::ui::AppThemeMode;
using sak::ui::ThemePreference;

constexpr int kDarkLightnessCeiling = 128;
// Muted text (palette Dark, the File Explorer's secondary text) against the base surface.
constexpr int kMutedTextMinimumLightnessGap = 60;
constexpr int kButtonFillProbeInsetPx = 6;

/// Colours that belong to one theme only. A widget's own style sheet that names one of them
/// paints that theme's surface or text whatever the app theme is, so it must not appear while
/// the other theme is applied. (Theme-neutral colours -- the accent, status colours, white text
/// on a coloured button -- are not listed.)
const QStringList& lightOnlySurfaces() {
    static const QStringList kColours = {QStringLiteral("#ffffff"),
                                         QStringLiteral("#f3f5f9"),
                                         QStringLiteral("#eef2f7"),
                                         QStringLiteral("#f8fafc"),
                                         QStringLiteral("#e0f2fe"),
                                         QStringLiteral("#fef3c7"),
                                         QStringLiteral("#fee2e2"),
                                         QStringLiteral("#dbeafe"),
                                         QStringLiteral("#d1fae5"),
                                         QStringLiteral("#fce7f3"),
                                         QStringLiteral("#fef9c3"),
                                         QStringLiteral("#f3f4f6")};
    return kColours;
}
const QStringList& lightOnlyText() {
    static const QStringList kColours = {QStringLiteral("#0f172a"),
                                         QStringLiteral("#1e293b"),
                                         QStringLiteral("#334155"),
                                         QStringLiteral("#475569")};
    return kColours;
}
const QStringList& darkOnlySurfaces() {
    static const QStringList kColours = {QStringLiteral("#0f172a"),
                                         QStringLiteral("#111827"),
                                         QStringLiteral("#1e293b"),
                                         QStringLiteral("#172033"),
                                         QStringLiteral("#0b1220"),
                                         QStringLiteral("#243247"),
                                         QStringLiteral("#29384f")};
    return kColours;
}
const QStringList& darkOnlyText() {
    static const QStringList kColours = {QStringLiteral("#f8fafc"), QStringLiteral("#e2e8f0")};
    return kColours;
}

/// "background: #fff" and friends in a widget's own style sheet that the other theme owns.
QStringList foreignColours(const QString& sheet, AppThemeMode mode) {
    static const QRegularExpression kDeclaration(QStringLiteral(
        R"((?<![-\w])(background(?:-color)?|color)\s*:\s*(#[0-9a-fA-F]{6}|#[0-9a-fA-F]{3})\b)"));
    const bool dark = mode == AppThemeMode::Dark;
    QStringList found;
    auto matches = kDeclaration.globalMatch(sheet);
    while (matches.hasNext()) {
        const auto match = matches.next();
        QString colour = match.captured(2).toLower();
        if (colour.size() == 4) {
            colour = QStringLiteral("#%1%1%2%2%3%3").arg(colour[1]).arg(colour[2]).arg(colour[3]);
        }
        const bool text = match.captured(1) == QStringLiteral("color");
        const QStringList& foreign = dark ? (text ? lightOnlyText() : lightOnlySurfaces())
                                          : (text ? darkOnlyText() : darkOnlySurfaces());
        if (foreign.contains(colour)) {
            found << match.captured(0);
        }
    }
    return found;
}

/// Makes @p tabs reachable on screen: every tab widget around it shows the page it sits on.
void revealTabWidget(QTabWidget* tabs) {
    for (QWidget* child = tabs; child != nullptr; child = child->parentWidget()) {
        auto* outer = qobject_cast<QTabWidget*>(child->parentWidget());
        for (QWidget* up = child->parentWidget(); outer == nullptr && up != nullptr;
             up = up->parentWidget()) {
            outer = qobject_cast<QTabWidget*>(up);
        }
        if (outer == nullptr) {
            return;
        }
        for (int index = 0; index < outer->count(); ++index) {
            if (outer->widget(index)->isAncestorOf(tabs)) {
                outer->setCurrentIndex(index);
            }
        }
        child = outer;
    }
}

QPushButton* themeButton(const QWidget& window) {
    const auto buttons = window.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == QStringLiteral("App theme")) {
            return button;
        }
    }
    return nullptr;
}

QAction* themeAction(const QPushButton& button, const QString& text) {
    const auto actions = button.menu()->actions();
    for (QAction* action : actions) {
        if (action->text().remove(QLatin1Char('&')) == text) {
            return action;
        }
    }
    return nullptr;
}

void tellAppWindowsSchemeIs(Qt::ColorScheme scheme) {
    Q_EMIT QGuiApplication::styleHints()->colorSchemeChanged(scheme);
    QCoreApplication::processEvents();
}

QString describe(const QWidget* widget) {
    QString text = QString::fromLatin1(widget->metaObject()->className());
    if (const auto* group = qobject_cast<const QGroupBox*>(widget)) {
        text += QStringLiteral(" \"%1\"").arg(group->title());
    }
    return text;
}

}  // namespace

class AppThemeTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void themeSettingIsSavedAndDefaultsToSystem();
    void systemPreferenceFollowsWindowsLive();
    void explicitPreferenceIgnoresWindows();
    void statusBarThemeMenuSetsThePreference();
    void noPerPanelDarkSwitchRemains();
    void everyTabRendersInTheTheme_data();
    void everyTabRendersInTheTheme();
    void mutedTextAndDisabledButtonsSuitTheTheme_data();
    void mutedTextAndDisabledButtonsSuitTheTheme();
    void noSurfaceCarriesADropShadow();
    void infoPopupIsOutlinedNotShadowed();
    void cardsAndGroupsDrawAOnePixelBorder_data();
    void cardsAndGroupsDrawAOnePixelBorder();

private:
    std::unique_ptr<sak::MainWindow> m_window;
};

void AppThemeTests::initTestCase() {
    // What main() does at startup, then the real window with every tab built.
    sak::ConfigManager::instance().setThemePreference(QStringLiteral("light"));
    sak::ui::initializeAppTheme(*qApp);
    m_window = std::make_unique<sak::MainWindow>();
    m_window->resize(kReviewWindowWidth, kReviewWindowHeight);
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    showEveryTab(*m_window);
}

void AppThemeTests::cleanupTestCase() {
    m_window.reset();
    sak::ui::setThemePreference(*qApp, ThemePreference::System);
}

// One saved setting in the portable config (ConfigManager, ui/theme), System when unset or
// unreadable; the per-panel Dark switches it replaces were never saved at all.
void AppThemeTests::themeSettingIsSavedAndDefaultsToSystem() {
    auto& config = sak::ConfigManager::instance();
    config.remove(QStringLiteral("ui/theme"));
    QCOMPARE(sak::ui::savedThemePreference(), ThemePreference::System);
    config.setValue(QStringLiteral("ui/theme"), QStringLiteral("purple"));
    QCOMPARE(sak::ui::savedThemePreference(), ThemePreference::System);

    sak::ui::setThemePreference(*qApp, ThemePreference::Dark);
    QCOMPARE(config.getThemePreference(), QStringLiteral("dark"));
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Dark);
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    QCOMPARE(config.getThemePreference(), QStringLiteral("light"));
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Light);
}

// System follows the Windows app theme while the app runs (QStyleHints::colorSchemeChanged).
void AppThemeTests::systemPreferenceFollowsWindowsLive() {
    sak::ui::setThemePreference(*qApp, ThemePreference::System);
    tellAppWindowsSchemeIs(Qt::ColorScheme::Dark);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Dark);
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < kDarkLightnessCeiling);
    tellAppWindowsSchemeIs(Qt::ColorScheme::Light);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Light);
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= kDarkLightnessCeiling);
}

void AppThemeTests::explicitPreferenceIgnoresWindows() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    tellAppWindowsSchemeIs(Qt::ColorScheme::Dark);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Light);
    sak::ui::setThemePreference(*qApp, ThemePreference::Dark);
    tellAppWindowsSchemeIs(Qt::ColorScheme::Light);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Dark);
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
}

// The one place to choose: the status bar's Theme button, a menu of System, Light and Dark.
void AppThemeTests::statusBarThemeMenuSetsThePreference() {
    QPushButton* button = themeButton(*m_window);
    QVERIFY(button != nullptr);
    QVERIFY(button->menu() != nullptr);
    QAction* dark = themeAction(*button, QStringLiteral("Dark"));
    QAction* light = themeAction(*button, QStringLiteral("Light"));
    QAction* system = themeAction(*button, QStringLiteral("System"));
    QVERIFY(dark != nullptr && light != nullptr && system != nullptr);

    dark->trigger();
    QCOMPARE(sak::ConfigManager::instance().getThemePreference(), QStringLiteral("dark"));
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Dark);
    QVERIFY(button->text().contains(QStringLiteral("Dark")));
    QVERIFY(dark->isChecked());

    system->trigger();
    QCOMPARE(sak::ConfigManager::instance().getThemePreference(), QStringLiteral("system"));
    tellAppWindowsSchemeIs(Qt::ColorScheme::Dark);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Dark);

    light->trigger();
    QCOMPARE(sak::ui::currentThemeMode(*qApp), AppThemeMode::Light);
    QVERIFY(light->isChecked() && !dark->isChecked() && !system->isChecked());
}

void AppThemeTests::noPerPanelDarkSwitchRemains() {
    QStringList switches;
    const auto widgets = m_window->findChildren<QWidget*>();
    for (const QWidget* widget : widgets) {
        if (widget->accessibleName() == QStringLiteral("Toggle dark mode")) {
            switches << describe(widget);
        }
    }
    QVERIFY2(switches.isEmpty(), qPrintable(switches.join(QStringLiteral(", "))));
}

void AppThemeTests::everyTabRendersInTheTheme_data() {
    QTest::addColumn<ThemePreference>("preference");
    QTest::addColumn<AppThemeMode>("mode");
    QTest::newRow("light") << ThemePreference::Light << AppThemeMode::Light;
    QTest::newRow("dark") << ThemePreference::Dark << AppThemeMode::Dark;
}

// Switched live, with every tab already built: no widget keeps a colour of the other theme in
// its own style sheet. With SAK_THEME_SCREENSHOT_DIR set each tab is also saved as a PNG for
// review (light-<tabs>-<tab>.png, dark-...).
void AppThemeTests::everyTabRendersInTheTheme() {
    QFETCH(ThemePreference, preference);
    QFETCH(AppThemeMode, mode);
    sak::ui::setThemePreference(*qApp, preference);
    QCOMPARE(sak::ui::currentThemeMode(*qApp), mode);
    const QString shots = qEnvironmentVariable("SAK_THEME_SCREENSHOT_DIR");
    const QString prefix = QString::fromLatin1(QTest::currentDataTag());

    QStringList foreign;
    QSet<const QWidget*> seen;
    const auto tab_widgets = m_window->findChildren<QTabWidget*>();
    for (QTabWidget* tabs : tab_widgets) {
        revealTabWidget(tabs);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);
            QCoreApplication::processEvents();
            if (!shots.isEmpty() && tabs->isVisible()) {
                QString name = QStringLiteral("%1-%2-%3")
                                   .arg(prefix, tabs->accessibleName(), tabs->tabText(index));
                name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9-]+")),
                             QStringLiteral("_"));
                m_window->grab().save(QDir(shots).filePath(name + QStringLiteral(".png")));
            }
            const auto widgets = tabs->widget(index)->findChildren<QWidget*>();
            for (const QWidget* widget : widgets) {
                if (seen.contains(widget)) {
                    continue;
                }
                seen.insert(widget);
                const auto colours = foreignColours(widget->styleSheet(), mode);
                for (const QString& colour : colours) {
                    foreign << QStringLiteral("%1 [%2]: %3")
                                   .arg(describe(widget), widget->objectName(), colour);
                }
            }
        }
    }
    foreign.removeDuplicates();
    QVERIFY2(foreign.isEmpty(), qPrintable(foreign.join(QStringLiteral("\n"))));
}

// Owner decision 2026-10-24: 1px borders replace the app-wide drop-shadow effect. An
// application event filter used to put a QGraphicsDropShadowEffect on every group box, card and
// rounded, filled widget as it was polished or shown.
void AppThemeTests::noSurfaceCarriesADropShadow() {
    QStringList shadowed;
    auto widgets = m_window->findChildren<QWidget*>();
    widgets.prepend(m_window.get());
    for (const QWidget* widget : widgets) {
        if (widget->graphicsEffect() != nullptr) {
            shadowed << describe(widget);
        }
    }
    QVERIFY2(shadowed.isEmpty(), qPrintable(shadowed.join(QStringLiteral(", "))));
}

// The (i) buttons' popup drew its own graphics-effect shadow inside a popup window, which Windows
// already shadows. Its 1px border (infoPopupFrameStyle) is the outline now.
void AppThemeTests::infoPopupIsOutlinedNotShadowed() {
    // The (i) buttons live in dialogs and wizards; one stands alone here.
    QWidget host;
    auto* button = new sak::InfoButton(QStringLiteral("Explains the setting next to it."), &host);
    host.show();
    QVERIFY(QTest::qWaitForWindowExposed(&host));
    button->click();
    QWidget* popup = nullptr;
    QTRY_VERIFY([&popup]() {
        const auto windows = QApplication::topLevelWidgets();
        for (QWidget* window : windows) {
            if (window->objectName() == QStringLiteral("sakInfoPopup") && window->isVisible()) {
                popup = window;
                return true;
            }
        }
        return false;
    }());
    QVERIFY(popup->graphicsEffect() == nullptr);
    QVERIFY(popup->styleSheet().contains(QStringLiteral("border: 1px solid")));
    popup->close();
}

void AppThemeTests::cardsAndGroupsDrawAOnePixelBorder_data() {
    everyTabRendersInTheTheme_data();
}

void AppThemeTests::mutedTextAndDisabledButtonsSuitTheTheme_data() {
    everyTabRendersInTheTheme_data();
}

// Two colours that only suited the light theme: the dark palette's Dark role was a pressed-surface
// shade, so the File Explorer's secondary text (palette(dark)) all but vanished on the dark base;
// and a disabled button was filled with an opaque light slate that glared on the dark page.
void AppThemeTests::mutedTextAndDisabledButtonsSuitTheTheme() {
    QFETCH(ThemePreference, preference);
    QFETCH(AppThemeMode, mode);
    sak::ui::setThemePreference(*qApp, preference);
    const QPalette palette = QApplication::palette();
    const int gap =
        qAbs(palette.color(QPalette::Dark).lightness() - palette.color(QPalette::Base).lightness());
    QVERIFY2(gap >= kMutedTextMinimumLightnessGap,
             qPrintable(QStringLiteral("muted text is %1 lightness from the base").arg(gap)));

    QWidget page;
    auto* button = new QPushButton(QStringLiteral("Send"), &page);
    button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    button->setEnabled(false);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    // The page under the button, so the translucent fill is seen over the theme's own surface.
    const QImage image = page.grab(button->geometry()).toImage();
    const int fill = QColor(image.pixel(kButtonFillProbeInsetPx, image.height() / 2)).lightness();
    if (mode == AppThemeMode::Dark) {
        QVERIFY2(fill < kDarkLightnessCeiling,
                 qPrintable(
                     QStringLiteral("disabled fill lightness %1 on the dark page").arg(fill)));
    } else {
        QVERIFY2(fill >= kDarkLightnessCeiling,
                 qPrintable(
                     QStringLiteral("disabled fill lightness %1 on the light page").arg(fill)));
    }
}

// Each visible group box and card outlines itself, in both themes: halfway down its left edge
// the 1px border differs from the surface just inside it.
void AppThemeTests::cardsAndGroupsDrawAOnePixelBorder() {
    QFETCH(ThemePreference, preference);
    sak::ui::setThemePreference(*qApp, preference);
    const auto tabs = m_window->findChildren<QTabWidget*>();
    int checked = 0;
    QStringList borderless;
    for (QTabWidget* tab_widget : tabs) {
        for (int index = 0; index < tab_widget->count(); ++index) {
            tab_widget->setCurrentIndex(index);
            QCoreApplication::processEvents();
            const auto surfaces = borderedSurfaces(*tab_widget->widget(index));
            for (QWidget* surface : surfaces) {
                if (!surface->isVisible() || surface->width() <= kBorderProbeInsetPx) {
                    continue;
                }
                const QImage image = surface->grab().toImage();
                const int y = image.height() / 2;
                if (image.pixel(0, y) == image.pixel(kBorderProbeInsetPx, y)) {
                    borderless << describe(surface);
                }
                ++checked;
            }
        }
    }
    QVERIFY2(checked > 0, "no visible group box or card was found to check");
    QVERIFY2(borderless.isEmpty(), qPrintable(borderless.join(QStringLiteral(", "))));
}

QTEST_MAIN(AppThemeTests)
#include "test_app_theme.moc"
