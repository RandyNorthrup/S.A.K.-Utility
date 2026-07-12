// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_style.h"

#include "sak/style_constants.h"

namespace sak::ui {

namespace {

constexpr double kExplorerHoverAlpha = 0.10;
constexpr double kExplorerPressedAlpha = 0.16;
constexpr double kExplorerCheckedAlpha = 0.14;
constexpr double kExplorerSelectionAlpha = 0.16;
constexpr double kExplorerSelectionHoverAlpha = 0.07;
constexpr int kExplorerControlRadiusPx = 6;
constexpr int kExplorerTabRadiusPx = 7;

QString chromeButtonStyles() {
    const QString scope = QStringLiteral(
        "#fileExplorerOmnibar QPushButton, #fileExplorerCommandBar QPushButton, "
        "#fileExplorerCommandBar QToolButton, #fileExplorerTabRow QPushButton, "
        "#fileExplorerSidebarFooter QPushButton");
    return QStringLiteral(
               "%1 { background: transparent; border: 1px solid transparent; "
               "border-radius: %2px; padding: 5px 9px; color: palette(window-text); "
               "font-weight: 400; }"
               "%1:hover { background: %3; }"
               "%1:pressed { background: %4; }"
               "%1:checked { background: %5; }"
               "%1:disabled { color: %6; }"
               "#fileExplorerCommandBar QToolButton::menu-indicator { "
               "subcontrol-position: right center; subcontrol-origin: padding; "
               "left: -2px; }")
        .arg(scope)
        .arg(kExplorerControlRadiusPx)
        .arg(colorWithAlpha(kColorAccentWindows, kExplorerHoverAlpha),
             colorWithAlpha(kColorAccentWindows, kExplorerPressedAlpha),
             colorWithAlpha(kColorAccentWindows, kExplorerCheckedAlpha),
             cssColor(kColorTextDisabled));
}

QString addressInputStyles() {
    return QStringLiteral(
               "#fileExplorerOmnibar QLineEdit { background: palette(base); "
               "border: 1px solid palette(mid); border-radius: %1px; "
               "padding: 4px 10px; color: palette(text); "
               "selection-background-color: %2; }"
               "#fileExplorerOmnibar QLineEdit:focus { border-color: %3; }"
               "#fileExplorerBreadcrumb { background: palette(base); "
               "border: 1px solid palette(mid); border-radius: %1px; }"
               "#fileExplorerBreadcrumb QPushButton { background: transparent; "
               "border: none; border-radius: 4px; padding: 2px 7px; "
               "color: palette(text); }"
               "#fileExplorerBreadcrumb QPushButton:hover { background: %4; }"
               "#fileExplorerBreadcrumb QPushButton[breadcrumbCurrent=\"true\"] { "
               "font-weight: 600; color: palette(window-text); }"
               "#fileExplorerBreadcrumbOverflow::menu-indicator { image: none; }"
               "#fileExplorerBreadcrumbChevron { padding: 0 1px; }")
        .arg(kExplorerControlRadiusPx)
        .arg(colorWithAlpha(kColorAccentWindows, kExplorerSelectionAlpha),
             QString::fromLatin1(kColorAccentWindows),
             colorWithAlpha(kColorAccentWindows, kExplorerHoverAlpha));
}

QString tabStripStyles() {
    return QStringLiteral(
               "QTabBar#fileExplorerTabBar { background: transparent; }"
               "QTabBar#fileExplorerTabBar::tab { background: transparent; "
               "border: 1px solid transparent; border-top-left-radius: %1px; "
               "border-top-right-radius: %1px; padding: 5px 12px; margin-right: 2px; "
               "color: palette(text); }"
               "QTabBar#fileExplorerTabBar::tab:selected { background: palette(base); "
               "border-color: palette(mid); border-bottom-color: palette(base); "
               "color: palette(window-text); }"
               "QTabBar#fileExplorerTabBar::tab:hover:!selected { background: %2; }"
               "QTabBar#fileExplorerTabBar::close-button { "
               "image: url(:/icons/icons/fluent/close.svg); border-radius: 4px; "
               "margin: 2px; }"
               "QTabBar#fileExplorerTabBar::close-button:hover { background: %2; }"
               // Files InfoPane.xaml segmented pill: 4px-radius bordered
               // container, transparent radio buttons with 16px side padding;
               // the checked state gets a filled card and an accent selection
               // pill (approximated as a bottom border in QSS).
               "#fileExplorerInfoPanePill { background: transparent; "
               "border: 1px solid palette(mid); border-radius: 4px; }"
               "#fileExplorerInfoPanePill QPushButton { background: transparent; "
               "border: 1px solid transparent; border-radius: 4px; "
               "padding: 0 16px; color: palette(text); }"
               "#fileExplorerInfoPanePill QPushButton:checked { "
               "background: palette(base); border-color: palette(mid); "
               "border-bottom: 2px solid %3; color: palette(window-text); }"
               "#fileExplorerInfoPanePill QPushButton:hover:!checked { "
               "background: %2; }"
               "#fileExplorerInfoPaneDetailsScroll { border: none; "
               "border-top: 1px solid palette(mid); }"
               "#fileExplorerInfoPane QLabel[infoPaneSectionHeader=\"true\"] { "
               "font-weight: 600; color: palette(dark); padding: 4px 2px 0 2px; }"
               "#fileExplorerInfoPane QPlainTextEdit { border: none; "
               "background: palette(base); }")
        .arg(kExplorerTabRadiusPx)
        .arg(colorWithAlpha(kColorAccentWindows, kExplorerHoverAlpha),
             QString::fromLatin1(kColorAccentWindows));
}

QString sidebarStyles() {
    return QStringLiteral(
               "QListWidget#fileExplorerTargetList { background: transparent; "
               "border: none; padding: 4px 2px; }"
               "QListWidget#fileExplorerTargetList::item { border-radius: %1px; "
               "padding: 4px 8px; margin: 1px 4px; color: palette(text); }"
               "QListWidget#fileExplorerTargetList::item:hover { background: %2; }"
               "QListWidget#fileExplorerTargetList::item:selected { background: %3; "
               "color: palette(window-text); }"
               "QListWidget#fileExplorerTargetList::item:disabled { "
               "color: palette(dark); }")
        .arg(kExplorerControlRadiusPx)
        .arg(colorWithAlpha(kColorAccentWindows, kExplorerHoverAlpha),
             colorWithAlpha(kColorAccentWindows, kExplorerCheckedAlpha));
}

QString itemViewStyles() {
    return QStringLiteral(
               "#fileExplorerRoot QTableView, #fileExplorerRoot QListView { "
               "background: palette(base); alternate-background-color: palette(base); "
               "border: none; }"
               "#fileExplorerRoot QTableView { gridline-color: transparent; }"
               "#fileExplorerRoot QTableView::item, #fileExplorerRoot QListView::item { "
               "padding: 3px 6px; border: none; }"
               "#fileExplorerRoot QTableView::item:hover, "
               "#fileExplorerRoot QListView::item:hover { background: %1; }"
               "#fileExplorerRoot QTableView::item:selected, "
               "#fileExplorerRoot QListView::item:selected { background: %2; "
               "color: palette(window-text); }"
               "#fileExplorerRoot QHeaderView::section { background: transparent; "
               "border: none; border-bottom: 1px solid palette(mid); padding: 6px 8px; "
               "color: palette(text); font-weight: 400; }")
        .arg(colorWithAlpha(kColorAccentWindows, kExplorerSelectionHoverAlpha),
             colorWithAlpha(kColorAccentWindows, kExplorerSelectionAlpha));
}

QString statusAndSplitterStyles() {
    return QStringLiteral(
               "#fileExplorerStatusRow { border-top: 1px solid palette(mid); "
               "background: transparent; }"
               "#fileExplorerStatusRow QLabel, #fileExplorerStatusLabel, "
               "#fileExplorerStateLabel, #fileExplorerSummaryLabel { "
               "color: palette(dark); font-size: %1pt; padding: 2px 8px; "
               "background: transparent; }"
               "#fileExplorerRoot QSplitter::handle { background: transparent; }"
               "#fileExplorerCommandBar QFrame { color: palette(mid); "
               "max-height: 16px; margin: 0 4px; }")
        .arg(kFontSizeNote);
}

}  // namespace

QString fileExplorerShellStyleSheet() {
    return chromeButtonStyles() + addressInputStyles() + tabStripStyles() + sidebarStyles() +
           itemViewStyles() + statusAndSplitterStyles();
}

}  // namespace sak::ui
