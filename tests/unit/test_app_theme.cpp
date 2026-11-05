// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_theme.cpp
/// @brief The main window's appearance: the theme follows Windows (Light / Dark / System), every
/// tab renders in both, and surfaces are flat with 1px borders instead of drop shadows.

#include "sak/config_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/layout_constants.h"
#include "sak/main_window.h"
#include "sak/style_constants.h"
#include "sak/windows11_theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGroupBox>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QtTest/QtTest>
#include <QVBoxLayout>

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
        if (button->accessibleDescription() == QStringLiteral("App theme")) {
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
    void plainButtonsShowWhetherTheyAreEnabled_data();
    void plainButtonsShowWhetherTheyAreEnabled();
    void noSurfaceCarriesADropShadow();
    void infoPopupIsOutlinedNotShadowed();
    void cardsAndGroupsDrawAOnePixelBorder_data();
    void cardsAndGroupsDrawAOnePixelBorder();
    void subTabPagesKeepAMarginInsideTheirPane();
    void everyToolTabHasOneLogSwitchInOnePlace_data();
    void everyToolTabHasOneLogSwitchInOnePlace();
    void windowTextUsesOneDashStyle();
    void selectedTabIconsAreWhite();
    void aTabsStatusMessageLeavesWithIt();
    void aiWorkspaceLabelsTakeTheirPanesFill();

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

void AppThemeTests::plainButtonsShowWhetherTheyAreEnabled_data() {
    everyTabRendersInTheTheme_data();
}

// The theme's button rule names two widget types. A state written after the whole list belongs to
// the last type only, so every plain QPushButton matched the disabled rule: Next, Cancel and OK
// looked switched off, and a Finish that really was off looked the same as a Cancel that was on.
void AppThemeTests::plainButtonsShowWhetherTheyAreEnabled() {
    QFETCH(ThemePreference, preference);
    sak::ui::setThemePreference(*qApp, preference);
    const QString sheet = qApp->styleSheet();
    for (const QString& state :
         {QStringLiteral(":hover"), QStringLiteral(":pressed"), QStringLiteral(":disabled")}) {
        QVERIFY2(sheet.contains(QStringLiteral("QPushButton") + state),
                 qPrintable(QStringLiteral("no QPushButton%1 rule").arg(state)));
        QVERIFY2(sheet.contains(QStringLiteral("QToolButton") + state),
                 qPrintable(QStringLiteral("no QToolButton%1 rule").arg(state)));
    }

    QWidget page;
    auto* layout = new QVBoxLayout(&page);
    auto* enabled = new QPushButton(QStringLiteral("Next"), &page);
    auto* disabled = new QPushButton(QStringLiteral("Next"), &page);
    disabled->setEnabled(false);
    layout->addWidget(enabled);
    layout->addWidget(disabled);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    const auto fillOf = [&page](const QPushButton* button) {
        const QImage image = page.grab(button->geometry()).toImage();
        return QColor(image.pixel(kButtonFillProbeInsetPx, image.height() / 2)).rgb();
    };
    QVERIFY2(fillOf(enabled) != fillOf(disabled),
             "an enabled plain button draws the same as a disabled one");
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

namespace {

QTabWidget* mainTabs(const QMainWindow& window) {
    return qobject_cast<QTabWidget*>(window.centralWidget());
}

/// Every (sub-tab widget, index) a tab page shows, with {nullptr, -1} for the page itself.
QList<QPair<QTabWidget*, int>> pageViews(const QWidget& page) {
    QList<QPair<QTabWidget*, int>> views{{nullptr, -1}};
    for (QTabWidget* sub : page.findChildren<QTabWidget*>()) {
        for (int sub_index = 0; sub_index < sub->count(); ++sub_index) {
            views.append({sub, sub_index});
        }
    }
    return views;
}

bool isUtilityTab(const QString& tab) {
    return tab == QStringLiteral("Help and Support") || tab == QStringLiteral("About");
}

QList<sak::LogToggleSwitch*> visibleLogSwitches(const QWidget& page);

/// What is wrong with the Log switch on @p page, or nothing.
QString logSwitchProblem(const QWidget& page, const QString& where, bool utility) {
    const auto toggles = visibleLogSwitches(page);
    if (toggles.isEmpty()) {
        return utility ? QString() : where + QStringLiteral(": no Log switch");
    }
    if (toggles.size() > 1) {
        return where + QStringLiteral(": %1 Log switches").arg(toggles.size());
    }
    const QRect rect(toggles.first()->mapTo(&page, QPoint(0, 0)), toggles.first()->size());
    const int below = page.height() - rect.bottom() - 1;
    if (rect.left() == sak::ui::kMarginMedium && below >= 0 &&
        below <= sak::ui::kMarginMedium + 2) {
        return {};
    }
    return QStringLiteral("%1: at x=%2, %3 px above the page bottom")
        .arg(where)
        .arg(rect.left())
        .arg(below);
}

/// Group boxes and item views on @p page that touch or pass its left or right edge.
QStringList framesTouchingThePane(QWidget* page, const QString& where) {
    QStringList touching;
    for (QWidget* framed : page->findChildren<QWidget*>()) {
        const bool bordered = qobject_cast<QGroupBox*>(framed) != nullptr ||
                              qobject_cast<QAbstractItemView*>(framed) != nullptr;
        if (!bordered || !framed->isVisible()) {
            continue;
        }
        const QRect rect(framed->mapTo(page, QPoint(0, 0)), framed->size());
        if (rect.left() < 2 || rect.right() > page->width() - 3) {
            touching << QStringLiteral("%1: %2 at x %3-%4 of %5")
                            .arg(where, QString::fromLatin1(framed->metaObject()->className()))
                            .arg(rect.left())
                            .arg(rect.right())
                            .arg(page->width());
        }
    }
    return touching;
}

/// The user-visible strings one widget carries: its tooltip, title and its own text.
QList<QPair<QString, QString>> widgetTexts(const QWidget* widget) {
    const QString kind = QString::fromLatin1(widget->metaObject()->className());
    QList<QPair<QString, QString>> texts{{kind + QStringLiteral(" tooltip"), widget->toolTip()},
                                         {kind + QStringLiteral(" title"), widget->windowTitle()}};
    if (const auto* label = qobject_cast<const QLabel*>(widget)) {
        texts.append({kind, label->text()});
    } else if (const auto* button = qobject_cast<const QAbstractButton*>(widget)) {
        texts.append({kind, button->text()});
    } else if (const auto* group = qobject_cast<const QGroupBox*>(widget)) {
        texts.append({kind, group->title()});
    } else if (const auto* edit = qobject_cast<const QLineEdit*>(widget)) {
        texts.append({kind + QStringLiteral(" placeholder"), edit->placeholderText()});
    } else if (const auto* tabs = qobject_cast<const QTabWidget*>(widget)) {
        for (int index = 0; index < tabs->count(); ++index) {
            texts.append({kind, tabs->tabText(index)});
            texts.append({kind + QStringLiteral(" tab tooltip"), tabs->tabToolTip(index)});
        }
    }
    return texts;
}

/// Whether @p label, inside @p workspace's picture, paints a fill different from what is beside it.
bool labelPaintsItsOwnFill(const QLabel* label, const QWidget* workspace, const QImage& image) {
    if (!label->isVisible() || label->width() < 8 || label->height() < 8 ||
        !label->styleSheet().isEmpty() || label->parentWidget() == workspace) {
        return false;
    }
    const QPoint corner = label->mapTo(workspace, QPoint(1, 1));
    const QPoint outside = label->mapTo(workspace, QPoint(-2, 1));
    return image.rect().contains(outside) && image.pixel(corner) != image.pixel(outside);
}

QList<sak::LogToggleSwitch*> visibleLogSwitches(const QWidget& page) {
    QList<sak::LogToggleSwitch*> found;
    for (sak::LogToggleSwitch* toggle : page.findChildren<sak::LogToggleSwitch*>()) {
        if (toggle->isVisible() && toggle->toolTip() == QStringLiteral("Toggle log window")) {
            found.append(toggle);
        }
    }
    return found;
}

}  // namespace

void AppThemeTests::everyToolTabHasOneLogSwitchInOnePlace_data() {
    QTest::addColumn<QSize>("size");
    QTest::newRow("capture size") << QSize(kReviewWindowWidth, kReviewWindowHeight);
    QTest::newRow("1680x900") << QSize(1680, 900);
    QTest::newRow("minimum") << QSize(sak::kMainWindowMinW, sak::kMainWindowMinH);
}

// UI review S13 and S14. Each tab placed its own Log switch: under the page, inside a sub-tab,
// beside Settings, at the far right, in the disk map's legend, half off the page or off it
// altogether. One footer puts it at the page's left margin, under the page, on every tool tab and
// every sub-tab, and the page fits the window around it, so nothing is pushed out of reach.
void AppThemeTests::everyToolTabHasOneLogSwitchInOnePlace() {
    QFETCH(QSize, size);
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    m_window->resize(size);
    QCoreApplication::processEvents();
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr);
    QStringList wrong;
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        QCoreApplication::processEvents();
        QWidget* page = tabs->widget(index);
        const QString tab = tabs->tabText(index);
        for (const auto& [sub, sub_index] : pageViews(*page)) {
            if (sub != nullptr) {
                revealTabWidget(sub);
                sub->setCurrentIndex(sub_index);
                QCoreApplication::processEvents();
                if (!sub->isVisible()) {
                    continue;
                }
            }
            const QString where =
                sub == nullptr ? tab : tab + QStringLiteral(" > ") + sub->tabText(sub_index);
            const QString problem = logSwitchProblem(*page, where, isUtilityTab(tab));
            if (!problem.isEmpty()) {
                wrong << problem;
            }
        }
    }
    m_window->resize(kReviewWindowWidth, kReviewWindowHeight);
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("\n"))));
}

// UI review S16: App Installation's Online Install and Offline Deploy and the Vulnerability
// Scanner set zero side margins, so their group boxes drew against the tab pane's own border.
void AppThemeTests::subTabPagesKeepAMarginInsideTheirPane() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    // Wide enough for every page's content in the offscreen platform's wider fallback font, so
    // the check sees the pages' margins rather than content too wide for its window.
    m_window->resize(1680, 900);
    QCoreApplication::processEvents();
    QStringList touching;
    const QStringList reviewed = {QStringLiteral("Application management tabs"),
                                  QStringLiteral("Application installation mode tabs")};
    for (QTabWidget* tabs : m_window->findChildren<QTabWidget*>()) {
        if (!reviewed.contains(tabs->accessibleName())) {
            continue;
        }
        revealTabWidget(tabs);
        for (int index = 0; index < tabs->count(); ++index) {
            tabs->setCurrentIndex(index);
            QCoreApplication::processEvents();
            QWidget* page = tabs->widget(index);
            if (!page->isVisible()) {
                continue;
            }
            touching << framesTouchingThePane(
                page, tabs->accessibleName() + QStringLiteral(" > ") + tabs->tabText(index));
        }
    }
    touching.removeDuplicates();
    m_window->resize(kReviewWindowWidth, kReviewWindowHeight);
    QVERIFY2(touching.isEmpty(), qPrintable(touching.join(QStringLiteral("\n"))));
}

// UI review S24: two dash styles ("--" and an em dash) and other non-ASCII punctuation in the
// window's text. Every tab is built; every label, button, group title, tab, tooltip and
// placeholder uses " - ".
void AppThemeTests::windowTextUsesOneDashStyle() {
    QStringList found;
    const auto check = [&found](const QString& where, const QString& text) {
        if (text.contains(QChar(0x2014)) || text.contains(QChar(0x2013)) ||
            text.contains(QStringLiteral(" -- "))) {
            found << QStringLiteral("%1: %2").arg(where, text.left(90));
        }
    };
    auto widgets = m_window->findChildren<QWidget*>();
    widgets.prepend(m_window.get());
    for (const QWidget* widget : widgets) {
        for (const auto& [where, text] : widgetTexts(widget)) {
            check(where, text);
        }
    }
    found.removeDuplicates();
    QVERIFY2(found.isEmpty(), qPrintable(found.join(QStringLiteral("\n"))));
}

// UI review F9: a selector-less white background on the chat workspace cascaded to every label
// in it, which drew white blocks on the grey panes.
void AppThemeTests::aiWorkspaceLabelsTakeTheirPanesFill() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    auto* workspace = m_window->findChild<QWidget*>(QStringLiteral("aiChatWorkspace"));
    QVERIFY(workspace != nullptr);
    QTabWidget* tabs = mainTabs(*m_window);
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->widget(index)->isAncestorOf(workspace)) {
            tabs->setCurrentIndex(index);
        }
    }
    QCoreApplication::processEvents();
    QVERIFY(workspace->isVisible());
    const QImage image = workspace->grab().toImage();
    QStringList painted;
    for (const QLabel* label : workspace->findChildren<QLabel*>()) {
        if (labelPaintsItsOwnFill(label, workspace, image)) {
            const QPoint corner = label->mapTo(workspace, QPoint(1, 1));
            painted << QStringLiteral("\"%1\" %2")
                           .arg(label->text().left(30), QColor(image.pixel(corner)).name());
        }
    }
    QVERIFY2(painted.isEmpty(), qPrintable(painted.join(QStringLiteral("\n"))));
}

// UI review S25: on the selected tab's blue gradient the coloured icon all but vanished; the
// selected state (QIcon::On, which the tab bar draws for the current tab) is white.
void AppThemeTests::selectedTabIconsAreWhite() {
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr);
    for (int index = 0; index < tabs->count(); ++index) {
        const QIcon icon = tabs->tabIcon(index);
        if (icon.isNull()) {
            continue;
        }
        const QImage on = icon.pixmap(tabs->iconSize(), QIcon::Normal, QIcon::On).toImage();
        int ink = 0;
        int white = 0;
        for (int y = 0; y < on.height(); ++y) {
            for (int x = 0; x < on.width(); ++x) {
                const QColor pixel = on.pixelColor(x, y);
                if (pixel.alpha() > 200) {
                    ++ink;
                    white += pixel.lightness() > 240 ? 1 : 0;
                }
            }
        }
        QVERIFY2(ink > 0 && white == ink,
                 qPrintable(QStringLiteral("%1: %2 of %3 pixels white")
                                .arg(tabs->tabText(index))
                                .arg(white)
                                .arg(ink)));
    }
}

// UI review S22: "Explorer loaded 6 item(s)" stayed in the status bar on File Organizer and Help.
void AppThemeTests::aTabsStatusMessageLeavesWithIt() {
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr && tabs->count() > 1);
    tabs->setCurrentIndex(0);
    QCoreApplication::processEvents();
    m_window->updateStatus(QStringLiteral("Explorer loaded 6 item(s)"), 0);
    m_window->updateStatus(QStringLiteral("Querying DNS for example.com..."), 60'000);
    tabs->setCurrentIndex(1);
    QCoreApplication::processEvents();
    QVERIFY(m_window->statusBar()->currentMessage().isEmpty());
    QStringList shown;
    for (const QLabel* label : m_window->statusBar()->findChildren<QLabel*>()) {
        if (label->isVisible()) {
            shown << label->text();
        }
    }
    QVERIFY2(!shown.contains(QStringLiteral("Explorer loaded 6 item(s)")),
             qPrintable(shown.join(QStringLiteral(" | "))));
    QVERIFY(shown.contains(QStringLiteral("Ready")));
}

QTEST_MAIN(AppThemeTests)
#include "test_app_theme.moc"
