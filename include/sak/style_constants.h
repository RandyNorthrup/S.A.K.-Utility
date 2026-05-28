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
#include <QPalette>
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
constexpr int kUiSpinBoxMinHeight = 32;
constexpr int kUiSpinBoxStepperWidth = 24;
constexpr int kUiSpinBoxStepperMargin = 2;
constexpr int kUiSpinBoxArrowWidth = 4;
constexpr int kUiSpinBoxArrowHeight = 5;

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

/// Primary action button -- blue gradient with hover/pressed/disabled states.
inline const QString kPrimaryButtonStyle = actionButtonStyle("QPushButton", kPrimaryButtonTone);

/// Primary QToolButton variant with dropdown indicator styling.
inline const QString kPrimaryToolButtonStyle =
    actionButtonStyle("QToolButton", kPrimaryButtonTone, true);

/// Secondary action button -- subtle slate gradient for non-primary actions.
inline const QString kSecondaryButtonStyle = actionButtonStyle("QPushButton", kSecondaryButtonTone);

/// Success action button -- green gradient, uniform with theme QPushButton.
inline const QString kSuccessButtonStyle = actionButtonStyle("QPushButton", kSuccessButtonTone);

/// Danger action button -- red gradient, uniform with theme QPushButton.
inline const QString kDangerButtonStyle = actionButtonStyle("QPushButton", kDangerButtonTone);

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
        .arg(cssColor(kColorBgWhite))
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
        .arg(kMarginMedium)
        .arg(kFontSizeTitle)
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
