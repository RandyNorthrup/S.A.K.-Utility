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
#include "sak/widget_helpers.h"
#include "sak/windows11_theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGroupBox>
#include <QHash>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QStatusBar>
#include <QStyleFactory>
#include <QStyleHints>
#include <QTabBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QtTest/QtTest>
#include <QVBoxLayout>
#include <QWizard>

#include <algorithm>
#include <cmath>
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
    void dialogButtonsTakeTheToneOfTheirRole();
    void everyActionCardHasOneLayout();
    void tabIconsReadOnTheirTabsInBothThemes();
    void theLinuxCardsPenguinReadsInDark();
    void aTabsStatusMessageLeavesWithIt();
    void aRunningJobsMessageStaysUntilItEnds();
    void aiWorkspaceLabelsTakeTheirPanesFill();
    void partitionManagerKeepsItsDiskMapHeight();
    void matchNavigationButtonsShowTheirArrows();
    void promptsAreWideEnoughAndWrapTheirQuestion();
    void aSubTabsStatusMessageLeavesWithIt();
    void cellWidgetsFillTheirCells();
    void wizardsDrawNoAeroBackArrowOverTheTitle();

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
    } else if (const auto* browser = qobject_cast<const QTextBrowser*>(widget)) {
        // The About and Help pages: their feature lists carried em dashes (UI re-check S24).
        texts.append({kind, browser->toPlainText()});
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

// UI re-check R1: the shared Log footer took the disk map's height at the window's default size,
// and the last disk's tile was cut through its name. The legend now shares the Log switch's row,
// and the map keeps room for three whole disks.
void AppThemeTests::partitionManagerKeepsItsDiskMapHeight() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    m_window->resize(kReviewWindowWidth, kReviewWindowHeight);
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr);
    int partition_tab = -1;
    for (int index = 0; index < tabs->count(); ++index) {
        if (tabs->tabText(index) == QStringLiteral("Partition Manager")) {
            partition_tab = index;
        }
    }
    QVERIFY(partition_tab >= 0);
    tabs->setCurrentIndex(partition_tab);
    QCoreApplication::processEvents();
    QWidget* page = tabs->widget(partition_tab);

    auto* map = page->findChild<QWidget*>(QStringLiteral("partitionDiskMapPane"));
    QVERIFY(map != nullptr && map->isVisible());
    // Three disk tiles of 86 px, the 2 px between them and the map's 1 px margins.
    constexpr int kThreeWholeDisks = (3 * 86) + (2 * 2) + (2 * 1);
    QVERIFY2(map->height() >= kThreeWholeDisks,
             qPrintable(QStringLiteral("disk map %1 px tall").arg(map->height())));

    const auto toggles = visibleLogSwitches(*page);
    QCOMPARE(toggles.size(), 1);
    const QRect toggle(toggles.first()->mapTo(page, QPoint(0, 0)), toggles.first()->size());
    auto* swatch = page->findChild<QFrame*>(QStringLiteral("partitionLegendSwatch"));
    QVERIFY(swatch != nullptr && swatch->isVisible());
    const QPoint swatch_centre = swatch->mapTo(page, swatch->rect().center());
    QVERIFY2(swatch_centre.y() >= toggle.top() && swatch_centre.y() <= toggle.bottom(),
             qPrintable(QStringLiteral("legend at y=%1, Log switch %2-%3")
                            .arg(swatch_centre.y())
                            .arg(toggle.top())
                            .arg(toggle.bottom())));
}

// UI review F22 and F44: an input prompt was 202 px wide for an API key or a GUID, and the boot
// repair prompt 853 px wide for one sentence.
void AppThemeTests::promptsAreWideEnoughAndWrapTheirQuestion() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    QInputDialog prompt;
    prompt.setLabelText(QStringLiteral(
        "This looks for the Windows on Disk 2 and rebuilds its boot files on that disk, using "
        "temporary drive letters. This PC's own boot list is left alone.\n\nBoot mode (GPT disks "
        "start by UEFI, MBR disks by BIOS):"));
    prompt.show();
    QVERIFY(QTest::qWaitForWindowExposed(&prompt));
    QVERIFY2(prompt.width() >= 420, qPrintable(QString::number(prompt.width())));
    QVERIFY2(prompt.width() < 700, qPrintable(QString::number(prompt.width())));
    auto* question = prompt.findChild<QLabel*>();
    QVERIFY(question != nullptr && question->wordWrap());

    // A short question, such as the API key prompt's: the window was 202 px wide.
    QInputDialog key_prompt;
    key_prompt.setLabelText(QStringLiteral("API key:"));
    key_prompt.show();
    QVERIFY(QTest::qWaitForWindowExposed(&key_prompt));
    QVERIFY2(key_prompt.width() >= 420, qPrintable(QString::number(key_prompt.width())));
}

// UI re-check N1: Advanced Search's previous and next match buttons were blank squares. The
// secondary style's 8px 20px padding left a 28 px button no room for its arrow.
void AppThemeTests::matchNavigationButtonsShowTheirArrows() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    QStringList blank;
    for (const QString& name : {QStringLiteral("Previous match"), QStringLiteral("Next match")}) {
        QPushButton* found = nullptr;
        for (QPushButton* button : m_window->findChildren<QPushButton*>()) {
            if (button->accessibleName() == name) {
                found = button;
            }
        }
        QVERIFY2(found != nullptr, qPrintable(name));
        // Enabled, as after a search with matches: a white arrow on the slate secondary fill, which
        // is never that light. With the default padding no arrow was drawn at all.
        const bool was_enabled = found->isEnabled();
        found->setEnabled(true);
        const QImage image = found->grab().toImage();
        found->setEnabled(was_enabled);
        int ink = 0;
        for (int y = image.height() / 4; y < image.height() * 3 / 4; ++y) {
            for (int x = image.width() / 4; x < image.width() * 3 / 4; ++x) {
                ink += image.pixelColor(x, y).lightness() > 200 ? 1 : 0;
            }
        }
        if (ink < 4) {
            blank << QStringLiteral("%1 (%2 px)").arg(name).arg(ink);
        }
    }
    QVERIFY2(blank.isEmpty(), qPrintable(blank.join(QStringLiteral(", "))));
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

// UI review F27: Close was secondary in Contacts, Calendar and Metadata and primary in Run
// Details, Installation Summary and Space Analyzer; Cancel was primary in the backup wizard and
// secondary in the file scan. The theme made every push button primary, so a button box's Close
// and Cancel took the colour of the action beside them. A button that only leaves is secondary.
void AppThemeTests::dialogButtonsTakeTheToneOfTheirRole() {
    const auto polished = [](QWidget* window) {
        window->show();
        QVERIFY(QTest::qWaitForWindowExposed(window));
    };
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    auto* ok_cancel = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           &dialog);
    auto* close_only = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto* stop = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    stop->button(QDialogButtonBox::Cancel)->setStyleSheet(sak::ui::kDangerButtonStyle);
    layout->addWidget(ok_cancel);
    layout->addWidget(close_only);
    layout->addWidget(stop);
    polished(&dialog);
    QCOMPARE(ok_cancel->button(QDialogButtonBox::Cancel)->styleSheet(),
             sak::ui::kSecondaryButtonStyle);
    QCOMPARE(close_only->button(QDialogButtonBox::Close)->styleSheet(),
             sak::ui::kSecondaryButtonStyle);
    QVERIFY(ok_cancel->button(QDialogButtonBox::Ok)->styleSheet().isEmpty());
    // A Cancel that stops running work keeps the danger tone it was given.
    QCOMPARE(stop->button(QDialogButtonBox::Cancel)->styleSheet(), sak::ui::kDangerButtonStyle);

    QMessageBox question(QMessageBox::Question,
                         QStringLiteral("Overwrite"),
                         QStringLiteral("Replace the file?"),
                         QMessageBox::Yes | QMessageBox::No);
    polished(&question);
    QCOMPARE(question.button(QMessageBox::No)->styleSheet(), sak::ui::kSecondaryButtonStyle);
    QVERIFY(question.button(QMessageBox::Yes)->styleSheet().isEmpty());
    question.close();

    QWizard wizard;
    sak::useAppWizardStyle(&wizard);
    QCOMPARE(wizard.button(QWizard::BackButton)->styleSheet(), sak::ui::kSecondaryButtonStyle);
    QCOMPARE(wizard.button(QWizard::CancelButton)->styleSheet(), sak::ui::kSecondaryButtonStyle);
    QVERIFY(wizard.button(QWizard::NextButton)->styleSheet().isEmpty());
    QVERIFY(wizard.button(QWizard::FinishButton)->styleSheet().isEmpty());
}

namespace {

double relativeLuminance(const QColor& color) {
    const auto linear = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return (0.2126 * linear(color.redF())) + (0.7152 * linear(color.greenF())) +
           (0.0722 * linear(color.blueF()));
}

double contrastRatio(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// The share of @p icon's ink that, drawn on @p background, stands @p ratio or more from it. A
/// line a quarter of a pixel wide is drawn at a quarter of its colour and counts for little.
double inkAtLeast(const QImage& icon, const QColor& background, double ratio) {
    double ink = 0.0;
    double legible = 0.0;
    for (int y = 0; y < icon.height(); ++y) {
        for (int x = 0; x < icon.width(); ++x) {
            const QColor pixel = icon.pixelColor(x, y);
            const double alpha = pixel.alphaF();
            if (alpha <= 0.0) {
                continue;
            }
            const QColor drawn = QColor::fromRgbF(
                static_cast<float>((pixel.redF() * alpha) + (background.redF() * (1 - alpha))),
                static_cast<float>((pixel.greenF() * alpha) + (background.greenF() * (1 - alpha))),
                static_cast<float>((pixel.blueF() * alpha) + (background.blueF() * (1 - alpha))));
            ink += alpha;
            legible += contrastRatio(drawn, background) >= ratio ? alpha : 0.0;
        }
    }
    return ink > 0.0 ? legible / ink : 0.0;
}

/// The share of @p icon's ink at 3:1 or more, the bar for a graphic beside text.
double inkAtThreeToOne(const QImage& icon, const QColor& background) {
    constexpr double kNonTextContrast = 3.0;
    return inkAtLeast(icon, background, kNonTextContrast);
}

}  // namespace

namespace {

int opaquePixels(const QImage& image) {
    constexpr int kOpaque = 200;
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += image.pixelColor(x, y).alpha() > kOpaque ? 1 : 0;
        }
    }
    return count;
}

/// What is wrong with @p tabs' icons in the theme showing now; each one's reading goes to @p
/// report.
QStringList tabIconProblems(QTabWidget* tabs, bool dark, QStringList* report) {
    constexpr double kLegibleShare = 0.5;
    constexpr double kBarelyThere = 1.5;  // contrast under which a line is all but gone
    constexpr double kMostlyFaint = 0.2;
    constexpr int kGlyphPixels = 10;
    tabs->setCurrentIndex(0);
    QCoreApplication::processEvents();
    QTabBar* bar = tabs->tabBar();
    const QRect second = bar->tabRect(1);
    // An unselected tab's colour, at its right end, clear of its icon and text.
    const QColor tab_colour = bar->grab().toImage().pixelColor(second.right() - 3,
                                                               second.center().y());
    const QString theme = dark ? QStringLiteral("dark") : QStringLiteral("light");
    QStringList wrong;
    for (int index = 0; index < tabs->count(); ++index) {
        const QIcon icon = tabs->tabIcon(index);
        const QString name = tabs->tabText(index);
        if (icon.isNull()) {
            continue;
        }
        const QImage drawn = icon.pixmap(tabs->iconSize(), QIcon::Normal, QIcon::Off).toImage();
        const double share = inkAtThreeToOne(drawn, tab_colour);
        const QString line = QStringLiteral("%1 %2: %3 on %4")
                                 .arg(theme, name)
                                 .arg(share, 0, 'f', 2)
                                 .arg(tab_colour.name());
        *report << line;
        // Every icon on the dark tab. On the light tab, the two the review found faint: the others
        // are drawn in mid tones that read as shapes beside their tab's name, and are reported, not
        // held.
        const bool faint_before = QSet<QString>{QStringLiteral("File Management"),
                                                QStringLiteral("Email Tools")}
                                      .contains(name);
        // And no part of it all but gone: the old File Management icon's bright boxes carried its
        // share while its lines stood 1.41:1 from the dark tab.
        const double faint = 1.0 - inkAtLeast(drawn, tab_colour, kBarelyThere);
        if ((dark || faint_before) && (share < kLegibleShare || faint > kMostlyFaint)) {
            wrong << QStringLiteral("%1, %2 faint").arg(line).arg(faint, 0, 'f', 2);
        }
        // Selected, Help's question mark and About's "i", drawn in white, are left out of the white
        // drawing, so they show in the tab's blue.
        const bool glyph_inside =
            QSet<QString>{QStringLiteral("Help and Support"), QStringLiteral("About")}.contains(
                name);
        const QImage on = icon.pixmap(tabs->iconSize(), QIcon::Normal, QIcon::On).toImage();
        if (glyph_inside && opaquePixels(drawn) - opaquePixels(on) < kGlyphPixels) {
            wrong << QStringLiteral("%1 %2 selected: a blank disc").arg(theme, name);
        }
    }
    return wrong;
}

}  // namespace

// UI review S25 and DN4: the File Management tab's icon drew its lines a quarter of a pixel wide,
// #344152 on the dark tab (1.41:1), so only its bullets showed, and it and Email Tools' were faint
// on the light tab; selected, Help's and About's became blank white discs. Every tab icon is read
// on its own tab's colour in both themes.
void AppThemeTests::tabIconsReadOnTheirTabsInBothThemes() {
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr && tabs->count() > 2);
    const auto back_to_light = qScopeGuard([] {
        sak::ui::setThemePreference(*qApp, ThemePreference::Light);
        QCoreApplication::processEvents();
    });
    QStringList report;
    QStringList wrong;
    for (const ThemePreference theme : {ThemePreference::Light, ThemePreference::Dark}) {
        sak::ui::setThemePreference(*qApp, theme);
        QCoreApplication::processEvents();
        wrong << tabIconProblems(tabs, theme == ThemePreference::Dark, &report);
    }
    qInfo().noquote() << report.join(QStringLiteral("\n"));
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("; "))));
}
// UI review DN5: the Linux ISO card's penguin is black, and its body and outline all but vanished
// on the dark card. In dark it is drawn in a light grey, switched with the theme.
void AppThemeTests::theLinuxCardsPenguinReadsInDark() {
    constexpr double kLegibleShare = 0.5;
    const auto penguin = [this]() -> QLabel* {
        for (auto* card : m_window->findChildren<QFrame*>()) {
            if (!card->property("sakCard").toBool()) {
                continue;
            }
            QLabel* logo = nullptr;
            bool linux_card = false;
            for (auto* label : card->findChildren<QLabel*>(Qt::FindDirectChildrenOnly)) {
                linux_card = linux_card || label->text() == QStringLiteral("Linux ISO");
                logo = label->pixmap().isNull() ? logo : label;
            }
            if (linux_card) {
                return logo;
            }
        }
        return nullptr;
    };
    QLabel* logo = penguin();
    QVERIFY(logo != nullptr);
    const auto back_to_light = qScopeGuard([] {
        sak::ui::setThemePreference(*qApp, ThemePreference::Light);
        QCoreApplication::processEvents();
    });
    QStringList shares;
    for (const ThemePreference theme : {ThemePreference::Dark, ThemePreference::Light}) {
        sak::ui::setThemePreference(*qApp, theme);
        QCoreApplication::processEvents();
        const QColor card_colour = sak::ui::themedColor(sak::ui::kColorBgSurface);
        const double share = inkAtThreeToOne(logo->pixmap().toImage(), card_colour);
        shares << QString::number(share, 'f', 2);
        QVERIFY2(share >= kLegibleShare,
                 qPrintable(QStringLiteral("%1: %2 of the penguin reads on %3")
                                .arg(theme == ThemePreference::Dark ? "dark" : "light")
                                .arg(share, 0, 'f', 2)
                                .arg(card_colour.name())));
    }
    qInfo().noquote() << "penguin dark, light:" << shares.join(QStringLiteral(", "));
}

// UI review F41: Help's and the Image Flasher's cards centred their titles under a 48 px logo;
// Backup and Restore's left-aligned them under a 96 px one. Every card is built one way.
void AppThemeTests::everyActionCardHasOneLayout() {
    constexpr int kCardLogo = 48;
    QStringList cards;
    QStringList wrong;
    for (auto* card : m_window->findChildren<QFrame*>()) {
        if (!card->property("sakCard").toBool()) {
            continue;
        }
        const auto labels = card->findChildren<QLabel*>(Qt::FindDirectChildrenOnly);
        const auto logo = std::ranges::find_if(labels, [](const QLabel* label) {
            return !label->pixmap().isNull();
        });
        const auto title = std::ranges::find_if(labels, [](const QLabel* label) {
            return label->styleSheet() == sak::ui::cardTitleTextStyle();
        });
        if (logo == labels.end() || title == labels.end()) {
            wrong << QStringLiteral("a card with no logo or title");
            continue;
        }
        cards << (*title)->text();
        if ((*logo)->pixmap().deviceIndependentSize().toSize() != QSize(kCardLogo, kCardLogo)) {
            wrong << QStringLiteral("%1: logo %2 px")
                         .arg((*title)->text())
                         .arg((*logo)->pixmap().deviceIndependentSize().width());
        }
        if (((*title)->alignment() & Qt::AlignHCenter) == 0) {
            wrong << QStringLiteral("%1: title not centred").arg((*title)->text());
        }
    }
    // Two on Backup and Restore, three on the Image Flasher, four on Help.
    for (const char* expected :
         {"Backup User Profiles", "Restore User Profiles", "Linux ISO", "Report a Bug"}) {
        QVERIFY2(cards.contains(QString::fromLatin1(expected)),
                 qPrintable(expected + QStringLiteral(" not among ") + cards.join(", ")));
    }
    QVERIFY2(wrong.isEmpty(), qPrintable(wrong.join(QStringLiteral("; "))));
}

// UI review S22: "Explorer loaded 6 item(s)" stayed in the status bar on File Organizer and Help.
void AppThemeTests::aTabsStatusMessageLeavesWithIt() {
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr && tabs->count() > 1);
    tabs->setCurrentIndex(0);
    QCoreApplication::processEvents();
    m_window->updateStatus(QStringLiteral("Explorer loaded 6 items"), 0);
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
    QVERIFY2(!shown.contains(QStringLiteral("Explorer loaded 6 items")),
             qPrintable(shown.join(QStringLiteral(" | "))));
    QVERIFY(shown.contains(QStringLiteral("Ready")));
}

// Review 2: a recovery scan's "Running Partition Recovery Scan" ran out after 5 s, and the bar
// read "Ready" beside its progress at 5% for the rest of the scan.
void AppThemeTests::aRunningJobsMessageStaysUntilItEnds() {
    m_window->updateProgress(5, 100);
    m_window->updateStatus(QStringLiteral("Running Partition Recovery Scan"), 40);
    QTest::qWait(200);
    QCOMPARE(m_window->statusBar()->currentMessage(),
             QStringLiteral("Running Partition Recovery Scan"));
    m_window->updateProgress(100, 100);
    QVERIFY(m_window->statusBar()->currentMessage().isEmpty());
    m_window->updateStatus(QStringLiteral("Partition operation complete"), 40);
    QCOMPARE(m_window->statusBar()->currentMessage(),
             QStringLiteral("Partition operation complete"));
    QTRY_VERIFY_WITH_TIMEOUT(m_window->statusBar()->currentMessage().isEmpty(), 2000);
}

// UI re-check S22: a sub-tab's message stayed when another sub-tab of the same tab was opened,
// "Explorer loaded 6 item(s)" on File Organizer among them.
void AppThemeTests::aSubTabsStatusMessageLeavesWithIt() {
    QTabWidget* tabs = mainTabs(*m_window);
    QVERIFY(tabs != nullptr);
    QStringList stayed;
    QStringList checked;
    for (int index = 0; index < tabs->count(); ++index) {
        tabs->setCurrentIndex(index);
        QCoreApplication::processEvents();
        for (QTabWidget* sub : tabs->widget(index)->findChildren<QTabWidget*>()) {
            const QStringList wrappers = {QStringLiteral("Email tools tabs"),
                                          QStringLiteral("Application management tabs"),
                                          QStringLiteral("Network management tabs")};
            bool file_management = false;  // the tab widget that holds Advanced Search
            for (int sub_index = 0; sub_index < sub->count(); ++sub_index) {
                file_management |= sub->tabText(sub_index) == QStringLiteral("Advanced Search");
            }
            if (sub->count() < 2 ||
                (!wrappers.contains(sub->accessibleName()) && !file_management)) {
                continue;
            }
            checked << (file_management ? QStringLiteral("File Management")
                                        : sub->accessibleName());
            sub->setCurrentIndex(0);
            QCoreApplication::processEvents();
            // A message without a timeout goes to the status label, one with a timeout to the bar.
            const QString lasting = QStringLiteral("Explorer loaded 6 items");
            m_window->updateStatus(lasting, 0);
            m_window->updateStatus(QStringLiteral("2 adapters found"), 60'000);
            const auto shows = [this](const QString& text) {
                const auto labels = m_window->statusBar()->findChildren<QLabel*>();
                return std::ranges::any_of(labels, [&text](const QLabel* label) {
                    return label->text() == text;
                });
            };
            QVERIFY2(shows(lasting), "control: the message is on the status bar before the switch");
            QVERIFY(!m_window->statusBar()->currentMessage().isEmpty());
            sub->setCurrentIndex(1);
            QCoreApplication::processEvents();
            if (!m_window->statusBar()->currentMessage().isEmpty() || shows(lasting)) {
                stayed << tabs->tabText(index);
            }
        }
    }
    // Email Tools, Application Management, Network Management and File Management.
    checked.removeDuplicates();
    QVERIFY2(checked.size() == 4, qPrintable(checked.join(QStringLiteral(", "))));
    QVERIFY2(stayed.isEmpty(), qPrintable(stayed.join(QStringLiteral(", "))));
}

// UI review F4, re-check N2 and N3: Qt puts a cell widget in the item's text area, which the
// theme's 6 px item padding shrinks, so an editor lost 12 px of a row sized for it and its text was
// cut through the middle. A view's cell widgets get their whole cells.
void AppThemeTests::cellWidgetsFillTheirCells() {
    sak::ui::setThemePreference(*qApp, ThemePreference::Light);
    QTableWidget table(1, 1);
    sak::placeCellWidgetsInWholeCells(&table);
    auto* editor = new QLineEdit(QStringLiteral("Data1"));
    table.setCellWidget(0, 0, editor);
    table.setRowHeight(0, 36);
    table.resize(320, 140);
    table.show();
    QVERIFY(QTest::qWaitForWindowExposed(&table));
    QCoreApplication::processEvents();
    const QRect cell = table.visualRect(table.model()->index(0, 0));
    QCOMPARE(editor->geometry(), cell);
}


void AppThemeTests::wizardsDrawNoAeroBackArrowOverTheTitle() {
    // In light Windows, Qt starts a wizard in AeroStyle (QWindowsStyle's SH_WizardStyle) and marks
    // its private QVistaBackButton to be shown before the app picks ModernStyle, so the arrow came
    // up over every page title (review 3, testpc1). The Windows style gives AeroStyle here too.
    QStyle* const previous_style = QApplication::style();
    const QString previous_name = previous_style->name();
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Windows")));
    const auto restore_style = qScopeGuard(
        [&previous_name]() { QApplication::setStyle(QStyleFactory::create(previous_name)); });
    QVERIFY2(
        QWizard::WizardStyle(QApplication::style()->styleHint(QStyle::SH_WizardStyle)) ==
            QWizard::AeroStyle,
        "Precondition: Qt's Windows style starts a wizard in AeroStyle, as light Windows does");

    QWizard wizard;
    QAbstractButton* const arrow = sak::aeroBackArrowOf(&wizard);
    QVERIFY2(arrow != nullptr, "Qt's Aero back arrow was not found among the wizard's children");
    QVERIFY2(!arrow->isHidden(),
             "Precondition: Qt has marked the Aero back arrow to be shown with the wizard");

    sak::useAppWizardStyle(&wizard);
    // Under ModernStyle the wizard's Back is its own push button, not the arrow.
    QVERIFY(arrow != wizard.button(QWizard::BackButton));
    auto* page = new QWizardPage(&wizard);
    page->setTitle(QStringLiteral("Review"));
    page->setSubTitle(QStringLiteral("Check the plan before it is queued."));
    wizard.addPage(page);
    wizard.show();
    QVERIFY(QTest::qWaitForWindowExposed(&wizard));
    QCoreApplication::processEvents();
    QCOMPARE(wizard.wizardStyle(), QWizard::ModernStyle);
    QVERIFY2(!arrow->isVisible(), "The Aero back arrow is drawn over the page title");
    wizard.close();
}

QTEST_MAIN(AppThemeTests)
#include "test_app_theme.moc"
