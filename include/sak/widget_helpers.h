// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file widget_helpers.h
/// @brief Shared UI helper functions for panel headers, accessibility,
///        status coloring, and table widget styling.

#pragma once

#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QString>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include "style_constants.h"

namespace sak {

// ── Panel Header Factory ────────────────────────────────────────────────────

/// @brief Create a consistent panel header with bold title and muted subtitle.
///
/// Every panel should call this to produce the same visual structure:
///   - A bold title at kFontSizeSection (13 pt)
///   - A muted subtitle in kColorTextMuted with 5 px bottom margin
///
/// @param parent   The parent widget that owns the labels.
/// @param title    The panel title text (e.g. "Quick Actions").
/// @param subtitle A one-line description of the panel's purpose.
/// @param layout   The QVBoxLayout to which the labels are appended.
inline void createPanelHeader(QWidget* parent, const QString& title,
                              const QString& subtitle, QVBoxLayout* layout) {
    auto* title_label = new QLabel(title, parent);
    QFont title_font  = title_label->font();
    title_font.setPointSize(ui::kFontSizeSection);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setAccessibleName(title);
    layout->addWidget(title_label);

    auto* subtitle_label = new QLabel(subtitle, parent);
    subtitle_label->setWordWrap(true);
    subtitle_label->setStyleSheet(
        QString("color: %1; margin-bottom: 5px;").arg(ui::kColorTextMuted));
    subtitle_label->setAccessibleName(subtitle);
    layout->addWidget(subtitle_label);
}

/// @brief Create a panel header with an icon to the left of the title/subtitle.
///
/// Mirrors the About panel's icon layout: a 48×48 SVG icon in an HBox
/// alongside the bold title and muted subtitle in a VBox.
///
/// @param parent   The parent widget that owns the labels.
/// @param iconPath Qt resource path to the SVG icon (e.g. ":/icons/icons/panel_foo.svg").
/// @param title    The panel title text.
/// @param subtitle A one-line description of the panel's purpose.
/// @param layout   The QVBoxLayout to which the header row is appended.
inline void createPanelHeader(QWidget* parent, const QString& iconPath,
                              const QString& title, const QString& subtitle,
                              QVBoxLayout* layout) {
    constexpr int kPanelIconSize = 48;

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(12);

    auto* iconLabel = new QLabel(parent);
    iconLabel->setFixedSize(kPanelIconSize, kPanelIconSize);
    iconLabel->setPixmap(
        QIcon(iconPath).pixmap(kPanelIconSize, kPanelIconSize));
    iconLabel->setAccessibleName(title + QStringLiteral(" icon"));
    headerRow->addWidget(iconLabel);

    auto* titleLayout = new QVBoxLayout();
    auto* title_label = new QLabel(title, parent);
    QFont title_font  = title_label->font();
    title_font.setPointSize(ui::kFontSizeSection);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setAccessibleName(title);
    titleLayout->addWidget(title_label);

    auto* subtitle_label = new QLabel(subtitle, parent);
    subtitle_label->setWordWrap(true);
    subtitle_label->setStyleSheet(
        QString("color: %1; margin-bottom: 5px;").arg(ui::kColorTextMuted));
    subtitle_label->setAccessibleName(subtitle);
    titleLayout->addWidget(subtitle_label);

    headerRow->addLayout(titleLayout);
    headerRow->addStretch();
    layout->addLayout(headerRow);
}

// ── Dynamic Panel Header ────────────────────────────────────────────────────

/// @brief Pointers returned by createDynamicPanelHeader for runtime updates.
struct PanelHeaderWidgets {
    QLabel* iconLabel{nullptr};
    QLabel* titleLabel{nullptr};
    QLabel* subtitleLabel{nullptr};
};

/// @brief Create a panel header whose icon, title, and subtitle can be updated
///        at runtime (e.g. when switching sub-tabs inside a composite panel).
[[nodiscard]] inline PanelHeaderWidgets createDynamicPanelHeader(
        QWidget* parent, const QString& iconPath,
        const QString& title, const QString& subtitle,
        QVBoxLayout* layout) {
    constexpr int kPanelIconSize = 48;

    auto* headerRow = new QHBoxLayout();
    headerRow->setSpacing(12);

    auto* iconLabel = new QLabel(parent);
    iconLabel->setFixedSize(kPanelIconSize, kPanelIconSize);
    iconLabel->setPixmap(
        QIcon(iconPath).pixmap(kPanelIconSize, kPanelIconSize));
    iconLabel->setAccessibleName(title + QStringLiteral(" icon"));
    headerRow->addWidget(iconLabel);

    auto* titleLayout = new QVBoxLayout();
    auto* title_label = new QLabel(title, parent);
    QFont title_font  = title_label->font();
    title_font.setPointSize(ui::kFontSizeSection);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setAccessibleName(title);
    titleLayout->addWidget(title_label);

    auto* subtitle_label = new QLabel(subtitle, parent);
    subtitle_label->setWordWrap(true);
    subtitle_label->setStyleSheet(
        QString("color: %1; margin-bottom: 5px;").arg(ui::kColorTextMuted));
    subtitle_label->setAccessibleName(subtitle);
    titleLayout->addWidget(subtitle_label);

    headerRow->addLayout(titleLayout);
    headerRow->addStretch();
    layout->addLayout(headerRow);

    return {iconLabel, title_label, subtitle_label};
}

/// @brief Update a dynamic panel header's icon, title, and subtitle in-place.
inline void updatePanelHeader(const PanelHeaderWidgets& hw,
                              const QString& iconPath,
                              const QString& title,
                              const QString& subtitle) {
    constexpr int kPanelIconSize = 48;
    if (hw.iconLabel) {
        hw.iconLabel->setPixmap(
            QIcon(iconPath).pixmap(kPanelIconSize, kPanelIconSize));
        hw.iconLabel->setAccessibleName(title + QStringLiteral(" icon"));
    }
    if (hw.titleLabel) {
        hw.titleLabel->setText(title);
        hw.titleLabel->setAccessibleName(title);
    }
    if (hw.subtitleLabel) {
        hw.subtitleLabel->setText(subtitle);
        hw.subtitleLabel->setAccessibleName(subtitle);
    }
}

// ── Accessibility Helpers ───────────────────────────────────────────────────

/// @brief Set accessible name and optional description on any QWidget.
///
/// Use this instead of calling setAccessibleName / setAccessibleDescription
/// individually to ensure widgets are never left without accessibility info.
///
/// @param widget The target widget (null-safe).
/// @param name   Short accessible name (read by screen readers).
/// @param description Optional longer description for context.
inline void setAccessible(QWidget* widget, const QString& name,
                          const QString& description = {}) {
    if (!widget) return;
    widget->setAccessibleName(name);
    if (!description.isEmpty()) {
        widget->setAccessibleDescription(description);
    }
}

/// @brief Map a status string to an appropriate semantic color
/// @param status The status text (e.g., "Success", "Failed", "Active")
/// @return Green for success/complete/ready, Red for fail/error,
///     Orange for active/progress, Gray default
[[nodiscard]] inline QColor statusColor(const QString& status) {
    const QString value = status.trimmed().toLower();
    if (value.contains("success") || value.contains("complete") || value.contains("ready")) {
        return QColor(ui::kStatusColorSuccess);
    }
    if (value.contains("fail") || value.contains("error") || value.contains("reject") ||
        value.contains("cancel")) {
        return QColor(ui::kStatusColorError);
    }
    if (value.contains("active") || value.contains("transfer") || value.contains("approved") ||
        value.contains("queued") || value.contains("progress")) {
        return QColor(ui::kStatusColorWarning);
    }
    return QColor(ui::kStatusColorIdle);
}

/// @brief Map a progress percentage to an appropriate color
/// @param percent Progress value (0-100+)
/// @return Green for 100%+, Orange for in-progress, Gray for 0%
[[nodiscard]] inline QColor progressColor(int percent) {
    if (percent >= 100) return QColor(ui::kStatusColorSuccess);
    if (percent > 0) return QColor(ui::kStatusColorWarning);
    return QColor(ui::kStatusColorIdle);
}

/// @brief Apply background and foreground colors to a table widget item
/// @param item The table widget item to style (null-safe)
/// @param color The background color to apply (foreground set to white)
inline void applyStatusColors(QTableWidgetItem* item, const QColor& color) {
    if (!item) return;
    item->setBackground(color);
    item->setForeground(Qt::white);
}

} // namespace sak
