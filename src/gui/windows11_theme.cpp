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

QString themeBaseAndChromeStyles() {
    return QString::fromLatin1(kWindows11ThemeBaseAndChromeStyles)
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
    return QString::fromLatin1(kWindows11ThemeTabAndButtonStyles)
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
    return QString::fromLatin1(kWindows11ThemeInputStyles)
        .arg(colorWithAlpha(kColorBgWhite, kInputBackgroundAlpha),
             QString::fromLatin1(kColorBorderDefault),
             colorWithAlpha(kColorPrimary, kGridLineAlpha),
             QString::fromLatin1(kColorPrimary),
             QString::fromLatin1(kColorBgWhite));
}

QString themeInputSelectorStyles(bool dark) {
    const char* buttonBackground = dark ? kColorDarkBgSurface : kColorBgSurface;
    const char* buttonHover = dark ? kColorDarkBgHover : kColorBgPageHover;
    const char* border = dark ? kColorDarkBorderDefault : kColorBorderDefault;
    const char* text = dark ? kColorDarkTextBody : kColorTextSecondary;
    const QString border_color = QString::fromLatin1(border);
    const QString button_color = QString::fromLatin1(buttonBackground);
    const QString hover_color = QString::fromLatin1(buttonHover);
    const QString text_color = QString::fromLatin1(text);
    const QString down_icon_path = dark ? kIconSelectorChevronDownDark : kIconSelectorChevronDown;
    const QString up_icon_path = dark ? kIconSelectorChevronUpDark : kIconSelectorChevronUp;
    const QString calendar_icon_path = dark ? kIconSelectorCalendarDark : kIconSelectorCalendar;
    const QString panel_color = QString::fromLatin1(dark ? kColorDarkBgPanel : kColorBgWhite);
    const QString selection_color = QString::fromLatin1(kColorPrimary);
    const CalendarPopupSelectorTone calendar_tone{panel_color,
                                                  border_color,
                                                  hover_color,
                                                  text_color,
                                                  selection_color,
                                                  QString::fromLatin1(kColorButtonTextOnTone)};
    return comboBoxSelectorStyle(border_color, button_color, hover_color, down_icon_path) +
           spinBoxSelectorStyle(
               border_color, button_color, hover_color, up_icon_path, down_icon_path) +
           datePickerSelectorStyle(border_color, button_color, hover_color, calendar_icon_path) +
           calendarPopupSelectorStyle(calendar_tone);
}

QString themeIndicatorStyles() {
    return QString::fromLatin1(kWindows11ThemeIndicatorStyles)
        .arg(kUiIconSmall)
        .arg(QString::fromLatin1(kColorBorderMuted))
        .arg(kCssRadiusSmallPx)
        .arg(QString::fromLatin1(kColorBgSurface))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(kCssRadiusLargePx);
}

QString themeProgressStyles() {
    return QString::fromLatin1(kWindows11ThemeProgressStyles)
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(kCssRadiusXLargePx - kCssBorderWidthDefaultPx)
        .arg(colorWithAlpha(kColorBorderDefault, kChromeProgressTrackAlpha))
        .arg(kUiProgressBarHeight)
        .arg(colorWithAlpha(kColorPrimary, kPrimaryGradientTopAlpha))
        .arg(colorWithAlpha(kColorPrimaryPressed, kPrimaryGradientChunkAlpha))
        .arg(kCssRadiusLargePx);
}

QString themeInputAndIndicatorStyles() {
    return themeInputStyles() + themeInputSelectorStyles(false) + themeIndicatorStyles() +
           themeProgressStyles();
}

QString themeContainerAndTableStyles() {
    return QString::fromLatin1(kWindows11ThemeContainerAndTableStyles)
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
    return QString::fromLatin1(kWindows11ThemeSliderStyles)
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark));
}

QString themeScrollBarStyles() {
    return QString::fromLatin1(kWindows11ThemeScrollBarStyles)
        .arg(colorWithAlpha(kColorBorderMuted, kScrollHandleAlpha))
        .arg(colorWithAlpha(kColorPrimary, kScrollHandleHoverAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeTabAlpha))
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha));
}

QString darkBaseChromeStyles() {
    return QString::fromLatin1(kWindows11DarkBaseChromeStyles)
        .arg(QString::fromLatin1(kColorDarkTextPrimary))
        .arg(QString::fromLatin1(kColorDarkBgPage))
        .arg(QString::fromLatin1(kColorDarkBgChrome))
        .arg(QString::fromLatin1(kColorDarkBgPanel))
        .arg(QString::fromLatin1(kColorDarkBorderDefault))
        .arg(QString::fromLatin1(kColorDarkBgHover));
}

QString darkTabAndInputStyles() {
    return QString::fromLatin1(kWindows11DarkTabAndInputStyles)
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
    return QString::fromLatin1(kWindows11DarkContainerAndViewStyles)
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
           themeInputSelectorStyles(true);
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
