// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows11_theme.h"

#include "sak/style_constants.h"

#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QMenu>
#include <QPalette>
#include <QStyle>
#include <QWidget>

namespace sak::ui {

namespace {

void applyShadow(QWidget* widget) {
    Q_ASSERT(widget);
    if (!widget || widget->graphicsEffect()) {
        return;
    }

    const bool should_shadow = qobject_cast<QGroupBox*>(widget) ||
                               widget->property("sakElevated").toBool() ||
                               widget->property("sakCard").toBool() ||
                               (widget->styleSheet().contains("border-radius") &&
                                widget->styleSheet().contains("background-color"));

    if (should_shadow) {
        auto* shadow = new QGraphicsDropShadowEffect(widget);
        shadow->setBlurRadius(kCardShadowBlurRadius);
        shadow->setColor(
            QColor(kCardShadowRed, kCardShadowGreen, kCardShadowBlue, kCardShadowAlpha));
        shadow->setOffset(0.0, kCardShadowOffsetY);
        shadow->setEnabled(true);
        widget->setGraphicsEffect(shadow);
        widget->setAutoFillBackground(true);
    }
}

class ThemePolishEventFilter final : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_ASSERT(watched);
        if (!watched || !event) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() != QEvent::Show && event->type() != QEvent::Polish) {
            return QObject::eventFilter(watched, event);
        }

        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget) {
            return QObject::eventFilter(watched, event);
        }

        if (!widget->property("sakShadowApplied").toBool()) {
            applyShadow(widget);
            widget->setProperty("sakShadowApplied", true);
        }

        return QObject::eventFilter(watched, event);
    }
};

constexpr double kChromeDialogAlpha = 0.92;
constexpr double kChromeTooltipAlpha = 0.95;
constexpr double kChromeMenuBarAlpha = 0.85;
constexpr double kChromeToolBarAlpha = 0.88;
constexpr double kChromeMenuAlpha = 0.97;
constexpr double kChromeTabPaneAlpha = 0.80;
constexpr double kChromeTabAlpha = 0.90;
constexpr double kChromeHeaderAlpha = 0.95;
constexpr double kChromeViewAlpha = 0.96;
constexpr double kChromeProgressTrackAlpha = 0.85;
constexpr double kChromeBorderAlpha = 0.40;
constexpr double kChromeBorderSubtleAlpha = 0.35;
constexpr double kChromeBorderMediumAlpha = 0.45;
constexpr double kChromeBorderStrongAlpha = 0.55;
constexpr double kSelectionSubtleAlpha = 0.15;
constexpr double kSelectionSoftAlpha = 0.18;
constexpr double kSelectionMediumAlpha = 0.20;
constexpr double kSelectionStrongAlpha = 0.22;
constexpr double kGridLineAlpha = 0.30;
constexpr double kInputBackgroundAlpha = 0.98;
constexpr double kPrimaryGradientTopAlpha = 0.92;
constexpr double kPrimaryGradientBottomAlpha = 0.88;
constexpr double kPrimaryGradientChunkAlpha = 0.90;
constexpr double kScrollHandleAlpha = 0.60;
constexpr double kScrollHandleHoverAlpha = 0.70;

static constexpr const char kThemeBaseAndChromeStyles[] = R"SAK(
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

QString themeBaseAndChromeStyles() {
    return QString::fromUtf8(kThemeBaseAndChromeStyles)
        .arg(QString::fromLatin1(kColorTextPrimary))
        .arg(QString::fromLatin1(kColorBgPage))
        .arg(QString::fromLatin1(kColorBgPageHover))
        .arg(colorWithAlpha(kColorBgWhite, kChromeDialogAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeTooltipAlpha))
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(colorWithAlpha(kColorBgWhite, kChromeMenuBarAlpha))
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeToolBarAlpha))
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderSubtleAlpha))
        .arg(colorWithAlpha(kColorPrimary, kSelectionSubtleAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeMenuAlpha))
        .arg(colorWithAlpha(kColorPrimary, kSelectionSoftAlpha));
}

QString themeTabAndButtonStyles() {
    return QStringLiteral(R"(
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
    )")
               .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderMediumAlpha),
                    colorWithAlpha(kColorBgWhite, kChromeTabPaneAlpha),
                    colorWithAlpha(kColorBorderDefault, kChromeTabAlpha),
                    QString::fromLatin1(kColorTextHeading),
                    colorWithAlpha(kColorPrimary, kPrimaryGradientTopAlpha),
                    colorWithAlpha(kColorPrimaryDark, kPrimaryGradientBottomAlpha),
                    QString::fromLatin1(kColorButtonTextOnTone)) +
           actionButtonStyle(
               "QPushButton, QToolButton", kPrimaryButtonTone, false, kButtonPaddingCompactCss);
}

QString themeInputStyles() {
    return QStringLiteral(R"(
        QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit,
            QTimeEdit {
            background-color: %1;
            border: 1px solid %2;
            border-radius: 10px;
            padding: 6px 10px;
            selection-background-color: %3;
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus,
            QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus {
            border: 1px solid %4;
            background-color: %5;
        }

        QComboBox::drop-down {
            border-left: 1px solid %2;
            width: 24px;
        }

        QCheckBox, QRadioButton {
            spacing: 8px;
        }
    )")
        .arg(colorWithAlpha(kColorBgWhite, kInputBackgroundAlpha),
             QString::fromLatin1(kColorBorderDefault),
             colorWithAlpha(kColorPrimary, kGridLineAlpha),
             QString::fromLatin1(kColorPrimary),
             QString::fromLatin1(kColorBgWhite));
}

QString spinBoxFieldStyles() {
    return QStringLiteral(R"(
        QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit {
            min-height: %1px;
            padding-right: %2px;
        }
    )")
        .arg(kUiSpinBoxMinHeight)
        .arg(kUiSpinBoxStepperWidth);
}

QString spinBoxButtonStyles(const char* border,
                            const char* buttonBackground,
                            const char* buttonHover) {
    return QStringLiteral(R"(
        QSpinBox::up-button, QDoubleSpinBox::up-button, QDateEdit::up-button,
            QTimeEdit::up-button {
            subcontrol-origin: border;
            subcontrol-position: top right;
            width: %1px;
            margin: %2px %2px 0 0;
            border-left: 1px solid %3;
            border-bottom: 1px solid %3;
            border-top-right-radius: %4px;
            background: %5;
        }

        QSpinBox::down-button, QDoubleSpinBox::down-button, QDateEdit::down-button,
            QTimeEdit::down-button {
            subcontrol-origin: border;
            subcontrol-position: bottom right;
            width: %1px;
            margin: 0 %2px %2px 0;
            border-left: 1px solid %3;
            border-top: 1px solid %3;
            border-bottom-right-radius: %4px;
            background: %5;
        }

        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover, QDateEdit::up-button:hover,
            QTimeEdit::up-button:hover, QSpinBox::down-button:hover,
            QDoubleSpinBox::down-button:hover, QDateEdit::down-button:hover,
            QTimeEdit::down-button:hover {
            background: %6;
        }
    )")
        .arg(kUiSpinBoxStepperWidth)
        .arg(kUiSpinBoxStepperMargin)
        .arg(QString::fromLatin1(border))
        .arg(kCssRadiusMediumPx)
        .arg(QString::fromLatin1(buttonBackground))
        .arg(QString::fromLatin1(buttonHover));
}

QString spinBoxArrowStyles(const char* arrow) {
    return QStringLiteral(R"(
        QSpinBox::up-arrow, QDoubleSpinBox::up-arrow, QDateEdit::up-arrow, QTimeEdit::up-arrow {
            image: none;
            width: 0;
            height: 0;
            border-left: %1px solid transparent;
            border-right: %1px solid transparent;
            border-bottom: %2px solid %3;
        }

        QSpinBox::down-arrow, QDoubleSpinBox::down-arrow, QDateEdit::down-arrow,
            QTimeEdit::down-arrow {
            image: none;
            width: 0;
            height: 0;
            border-left: %1px solid transparent;
            border-right: %1px solid transparent;
            border-top: %2px solid %3;
        }
    )")
        .arg(kUiSpinBoxArrowWidth)
        .arg(kUiSpinBoxArrowHeight)
        .arg(QString::fromLatin1(arrow));
}

QString themeSpinBoxStepperStyles(bool dark) {
    const char* buttonBackground = dark ? kColorDarkBgSurface : kColorBgSurface;
    const char* buttonHover = dark ? kColorDarkBgHover : kColorBgPageHover;
    const char* border = dark ? kColorDarkBorderDefault : kColorBorderDefault;
    const char* arrow = dark ? kColorDarkTextBody : kColorTextSecondary;
    return spinBoxFieldStyles() + spinBoxButtonStyles(border, buttonBackground, buttonHover) +
           spinBoxArrowStyles(arrow);
}

QString themeIndicatorStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(kUiIconSmall)
        .arg(QString::fromLatin1(kColorBorderMuted))
        .arg(kCssRadiusSmallPx)
        .arg(QString::fromLatin1(kColorBgSurface))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(kCssRadiusLargePx);
}

QString themeProgressStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(kCssRadiusXLargePx - kCssBorderWidthDefaultPx)
        .arg(colorWithAlpha(kColorBorderDefault, kChromeProgressTrackAlpha))
        .arg(kUiProgressBarHeight)
        .arg(colorWithAlpha(kColorPrimary, kPrimaryGradientTopAlpha))
        .arg(colorWithAlpha(kColorPrimaryPressed, kPrimaryGradientChunkAlpha))
        .arg(kCssRadiusLargePx);
}

QString themeInputAndIndicatorStyles() {
    return themeInputStyles() + themeSpinBoxStepperStyles(false) + themeIndicatorStyles() +
           themeProgressStyles();
}

QString themeContainerAndTableStyles() {
    return QStringLiteral(R"(
        QGroupBox {
            border: 1px solid %1;
            border-radius: 12px;
            margin-top: 18px;
            padding: 26px 10px 10px 10px;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0px 8px;
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
    )")
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderStrongAlpha))
        .arg(QString::fromLatin1(kColorTextBody))
        .arg(colorWithAlpha(kColorBorderDefault, kChromeHeaderAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeViewAlpha))
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(colorWithAlpha(kColorBorderMuted, kGridLineAlpha))
        .arg(colorWithAlpha(kColorPrimary, kSelectionMediumAlpha))
        .arg(QString::fromLatin1(kColorTextPrimary))
        .arg(colorWithAlpha(kColorPrimary, kSelectionStrongAlpha))
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha));
}

QString themeSliderStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark));
}

QString themeScrollBarStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(colorWithAlpha(kColorBorderMuted, kScrollHandleAlpha))
        .arg(colorWithAlpha(kColorPrimary, kScrollHandleHoverAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeTabAlpha))
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha));
}

QString darkBaseChromeStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(QString::fromLatin1(kColorDarkTextPrimary))
        .arg(QString::fromLatin1(kColorDarkBgPage))
        .arg(QString::fromLatin1(kColorDarkBgChrome))
        .arg(QString::fromLatin1(kColorDarkBgPanel))
        .arg(QString::fromLatin1(kColorDarkBorderDefault))
        .arg(QString::fromLatin1(kColorDarkBgHover));
}

QString darkTabAndInputStyles() {
    return QStringLiteral(R"(
        QTabWidget::pane {
            background-color: %1;
            border: 1px solid %2;
        }

        QTabBar::tab {
            background: %3;
            color: %4;
        }

        QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit,
            QTimeEdit {
            background-color: %5;
            color: %6;
            border: 1px solid %2;
            selection-background-color: %7;
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus,
            QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus {
            background-color: %8;
            border: 1px solid %7;
        }

        QComboBox::drop-down {
            border-left: 1px solid %2;
        }

        QCheckBox::indicator {
            border: 1px solid %9;
            background: %5;
        }

        QRadioButton::indicator {
            border: 1px solid %9;
            background: %5;
        }
    )")
        .arg(QString::fromLatin1(kColorDarkBgPanel))
        .arg(QString::fromLatin1(kColorDarkBorderDefault))
        .arg(QString::fromLatin1(kColorDarkBgSurface))
        .arg(QString::fromLatin1(kColorDarkTextHeading))
        .arg(QString::fromLatin1(kColorDarkBgInput))
        .arg(QString::fromLatin1(kColorDarkTextPrimary))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorDarkBgInputFocus))
        .arg(QString::fromLatin1(kColorDarkBorderMuted));
}

QString darkContainerAndViewStyles() {
    return QStringLiteral(R"(
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
    )")
        .arg(QString::fromLatin1(kColorDarkBorderDefault))
        .arg(QString::fromLatin1(kColorDarkTextHeading))
        .arg(QString::fromLatin1(kColorDarkBgSurface))
        .arg(QString::fromLatin1(kColorDarkBgInput))
        .arg(QString::fromLatin1(kColorDarkTextPrimary))
        .arg(QString::fromLatin1(kColorDarkBgHover))
        .arg(QString::fromLatin1(kColorDarkBorderMuted))
        .arg(QString::fromLatin1(kColorPrimary));
}

QString darkThemeOverrideStyles() {
    return darkBaseChromeStyles() + darkTabAndInputStyles() + darkContainerAndViewStyles() +
           themeSpinBoxStepperStyles(true);
}

struct ThemePaletteSpec {
    const char* window;
    const char* chrome;
    const char* base;
    const char* alternate;
    const char* text;
    const char* body;
    const char* muted;
    const char* disabled;
    const char* border;
    const char* light;
    const char* dark;
    const char* shadow;
};

inline constexpr ThemePaletteSpec kLightPaletteSpec{kColorBgPage,
                                                    kColorBgPageHover,
                                                    kColorBgWhite,
                                                    kColorBgSurface,
                                                    kColorTextPrimary,
                                                    kColorTextBody,
                                                    kColorTextMuted,
                                                    kColorTextDisabled,
                                                    kColorBorderDefault,
                                                    kColorBgWhite,
                                                    kColorBorderMuted,
                                                    kColorTextPrimary};

inline constexpr ThemePaletteSpec kDarkPaletteSpec{kColorDarkBgPage,
                                                   kColorDarkBgChrome,
                                                   kColorDarkBgInput,
                                                   kColorDarkBgSurface,
                                                   kColorDarkTextPrimary,
                                                   kColorDarkTextBody,
                                                   kColorDarkTextSecondary,
                                                   kColorDarkTextDisabled,
                                                   kColorDarkBorderDefault,
                                                   kColorDarkBgHover,
                                                   kColorDarkBgPressed,
                                                   kColorDarkBgInputFocus};

const ThemePaletteSpec& themePaletteSpec(AppThemeMode mode) {
    return mode == AppThemeMode::Dark ? kDarkPaletteSpec : kLightPaletteSpec;
}

QPalette themePalette(AppThemeMode mode) {
    QPalette palette;
    const QColor highlight(QString::fromLatin1(kColorPrimary));
    const ThemePaletteSpec& spec = themePaletteSpec(mode);
    const auto color = [](const char* value) {
        return QColor(QString::fromLatin1(value));
    };

    palette.setColor(QPalette::Window, color(spec.window));
    palette.setColor(QPalette::WindowText, color(spec.text));
    palette.setColor(QPalette::Base, color(spec.base));
    palette.setColor(QPalette::AlternateBase, color(spec.alternate));
    palette.setColor(QPalette::ToolTipBase, color(spec.alternate));
    palette.setColor(QPalette::ToolTipText, color(spec.text));
    palette.setColor(QPalette::Text, color(spec.body));
    palette.setColor(QPalette::Button, color(spec.chrome));
    palette.setColor(QPalette::ButtonText, color(spec.text));
    palette.setColor(QPalette::BrightText, QColor(QString::fromLatin1(kColorButtonTextOnTone)));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText,
                     QColor(QString::fromLatin1(kColorButtonTextOnTone)));
    palette.setColor(QPalette::Light, color(spec.light));
    palette.setColor(QPalette::Midlight, color(spec.muted));
    palette.setColor(QPalette::Mid, color(spec.border));
    palette.setColor(QPalette::Dark, color(spec.dark));
    palette.setColor(QPalette::Shadow, color(spec.shadow));
    palette.setColor(QPalette::PlaceholderText, color(spec.muted));

    palette.setColor(QPalette::Disabled, QPalette::WindowText, color(spec.disabled));
    palette.setColor(QPalette::Disabled, QPalette::Text, color(spec.disabled));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, color(spec.disabled));
    return palette;
}

}  // namespace

QString windows11ThemeStyleSheet() {
    return windows11ThemeStyleSheet(AppThemeMode::Light);
}

QString windows11ThemeStyleSheet(AppThemeMode mode) {
    QString style = themeBaseAndChromeStyles() + themeTabAndButtonStyles() +
                    themeInputAndIndicatorStyles() + themeContainerAndTableStyles() +
                    themeSliderStyles() + themeScrollBarStyles();
    if (mode == AppThemeMode::Dark) {
        style += darkThemeOverrideStyles();
    }
    return style;
}

void applyWindows11Theme(QApplication& app, AppThemeMode mode) {
    app.setProperty("sakThemeMode",
                    mode == AppThemeMode::Dark ? QStringLiteral("dark") : QStringLiteral("light"));
    app.setPalette(themePalette(mode));
    app.setStyleSheet(windows11ThemeStyleSheet(mode));
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (!widget) {
            continue;
        }
        widget->setPalette(app.palette());
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}

AppThemeMode currentThemeMode(const QApplication& app) {
    return app.property("sakThemeMode").toString() == QStringLiteral("dark") ? AppThemeMode::Dark
                                                                             : AppThemeMode::Light;
}

void installThemePolishHelper(QApplication& app) {
    app.installEventFilter(new ThemePolishEventFilter(&app));
}

}  // namespace sak::ui
