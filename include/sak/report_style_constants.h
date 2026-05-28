// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file report_style_constants.h
/// @brief Shared HTML report CSS tied to GUI design tokens.

#pragma once

#include "sak/color_constants.h"
#include "sak/design_token_constants.h"

#include <QString>

namespace sak::report {

inline constexpr int kEnterpriseReportDefaultMaxWidthPx = 900;
inline constexpr qsizetype kEnterpriseReportStyleReserveChars = 3600;
inline constexpr int kAiHealthReportMaxWidthPx = 1080;

inline constexpr int kReportCssPercentFull = 100;
inline constexpr int kReportCssFractionDigits = 2;
inline constexpr double kEnterpriseReportBodyLineHeight = 1.55;
inline constexpr double kMarkdownReportBodyLineHeight = 1.48;
inline constexpr int kReportPageMarginPx = 24;
inline constexpr int kReportContainerVerticalMarginPx = 20;
inline constexpr int kReportContainerPaddingPx = 24;
inline constexpr int kReportHeading1FontPx = 24;
inline constexpr int kReportHeading1MarginBottomPx = 10;
inline constexpr int kReportHeading1BorderBottomPx = 3;
inline constexpr int kReportHeading1PaddingBottomPx = 8;
inline constexpr int kReportHeading2FontPx = 18;
inline constexpr int kReportHeading2MarginTopPx = 24;
inline constexpr int kReportHeading2MarginBottomPx = 10;
inline constexpr int kReportHeading2PaddingBottomPx = 5;
inline constexpr int kReportHeading3FontPx = 15;
inline constexpr int kReportHeading3MarginTopPx = 10;
inline constexpr int kReportHeading3MarginBottomPx = 6;
inline constexpr int kReportTableMarginVerticalPx = 12;
inline constexpr int kReportTableFontPx = 14;
inline constexpr int kReportTableHeaderPaddingVerticalPx = 8;
inline constexpr int kReportTableCellPaddingVerticalPx = 7;
inline constexpr int kReportTableCellPaddingHorizontalPx = 12;
inline constexpr int kReportStatMarginTopPx = 8;
inline constexpr int kReportStatMarginRightPx = 16;
inline constexpr int kReportMetaPaddingPx = 12;
inline constexpr int kReportStatsGridMinWidthPx = 190;
inline constexpr int kReportGridGapPx = 12;
inline constexpr int kReportStatValueFontPx = 26;
inline constexpr int kReportBadgePaddingVerticalPx = 4;
inline constexpr int kReportBadgePaddingHorizontalPx = 10;
inline constexpr int kReportListMarginVerticalPx = 10;
inline constexpr int kReportListPaddingLeftPx = 20;
inline constexpr int kReportListItemMarginVerticalPx = 5;
inline constexpr int kReportFooterMarginTopPx = 28;
inline constexpr int kReportFooterPaddingTopPx = 14;
inline constexpr int kReportFooterFontPx = 12;

inline constexpr int kAiReportMainPaddingHorizontalPx = 28;
inline constexpr int kAiReportMainPaddingTopPx = 28;
inline constexpr int kAiReportMainPaddingBottomPx = 42;
inline constexpr int kAiReportHeroGradientDegrees = 135;
inline constexpr int kAiReportHeroPaddingVerticalPx = 28;
inline constexpr int kAiReportHeroPaddingHorizontalPx = 30;
inline constexpr int kAiReportHeroMarginBottomPx = 18;
inline constexpr int kAiReportHeroHeadingFontPx = 30;
inline constexpr int kAiReportHeroHeadingMarginBottomPx = 8;
inline constexpr int kAiReportGridMinWidthPx = 220;
inline constexpr int kAiReportGridMarginVerticalPx = 16;
inline constexpr int kAiReportCardPaddingPx = 16;
inline constexpr int kAiReportCardMarginVerticalPx = 14;
inline constexpr int kAiReportLabelFontPx = 12;
inline constexpr int kAiReportValueFontPx = 18;
inline constexpr int kAiReportValueFontWeight = 750;
inline constexpr int kAiReportValueMarginTopPx = 3;
inline constexpr int kAiReportFindingBorderLeftPx = 4;
inline constexpr int kAiReportFindingPaddingVerticalPx = 12;
inline constexpr int kAiReportFindingPaddingHorizontalPx = 14;
inline constexpr int kAiReportSeverityFontPx = 12;

inline constexpr int kMarkdownReportMaxWidthPx = 980;
inline constexpr int kMarkdownHeading1FontPx = 26;
inline constexpr int kMarkdownHeading1MarginBottomPx = 18;
inline constexpr int kMarkdownHeading2FontPx = 18;
inline constexpr int kMarkdownHeading2MarginTopPx = 24;
inline constexpr int kMarkdownHeading2PaddingBottomPx = 6;

namespace detail {

inline QString cssColor(const char* color) {
    return QString::fromLatin1(color);
}

inline QString cssPx(int value) {
    return QStringLiteral("%1px").arg(value);
}

inline QString cssPercent(int value) {
    return QStringLiteral("%1%").arg(value);
}

inline QString cssNumber(double value) {
    return QString::number(value, 'f', kReportCssFractionDigits);
}

inline void appendReportPageStyles(QString& css, int max_width_px) {
    css += QStringLiteral("* { box-sizing: border-box; }");
    css += QStringLiteral(
               "body { font-family: 'Segoe UI', Arial, sans-serif; margin: %1; "
               "background: %2; color: %3; line-height: %4; }")
               .arg(cssPx(kReportPageMarginPx),
                    cssColor(ui::kColorBgPage),
                    cssColor(ui::kColorTextBody),
                    cssNumber(kEnterpriseReportBodyLineHeight));
    css += QStringLiteral(
               ".container, .metadata, .stats, section { max-width: %1; margin: "
               "%2 auto; background: %3; padding: %4; border-radius: %5; border: %6 solid %7; }")
               .arg(cssPx(max_width_px),
                    cssPx(kReportContainerVerticalMarginPx),
                    cssColor(ui::kColorBgWhite),
                    cssPx(kReportContainerPaddingPx),
                    cssPx(ui::kCssRadiusLargePx),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault));
}

inline void appendReportHeadingStyles(QString& css) {
    css += QStringLiteral(
               "h1 { color: %1; margin: 0 0 %2; font-size: %3; "
               "border-bottom: %4 solid %5; padding-bottom: %6; }")
               .arg(cssColor(ui::kColorTextPrimary),
                    cssPx(kReportHeading1MarginBottomPx),
                    cssPx(kReportHeading1FontPx),
                    cssPx(kReportHeading1BorderBottomPx),
                    cssColor(ui::kColorPrimary),
                    cssPx(kReportHeading1PaddingBottomPx));
    css += QStringLiteral(
               "h2 { color: %1; margin: %2 0 %3; padding-bottom: %4; "
               "border-bottom: %5 solid %6; font-size: %7; }")
               .arg(cssColor(ui::kColorTextHeading),
                    cssPx(kReportHeading2MarginTopPx),
                    cssPx(kReportHeading2MarginBottomPx),
                    cssPx(kReportHeading2PaddingBottomPx),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault),
                    cssPx(kReportHeading2FontPx));
    css += QStringLiteral("h3 { color: %1; margin: %2 0 %3; font-size: %4; }")
               .arg(cssColor(ui::kColorTextHeading),
                    cssPx(kReportHeading3MarginTopPx),
                    cssPx(kReportHeading3MarginBottomPx),
                    cssPx(kReportHeading3FontPx));
}

inline void appendReportTableStyles(QString& css) {
    css += QStringLiteral(
               "table { width: %1; border-collapse: collapse; margin: %2 0; "
               "font-size: %3; background: %4; }")
               .arg(cssPercent(kReportCssPercentFull),
                    cssPx(kReportTableMarginVerticalPx),
                    cssPx(kReportTableFontPx),
                    cssColor(ui::kColorBgWhite));
    css += QStringLiteral(
               "th { background: %1; color: %2; padding: %3 %4; text-align: left; "
               "border: %5 solid %6; font-weight: %7; }")
               .arg(cssColor(ui::kColorBgSurface),
                    cssColor(ui::kColorTextHeading),
                    cssPx(kReportTableHeaderPaddingVerticalPx),
                    cssPx(kReportTableCellPaddingHorizontalPx),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault),
                    QString::number(ui::kFontWeightBold));
    css += QStringLiteral("td { padding: %1 %2; border: %3 solid %4; vertical-align: top; }")
               .arg(cssPx(kReportTableCellPaddingVerticalPx),
                    cssPx(kReportTableCellPaddingHorizontalPx),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault));
    css += QStringLiteral("tr:nth-child(even) td, tr:hover { background: %1; }")
               .arg(cssColor(ui::kColorBgSurface));
}

inline void appendReportStatusStyles(QString& css) {
    css += QStringLiteral(".meta, .stat-label, .footer, .empty, .note { color: %1; }")
               .arg(cssColor(ui::kColorTextMuted));
    css += QStringLiteral(".stat { display: inline-block; margin: %1 %2 %1 0; }")
               .arg(cssPx(kReportStatMarginTopPx), cssPx(kReportStatMarginRightPx));
    css += QStringLiteral(
               ".meta-bar, .stat-card, .stat-box { background: %1; border-radius: %2; "
               "padding: %3; margin: %4 0; }")
               .arg(cssColor(ui::kColorBgInfoPanel),
                    cssPx(ui::kCssRadiusLargePx),
                    cssPx(kReportMetaPaddingPx),
                    cssPx(kReportStatMarginTopPx));
    css += QStringLiteral(
               ".stats-grid { display: grid; grid-template-columns: repeat(auto-fit, "
               "minmax(%1, 1fr)); gap: %2; margin: %2 0; }")
               .arg(cssPx(kReportStatsGridMinWidthPx), cssPx(kReportGridGapPx));
    css += QStringLiteral(".stat-value, .score { font-size: %1; font-weight: %2; color: %3; }")
               .arg(cssPx(kReportStatValueFontPx),
                    QString::number(ui::kFontWeightHeavy),
                    cssColor(ui::kColorPrimary));
    css += QStringLiteral(
               ".success, .selected, .status-healthy, .badge-healthy { color: %1; "
               "font-weight: %2; }")
               .arg(cssColor(ui::kStatusColorSuccess), QString::number(ui::kFontWeightBold));
    css += QStringLiteral(
               ".warning, .warn, .rec-warning, .status-warning, .badge-warning { "
               "color: %1; font-weight: %2; }")
               .arg(cssColor(ui::kStatusColorWarning), QString::number(ui::kFontWeightBold));
    css += QStringLiteral(
               ".error, .unmatched, .rec-critical, .status-critical, .badge-critical { "
               "color: %1; font-weight: %2; }")
               .arg(cssColor(ui::kStatusColorError), QString::number(ui::kFontWeightBold));
    css += QStringLiteral(".info, a { color: %1; }").arg(cssColor(ui::kStatusColorRunning));
    css += QStringLiteral(
               ".overall-status, .badge { display: inline-block; padding: %1 %2; "
               "border-radius: %3; font-weight: %4; background: %5; }")
               .arg(cssPx(kReportBadgePaddingVerticalPx),
                    cssPx(kReportBadgePaddingHorizontalPx),
                    cssPx(ui::kCssRadiusMediumPx),
                    QString::number(ui::kFontWeightHeavy),
                    cssColor(ui::kColorBgSurface));
    css += QStringLiteral(".badge-unknown { color: %1; }").arg(cssColor(ui::kColorTextMuted));
}

inline void appendReportUtilityStyles(QString& css) {
    css += QStringLiteral(".exact { background: %1; } .fuzzy { background: %2; }")
               .arg(cssColor(ui::kColorBgInfoPanel), cssColor(ui::kColorBgWarningPanel));
    css += QStringLiteral(".rec-list { margin: %1 0; padding-left: %2; } li { margin: %3 0; }")
               .arg(cssPx(kReportListMarginVerticalPx),
                    cssPx(kReportListPaddingLeftPx),
                    cssPx(kReportListItemMarginVerticalPx));
    css += QStringLiteral(
               ".footer { text-align: center; margin-top: %1; padding-top: %2; "
               "border-top: %3 solid %4; font-size: %5; }")
               .arg(cssPx(kReportFooterMarginTopPx),
                    cssPx(kReportFooterPaddingTopPx),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault),
                    cssPx(kReportFooterFontPx));
    css += QStringLiteral(
               "@media print { body { background: %1; margin: 0; } "
               ".container, .metadata, .stats, section { border: 0; margin: 0; "
               "max-width: none; } }")
               .arg(cssColor(ui::kColorBgWhite));
}

inline void appendAiReportShellStyles(QString& css) {
    css += QStringLiteral("main { max-width: %1; margin: 0 auto; padding: %2 %3 %4; }")
               .arg(cssPx(kAiHealthReportMaxWidthPx),
                    cssPx(kAiReportMainPaddingTopPx),
                    cssPx(kAiReportMainPaddingHorizontalPx),
                    cssPx(kAiReportMainPaddingBottomPx));
    css += QStringLiteral(
               ".hero { background: linear-gradient(%1deg, %2, %3); color: %4; "
               "border-radius: %5; padding: %6 %7; margin-bottom: %8; }")
               .arg(kAiReportHeroGradientDegrees)
               .arg(cssColor(ui::kColorTextPrimary),
                    cssColor(ui::kColorTextHeading),
                    cssColor(ui::kColorBgWhite),
                    cssPx(ui::kCssRadiusLargePx),
                    cssPx(kAiReportHeroPaddingVerticalPx),
                    cssPx(kAiReportHeroPaddingHorizontalPx),
                    cssPx(kAiReportHeroMarginBottomPx));
    css += QStringLiteral(
               ".hero h1 { color: %1; border: 0; margin: 0 0 %2; font-size: %3; padding: 0; }")
               .arg(cssColor(ui::kColorBgWhite),
                    cssPx(kAiReportHeroHeadingMarginBottomPx),
                    cssPx(kAiReportHeroHeadingFontPx));
    css += QStringLiteral(".hero p { margin: 0; color: %1; }")
               .arg(cssColor(ui::kColorBgUserBubbleText));
}

inline void appendAiReportGridStyles(QString& css) {
    css += QStringLiteral(
               ".grid, .finding-grid { display: grid; grid-template-columns: "
               "repeat(auto-fit, minmax(%1, 1fr)); gap: %2; margin: %3 0; }")
               .arg(cssPx(kAiReportGridMinWidthPx),
                    cssPx(kReportGridGapPx),
                    cssPx(kAiReportGridMarginVerticalPx));
    css += QStringLiteral(
               ".card, section { background: %1; border: %2 solid %3; border-radius: %4; "
               "padding: %5; margin: %6 0; }")
               .arg(cssColor(ui::kColorBgWhite),
                    cssPx(ui::kCssBorderWidthDefaultPx),
                    cssColor(ui::kColorBorderDefault),
                    cssPx(ui::kCssRadiusLargePx),
                    cssPx(kAiReportCardPaddingPx),
                    cssPx(kAiReportCardMarginVerticalPx));
    css += QStringLiteral(".label { color: %1; font-size: %2; text-transform: uppercase; }")
               .arg(cssColor(ui::kColorTextMuted), cssPx(kAiReportLabelFontPx));
    css += QStringLiteral(".value { font-size: %1; font-weight: %2; margin-top: %3; }")
               .arg(cssPx(kAiReportValueFontPx),
                    QString::number(kAiReportValueFontWeight),
                    cssPx(kAiReportValueMarginTopPx));
}

inline void appendAiReportFindingStyles(QString& css) {
    css += QStringLiteral(
               ".finding { border-left: %1 solid %2; padding: %3 %4; background: %5; "
               "border-radius: %6; }")
               .arg(cssPx(kAiReportFindingBorderLeftPx),
                    cssColor(ui::kStatusColorRunning),
                    cssPx(kAiReportFindingPaddingVerticalPx),
                    cssPx(kAiReportFindingPaddingHorizontalPx),
                    cssColor(ui::kColorBgSurface),
                    cssPx(ui::kCssRadiusMediumPx));
    css += QStringLiteral(
               ".severity { font-size: %1; font-weight: %2; text-transform: "
               "uppercase; color: %3; }")
               .arg(cssPx(kAiReportSeverityFontPx),
                    QString::number(ui::kFontWeightHeavy),
                    cssColor(ui::kStatusColorRunning));
    css += QStringLiteral(".sev-high { border-left-color: %1; } .sev-high .severity { color: %1; }")
               .arg(cssColor(ui::kStatusColorError));
    css += QStringLiteral(".sev-med { border-left-color: %1; } .sev-med .severity { color: %1; }")
               .arg(cssColor(ui::kStatusColorWarning));
    css += QStringLiteral(".sev-low { border-left-color: %1; } .sev-low .severity { color: %1; }")
               .arg(cssColor(ui::kStatusColorSuccess));
    css += QStringLiteral(
               ".mono { font-family: Consolas, 'Cascadia Mono', monospace; } "
               "summary { cursor: pointer; font-weight: %1; }")
               .arg(ui::kFontWeightBold);
}

}  // namespace detail

inline QString enterpriseReportStyleSheet(int max_width_px = kEnterpriseReportDefaultMaxWidthPx) {
    QString css;
    css.reserve(kEnterpriseReportStyleReserveChars);
    detail::appendReportPageStyles(css, max_width_px);
    detail::appendReportHeadingStyles(css);
    detail::appendReportTableStyles(css);
    detail::appendReportStatusStyles(css);
    detail::appendReportUtilityStyles(css);
    return css;
}

inline QString aiHealthReportStyleSheet() {
    QString css = enterpriseReportStyleSheet(kAiHealthReportMaxWidthPx);
    detail::appendAiReportShellStyles(css);
    detail::appendAiReportGridStyles(css);
    detail::appendAiReportFindingStyles(css);
    return css;
}

inline QString markdownReportStyleSheet() {
    QString css;
    css += QStringLiteral(
               "body { font-family: 'Segoe UI', Arial, sans-serif; line-height: %1; "
               "color: %2; max-width: %3; margin: %4 auto; padding: 0 %4; background: %5; }")
               .arg(detail::cssNumber(kMarkdownReportBodyLineHeight),
                    detail::cssColor(ui::kColorTextBody),
                    detail::cssPx(kMarkdownReportMaxWidthPx),
                    detail::cssPx(kReportPageMarginPx),
                    detail::cssColor(ui::kColorBgWhite));
    css += QStringLiteral("h1 { font-size: %1; margin: 0 0 %2; color: %3; }")
               .arg(detail::cssPx(kMarkdownHeading1FontPx),
                    detail::cssPx(kMarkdownHeading1MarginBottomPx),
                    detail::cssColor(ui::kColorTextPrimary));
    css += QStringLiteral(
               "h2 { font-size: %1; margin-top: %2; border-bottom: %3 solid %4; "
               "padding-bottom: %5; color: %6; }")
               .arg(detail::cssPx(kMarkdownHeading2FontPx),
                    detail::cssPx(kMarkdownHeading2MarginTopPx),
                    detail::cssPx(ui::kCssBorderWidthDefaultPx),
                    detail::cssColor(ui::kColorBorderDefault),
                    detail::cssPx(kMarkdownHeading2PaddingBottomPx),
                    detail::cssColor(ui::kColorTextPrimary));
    css += QStringLiteral("li { margin: %1 0; } a { color: %2; }")
               .arg(detail::cssPx(kReportListItemMarginVerticalPx),
                    detail::cssColor(ui::kStatusColorRunning));
    return css;
}

}  // namespace sak::report
