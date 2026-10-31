// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_theme.cpp
/// @brief The main window's appearance: flat surfaces with 1px borders, no drop shadows.

#include "sak/info_button.h"
#include "sak/main_window.h"
#include "sak/windows11_theme.h"

#include <QApplication>
#include <QFrame>
#include <QGraphicsEffect>
#include <QGroupBox>
#include <QImage>
#include <QTabWidget>
#include <QtTest/QtTest>

#include <memory>

namespace {

constexpr int kBorderProbeInsetPx = 4;

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
    void noSurfaceCarriesADropShadow();
    void infoPopupIsOutlinedNotShadowed();
    void cardsAndGroupsDrawAOnePixelBorder();

private:
    std::unique_ptr<sak::MainWindow> m_window;
};

void AppThemeTests::initTestCase() {
    // What main() applies at startup, then the real window with every tab built.
    sak::ui::applyWindows11Theme(*qApp);
    m_window = std::make_unique<sak::MainWindow>();
    m_window->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    showEveryTab(*m_window);
}

void AppThemeTests::cleanupTestCase() {
    m_window.reset();
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

// Each visible group box and card outlines itself: halfway down its left edge the 1px border
// differs from the surface just inside it.
void AppThemeTests::cardsAndGroupsDrawAOnePixelBorder() {
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
