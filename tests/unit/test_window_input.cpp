// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_window_input.cpp
/// @brief Keyboard shortcuts and the mouse wheel behave like a normal Windows tool.

#include "sak/file_explorer_pane.h"
#include "sak/file_explorer_types.h"
#include "sak/file_management_explorer_panel.h"
#include "sak/main_window.h"
#include "sak/wheel_policy.h"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QtTest/QtTest>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace {

constexpr int kWheelNotch = 120;
constexpr int kTallContentHeight = 2000;
constexpr int kScrollAreaWidth = 320;
constexpr int kScrollAreaHeight = 240;
constexpr int kComboItems = 5;
constexpr int kSpinMaximum = 100;
constexpr int kSliderMaximum = 100;
constexpr int kSliderStart = 50;
constexpr int kExplorerTabsNeeded = 2;
constexpr int kTenthTabIndex = 9;
constexpr int kAboutTabIndex = 10;

/// One wheel notch down over @p target, delivered the way the platform delivers it
/// (spontaneous), so QApplication applies wheel focus and offers it to each parent in turn.
/// QCoreApplication::sendEvent would clear the spontaneous flag, so this goes through
/// notify() the way QTest delivers its own simulated input.
void wheelDownOver(QWidget* target) {
    const QPoint local = target->rect().center();
    QWheelEvent wheel(QPointF(local),
                      QPointF(target->mapToGlobal(local)),
                      QPoint(),
                      QPoint(0, -kWheelNotch),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QSpontaneKeyEvent::setSpontaneous(&wheel);
    qApp->notify(target, &wheel);
}

/// A scrolling page with every kind of wheel-stepped control on it, plus somewhere to park
/// keyboard focus. Members are destroyed children first, then the content (which clears the
/// scroll area's guarded pointer to it), then the scroll area.
struct ScrollingPage {
    QScrollArea area;
    QWidget content;
    QVBoxLayout layout{&content};
    QLineEdit focus_sink{&content};
    QComboBox combo{&content};
    QSpinBox spin{&content};
    QSlider slider{Qt::Horizontal, &content};
    QTabBar tabs{&content};
    QTextEdit text{&content};

    ScrollingPage() {
        for (int item = 0; item < kComboItems; ++item) {
            combo.addItem(QString::number(item));
        }
        spin.setRange(0, kSpinMaximum);
        slider.setRange(0, kSliderMaximum);
        slider.setValue(kSliderStart);
        tabs.addTab(QStringLiteral("One"));
        tabs.addTab(QStringLiteral("Two"));
        for (QWidget* widget :
             std::initializer_list<QWidget*>{&focus_sink, &combo, &spin, &slider, &tabs, &text}) {
            layout.addWidget(widget);
        }
        layout.addStretch();
        content.setMinimumHeight(kTallContentHeight);
        area.setWidget(&content);
        area.setWidgetResizable(true);
        area.resize(kScrollAreaWidth, kScrollAreaHeight);
    }
    ScrollingPage(const ScrollingPage&) = delete;
    ScrollingPage& operator=(const ScrollingPage&) = delete;
    ScrollingPage(ScrollingPage&&) = delete;
    ScrollingPage& operator=(ScrollingPage&&) = delete;
    ~ScrollingPage() = default;
};

QTabWidget* tabWidgetNamed(const QWidget& root, const QString& accessible_name) {
    const auto widgets = root.findChildren<QTabWidget*>();
    for (QTabWidget* tabs : widgets) {
        if (tabs->accessibleName() == accessible_name) {
            return tabs;
        }
    }
    return nullptr;
}

int tabIndexTitled(const QTabWidget& tabs, const QString& title) {
    for (int index = 0; index < tabs.count(); ++index) {
        if (tabs.tabText(index) == title) {
            return index;
        }
    }
    return -1;
}

sak::FileExplorerPane* visiblePane(const QWidget& explorer) {
    const auto panes = explorer.findChildren<sak::FileExplorerPane*>();
    for (sak::FileExplorerPane* pane : panes) {
        if (pane->isVisible()) {
            return pane;
        }
    }
    return nullptr;
}

void press(int key, Qt::KeyboardModifiers modifiers) {
    QWidget* focus = QApplication::focusWidget();
    QVERIFY(focus != nullptr);
    QTest::keyClick(focus, static_cast<Qt::Key>(key), modifiers);
}

}  // namespace

class WindowInputTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void unfocusedValueControlsLeaveTheWheelToTheView();
    void focusedComboStepsItsValue();
    void tabBarNeverSwitchesTabsOnTheWheel();
    void wheelNeverMovesKeyboardFocus();
    void focusedPanelShortcutsWinOverWindowShortcuts();
};

void WindowInputTests::initTestCase() {
    sak::ui::installWheelPolicy(*qApp);
}

// The wheel over a combo box, spin box or slider that does not have focus scrolls the page it
// sits on, and the control keeps its value (owner ruling 2026-10-24: standard Windows).
void WindowInputTests::unfocusedValueControlsLeaveTheWheelToTheView() {
    ScrollingPage page;
    page.area.show();
    QVERIFY(QTest::qWaitForWindowActive(&page.area));
    page.focus_sink.setFocus();
    QTRY_VERIFY(page.focus_sink.hasFocus());

    QScrollBar* bar = page.area.verticalScrollBar();
    int scrolled = bar->value();
    wheelDownOver(&page.combo);
    QCOMPARE(page.combo.currentIndex(), 0);
    QVERIFY2(bar->value() > scrolled, "the wheel over an unfocused combo must scroll the page");

    scrolled = bar->value();
    wheelDownOver(&page.spin);
    QCOMPARE(page.spin.value(), 0);
    QVERIFY2(bar->value() > scrolled, "the wheel over an unfocused spin box must scroll the page");

    scrolled = bar->value();
    wheelDownOver(&page.slider);
    QCOMPARE(page.slider.value(), kSliderStart);
    QVERIFY2(bar->value() > scrolled, "the wheel over an unfocused slider must scroll the page");
}

// With focus, a combo box steps its value on the wheel, as a Windows combo box does.
void WindowInputTests::focusedComboStepsItsValue() {
    ScrollingPage page;
    page.area.show();
    QVERIFY(QTest::qWaitForWindowActive(&page.area));
    page.combo.setFocus();
    QTRY_VERIFY(page.combo.hasFocus());
    wheelDownOver(&page.combo);
    QCOMPARE(page.combo.currentIndex(), 1);
}

// A tab bar is not a value control: the wheel never switches its tab, and goes on to the page.
void WindowInputTests::tabBarNeverSwitchesTabsOnTheWheel() {
    ScrollingPage page;
    page.area.show();
    QVERIFY(QTest::qWaitForWindowActive(&page.area));
    page.focus_sink.setFocus();
    QScrollBar* bar = page.area.verticalScrollBar();
    const int scrolled = bar->value();
    wheelDownOver(&page.tabs);
    QCOMPARE(page.tabs.currentIndex(), 0);
    QVERIFY2(bar->value() > scrolled, "the wheel over a tab bar must scroll the page");
}

// Windows never moves keyboard focus on the wheel. Qt gives focus to a Qt::WheelFocus widget
// (a combo box and a spin box are, by default) before anything else sees the wheel; the policy
// turns that focus policy into Qt::StrongFocus when the widget is polished.
void WindowInputTests::wheelNeverMovesKeyboardFocus() {
    ScrollingPage page;
    page.area.show();
    QVERIFY(QTest::qWaitForWindowActive(&page.area));
    page.focus_sink.setFocus();
    QTRY_VERIFY(page.focus_sink.hasFocus());
    QCOMPARE(page.combo.focusPolicy(), Qt::StrongFocus);
    QCOMPARE(page.spin.focusPolicy(), Qt::StrongFocus);
    wheelDownOver(page.text.viewport());
    QVERIFY2(page.focus_sink.hasFocus(), "the wheel over a text box must not take focus");
    wheelDownOver(&page.combo);
    QVERIFY2(page.focus_sink.hasFocus(), "the wheel over a combo box must not take focus");
}

// The File Explorer binds Ctrl+1..9, Ctrl+Tab and Ctrl+Shift+Tab to its own tabs and
// Ctrl+Shift+1..6 to its layouts (the Files bindings); the main window binds Ctrl+1..9,
// Ctrl+0, Ctrl+Shift+1, Ctrl+Tab and Ctrl+Shift+Tab application-wide to the main tabs. Two
// enabled shortcuts on one sequence are ambiguous and Qt fires neither, so inside the explorer
// these keys did nothing at all. The focused panel's own binding now wins, and the window's
// comes back when focus leaves the panel.
void WindowInputTests::focusedPanelShortcutsWinOverWindowShortcuts() {
    sak::MainWindow window;
    QTabWidget* main_tabs = tabWidgetNamed(window, QStringLiteral("Main panel tabs"));
    QVERIFY(main_tabs != nullptr);
    const int file_tab = tabIndexTitled(*main_tabs, QStringLiteral("File Management"));
    QVERIFY(file_tab >= 0);
    main_tabs->setCurrentIndex(file_tab);
    window.show();
    QVERIFY(QTest::qWaitForWindowActive(&window));

    auto* explorer = window.findChild<sak::FileManagementExplorerPanel*>();
    QVERIFY(explorer != nullptr);
    QTabWidget* file_tools = tabWidgetNamed(window, QStringLiteral("File management tools"));
    QVERIFY(file_tools != nullptr);
    file_tools->setCurrentWidget(explorer);
    auto* explorer_tabs = explorer->findChild<QTabBar*>(QStringLiteral("fileExplorerTabBar"));
    auto* new_tab = explorer->findChild<QPushButton*>(QStringLiteral("fileExplorerNewTabButton"));
    QVERIFY(explorer_tabs != nullptr);
    QVERIFY(new_tab != nullptr);
    while (explorer_tabs->count() < kExplorerTabsNeeded) {
        new_tab->click();
    }
    explorer_tabs->setCurrentIndex(0);
    explorer->setFocus();
    QTRY_VERIFY(explorer->isAncestorOf(QApplication::focusWidget()) ||
                QApplication::focusWidget() == explorer);

    press(Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(explorer_tabs->currentIndex(), 1);
    QCOMPARE(main_tabs->currentIndex(), file_tab);

    press(Qt::Key_Tab, Qt::ControlModifier);
    QCOMPARE(explorer_tabs->currentIndex(), 0);
    QCOMPARE(main_tabs->currentIndex(), file_tab);

    const sak::FileExplorerPane* pane = visiblePane(*explorer);
    QVERIFY(pane != nullptr);
    press(Qt::Key_2, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(pane->viewMode(), sak::FileExplorerViewMode::List);
    press(Qt::Key_1, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(pane->viewMode(), sak::FileExplorerViewMode::Details);
    QCOMPARE(main_tabs->currentIndex(), file_tab);

    // Outside the explorer the same keys drive the main tabs again.
    main_tabs->tabBar()->setFocus();
    QTRY_VERIFY(main_tabs->tabBar()->hasFocus());
    press(Qt::Key_0, Qt::ControlModifier);
    QCOMPARE(main_tabs->currentIndex(), kTenthTabIndex);
    if (main_tabs->count() > kAboutTabIndex) {
        press(Qt::Key_1, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(main_tabs->currentIndex(), kAboutTabIndex);
    }
    press(Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(main_tabs->currentIndex(), 1);
}

QTEST_MAIN(WindowInputTests)
#include "test_window_input.moc"
