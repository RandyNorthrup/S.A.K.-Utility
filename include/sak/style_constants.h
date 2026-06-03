// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file style_constants.h
/// @brief Centralized design tokens for the SAK Utility GUI
///
/// All panels should reference these constants instead of hardcoding
/// color hex values, margin/spacing values, and font sizes.
/// This enables maintainable theming and eventual dark-mode support.

#pragma once

#include "sak/color_constants.h"
#include "sak/design_token_constants.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QSize>
#include <QString>

namespace sak::ui {

// -- Spacing & Margins -------------------------------------------------------

constexpr int kMarginNone = 0;       ///< No margin (flush layouts)
constexpr int kMarginTight = 6;      ///< Tight margin (compact widgets)
constexpr int kMarginSmall = 8;      ///< Small margin (compact panels)
constexpr int kMarginMedium = 12;    ///< Standard panel margin (most panels)
constexpr int kMarginLarge = 16;     ///< Large margin (dialogs, roomy layouts)
constexpr int kMarginXLarge = 20;    ///< Extra-large margin (wide panels)

constexpr int kSpacingNone = 0;      ///< No spacing (flush layouts)
constexpr int kSpacingTight = 4;     ///< Tight spacing (icon-text pairs)
constexpr int kSpacingSmall = 6;     ///< Small spacing (dense groups)
constexpr int kSpacingMedium = 8;    ///< Standard widget spacing
constexpr int kSpacingDefault = 10;  ///< Default layout spacing
constexpr int kSpacingLarge = 12;    ///< Section/dialog spacing
constexpr int kSpacingXLarge = 15;   ///< Extra-large spacing

// -- Font Sizes (points) -----------------------------------------------------

constexpr int kFontSizeTitle = 18;    ///< Application title (about dialog)
constexpr int kFontSizeSection = 13;  ///< Panel/section headers
constexpr int kFontSizeStatus = 11;   ///< Status labels
constexpr int kFontSizeBody = 10;     ///< Base body text
constexpr int kFontSizeNote = 9;      ///< Notes, hints, descriptions
constexpr int kFontSizeSmall = 8;     ///< Sub-labels, fine print

// -- CSS Numeric Tokens ------------------------------------------------------

constexpr int kButtonStyleReserveChars = 900;
constexpr int kToolButtonMenuIndicatorOffsetPx = -4;
constexpr auto kButtonPaddingDefaultCss = "8px 20px";
constexpr auto kButtonPaddingCompactCss = "8px 14px";
constexpr auto kButtonPaddingSlimCss = "5px 14px";
constexpr auto kButtonPaddingWideCss = "8px 24px";

// -- Common UI Dimensions ----------------------------------------------------

constexpr int kUiIconTiny = 12;
constexpr int kUiIconCompact = 14;
constexpr int kUiIconSmall = 16;
constexpr int kUiIconMedium = 20;
constexpr int kUiButtonSizeInline = 28;
constexpr int kUiButtonSizeMicro = 18;
constexpr int kUiButtonHeightMini = 30;
constexpr int kUiButtonHeightDialog = 34;
constexpr int kUiProgressBarHeight = 18;
constexpr int kUiSeparatorWidth = 1;
constexpr int kUiWidthNoMinimum = 0;
constexpr int kUiCompactColumnWidth = 42;
constexpr int kUiSmallColumnWidth = 60;
constexpr int kUiMediumColumnWidth = 80;
constexpr int kUiWideColumnWidth = 100;
constexpr int kUiSearchMinWidth = 180;
constexpr int kUiSearchMaxWidth = 280;
constexpr int kUiSelectorButtonWidth = 24;
constexpr int kUiSelectorButtonMargin = 2;
constexpr int kUiTabScrollButtonWidth = 26;
constexpr int kUiTabScrollButtonHeight = kUiButtonHeightMini;
constexpr int kUiTabScrollIconSize = kUiIconCompact;
constexpr int kUiTabScrollAreaWidth = (kUiTabScrollButtonWidth * 2) + (kUiSelectorButtonMargin * 4);
constexpr int kUiGroupBoxBorderRadius = kCssRadiusXLargePx;
constexpr int kUiGroupBoxTitleMarginTop = kMarginMedium;
constexpr int kUiGroupBoxContentPaddingTop = kMarginMedium;
constexpr int kUiGroupBoxContentPaddingHorizontal = kSpacingDefault;
constexpr int kUiGroupBoxContentPaddingBottom = kSpacingDefault;
constexpr int kUiGroupBoxTitlePaddingHorizontal = kMarginSmall;
constexpr int kUiSpinBoxMinHeight = 32;
constexpr int kUiSpinBoxStepperWidth = kUiSelectorButtonWidth;
constexpr int kUiSpinBoxStepperMargin = kUiSelectorButtonMargin;
inline const QString kIconSelectorChevronDown =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-down.svg");
inline const QString kIconSelectorChevronDownDark =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-down-dark.svg");
inline const QString kIconSelectorChevronUp =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-up.svg");
inline const QString kIconSelectorChevronUpDark =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-up-dark.svg");
inline const QString kIconSelectorChevronUpOnTone =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-up-on-tone.svg");
inline const QString kIconSelectorChevronDownOnTone =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-down-on-tone.svg");
inline const QString kIconSelectorChevronLeft =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-left.svg");
inline const QString kIconSelectorChevronLeftDark =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-left-dark.svg");
inline const QString kIconSelectorChevronRight =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-right.svg");
inline const QString kIconSelectorChevronRightDark =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-right-dark.svg");
inline const QString kIconSelectorChevronLeftOnTone =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-left-on-tone.svg");
inline const QString kIconSelectorChevronRightOnTone =
    QStringLiteral(":/icons/icons/icons8-selector-chevron-right-on-tone.svg");
inline const QString kIconSelectorCalendar =
    QStringLiteral(":/icons/icons/icons8-selector-calendar.svg");
inline const QString kIconSelectorCalendarDark =
    QStringLiteral(":/icons/icons/icons8-selector-calendar-dark.svg");
inline const QString kIconSelectorCheck = QStringLiteral(":/icons/icons/icons8-selector-check.svg");
inline const QString kIconPasswordEyeOpen = QStringLiteral(":/icons/icons/eye_open.svg");
inline const QString kIconPasswordEyeClosed = QStringLiteral(":/icons/icons/eye_closed.svg");
inline const QString kIconPasswordEyeOpenOnTone =
    QStringLiteral(":/icons/icons/eye_open_on_tone.svg");
inline const QString kIconPasswordEyeClosedOnTone =
    QStringLiteral(":/icons/icons/eye_closed_on_tone.svg");

inline bool usingDarkUiTheme() {
    return qApp && qApp->property("sakThemeMode").toString() == QStringLiteral("dark");
}

inline QString themedSelectorChevronLeftIcon() {
    return usingDarkUiTheme() ? kIconSelectorChevronLeftDark : kIconSelectorChevronLeft;
}

inline QString themedSelectorChevronRightIcon() {
    return usingDarkUiTheme() ? kIconSelectorChevronRightDark : kIconSelectorChevronRight;
}

inline QIcon selectorChevronUpToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconSelectorChevronUpOnTone, QSize(), QIcon::Normal);
    icon.addFile(usingDarkUiTheme() ? kIconSelectorChevronUpDark : kIconSelectorChevronUp,
                 QSize(),
                 QIcon::Disabled);
    return icon;
}

inline QIcon selectorChevronDownToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconSelectorChevronDownOnTone, QSize(), QIcon::Normal);
    icon.addFile(usingDarkUiTheme() ? kIconSelectorChevronDownDark : kIconSelectorChevronDown,
                 QSize(),
                 QIcon::Disabled);
    return icon;
}

inline QIcon selectorChevronLeftToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconSelectorChevronLeftOnTone, QSize(), QIcon::Normal);
    icon.addFile(themedSelectorChevronLeftIcon(), QSize(), QIcon::Disabled);
    return icon;
}

inline QIcon selectorChevronRightToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconSelectorChevronRightOnTone, QSize(), QIcon::Normal);
    icon.addFile(themedSelectorChevronRightIcon(), QSize(), QIcon::Disabled);
    return icon;
}

inline QIcon passwordEyeOpenToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconPasswordEyeOpenOnTone, QSize(), QIcon::Normal);
    icon.addFile(kIconPasswordEyeOpen, QSize(), QIcon::Disabled);
    return icon;
}

inline QIcon passwordEyeClosedToolButtonIcon() {
    QIcon icon;
    icon.addFile(kIconPasswordEyeClosedOnTone, QSize(), QIcon::Normal);
    icon.addFile(kIconPasswordEyeClosed, QSize(), QIcon::Disabled);
    return icon;
}

constexpr auto kSelectorComboDropDown = "QComboBox::drop-down";
constexpr auto kSelectorComboDownArrow = "QComboBox::down-arrow";
constexpr auto kSelectorSpinEditor =
    "QSpinBox, QDoubleSpinBox, QDateEdit, QDateTimeEdit, QTimeEdit";
constexpr auto kSelectorSpinUpButton =
    "QSpinBox::up-button, QDoubleSpinBox::up-button, QDateEdit::up-button, "
    "QDateTimeEdit::up-button, QTimeEdit::up-button";
constexpr auto kSelectorSpinDownButton =
    "QSpinBox::down-button, QDoubleSpinBox::down-button, QDateEdit::down-button, "
    "QDateTimeEdit::down-button, QTimeEdit::down-button";
constexpr auto kSelectorSpinUpArrow =
    "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QDateEdit::up-arrow, "
    "QDateTimeEdit::up-arrow, QTimeEdit::up-arrow";
constexpr auto kSelectorSpinDownArrow =
    "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QDateEdit::down-arrow, "
    "QDateTimeEdit::down-arrow, QTimeEdit::down-arrow";
constexpr auto kSelectorDatePickerDropDown = "QDateEdit::drop-down, QDateTimeEdit::drop-down";
constexpr auto kSelectorDatePickerDownArrow = "QDateEdit::down-arrow, QDateTimeEdit::down-arrow";
constexpr auto kSelectorCalendarWidget = "QCalendarWidget";
constexpr auto kSelectorCalendarNavBar = "QCalendarWidget QWidget#qt_calendar_navigationbar";
constexpr auto kSelectorCalendarNavButton = "QCalendarWidget QToolButton";
constexpr auto kSelectorCalendarView = "QCalendarWidget QAbstractItemView";

constexpr double kOverlayAlphaStrong = 0.97;
constexpr double kHoverAlphaSubtle = 0.08;
constexpr double kPressedAlphaSubtle = 0.15;
constexpr int kCssAlphaPrecision = 2;
constexpr double kCardShadowBlurRadius = 22.0;
constexpr double kCardShadowOffsetY = 6.0;
constexpr int kCardShadowRed = 15;
constexpr int kCardShadowGreen = 23;
constexpr int kCardShadowBlue = 42;
constexpr int kCardShadowAlpha = 38;
constexpr int kPaletteDarkModeLightnessThreshold = 128;
constexpr int kHtmlPreviewBodyFontPx = 13;
constexpr int kHtmlPreviewBodyPaddingPx = 8;
constexpr int kHtmlDetailPaddingPx = 12;
constexpr int kHtmlDetailLargePaddingPx = 16;
constexpr int kHtmlNoteMinHeightPx = 200;
constexpr int kHtmlTimelineBadgeBorderPx = 1;
constexpr int kHtmlTimelineBadgeRadiusPx = 4;
constexpr int kHtmlTimelineBadgePaddingVerticalPx = 1;
constexpr int kHtmlTimelineBadgePaddingHorizontalPx = 5;
constexpr int kHtmlTimelineMetaMarginTopPx = 2;

// -- HTML/Rich-Text Style Templates -----------------------------------------

inline constexpr auto kHtmlStyledDocumentOpen =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%1</title><style>%2</style>"
    "</head><body>";
inline constexpr auto kHtmlEnterpriseDocumentOpen =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>%1</title><style>%2</style>"
    "</head><body><div class='container'>";
inline constexpr auto kHtmlStyleTagOpen = "<style>\n";
inline constexpr auto kHtmlStyleTagClose = "</style>\n";
inline constexpr auto kHtmlStyleHeadBodyCloseOpen = "</style>\n</head>\n<body>\n";
inline constexpr auto kHtmlStyleHeadMainCloseOpen = "</style></head><body><main>";
inline constexpr auto kHtmlContainerClose = "</div>";
inline constexpr auto kHtmlDocumentClose = "</body></html>";
inline constexpr auto kHtmlDetailShellOpen =
    "<div style='font-family: Segoe UI, sans-serif; padding: %1px;'>";
inline constexpr auto kHtmlDetailHeading2Open =
    "<div style='font-family: Segoe UI, sans-serif; padding: %1px;'>"
    "<h2 style='color: %2;'>%3</h2>";
inline constexpr auto kHtmlContactDetailHeadingOpen =
    "<div style='font-family: Segoe UI, sans-serif; padding: %1px;'>"
    "<h2 style='color: %2; margin-bottom: %3px;'>%4 %5</h2>";
inline constexpr auto kHtmlNoteDetailTemplate =
    "<div style='font-family: Segoe UI, sans-serif; padding: %1px; background: %2; "
    "border-radius: %3px; min-height: %4px;'>"
    "<h3 style='color: %5;'>%6</h3>"
    "<p style='white-space: pre-wrap; color: %7;'>%8</p></div>";
inline constexpr auto kHtmlCalendarEventDetailOpen =
    "<div style='font-family: Segoe UI, sans-serif; padding: %1px;'>"
    "<div style='border-left: %2px solid %3; background: %4; padding: %5px %6px; "
    "border-radius: %7px; margin-bottom: %8px;'>"
    "<h3 style='color: %9; margin: 0 0 %10px 0;'>%11</h3>";
inline constexpr auto kHtmlEmailPreviewDocument =
    "<html><head><meta charset=\"utf-8\">"
    "<style>body { font-family: 'Segoe UI', sans-serif; font-size: %1px; margin: 0; "
    "padding: %2px; word-wrap: break-word; }"
    " img { max-width: 100%%; height: auto; }</style></head><body>%3</body></html>";
inline constexpr auto kHtmlHeading3Color = "<h3 style='color: %1;'>%2</h3>";
inline constexpr auto kHtmlBoldColor = "<b style='color: %1;'>%2</b>";
inline constexpr auto kHtmlSpanColor = "<span style='color: %1;'>%2</span>";
inline constexpr auto kHtmlSpanColorWeight = "<span style='color:%1; font-weight:%2;'>%3</span>";
inline constexpr auto kHtmlParagraphColorMargin = "<p style='color: %1; margin: %2px 0;'>%3</p>";
inline constexpr auto kHtmlParagraphColorMarginOpen = "<p style='color: %1; margin: %2px 0;'>%3";
inline constexpr auto kHtmlPreWrap = "<pre style='white-space: pre-wrap;'>%1</pre>";
inline constexpr auto kHtmlHorizontalRule = "<hr style='border: %1px solid %2;'>";
inline constexpr auto kHtmlHorizontalRulePreWrapParagraph =
    "<hr style='border: %1px solid %2;'><p style='white-space: pre-wrap;'>%3</p>";
inline constexpr auto kHtmlHorizontalRuleColorPreWrapParagraph =
    "<hr style='border: %1px solid %2;'><p style='color: %3; white-space: pre-wrap;'>%4</p>";
inline constexpr auto kHtmlLogMessage =
    "<span style='color: %1;'>%2</span> "
    "<span style='color: %3; font-weight: %4;'>[%5]</span> %6";
inline constexpr auto kHtmlTimelineBadge =
    "<span style=\"display:inline-block;border:%1px solid %2;"
    "color:%2;background:%3;border-radius:%4px;padding:%5px %6px;"
    "font-size:%7pt;font-weight:%8;white-space:nowrap;\">%9</span>";
inline constexpr auto kHtmlTimelineMeta =
    "<div style=\"color:%1;font-size:%2pt;margin-top:%3px;\">%4</div>";

// -- Windows 11 Theme Style Templates ---------------------------------------

inline constexpr auto kWindows11ThemeBaseAndChromeStyles = R"SAK(
        * {
            font-family: "Segoe UI";
            font-size: 10pt;
        }

        QWidget {
            color: %1;
            background-color: %2;
        }

        /* Leaf widgets & generic frames inherit parent bg
           -- prevents gray-on-white and white-on-gray patches */
        QLabel, QFrame, QCheckBox, QRadioButton {
            background: transparent;
        }

        QMainWindow {
            background-color: %3;
        }

        QDialog, QGroupBox {
            background-color: %4;
        }

        QToolTip {
            color: %1;
            background-color: %5;
            border: 1px solid %6;
            border-radius: 8px;
            padding: 6px 10px;
        }

        QMenuBar {
            background-color: %7;
            border-bottom: 1px solid %8;
        }

        QToolBar {
            background-color: %9;
            border-bottom: 1px solid %10;
            spacing: 8px;
            padding: 6px;
        }

        QMenuBar::item {
            padding: 6px 12px;
            border-radius: 6px;
            background: transparent;
        }

        QMenuBar::item:selected {
            background-color: %11;
        }

        QMenu {
            background-color: %12;
            border: 1px solid %6;
            border-radius: 8px;
            padding: 6px;
        }

        QMenu::item {
            padding: 6px 16px;
            border-radius: 6px;
        }

        QMenu::item:selected {
            background-color: %13;
        }
    )SAK";

inline constexpr auto kWindows11ThemeTabAndButtonStyles = R"SAK(
        QTabWidget::pane {
            border: 1px solid %1;
            border-radius: 12px;
            padding: 0px;
            background-color: %2;
        }

        QTabBar::tab {
            background: %3;
            border-radius: 10px;
            padding: 6px 12px;
            margin: 2px;
            color: %4;
        }

        QTabBar::tab:selected {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 %5,
                stop:1 %6);
            color: %7;
        }
    )SAK";

inline constexpr auto kWindows11ThemeInputStyles = R"SAK(
        QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit,
            QDateTimeEdit, QTimeEdit {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 10px;
            padding: 6px 10px;
            selection-background-color: %3;
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus,
            QDoubleSpinBox:focus, QDateEdit:focus, QDateTimeEdit:focus, QTimeEdit:focus {
            border: 1px solid %4;
            background-color: %5;
        }

        QCheckBox, QRadioButton {
            spacing: 8px;
        }
    )SAK";

inline constexpr auto kWindows11ThemeIndicatorStyles = R"SAK(
        QCheckBox::indicator, QRadioButton::indicator {
            width: %1px;
            height: %1px;
        }

        QCheckBox::indicator {
            border: 1px solid %2;
            border-radius: %3px;
            background: %4;
        }

        QCheckBox::indicator:checked {
            background: %5;
            border: 1px solid %6;
            image: url("data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'><path d='M6.5 12.5l-4-4 1.4-1.4 2.6 2.6 5.6-5.6 1.4 1.4z' fill='white'/></svg>");
        }

        QRadioButton::indicator {
            border: 1px solid %2;
            border-radius: %7px;
            background: %4;
        }

        QRadioButton::indicator:checked {
            background: %5;
            border: 1px solid %6;
        }
    )SAK";

inline constexpr auto kWindows11ThemeProgressStyles = R"SAK(
        QProgressBar {
            border: 1px solid %1;
            border-radius: %2px;
            background: %3;
            text-align: center;
            min-height: %4px;
            max-height: %4px;
        }

        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 %5,
                stop:1 %6);
            border-radius: %7px;
        }
    )SAK";

inline constexpr auto kWindows11ThemeContainerAndTableStyles = R"SAK(
        QGroupBox {
            border: 1px solid %1;
            border-radius: %11px;
            margin-top: %12px;
            padding: %13px %14px %15px %14px;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0px %16px;
            color: %2;
        }

        QHeaderView::section {
            background-color: %3;
            border: none;
            padding: 6px 8px;
        }

        QTableView, QListView, QTreeView {
            background: %4;
            border: 1px solid %5;
            border-radius: 10px;
            gridline-color: %6;
            selection-background-color: %7;
            selection-color: %8;
            padding: 4px;
        }

        QAbstractItemView::item {
            padding: 6px;
            border-radius: 6px;
        }

        QAbstractItemView::item:selected {
            background: %9;
        }

        QScrollArea {
            border: none;
            background: transparent;
        }

        QSplitter::handle {
            background: %10;
        }
    )SAK";

inline constexpr auto kWindows11ThemeSliderStyles = R"SAK(
        QSlider::groove:horizontal {
            height: 6px;
            background: %1;
            border-radius: 3px;
        }

        QSlider::handle:horizontal {
            background: %2;
            border: 1px solid %3;
            width: 16px;
            margin: -6px 0;
            border-radius: 8px;
        }

        QSlider::groove:vertical {
            width: 6px;
            background: %1;
            border-radius: 3px;
        }

        QSlider::handle:vertical {
            background: %2;
            border: 1px solid %3;
            height: 16px;
            margin: 0 -6px;
            border-radius: 8px;
        }
    )SAK";

inline constexpr auto kWindows11ThemeScrollBarStyles = R"SAK(
        QScrollBar:vertical, QScrollBar:horizontal {
            background: transparent;
            border: none;
            margin: 2px;
        }

        QScrollBar::groove:vertical, QScrollBar::groove:horizontal {
            background: transparent;
            border: none;
        }

        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: %1;
            border-radius: 6px;
            min-height: 32px;
            min-width: 32px;
        }

        QScrollBar::add-line, QScrollBar::sub-line {
            background: transparent;
            border: none;
            width: 0px;
            height: 0px;
        }

        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
        }

        QScrollBar::handle:hover {
            background: %2;
        }

        QAbstractScrollArea::corner {
            background: transparent;
        }

        QStatusBar {
            background: %3;
            border-top: 1px solid %4;
        }
    )SAK";

inline constexpr auto kWindows11DarkBaseChromeStyles = R"SAK(
        QWidget {
            color: %1;
            background-color: %2;
        }

        QLabel, QFrame, QCheckBox, QRadioButton {
            background: transparent;
        }

        QMainWindow {
            background-color: %3;
        }

        QDialog, QGroupBox {
            background-color: %4;
        }

        QToolTip {
            color: %1;
            background-color: %4;
            border: 1px solid %5;
            border-radius: 8px;
            padding: 6px 10px;
        }

        QMenuBar, QToolBar, QStatusBar {
            background-color: %3;
            border-color: %5;
        }

        QMenu {
            background-color: %4;
            color: %1;
            border: 1px solid %5;
        }

        QMenu::item:selected, QMenuBar::item:selected {
            background-color: %6;
        }
    )SAK";

inline constexpr auto kWindows11DarkTabAndInputStyles = R"SAK(
        QTabWidget::pane {
            background-color: %1;
            border: 1px solid %2;
        }

        QTabBar::tab {
            background: %3;
            color: %4;
        }

        QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit,
            QDateTimeEdit, QTimeEdit {
            background-color: %5;
            color: %6;
            border: 1px solid %2;
            selection-background-color: %7;
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus,
            QDoubleSpinBox:focus, QDateEdit:focus, QDateTimeEdit:focus, QTimeEdit:focus {
            background-color: %8;
            border: 1px solid %7;
        }

        QCheckBox::indicator {
            border: 1px solid %9;
            background: %5;
        }

        QRadioButton::indicator {
            border: 1px solid %9;
            background: %5;
        }
    )SAK";

inline constexpr auto kWindows11DarkContainerAndViewStyles = R"SAK(
        QGroupBox {
            border: 1px solid %1;
        }

        QGroupBox::title {
            color: %2;
        }

        QHeaderView::section {
            background-color: %3;
            color: %2;
        }

        QTableView, QListView, QTreeView {
            background: %4;
            color: %5;
            border: 1px solid %1;
            gridline-color: %1;
            selection-background-color: %6;
            selection-color: %5;
        }

        QAbstractItemView::item:selected {
            background: %6;
        }

        QSplitter::handle {
            background: %1;
        }

        QSlider::groove:horizontal, QSlider::groove:vertical {
            background: %1;
        }

        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: %7;
        }

        QScrollBar::handle:hover {
            background: %8;
        }
    )SAK";
inline constexpr auto kEmailRibbonStyle =
    "QWidget#ribbonBar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    " stop:0 %1, stop:1 %2); border: %3px solid %4; border-radius: %5px; }";
inline constexpr auto kEmailRibbonButtonStyle =
    "QToolButton { background: transparent; border: %1px solid transparent; "
    "border-radius: %2px; padding: %3px %4px; font-size: %5px; font-weight: %6; "
    "color: %7; }"
    "QToolButton:hover { background: %8; border: %1px solid %9; }"
    "QToolButton:pressed { background: %10; border: %1px solid %11; }"
    "QToolButton:disabled { color: %12; }";
inline constexpr auto kEmailRibbonSeparatorStyle = "background: %1; margin: %2px 0px;";
inline constexpr auto kAiWorkflowMarkdownStyle =
    "body{font-family:'Segoe UI',Arial,sans-serif;font-size:%1pt;color:%2;background:%3;}"
    "h1{font-size:%4pt;margin:0 0 %5px;color:%6;}h2{font-size:%7pt;margin:%8px 0 "
    "%9px;color:%6;}li{margin:%10px 0;}";
inline constexpr auto kAiWorkbenchListStyle =
    "QListWidget { background: %1; border: %2px solid %3; border-radius: %4px; "
    "padding: %5px; color: %6; }"
    "QListWidget::item { padding: %7px %8px; border-radius: %9px; }"
    "QListWidget::item:selected { background: %10; color: %6; }"
    "QListWidget::item:hover { background: %11; }";
inline constexpr auto kAiComposerEditStyle =
    "QPlainTextEdit { background: transparent; border: 0; color: %1; padding: %2px; "
    "font-size: %3pt; }";
inline constexpr auto kAiRunDetailsMarkdownStyle =
    "body{font-family:'Segoe UI',Arial,sans-serif;color:%1;background:%2;font-size:%3pt;}"
    "h1{font-size:%4pt;color:%5;margin:0 0 %6px;}"
    "pre{background:%7;border:%8px solid %9;border-radius:%10px;padding:%11px;white-space:"
    "pre-wrap;}";
inline constexpr auto kHtmlBrowserDocumentStyle =
    "body { font-family: 'Segoe UI', sans-serif; margin: %1px; color: %2; "
    "background: transparent; }"
    "h2, h3 { color: %3; margin-bottom: %4px; }"
    "p { color: %2; }"
    ".subtitle { color: %5; font-size: %6pt; margin-bottom: %7px; }"
    ".section, .dep { margin-bottom: %8px; }"
    ".section-title { font-weight: %9; font-size: %6pt; color: %10; "
    "border-bottom: %11px solid %12; padding-bottom: %13px; margin-bottom: %14px; }"
    "ul { margin: %13px 0 0 %15px; padding: 0; }"
    "li { margin-bottom: %13px; color: %2; }"
    "b { color: %3; }"
    ".dep .desc { color: %5; font-size: %16pt; }"
    "a { color: %10; text-decoration: none; }"
    "a:hover { text-decoration: underline; }"
    ".footer { color: %17; font-size: %16pt; margin-top: %8px; "
    "border-top: %11px solid %12; padding-top: %14px; }";

// -- Button Stylesheets ------------------------------------------------------

struct ButtonTone {
    const char* normalTop;
    const char* normalMiddle;
    const char* normalBottom;
    const char* hoverTop;
    const char* hoverMiddle;
    const char* hoverBottom;
    const char* pressedTop;
    const char* pressedMiddle;
    const char* pressedBottom;
    const char* border;
};

struct SolidButtonTone {
    const char* normalBackground;
    const char* hoverBackground;
    const char* pressedBackground;
    const char* disabledBackground;
};

struct SolidButtonMetrics {
    const char* padding;
    int radiusPx;
};

struct CalendarPopupSelectorTone {
    QString panel;
    QString border;
    QString hoverBackground;
    QString text;
    QString selection;
    QString selectionText;
};

inline constexpr SolidButtonMetrics kSolidButtonMetrics{kButtonPaddingWideCss, kCssRadiusSmallPx};

inline QString buttonGradientRule(const QString& selector,
                                  const char* state,
                                  const char* top,
                                  const char* middle,
                                  const char* bottom) {
    return selector + QString::fromLatin1(state) +
           QStringLiteral(
               " {"
               "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
               "    stop:0 ") +
           QString::fromLatin1(top) +
           QStringLiteral(
               ","
               "    stop:0.5 ") +
           QString::fromLatin1(middle) +
           QStringLiteral(
               ","
               "    stop:1 ") +
           QString::fromLatin1(bottom) +
           QStringLiteral(
               ");"
               "}");
}

inline QString buttonBaseRule(const QString& selector,
                              const ButtonTone& tone,
                              const char* padding) {
    return selector +
           QStringLiteral(
               " {"
               "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
               "    stop:0 ") +
           QString::fromLatin1(tone.normalTop) +
           QStringLiteral(
               ","
               "    stop:0.5 ") +
           QString::fromLatin1(tone.normalMiddle) +
           QStringLiteral(
               ","
               "    stop:1 ") +
           QString::fromLatin1(tone.normalBottom) +
           QStringLiteral(
               ");"
               "  color: ") +
           QString::fromLatin1(kColorButtonTextOnTone) +
           QStringLiteral(
               "; font-weight: 600;"
               "  padding: ") +
           QString::fromLatin1(padding) + QStringLiteral("; border-radius: ") +
           QString::number(kCssRadiusXLargePx) +
           QStringLiteral(
               "px;"
               "  border: ") +
           QString::number(kCssBorderWidthDefaultPx) + QStringLiteral("px solid ") +
           QString::fromLatin1(tone.border) +
           QStringLiteral(
               ";"
               "}");
}

inline QString buttonDisabledRule(const QString& selector) {
    return selector +
           QStringLiteral(
               ":disabled {"
               "  background: ") +
           QString::fromLatin1(kColorButtonDisabledBackground) + QStringLiteral("; color: ") +
           QString::fromLatin1(kColorTextMuted) +
           QStringLiteral(
               ";"
               "  border: ") +
           QString::number(kCssBorderWidthDefaultPx) + QStringLiteral("px solid ") +
           QString::fromLatin1(kColorButtonDisabledBorder) +
           QStringLiteral(
               ";"
               "}");
}

inline QString buttonMenuIndicatorRule(const char* selector) {
    return QStringLiteral(
               "%1::menu-indicator {"
               "  subcontrol-position: right center;"
               "  subcontrol-origin: padding;"
               "  left: %2px;"
               "}")
        .arg(QString::fromLatin1(selector))
        .arg(kToolButtonMenuIndicatorOffsetPx);
}

inline QString actionButtonStyle(const char* selector,
                                 const ButtonTone& tone,
                                 bool include_menu_indicator = false,
                                 const char* padding = kButtonPaddingDefaultCss) {
    const QString s = QString::fromLatin1(selector);
    QString style;
    style.reserve(kButtonStyleReserveChars);
    style += buttonBaseRule(s, tone, padding);
    style += buttonGradientRule(s, ":hover", tone.hoverTop, tone.hoverMiddle, tone.hoverBottom);
    style +=
        buttonGradientRule(s, ":pressed", tone.pressedTop, tone.pressedMiddle, tone.pressedBottom);
    style += buttonDisabledRule(s);
    if (include_menu_indicator) {
        style += buttonMenuIndicatorRule(selector);
    }
    return style;
}

inline constexpr ButtonTone kPrimaryButtonTone{kColorPrimaryButtonNormalTop,
                                               kColorPrimaryButtonNormalMiddle,
                                               kColorPrimaryButtonNormalBottom,
                                               kColorPrimaryButtonHoverTop,
                                               kColorPrimaryButtonHoverMiddle,
                                               kColorPrimaryButtonHoverBottom,
                                               kColorPrimaryButtonPressedTop,
                                               kColorPrimaryButtonPressedMiddle,
                                               kColorPrimaryButtonPressedBottom,
                                               kColorPrimaryButtonBorder};
inline constexpr ButtonTone kSecondaryButtonTone{kColorSecondaryButtonNormalTop,
                                                 kColorSecondaryButtonNormalMiddle,
                                                 kColorSecondaryButtonNormalBottom,
                                                 kColorSecondaryButtonHoverTop,
                                                 kColorSecondaryButtonHoverMiddle,
                                                 kColorSecondaryButtonHoverBottom,
                                                 kColorSecondaryButtonPressedTop,
                                                 kColorSecondaryButtonPressedMiddle,
                                                 kColorSecondaryButtonPressedBottom,
                                                 kColorSecondaryButtonBorder};
inline constexpr ButtonTone kSuccessButtonTone{kColorSuccessButtonNormalTop,
                                               kColorSuccessButtonNormalMiddle,
                                               kColorSuccessButtonNormalBottom,
                                               kColorSuccessButtonHoverTop,
                                               kColorSuccessButtonHoverMiddle,
                                               kColorSuccessButtonHoverBottom,
                                               kColorSuccessButtonPressedTop,
                                               kColorSuccessButtonPressedMiddle,
                                               kColorSuccessButtonPressedBottom,
                                               kColorSuccessButtonBorder};
inline constexpr ButtonTone kDangerButtonTone{kColorDangerButtonNormalTop,
                                              kColorDangerButtonNormalMiddle,
                                              kColorDangerButtonNormalBottom,
                                              kColorDangerButtonHoverTop,
                                              kColorDangerButtonHoverMiddle,
                                              kColorDangerButtonHoverBottom,
                                              kColorDangerButtonPressedTop,
                                              kColorDangerButtonPressedMiddle,
                                              kColorDangerButtonPressedBottom,
                                              kColorDangerButtonBorder};
inline constexpr ButtonTone kPauseButtonTone{kColorPauseButtonNormalTop,
                                             kColorPauseButtonNormalMiddle,
                                             kColorPauseButtonNormalBottom,
                                             kColorPauseButtonHoverTop,
                                             kColorPauseButtonHoverMiddle,
                                             kColorPauseButtonHoverBottom,
                                             kColorPauseButtonPressedTop,
                                             kColorPauseButtonPressedMiddle,
                                             kColorPauseButtonPressedBottom,
                                             kColorPauseButtonBorder};
inline constexpr ButtonTone kDiscordButtonTone{kColorDiscordButtonNormalTop,
                                               kColorDiscordButtonNormalMiddle,
                                               kColorDiscordButtonNormalBottom,
                                               kColorDiscordButtonHoverTop,
                                               kColorDiscordButtonHoverMiddle,
                                               kColorDiscordButtonHoverBottom,
                                               kColorDiscordButtonPressedTop,
                                               kColorDiscordButtonPressedMiddle,
                                               kColorDiscordButtonPressedBottom,
                                               kColorDiscordButtonBorder};

inline constexpr SolidButtonTone kSolidPrimaryButtonTone{
    kColorPrimary, kColorPrimaryHover, kColorPrimaryPressed, kColorTextDisabled};
inline constexpr SolidButtonTone kSolidDangerButtonTone{
    kColorDangerBtnNormal, kColorDangerBtnHover, kColorDangerBtnPressed, kColorTextDisabled};

inline QString solidButtonStyle(const char* selector,
                                const SolidButtonTone& tone,
                                SolidButtonMetrics metrics = kSolidButtonMetrics) {
    const auto css = [](const char* value) {
        return QString::fromLatin1(value);
    };
    const QString s = css(selector);
    return QStringLiteral(
               "%1 { background: %2; color: %3; padding: %4; border-radius: %5px; "
               "font-weight: %6; }"
               "%1:hover { background: %7; }"
               "%1:pressed { background: %8; }"
               "%1:disabled { background: %9; }")
        .arg(s,
             css(tone.normalBackground),
             css(kColorButtonTextOnTone),
             css(metrics.padding),
             QString::number(metrics.radiusPx),
             QString::number(kFontWeightBold),
             css(tone.hoverBackground),
             css(tone.pressedBackground),
             css(tone.disabledBackground));
}

inline QString colorWithAlpha(const char* color, double alpha) {
    const QColor qcolor(QString::fromLatin1(color));
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(qcolor.red())
        .arg(qcolor.green())
        .arg(qcolor.blue())
        .arg(alpha, 0, 'f', kCssAlphaPrecision);
}

struct CssColorAlias {
    const char* token;
    const char* css;
};

inline constexpr CssColorAlias kCssColorAliases[] = {
    {kColorTextPrimary, "palette(window-text)"},
    {kColorTextHeading, "palette(window-text)"},
    {kColorTextBody, "palette(text)"},
    {kColorTextSecondary, "palette(text)"},
    {kColorTextMuted, "palette(midlight)"},
    {kColorTextDisabled, "palette(midlight)"},
    {kColorBgWhite, "palette(base)"},
    {kColorBgSurface, "palette(alternate-base)"},
    {kColorBgInfoPanel, "palette(alternate-base)"},
    {kColorNoteGray, "palette(alternate-base)"},
    {kColorBgPage, "palette(window)"},
    {kColorBgPageHover, "palette(window)"},
    {kColorBorderDefault, "palette(mid)"},
    {kColorBorderMuted, "palette(mid)"},
};

struct HtmlPaletteAlias {
    const char* token;
    QPalette::ColorRole role;
};

inline constexpr HtmlPaletteAlias kHtmlPaletteAliases[] = {
    {kColorTextPrimary, QPalette::WindowText},
    {kColorTextHeading, QPalette::WindowText},
    {kColorTextBody, QPalette::Text},
    {kColorTextSecondary, QPalette::Text},
    {kColorTextMuted, QPalette::Midlight},
    {kColorTextDisabled, QPalette::Midlight},
    {kColorBgWhite, QPalette::Base},
    {kColorBgSurface, QPalette::AlternateBase},
    {kColorBgInfoPanel, QPalette::AlternateBase},
    {kColorBgPage, QPalette::Window},
    {kColorBgPageHover, QPalette::Window},
    {kColorBorderDefault, QPalette::Mid},
    {kColorBorderMuted, QPalette::Mid},
};

struct DarkHtmlAlias {
    const char* lightToken;
    const char* darkToken;
};

inline constexpr DarkHtmlAlias kDarkHtmlAliases[] = {
    {kColorNoteBlue, kColorDarkNoteBlue},
    {kColorNoteGreen, kColorDarkNoteGreen},
    {kColorNotePink, kColorDarkNotePink},
    {kColorNoteYellow, kColorDarkNoteYellow},
    {kColorNoteGray, kColorDarkNoteGray},
};

inline QString cssColor(const QString& color) {
    for (const auto& alias : kCssColorAliases) {
        if (color == QString::fromLatin1(alias.token)) {
            return QString::fromLatin1(alias.css);
        }
    }
    return color;
}

inline QString cssColor(const char* color) {
    return cssColor(QString::fromLatin1(color));
}

inline QString selectorDropDownStyle(const char* selector,
                                     const QString& border,
                                     const QString& background,
                                     const QString& hover_background) {
    return QStringLiteral(
               "%1 { subcontrol-origin: border; subcontrol-position: top right; width: %2px; "
               "margin: %3px %3px %3px 0; border-left: %4px solid %5; "
               "border-top-right-radius: %6px; border-bottom-right-radius: %6px; "
               "background: %7; }"
               "%1:hover { background: %8; }")
        .arg(QString::fromLatin1(selector))
        .arg(kUiSelectorButtonWidth)
        .arg(kUiSelectorButtonMargin)
        .arg(kCssBorderWidthDefaultPx)
        .arg(border)
        .arg(kCssRadiusMediumPx)
        .arg(background)
        .arg(hover_background);
}

inline QString selectorIconStyle(const char* selector, const QString& icon_path) {
    return QStringLiteral("%1 { image: url(\"%2\"); width: %3px; height: %3px; }")
        .arg(QString::fromLatin1(selector))
        .arg(icon_path)
        .arg(kUiIconCompact);
}

inline QString comboBoxSelectorStyle(const QString& border,
                                     const QString& background,
                                     const QString& hover_background,
                                     const QString& down_icon_path) {
    return selectorDropDownStyle(kSelectorComboDropDown, border, background, hover_background) +
           selectorIconStyle(kSelectorComboDownArrow, down_icon_path);
}

inline QString spinBoxSelectorStyle(const QString& border,
                                    const QString& background,
                                    const QString& hover_background,
                                    const QString& up_icon_path,
                                    const QString& down_icon_path) {
    const QString width = QString::number(kUiSpinBoxStepperWidth);
    const QString margin = QString::number(kUiSpinBoxStepperMargin);
    const QString border_width = QString::number(kCssBorderWidthDefaultPx);
    const QString radius = QString::number(kCssRadiusMediumPx);
    const QString up_button = QString::fromLatin1(kSelectorSpinUpButton);
    const QString down_button = QString::fromLatin1(kSelectorSpinDownButton);
    return QStringLiteral("%1 { min-height: %2px; padding-right: %3px; }")
               .arg(QString::fromLatin1(kSelectorSpinEditor))
               .arg(kUiSpinBoxMinHeight)
               .arg(width) +
           QStringLiteral(
               "%1 { subcontrol-origin: border; subcontrol-position: top right; width: %2px; "
               "margin: %3px %3px 0 0; border-left: %4px solid %5; "
               "border-bottom: %4px solid %5; border-top-right-radius: %6px; "
               "background: %7; }")
               .arg(up_button, width, margin, border_width, border, radius, background) +
           QStringLiteral(
               "%1 { subcontrol-origin: border; subcontrol-position: bottom right; width: %2px; "
               "margin: 0 %3px %3px 0; border-left: %4px solid %5; "
               "border-top: %4px solid %5; border-bottom-right-radius: %6px; "
               "background: %7; }")
               .arg(down_button, width, margin, border_width, border, radius, background) +
           QStringLiteral("%1:hover, %2:hover { background: %3; }")
               .arg(up_button, down_button, hover_background) +
           selectorIconStyle(kSelectorSpinUpArrow, up_icon_path) +
           selectorIconStyle(kSelectorSpinDownArrow, down_icon_path);
}

inline QString datePickerSelectorStyle(const QString& border,
                                       const QString& background,
                                       const QString& hover_background,
                                       const QString& calendar_icon_path) {
    return selectorDropDownStyle(
               kSelectorDatePickerDropDown, border, background, hover_background) +
           selectorIconStyle(kSelectorDatePickerDownArrow, calendar_icon_path);
}

inline QString calendarPopupSelectorStyle(const CalendarPopupSelectorTone& tone) {
    const QString border_width = QString::number(kCssBorderWidthDefaultPx);
    return QStringLiteral(
               "%1 { background: %2; border: %3px solid %4; }"
               "%5 { background: %2; border-bottom: %3px solid %4; }")
               .arg(QString::fromLatin1(kSelectorCalendarWidget),
                    tone.panel,
                    border_width,
                    tone.border,
                    QString::fromLatin1(kSelectorCalendarNavBar)) +
           QStringLiteral(
               "%1 { background: transparent; border: %2px solid transparent; "
               "border-radius: %3px; padding: %4px %5px; color: %6; }"
               "%1:hover { background: %7; border-color: %8; }")
               .arg(QString::fromLatin1(kSelectorCalendarNavButton),
                    border_width,
                    QString::number(kCssRadiusSmallPx),
                    QString::number(kCssPaddingSmallPx),
                    QString::number(kCssPaddingLargePx),
                    tone.text,
                    tone.hoverBackground,
                    tone.border) +
           QStringLiteral(
               "%1 { background: %2; color: %3; border: %4px solid %5; "
               "selection-background-color: %6; selection-color: %7; }")
               .arg(QString::fromLatin1(kSelectorCalendarView),
                    tone.panel,
                    tone.text,
                    border_width,
                    tone.border,
                    tone.selection,
                    tone.selectionText);
}

inline QString htmlPaletteColor(QPalette::ColorRole role) {
    Q_ASSERT_X(QApplication::instance() != nullptr,
               "sak::ui::htmlPaletteColor",
               "HTML theme colors require a QApplication instance");
    return QApplication::palette().color(role).name(QColor::HexRgb);
}

inline bool htmlUsingDarkPalette() {
    return QApplication::palette().color(QPalette::Window).lightness() <
           kPaletteDarkModeLightnessThreshold;
}

inline QString htmlColor(const QString& color) {
    for (const auto& alias : kHtmlPaletteAliases) {
        if (color == QString::fromLatin1(alias.token)) {
            return htmlPaletteColor(alias.role);
        }
    }
    if (htmlUsingDarkPalette()) {
        for (const auto& alias : kDarkHtmlAliases) {
            if (color == QString::fromLatin1(alias.lightToken)) {
                return QString::fromLatin1(alias.darkToken);
            }
        }
    }
    return color;
}

inline QString htmlColor(const char* color) {
    return htmlColor(QString::fromLatin1(color));
}

inline QString htmlBrowserDocumentStyleSheet() {
    const bool dark = qApp && qApp->property("sakThemeMode").toString() == QStringLiteral("dark");
    const auto color = [](const char* token) {
        return QString::fromLatin1(token);
    };
    const QString heading = color(dark ? kColorDarkTextHeading : kColorTextHeading);
    const QString body = color(dark ? kColorDarkTextBody : kColorTextBody);
    const QString secondary = color(dark ? kColorDarkTextSecondary : kColorTextSecondary);
    const QString muted = color(kColorTextMuted);
    const QString border = color(dark ? kColorDarkBorderDefault : kColorBorderDefault);
    const QString link = color(kColorPrimary);

    return QString::fromLatin1(kHtmlBrowserDocumentStyle)
        .arg(kMarginSmall)
        .arg(body, heading)
        .arg(kSpacingTight)
        .arg(secondary)
        .arg(kFontSizeBody)
        .arg(kSpacingLarge)
        .arg(kSpacingLarge)
        .arg(kFontWeightBold)
        .arg(link)
        .arg(kCssBorderWidthDefaultPx)
        .arg(border)
        .arg(kCssPaddingTinyPx)
        .arg(kCssPaddingMediumPx)
        .arg(kMarginLarge)
        .arg(kFontSizeNote)
        .arg(muted);
}

/// Primary action button -- blue gradient with hover/pressed/disabled states.
inline const QString kPrimaryButtonStyle = actionButtonStyle("QPushButton", kPrimaryButtonTone);

/// Compact primary action button for dense tool rows.
inline const QString kCompactPrimaryButtonStyle =
    actionButtonStyle("QPushButton", kPrimaryButtonTone, false, kButtonPaddingSlimCss);

/// Primary QToolButton variant with dropdown indicator styling.
inline const QString kPrimaryToolButtonStyle =
    actionButtonStyle("QToolButton", kPrimaryButtonTone, true);

inline QString tabScrollButtonStyle() {
    return actionButtonStyle("QToolButton#sakTabScrollButton", kPrimaryButtonTone, false, "0px") +
           QStringLiteral(
               "QToolButton#sakTabScrollButton { min-width: %1px; max-width: %1px; "
               "min-height: %2px; max-height: %2px; margin: 0px %3px; padding: 0px; }")
               .arg(kUiTabScrollButtonWidth)
               .arg(kUiTabScrollButtonHeight)
               .arg(kUiSelectorButtonMargin);
}

inline QString tabBarScrollControlsStyle() {
    return QStringLiteral("QTabBar::scroller { width: %1px; }").arg(kUiTabScrollAreaWidth) +
           tabScrollButtonStyle();
}

/// Secondary action button -- subtle slate gradient for non-primary actions.
inline const QString kSecondaryButtonStyle = actionButtonStyle("QPushButton", kSecondaryButtonTone);

/// Compact secondary action button for dense tool rows.
inline const QString kCompactSecondaryButtonStyle =
    actionButtonStyle("QPushButton", kSecondaryButtonTone, false, kButtonPaddingSlimCss);

/// Success action button -- green gradient, uniform with theme QPushButton.
inline const QString kSuccessButtonStyle = actionButtonStyle("QPushButton", kSuccessButtonTone);

/// Danger action button -- red gradient, uniform with theme QPushButton.
inline const QString kDangerButtonStyle = actionButtonStyle("QPushButton", kDangerButtonTone);

/// Compact danger action button for dense tool rows.
inline const QString kCompactDangerButtonStyle =
    actionButtonStyle("QPushButton", kDangerButtonTone, false, kButtonPaddingSlimCss);

/// Pause action button -- amber gradient, uniform with theme QPushButton.
inline const QString kPauseButtonStyle = actionButtonStyle("QPushButton", kPauseButtonTone);

/// Discord action button -- brand gradient generated from the same button helper.
inline const QString kDiscordButtonStyle = actionButtonStyle("QPushButton", kDiscordButtonTone);

/// Compact Discord button -- preserves the original help-card button geometry.
inline const QString kDiscordCompactButtonStyle =
    actionButtonStyle("QPushButton", kDiscordButtonTone, false, kButtonPaddingCompactCss);

/// Solid primary button -- preserves legacy flat action colors.
inline const QString kSolidPrimaryButtonStyle = solidButtonStyle("QPushButton",
                                                                 kSolidPrimaryButtonTone);

/// Solid danger button -- preserves legacy flat destructive-action colors.
inline const QString kSolidDangerButtonStyle = solidButtonStyle("QPushButton",
                                                                kSolidDangerButtonTone);

/// Theme label text helpers.
inline const QString kMutedLabelStyle = QStringLiteral("color: %1;").arg(cssColor(kColorTextMuted));
inline const QString kTransparentWidgetStyle =
    QStringLiteral("border: none; background: transparent;");

inline QString textColorStyle(const char* color) {
    return QStringLiteral("color: %1;").arg(cssColor(color));
}

inline QString textColorStyle(const QString& color) {
    return QStringLiteral("color: %1;").arg(cssColor(color));
}

inline QString fontSizeStyle(int point_size) {
    return QStringLiteral("font-size: %1pt;").arg(point_size);
}

inline QString textColorAndFontSizeStyle(const char* color, int point_size) {
    return QStringLiteral("color: %1; font-size: %2pt;").arg(cssColor(color)).arg(point_size);
}

inline QString textColorAndFontSizeStyle(const QString& color, int point_size) {
    return QStringLiteral("color: %1; font-size: %2pt;").arg(cssColor(color)).arg(point_size);
}

inline QString paddedTextStyle(const char* color, int padding_px) {
    return QStringLiteral("color: %1; padding: %2px;").arg(cssColor(color)).arg(padding_px);
}

inline QString fontWeightStyle(int weight) {
    return QStringLiteral("font-weight: %1;").arg(weight);
}

inline QString fontWeightAndColorStyle(int weight, const char* color) {
    return QStringLiteral("font-weight: %1; color: %2;").arg(weight).arg(cssColor(color));
}

inline QString fontWeightAndColorStyle(int weight, const QString& color) {
    return QStringLiteral("font-weight: %1; color: %2;").arg(weight).arg(cssColor(color));
}

inline QString fontSizeWeightColorStyle(int point_size, int weight, const char* color) {
    return QStringLiteral("font-size: %1pt; font-weight: %2; color: %3;")
        .arg(point_size)
        .arg(weight)
        .arg(cssColor(color));
}

inline QString fontSizeWeightColorStyle(int point_size, int weight, const QString& color) {
    return QStringLiteral("font-size: %1pt; font-weight: %2; color: %3;")
        .arg(point_size)
        .arg(weight)
        .arg(cssColor(color));
}

inline QString backgroundStyle(const char* selector, const char* background) {
    return QStringLiteral("%1 { background: %2; }")
        .arg(QString::fromLatin1(selector))
        .arg(cssColor(background));
}

inline QString bareBackgroundStyle(const char* background) {
    return QStringLiteral("background: %1;").arg(cssColor(background));
}

inline QString transparentTextStyle(int point_size, int weight, const char* color) {
    return QStringLiteral(
               "background: transparent; border: 0; "
               "font-size: %1pt; font-weight: %2; color: %3;")
        .arg(point_size)
        .arg(weight)
        .arg(cssColor(color));
}

inline QString transparentTextStyle(double point_size, int weight, const char* color) {
    return QStringLiteral(
               "background: transparent; border: 0; "
               "font-size: %1pt; font-weight: %2; color: %3;")
        .arg(point_size, 0, 'f', 1)
        .arg(weight)
        .arg(cssColor(color));
}

inline QString transparentBodyTextStyle(double point_size, const char* color) {
    return QStringLiteral(
               "background: transparent; border: 0; "
               "font-size: %1pt; line-height: 142%; color: %2;")
        .arg(point_size, 0, 'f', 1)
        .arg(cssColor(color));
}

inline QString surfaceStyle(const char* selector,
                            const char* background,
                            const char* border,
                            int radius_px = kCssRadiusLargePx,
                            int padding_px = kCssPaddingLargePx) {
    return QStringLiteral(
               "%1 { background: %2; border: %3px solid %4; "
               "border-radius: %5px; padding: %6px; }")
        .arg(QString::fromLatin1(selector))
        .arg(cssColor(background))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(radius_px)
        .arg(padding_px);
}

inline constexpr auto kCardFrameSelector = "QFrame[sakCard=\"true\"]";

inline QString cardFrameStyle(int radius_px = kCssRadiusLargePx,
                              int padding_px = kMarginMedium,
                              const char* selector = kCardFrameSelector) {
    const QString selector_string = QString::fromLatin1(selector);
    return QStringLiteral(
               "%1 { background-color: %2; border: %3px solid %4; "
               "border-radius: %5px; padding: %6px; }"
               "%1:hover { border-color: %7; }")
        .arg(selector_string)
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(radius_px)
        .arg(padding_px)
        .arg(cssColor(kColorPrimary));
}

inline QString cardTitleTextStyle() {
    return transparentTextStyle(kFontSizeSection, kFontWeightBold, kColorTextHeading);
}

inline QString cardDescriptionTextStyle() {
    return QStringLiteral("background: transparent; border: 0; font-size: %1pt; color: %2;")
        .arg(kFontSizeBody)
        .arg(cssColor(kColorTextSecondary));
}

inline QString panelBorderStyle(const char* selector, const char* background, const char* border) {
    return QStringLiteral(
               "%1 { background: %2; border: %3px solid %4; "
               "border-radius: %5px; }")
        .arg(QString::fromLatin1(selector))
        .arg(cssColor(background))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(kCssRadiusSmallPx);
}

inline QString topOrBottomBarStyle(const char* background,
                                   const char* border,
                                   const char* edge = "bottom") {
    return QStringLiteral("background: %1; border-%2: %3px solid %4;")
        .arg(cssColor(background))
        .arg(QString::fromLatin1(edge))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border));
}

inline QString notePanelStyle(const char* background,
                              const char* text_color = kColorTextBody,
                              int padding_px = kCssPaddingXLargePx,
                              int radius_px = kCssRadiusXLargePx) {
    return QStringLiteral(
               "QLabel { padding: %1px; background-color: %2; "
               "border-radius: %3px; color: %4; }")
        .arg(padding_px)
        .arg(cssColor(background))
        .arg(radius_px)
        .arg(cssColor(text_color));
}

inline QString notePanelStyleWithFontSize(const char* background,
                                          const char* text_color,
                                          int point_size,
                                          int padding_px = kCssPaddingXLargePx,
                                          int radius_px = kCssRadiusLargePx) {
    return QStringLiteral(
               "QLabel { padding: %1px; background-color: %2; "
               "border-radius: %3px; color: %4; font-size: %5pt; }")
        .arg(padding_px)
        .arg(cssColor(background))
        .arg(radius_px)
        .arg(cssColor(text_color))
        .arg(point_size);
}

inline QString warningPanelStyle() {
    return QStringLiteral(
               "color: %1; padding: %2px; background-color: %3; "
               "border: %4px solid %5; border-radius: %6px;")
        .arg(QString::fromLatin1(kColorErrorText))
        .arg(kCssPaddingLargePx)
        .arg(QString::fromLatin1(kColorBgErrorPanel))
        .arg(kCssBorderWidthDefaultPx)
        .arg(QString::fromLatin1(kColorError))
        .arg(kCssRadiusSmallPx);
}

inline QString textBrowserSurfaceStyle(const char* background, const char* border) {
    return QStringLiteral(
               "QTextEdit, QTextBrowser { background-color: %1; border: %2px solid %3; "
               "border-radius: %4px; padding: %5px; }")
        .arg(cssColor(background))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(kCssRadiusSmallPx)
        .arg(kCssPaddingLargePx);
}

inline QString textEditPreviewStyle(const char* background,
                                    const char* text_color,
                                    const char* border) {
    return QStringLiteral("QTextEdit { background: %1; color: %2; border: %3px solid %4; }")
        .arg(cssColor(background))
        .arg(cssColor(text_color))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border));
}

inline QString emailInspectorSearchStyle() {
    return QStringLiteral(
               "QLineEdit { border: %1px solid %2; border-radius: %3px; "
               "padding: %4px %5px; background: %6; color: %7; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusMediumPx)
        .arg(kCssPaddingMediumPx)
        .arg(kCssPaddingXLargePx)
        .arg(cssColor(kColorBgWhite))
        .arg(cssColor(kColorTextBody));
}

inline QString transparentHoverButtonStyle(const char* hover_color) {
    return QStringLiteral(
               "QPushButton { background: transparent; border: %1px solid transparent; "
               "border-radius: %2px; }"
               "QPushButton:hover { background: %3; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(kCssRadiusMediumPx)
        .arg(cssColor(hover_color));
}

inline QString emailContentBrowserStyle() {
    return QStringLiteral(
               "QTextBrowser { font-family: 'Segoe UI', sans-serif; "
               "font-size: 13px; padding: %1px; background: %2; }")
        .arg(kCssPaddingLargePx)
        .arg(cssColor(kColorBgWhite));
}

inline QString borderedPreviewStyle(const char* border, const char* background) {
    return QStringLiteral("border: %1px solid %2; background: %3;")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(cssColor(background));
}

inline QString imagePreviewStyle(const char* background, const char* border) {
    return QStringLiteral("background: %1; border: %2px solid %3; padding: %4px;")
        .arg(cssColor(background))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(kCssPaddingSmallPx);
}

inline QString groupBoxPanelStyle(const char* text_color, const char* border) {
    return QStringLiteral(
               "QGroupBox { font-weight: 600; color: %1; border: %2px solid %3; "
               "border-radius: %4px; margin-top: %5px; padding-top: %6px; }"
               "QGroupBox::title { subcontrol-origin: margin; left: %7px; "
               "padding: 0 %8px; }")
        .arg(cssColor(text_color))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border))
        .arg(kCssRadiusLargePx)
        .arg(kUiGroupBoxTitleMarginTop)
        .arg(kUiGroupBoxContentPaddingTop)
        .arg(kSpacingDefault)
        .arg(kSpacingSmall);
}

inline QString infoPopupFrameStyle() {
    return QStringLiteral(
               "#sakInfoPopup { background-color: %1; border: %2px solid %3; "
               "border-radius: %4px; padding: 0px; }")
        .arg(cssColor(kColorBgWhite))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusLargePx);
}

inline QString infoButtonStyle() {
    return QStringLiteral(
               "QToolButton { background: transparent; border: none; padding: 0; }"
               "QToolButton:hover { background: %1; border-radius: %2px; }"
               "QToolButton:pressed { background: %3; border-radius: %2px; }")
        .arg(colorWithAlpha(kColorAccentWindows, kHoverAlphaSubtle))
        .arg(kCssRadiusXLargePx)
        .arg(colorWithAlpha(kColorAccentWindows, kPressedAlphaSubtle));
}

inline QString infoPopupLabelStyle() {
    return QStringLiteral(
               "QLabel { color: %1; font-size: %2pt; background: transparent; "
               "border: none; padding: 0px; }")
        .arg(cssColor(kColorTextPrimary))
        .arg(kFontSizeNote);
}

inline QString mainIconFallbackStyle() {
    return QStringLiteral(
               "QLabel { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
               "stop:0 %1,stop:1 %2); border-radius: %3px; }")
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(kMarginMedium);
}

inline QString paddedStatusTextStyle(const char* color,
                                     int point_size,
                                     int vertical_padding_px = kCssPaddingTinyPx) {
    return QStringLiteral("color: %1; font-size: %2pt; padding: %3px 0;")
        .arg(cssColor(color))
        .arg(point_size)
        .arg(vertical_padding_px);
}

inline QString resultBadgeStyle(const char* background, const char* color) {
    return QStringLiteral(
               "background: %1; color: %2; padding: %3px; border-radius: %4px; "
               "font-weight: 600;")
        .arg(cssColor(background))
        .arg(cssColor(color))
        .arg(kCssPaddingLargePx)
        .arg(kCssRadiusMediumPx);
}

inline QString vulnerabilityLookupPanelStyle() {
    return panelBorderStyle("QFrame#vulnerabilityLookupPanel",
                            kColorBgSurface,
                            kColorBorderDefault);
}

inline QString compactLinkButtonStyle() {
    return QStringLiteral(
               "QPushButton { border: none; padding: %1px %2px; color: %3; "
               "font-size: %4pt; }"
               "QPushButton:hover { text-decoration: underline; }")
        .arg(kCssPaddingTinyPx)
        .arg(kCssPaddingMediumPx)
        .arg(cssColor(kColorTextPrimary))
        .arg(kFontSizeBody);
}

constexpr double kPartitionActionTextLinkHoverAlpha = 0.12;
constexpr double kPartitionActionTextLinkPressedAlpha = 0.18;

inline QString partitionActionTextLinkStyle() {
    return QStringLiteral(
               "QToolButton#partitionActionTextLink { background: transparent; border: none; "
               "border-radius: 0px; padding: %1px %2px; text-align: left; color: %3; }"
               "QToolButton#partitionActionTextLink:hover { background: %4; color: %5; }"
               "QToolButton#partitionActionTextLink:pressed { background: %6; }"
               "QToolButton#partitionActionTextLink:disabled { background: transparent; "
               "color: %7; }")
        .arg(kCssPaddingTinyPx)
        .arg(kCssPaddingSmallPx)
        .arg(cssColor(kColorTextBody))
        .arg(colorWithAlpha(kColorPrimary, kPartitionActionTextLinkHoverAlpha))
        .arg(cssColor(kColorPrimaryDark))
        .arg(colorWithAlpha(kColorPrimary, kPartitionActionTextLinkPressedAlpha))
        .arg(cssColor(kColorTextMuted));
}

inline QString wifiIndicatorStyle() {
    return QStringLiteral(
               "QTableWidget::indicator { width: %1px; height: %1px; "
               "border: %2px solid %3; border-radius: %4px; background: %5; }"
               "QTableWidget::indicator:checked { background: %6; "
               "border: %2px solid %7; }"
               "QTableWidget::indicator:unchecked { background: %5; "
               "border: %2px solid %3; }")
        .arg(kUiIconSmall)
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderMuted))
        .arg(kCssRadiusSmallPx)
        .arg(cssColor(kColorBgSurface))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark));
}

inline QString elevationBannerStyle() {
    return QStringLiteral(
               "QFrame#elevationBanner { background-color: %1; border: %2px solid %3; "
               "border-radius: %4px; padding: %5px %6px; }")
        .arg(cssColor(kColorBgInfoPanel))
        .arg(kCssBorderWidthDefaultPx)
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(kCssRadiusMediumPx)
        .arg(kSpacingTight)
        .arg(kSpacingMedium);
}

inline QString panelSubtitleStyle() {
    return QStringLiteral("color: %1; margin-bottom: %2px;")
        .arg(cssColor(kColorTextMuted))
        .arg(kCssPaddingSmallPx + kCssBorderWidthDefaultPx);
}

inline QString transcriptBubbleStyle(bool user) {
    return user ? QStringLiteral(
                      "QFrame#aiTranscriptBubbleUser { background: %1; "
                      "border: %2px solid %3; border-radius: %4px; }")
                      .arg(QString::fromLatin1(kColorPrimaryDark))
                      .arg(kCssBorderWidthAccentPx)
                      .arg(cssColor(kColorBgWhite))
                      .arg(kCssPaddingXXLargePx)
                : QStringLiteral(
                      "QFrame#aiTranscriptBubbleResult { background: %1; "
                      "border: %2px solid %3; border-radius: %4px; }")
                      .arg(cssColor(kColorBgWhite))
                      .arg(kCssBorderWidthDefaultPx)
                      .arg(cssColor(kColorBorderDefault))
                      .arg(kCssPaddingXXLargePx);
}

inline QString transcriptToggleStyle(const QString& color) {
    return QStringLiteral(
               "QPushButton { border: 0; padding: %1px 0; text-align: left; "
               "font-weight: 700; color: %2; background: transparent; }")
        .arg(kCssPaddingTinyPx)
        .arg(cssColor(color));
}

inline QString transcriptActivityBubbleStyle() {
    return QStringLiteral(
               "QFrame#aiTranscriptActivityBubble { background: %1; "
               "border: %2px solid %3; border-radius: %4px; }")
        .arg(cssColor(kColorBgWhite))
        .arg(kCssBorderWidthDefaultPx)
        .arg(QString::fromLatin1(kStatusColorRunning))
        .arg(kCssPaddingXXLargePx);
}

inline QString approvalPromptStyle() {
    return QStringLiteral(
               "QDialog { background: %1; }"
               "QLabel#approvalHeading { color: %2; font-size: 20px; "
               "font-weight: 800; }"
               "QLabel#approvalBody { color: %3; font-size: %4pt; }"
               "QLabel#approvalCommandLabel { color: %2; font-size: %5pt; "
               "font-weight: 700; }"
               "QPlainTextEdit#approvalCommandBox { background: %6; color: %3; "
               "border: %7px solid %8; border-radius: %9px; padding: %10px; "
               "font-family: Consolas, 'Cascadia Mono', monospace; font-size: %5pt; }")
        .arg(cssColor(kColorBgWhite))
        .arg(cssColor(kColorTextHeading))
        .arg(cssColor(kColorTextBody))
        .arg(kFontSizeBody)
        .arg(kFontSizeNote)
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusMediumPx)
        .arg(kCssPaddingLargePx);
}

inline QString aiContextPaneStyle() {
    return QStringLiteral(
               "QFrame#aiContextPane { background: %1; border-left: %2px solid %3; }"
               "QLabel { color: %4; }"
               "QComboBox, QLineEdit { background: %5; border: %2px solid %3; "
               "border-radius: %6px; padding: %7px %8px; color: %4; }"
               "QComboBox:focus, QLineEdit:focus { border-color: %9; }")
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(cssColor(kColorTextBody))
        .arg(cssColor(kColorBgWhite))
        .arg(kCssRadiusSmallPx)
        .arg(kCssPaddingSmallPx)
        .arg(kCssPaddingMediumPx)
        .arg(QString::fromLatin1(kColorPrimaryDark));
}

inline QString aiWorkflowDetailsPanelStyle() {
    return panelBorderStyle("QFrame#aiWorkflowDetailsPanel", kColorBgWhite, kColorBorderDefault);
}

inline QString aiWorkflowDetailsBodyStyle() {
    return QStringLiteral(
               "QTextBrowser { background: %1; border: 0; color: %2; "
               "padding: %3px; font-size: %4pt; }")
        .arg(cssColor(kColorBgWhite))
        .arg(cssColor(kColorTextBody))
        .arg(kCssPaddingSmallPx)
        .arg(kFontSizeNote);
}

inline QString aiConversationPaneStyle() {
    return panelBorderStyle("QFrame#aiConversationPane", kColorBgWhite, kColorBorderDefault);
}

inline QString aiConversationHeaderStyle() {
    return QStringLiteral(
               "QFrame#aiConversationHeader { background: %1; "
               "border-bottom: %2px solid %3; }")
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault));
}

inline QString aiWorkflowProgressStyle() {
    return QStringLiteral(
               "QProgressBar { background: %1; border: %2px solid %3; "
               "border-radius: %4px; color: %5; text-align: center; "
               "font-size: %6pt; font-weight: 600; }"
               "QProgressBar::chunk { background: %7; border-radius: %8px; }")
        .arg(cssColor(kColorBgWhite))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusTinyPx)
        .arg(cssColor(kColorTextBody))
        .arg(kFontSizeSmall)
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(kCssPaddingTinyPx);
}

inline QString aiComposerStyle() {
    return QStringLiteral("QFrame#aiComposer { background: %1; border-top: %2px solid %3; }")
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault));
}

inline QString aiContextChipsListStyle() {
    return QStringLiteral(
        "QListWidget#aiContextChips { background: transparent; border: 0; "
        "padding: 0; }"
        "QListWidget#aiContextChips::item { background: transparent; "
        "border: 0; }");
}

inline QString aiContextChipStyle(const QString& background, const QString& border) {
    return QStringLiteral(
               "QFrame#aiContextChip { background: %1; border: %2px solid %3; "
               "border-radius: %4px; }")
        .arg(background)
        .arg(kCssBorderWidthDefaultPx)
        .arg(border)
        .arg(kCssRadiusSmallPx);
}

inline QString aiContextChipRemoveStyle() {
    return QStringLiteral(
               "QPushButton { background: transparent; border: %1px solid transparent; "
               "border-radius: %2px; padding: %3px; }"
               "QPushButton:hover { background: %4; border-color: %5; }"
               "QPushButton:pressed { background: %5; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(kCssRadiusTinyPx)
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBgPageHover))
        .arg(cssColor(kColorBorderDefault));
}

inline QString aiRunDetailsBodyStyle() {
    return QStringLiteral(
               "QTextBrowser { background: %1; border: %2px solid %3; "
               "border-radius: %4px; color: %5; padding: %6px; font-size: %7pt; }")
        .arg(cssColor(kColorBgSurface))
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusSmallPx)
        .arg(cssColor(kColorTextBody))
        .arg(kCssPaddingLargePx)
        .arg(kFontSizeNote);
}

inline QString calendarNavButtonStyle() {
    return QStringLiteral(
               "QToolButton { background: transparent; border: %1px solid %2; "
               "border-radius: %3px; padding: %4px %5px; color: %6; "
               "font-size: %7pt; }"
               "QToolButton:hover { background: %8; }"
               "QToolButton:pressed { background: %9; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusSmallPx)
        .arg(kCssPaddingSmallPx)
        .arg(kCssPaddingLargePx)
        .arg(cssColor(kColorTextSecondary))
        .arg(kFontSizeStatus)
        .arg(cssColor(kColorBgPageHover))
        .arg(cssColor(kColorBgSurface));
}

inline QString calendarViewToggleStyle() {
    return QStringLiteral(
               "QPushButton { background: transparent; border: %1px solid %2; "
               "border-radius: %3px; padding: %4px %5px; color: %6; "
               "font-weight: %7; }"
               "QPushButton:hover { background: %8; }"
               "QPushButton:checked { background: %9; color: %10; "
               "border: %1px solid %11; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusSmallPx)
        .arg(kCssPaddingSmallPx)
        .arg(kCssPaddingXXLargePx)
        .arg(cssColor(kColorTextBody))
        .arg(kFontWeightMedium)
        .arg(cssColor(kColorBgPageHover))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorButtonTextOnTone))
        .arg(QString::fromLatin1(kColorPrimaryDark));
}

inline QString calendarSearchStyle() {
    return QStringLiteral(
               "QLineEdit { border: %1px solid %2; border-radius: %3px; "
               "padding: %4px %5px; background: %6; color: %7; }"
               "QLineEdit:focus { border: %1px solid %8; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kCssRadiusSmallPx)
        .arg(kCssPaddingSmallPx)
        .arg(kCssPaddingLargePx)
        .arg(cssColor(kColorBgWhite))
        .arg(cssColor(kColorTextBody))
        .arg(QString::fromLatin1(kColorPrimary));
}

inline QString calendarClickableLabelStyle() {
    return QStringLiteral(
               "QLabel { font-size: %1pt; font-weight: 600; color: %2; "
               "padding: %3px %4px; border-radius: %5px; }"
               "QLabel:hover { background: %6; cursor: pointer; }")
        .arg(kFontSizeSection)
        .arg(cssColor(kColorTextHeading))
        .arg(kCssPaddingTinyPx)
        .arg(kCssPaddingMediumPx)
        .arg(kCssRadiusSmallPx)
        .arg(cssColor(kColorBgPageHover));
}

inline QString sectionLabelStyle(const char* color = kColorTextHeading, int margin_top_px = 0) {
    QString style = fontWeightAndColorStyle(600, color);
    if (margin_top_px > 0) {
        style += QStringLiteral(" margin-top: %1px;").arg(margin_top_px);
    }
    return style;
}

inline QString checkboxColorStyle(const QString& color) {
    return QStringLiteral("QCheckBox { color: %1; }").arg(color);
}

inline QString calendarEventTableStyle() {
    return QStringLiteral(
               "QTableWidget { border: %1px solid %2; font-size: %3pt; }"
               "QTableWidget::item:selected { background: %4; color: %5; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(kColorBorderDefault))
        .arg(kFontSizeSmall)
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorButtonTextOnTone));
}

inline QString aiWorkflowMarkdownStyle() {
    return QString::fromLatin1(kAiWorkflowMarkdownStyle)
        .arg(kFontSizeNote)
        .arg(cssColor(kColorTextBody),
             cssColor(kColorBgSurface),
             QString::number(kFontSizeSection),
             QString::number(kSpacingSmall),
             cssColor(kColorTextHeading),
             QString::number(kFontSizeBody),
             QString::number(kSpacingMedium),
             QString::number(kCssPaddingTinyPx))
        .arg(kSpacingTight);
}

inline QString aiWorkbenchListStyle() {
    return QString::fromLatin1(kAiWorkbenchListStyle)
        .arg(cssColor(kColorBgWhite),
             QString::number(kCssBorderWidthDefaultPx),
             cssColor(kColorBorderDefault),
             QString::number(kCssRadiusSmallPx),
             QString::number(kCssPaddingTinyPx),
             cssColor(kColorTextBody),
             QString::number(kCssPaddingSmallPx),
             QString::number(kCssPaddingMediumPx),
             QString::number(kCssRadiusTinyPx))
        .arg(cssColor(kColorBgInfoPanel), cssColor(kColorBgPageHover));
}

inline QString aiComposerEditStyle() {
    return QString::fromLatin1(kAiComposerEditStyle)
        .arg(cssColor(kColorTextBody),
             QString::number(kCssPaddingSmallPx),
             QString::number(kFontSizeBody));
}

inline QString aiRunDetailsMarkdownStyle() {
    return QString::fromLatin1(kAiRunDetailsMarkdownStyle)
        .arg(cssColor(kColorTextBody),
             cssColor(kColorBgSurface),
             QString::number(kFontSizeNote),
             QString::number(kFontSizeSection + 1),
             cssColor(kColorTextHeading),
             QString::number(kSpacingMedium),
             cssColor(kColorBgWhite),
             QString::number(kCssBorderWidthDefaultPx))
        .arg(cssColor(kColorBorderDefault),
             QString::number(kCssRadiusSmallPx),
             QString::number(kCssPaddingMediumPx));
}

inline QString leftBorderStyle(const char* border) {
    return QStringLiteral("QTextBrowser { border-left: %1px solid %2; }")
        .arg(kCssBorderWidthDefaultPx)
        .arg(cssColor(border));
}

inline QString passwordRevealButtonStyle() {
    return QStringLiteral("QToolButton { border: none; background: transparent; }");
}

inline const QString kFontWeightSemiboldStyle = fontWeightStyle(kFontWeightSemibold);
inline const QString kFontWeightBoldStyle = fontWeightStyle(kFontWeightBold);

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

// Margins must be monotonically increasing.
static_assert(kMarginNone < kMarginTight, "Margins must be monotonically increasing.");
static_assert(kMarginTight < kMarginSmall, "Margins must be monotonically increasing.");
static_assert(kMarginSmall < kMarginMedium, "Margins must be monotonically increasing.");
static_assert(kMarginMedium < kMarginLarge, "Margins must be monotonically increasing.");
static_assert(kMarginLarge < kMarginXLarge, "Margins must be monotonically increasing.");

// Spacings must be monotonically increasing.
static_assert(kSpacingNone < kSpacingTight, "Spacings must be monotonically increasing.");
static_assert(kSpacingTight < kSpacingSmall, "Spacings must be monotonically increasing.");
static_assert(kSpacingSmall < kSpacingMedium, "Spacings must be monotonically increasing.");
static_assert(kSpacingMedium < kSpacingDefault, "Spacings must be monotonically increasing.");
static_assert(kSpacingDefault < kSpacingLarge, "Spacings must be monotonically increasing.");
static_assert(kSpacingLarge < kSpacingXLarge, "Spacings must be monotonically increasing.");

// Font sizes must be monotonically increasing.
static_assert(kFontSizeSmall < kFontSizeNote, "Font sizes must be monotonically increasing.");
static_assert(kFontSizeNote < kFontSizeBody, "Font sizes must be monotonically increasing.");
static_assert(kFontSizeBody < kFontSizeStatus, "Font sizes must be monotonically increasing.");
static_assert(kFontSizeStatus < kFontSizeSection, "Font sizes must be monotonically increasing.");
static_assert(kFontSizeSection < kFontSizeTitle, "Font sizes must be monotonically increasing.");

// -- Icons (Icons8 win10, #7a8a9e) ------------------------------------------

/// Chevron left SVG (16 px logical, viewBox 0 0 32 32)
inline constexpr auto kIconChevronLeftSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
    "<path fill=\"#7a8a9e\" d=\"M 19.75 2.59375 L 19.03125 3.28125 "
    "L 7.03125 15.28125 L 6.34375 16 L 7.03125 16.71875 "
    "L 19.03125 28.71875 L 19.75 29.40625 L 20.46875 28.71875 "
    "L 24.0625 25.09375 L 24.75 24.40625 L 24.0625 23.6875 "
    "L 16.375 16 L 24.0625 8.3125 L 24.75 7.59375 L 24.0625 6.90625 "
    "L 20.46875 3.28125 Z M 19.75 5.4375 L 21.9375 7.625 "
    "L 14.25 15.28125 L 13.53125 16 L 14.25 16.71875 "
    "L 21.9375 24.375 L 19.75 26.5625 L 9.1875 16 Z\"/></svg>";

/// Chevron right SVG (16 px logical, viewBox 0 0 32 32)
inline constexpr auto kIconChevronRightSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
    "<path fill=\"#7a8a9e\" d=\"M 12.25 2.59375 L 11.53125 3.28125 "
    "L 7.9375 6.90625 L 7.25 7.59375 L 7.9375 8.3125 "
    "L 15.625 16 L 7.9375 23.6875 L 7.25 24.40625 L 7.9375 25.09375 "
    "L 11.53125 28.71875 L 12.25 29.40625 L 12.96875 28.71875 "
    "L 24.96875 16.71875 L 25.65625 16 L 24.96875 15.28125 "
    "L 12.96875 3.28125 Z M 12.25 5.4375 L 22.8125 16 "
    "L 12.25 26.5625 L 10.0625 24.375 L 17.75 16.71875 "
    "L 18.46875 16 L 17.75 15.28125 L 10.0625 7.625 Z\"/></svg>";

/// Chevron down SVG (16 px logical, viewBox 0 0 32 32)
inline constexpr auto kIconChevronDownSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 32 32\">"
    "<path fill=\"#7a8a9e\" d=\"M 2.59375 12.25 L 3.28125 11.53125 "
    "L 6.90625 7.9375 L 7.59375 7.25 L 8.3125 7.9375 "
    "L 16 15.625 L 23.6875 7.9375 L 24.40625 7.25 L 25.09375 7.9375 "
    "L 28.71875 11.53125 L 29.40625 12.25 L 28.71875 12.96875 "
    "L 16.71875 24.96875 L 16 25.65625 L 15.28125 24.96875 "
    "L 3.28125 12.96875 Z M 5.4375 12.25 L 16 22.8125 "
    "L 26.5625 12.25 L 24.375 10.0625 L 16.71875 17.75 "
    "L 16 18.46875 L 15.28125 17.75 L 7.625 10.0625 Z\"/></svg>";

}  // namespace sak::ui
