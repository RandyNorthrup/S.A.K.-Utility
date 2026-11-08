// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows11_theme.h"

#include "sak/config_manager.h"
#include "sak/dialog_button_tones.h"
#include "sak/style_constants.h"

#include <QMenu>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>

namespace sak::ui {

namespace {

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
constexpr double kDisabledInputBorderAlpha = 0.60;

/// @p color at @p alpha over @p backdrop, as one opaque colour. A table paints its selection
/// colour and then the selected item's colour on top, so a translucent selection came out darker
/// in tables than in trees and lists; an opaque one paints the same however often it is drawn.
QString blendedOver(const char* color, double alpha, const char* backdrop) {
    const QColor top(QString::fromLatin1(color));
    const QColor under(QString::fromLatin1(backdrop));
    const auto mix = [alpha](int over, int base) {
        return qRound((alpha * over) + ((1.0 - alpha) * base));
    };
    return QColor(mix(top.red(), under.red()),
                  mix(top.green(), under.green()),
                  mix(top.blue(), under.blue()))
        .name(QColor::HexRgb);
}

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
        .arg(colorWithAlpha(kColorPrimary, kSelectionSoftAlpha))
        .arg(QString::fromLatin1(kColorTextMuted));
}

QString themeTabAndButtonStyles() {
    return QString::fromLatin1(kWindows11ThemeTabAndButtonStyles)
               .arg(QString::fromLatin1(kColorBorderDefault),
                    colorWithAlpha(kColorBgWhite, kChromeTabPaneAlpha),
                    colorWithAlpha(kColorBorderDefault, kChromeTabAlpha),
                    QString::fromLatin1(kColorTextHeading),
                    colorWithAlpha(kColorSelectedTabTop, kPrimaryGradientTopAlpha),
                    colorWithAlpha(kColorSelectedTabBottom, kPrimaryGradientBottomAlpha),
                    QString::fromLatin1(kColorButtonTextOnTone)) +
           actionButtonStyle(
               "QPushButton, QToolButton", kPrimaryButtonTone, false, kButtonPaddingCompactCss) +
           tabBarScrollControlsStyle();
}

QString themeInputStyles() {
    return QString::fromLatin1(kWindows11ThemeInputStyles)
        .arg(colorWithAlpha(kColorBgWhite, kInputBackgroundAlpha),
             QString::fromLatin1(kColorBorderDefault),
             colorWithAlpha(kColorPrimary, kGridLineAlpha),
             QString::fromLatin1(kColorPrimary),
             QString::fromLatin1(kColorBgWhite),
             QString::fromLatin1(kColorTextPrimary),
             QString::fromLatin1(kColorTextMuted),
             QString::fromLatin1(kColorTextDisabled),
             QString::fromLatin1(kColorBgSurface))
        .arg(colorWithAlpha(kColorBorderDefault, kDisabledInputBorderAlpha));
}

QString themeInputSelectorStyles(bool dark) {
    const char* button_background = dark ? kColorDarkBgSurface : kColorBgSurface;
    const char* button_hover = dark ? kColorDarkBgHover : kColorBgPageHover;
    const char* border = dark ? kColorDarkBorderDefault : kColorBorderDefault;
    const char* text = dark ? kColorDarkTextBody : kColorTextSecondary;
    const QString border_color = QString::fromLatin1(border);
    const QString button_color = QString::fromLatin1(button_background);
    const QString hover_color = QString::fromLatin1(button_hover);
    const QString text_color = QString::fromLatin1(text);
    const QString down_icon_path = dark ? kIconSelectorChevronDownDark : kIconSelectorChevronDown;
    const QString up_icon_path = dark ? kIconSelectorChevronUpDark : kIconSelectorChevronUp;
    const QString calendar_icon_path = dark ? kIconSelectorCalendarDark : kIconSelectorCalendar;
    const QString panel_color = QString::fromLatin1(dark ? kColorDarkBgPanel : kColorBgWhite);
    const QString selection_color = QString::fromLatin1(kColorPrimary);
    const CalendarPopupSelectorTone calendar_tone{.panel = panel_color,
                                                  .border = border_color,
                                                  .hoverBackground = hover_color,
                                                  .text = text_color,
                                                  .selection = selection_color,
                                                  .selectionText =
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
        .arg(QString::fromLatin1(kColorCheckBoxBorder))
        .arg(kCssRadiusSmallPx)
        .arg(QString::fromLatin1(kColorBgSurface))
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(kCssRadiusLargePx)
        .arg(kIconCheckBoxTickOnTone)
        .arg(kIconCheckBoxDashOnTone)
        .arg(QString::fromLatin1(kColorButtonTextOnTone));
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
    const QString selection = blendedOver(kColorPrimary, kSelectionStrongAlpha, kColorBgWhite);
    return QString::fromLatin1(kWindows11ThemeContainerAndTableStyles)
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(QString::fromLatin1(kColorTextBody))
        .arg(colorWithAlpha(kColorBorderDefault, kChromeHeaderAlpha))
        .arg(colorWithAlpha(kColorBgWhite, kChromeViewAlpha))
        .arg(QString::fromLatin1(kColorBorderDefault))
        .arg(colorWithAlpha(kColorBorderMuted, kGridLineAlpha))
        .arg(selection)
        .arg(QString::fromLatin1(kColorTextPrimary))
        .arg(selection)
        .arg(colorWithAlpha(kColorBorderMuted, kChromeBorderAlpha))
        .arg(kUiGroupBoxBorderRadius)
        .arg(kUiGroupBoxTitleMarginTop)
        .arg(kUiGroupBoxContentPaddingTop)
        .arg(kUiGroupBoxContentPaddingHorizontal)
        .arg(kUiGroupBoxContentPaddingBottom)
        .arg(kUiGroupBoxTitlePaddingHorizontal)
        .arg(QString::fromLatin1(kColorBgSurface));
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
        .arg(QString::fromLatin1(kColorDarkBgHover))
        .arg(QString::fromLatin1(kColorDarkTextSecondary));
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
        .arg(QString::fromLatin1(kColorDarkCheckBoxBorder))
        .arg(QString::fromLatin1(kColorPrimaryDark))
        .arg(QString::fromLatin1(kColorButtonTextOnTone))
        .arg(QString::fromLatin1(kColorDarkTextSecondary))
        .arg(QString::fromLatin1(kColorDarkTextMuted))
        .arg(QString::fromLatin1(kColorDarkBgPage))
        .arg(colorWithAlpha(kColorDarkBorderDefault, kDisabledInputBorderAlpha));
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
        .arg(QString::fromLatin1(kColorPrimary))
        .arg(QString::fromLatin1(kColorDarkBgSurface));
}

/// The rules the dark QWidget rule shadows, written again after it (see
/// kWindows11DarkProgressStyles): plain buttons and progress bars.
QString darkShadowedRules() {
    return actionButtonStyle(
               "QPushButton, QToolButton", kPrimaryButtonTone, false, kButtonPaddingCompactCss) +
           QString::fromLatin1(kWindows11DarkProgressStyles)
               .arg(QString::fromLatin1(kColorDarkBorderDefault),
                    QString::fromLatin1(kColorDarkBgInput));
}

QString darkThemeOverrideStyles() {
    return darkBaseChromeStyles() + darkTabAndInputStyles() + darkContainerAndViewStyles() +
           themeInputSelectorStyles(true) + darkShadowedRules();
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

inline constexpr ThemePaletteSpec kLightPaletteSpec{.window = kColorBgPage,
                                                    .chrome = kColorBgPageHover,
                                                    .base = kColorBgWhite,
                                                    .alternate = kColorBgSurface,
                                                    .text = kColorTextPrimary,
                                                    .body = kColorTextBody,
                                                    .muted = kColorTextMuted,
                                                    .disabled = kColorTextDisabled,
                                                    .border = kColorBorderDefault,
                                                    .light = kColorBgWhite,
                                                    // Dark is secondary text (File Explorer's
                                                    // section headers and status row); slate-400
                                                    // was 2.6:1 on white.
                                                    .dark = kColorTextMuted,
                                                    .shadow = kColorTextPrimary};

inline constexpr ThemePaletteSpec kDarkPaletteSpec{.window = kColorDarkBgPage,
                                                   .chrome = kColorDarkBgChrome,
                                                   .base = kColorDarkBgInput,
                                                   .alternate = kColorDarkBgSurface,
                                                   .text = kColorDarkTextPrimary,
                                                   .body = kColorDarkTextBody,
                                                   .muted = kColorDarkTextSecondary,
                                                   .disabled = kColorDarkTextDisabled,
                                                   .border = kColorDarkBorderDefault,
                                                   .light = kColorDarkBgHover,
                                                   // Dark is the muted-text role on both
                                                   // palettes; the File Explorer's secondary
                                                   // text uses it (slate-500 was 3.1-3.7:1 here).
                                                   .dark = kColorDarkTextSecondary,
                                                   .shadow = kColorDarkBgInputFocus};

const ThemePaletteSpec& themePaletteSpec(AppThemeMode mode) {
    return mode == AppThemeMode::Dark ? kDarkPaletteSpec : kLightPaletteSpec;
}

/// Applies @p mode unless it is already the applied theme: Windows and the menu can both ask for
/// the theme that is showing, and re-polishing every widget for nothing is visible work.
void showThemeMode(QApplication& app, AppThemeMode mode) {
    if (app.property("sakThemeMode").isValid() && currentThemeMode(app) == mode) {
        return;
    }
    applyWindows11Theme(app, mode);
}

/// Light and Dark are asked of Qt so the window frame matches; System hands it back to Windows.
void requestFrameScheme(ThemePreference preference) {
    QStyleHints* hints = QGuiApplication::styleHints();
    switch (preference) {
    case ThemePreference::Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        break;
    case ThemePreference::Dark:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        break;
    case ThemePreference::System:
        hints->unsetColorScheme();
        break;
    }
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

QString itemSelectionColor() {
    return usingDarkUiTheme() ? QString::fromLatin1(kColorDarkBgHover)
                              : blendedOver(kColorPrimary, kSelectionStrongAlpha, kColorBgWhite);
}

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
        if (widget == nullptr) {
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

ThemePreference themePreferenceFromSetting(const QString& setting) {
    if (setting == QLatin1String("light")) {
        return ThemePreference::Light;
    }
    if (setting == QLatin1String("dark")) {
        return ThemePreference::Dark;
    }
    return ThemePreference::System;
}

QString themePreferenceSetting(ThemePreference preference) {
    switch (preference) {
    case ThemePreference::Light:
        return QStringLiteral("light");
    case ThemePreference::Dark:
        return QStringLiteral("dark");
    case ThemePreference::System:
        break;
    }
    return QStringLiteral("system");
}

AppThemeMode themeModeFor(ThemePreference preference, Qt::ColorScheme windows_scheme) {
    switch (preference) {
    case ThemePreference::Light:
        return AppThemeMode::Light;
    case ThemePreference::Dark:
        return AppThemeMode::Dark;
    case ThemePreference::System:
        break;
    }
    return windows_scheme == Qt::ColorScheme::Dark ? AppThemeMode::Dark : AppThemeMode::Light;
}

ThemePreference savedThemePreference() {
    return themePreferenceFromSetting(ConfigManager::instance().getThemePreference());
}

void initializeAppTheme(QApplication& app) {
    // The theme makes every push button primary; a dialog's Close, Cancel and No take the
    // secondary tone instead.
    installDialogButtonTones(app);
    const ThemePreference preference = savedThemePreference();
    requestFrameScheme(preference);
    showThemeMode(app, themeModeFor(preference, QGuiApplication::styleHints()->colorScheme()));
    QObject::connect(QGuiApplication::styleHints(),
                     &QStyleHints::colorSchemeChanged,
                     &app,
                     [&app](Qt::ColorScheme scheme) {
                         const ThemePreference current = savedThemePreference();
                         if (current == ThemePreference::System) {
                             showThemeMode(app, themeModeFor(current, scheme));
                         }
                     });
}

void setThemePreference(QApplication& app, ThemePreference preference) {
    ConfigManager::instance().setThemePreference(themePreferenceSetting(preference));
    requestFrameScheme(preference);
    showThemeMode(app, themeModeFor(preference, QGuiApplication::styleHints()->colorScheme()));
}

}  // namespace sak::ui
