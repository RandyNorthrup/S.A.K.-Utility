// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file widget_helpers.h
/// @brief Shared UI helper functions for panel headers, accessibility,
///        status coloring, and table widget styling.

#pragma once

#include "sak/layout_constants.h"

#include "style_constants.h"

#include <QColor>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QSizePolicy>
#include <QString>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace sak {

// -- Panel Header Factory ----------------------------------------------------

/// @brief Pointers returned by createDynamicPanelHeader for runtime updates.
struct PanelHeaderWidgets {
    QLabel* iconLabel{nullptr};
    QLabel* titleLabel{nullptr};
    QLabel* subtitleLabel{nullptr};
};

namespace detail {

constexpr int kPanelIconSize = 48;

inline QLabel* createHeaderTitleLabel(QWidget* parent, const QString& title) {
    auto* title_label = new QLabel(title, parent);
    QFont title_font = title_label->font();
    title_font.setPointSize(ui::kFontSizeSection);
    title_font.setBold(true);
    title_label->setFont(title_font);
    title_label->setAccessibleName(title);
    return title_label;
}

inline QLabel* createHeaderSubtitleLabel(QWidget* parent, const QString& subtitle) {
    auto* subtitle_label = new QLabel(subtitle, parent);
    subtitle_label->setWordWrap(true);
    subtitle_label->setStyleSheet(ui::panelSubtitleStyle());
    subtitle_label->setAccessibleName(subtitle);
    return subtitle_label;
}

inline PanelHeaderWidgets createPanelHeaderImpl(QWidget* parent,
                                                const QString& iconPath,
                                                const QString& title,
                                                const QString& subtitle,
                                                QVBoxLayout* layout) {
    QLabel* iconLabel = nullptr;
    auto* titleLayout = new QVBoxLayout();

    if (!iconPath.isEmpty()) {
        auto* headerRow = new QHBoxLayout();
        headerRow->setSpacing(ui::kSpacingLarge);

        iconLabel = new QLabel(parent);
        iconLabel->setFixedSize(kPanelIconSize, kPanelIconSize);
        iconLabel->setPixmap(QIcon(iconPath).pixmap(kPanelIconSize, kPanelIconSize));
        iconLabel->setAccessibleName(title + QStringLiteral(" icon"));
        headerRow->addWidget(iconLabel);
        headerRow->addLayout(titleLayout, 1);
        layout->addLayout(headerRow);
    } else {
        layout->addLayout(titleLayout);
    }

    auto* title_label = createHeaderTitleLabel(parent, title);
    titleLayout->addWidget(title_label);

    auto* subtitle_label = createHeaderSubtitleLabel(parent, subtitle);
    titleLayout->addWidget(subtitle_label);

    return {iconLabel, title_label, subtitle_label};
}

}  // namespace detail

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
inline void createPanelHeader(QWidget* parent,
                              const QString& title,
                              const QString& subtitle,
                              QVBoxLayout* layout) {
    (void)detail::createPanelHeaderImpl(parent, QString(), title, subtitle, layout);
}

/// @brief Create a panel header with an icon to the left of the title/subtitle.
///
/// Mirrors the About panel's icon layout: a 48x48 SVG icon in an HBox
/// alongside the bold title and muted subtitle in a VBox.
///
/// @param parent   The parent widget that owns the labels.
/// @param iconPath Qt resource path to the SVG icon (e.g. ":/icons/icons/panel_foo.svg").
/// @param title    The panel title text.
/// @param subtitle A one-line description of the panel's purpose.
/// @param layout   The QVBoxLayout to which the header row is appended.
inline void createPanelHeader(QWidget* parent,
                              const QString& iconPath,
                              const QString& title,
                              const QString& subtitle,
                              QVBoxLayout* layout) {
    (void)detail::createPanelHeaderImpl(parent, iconPath, title, subtitle, layout);
}

// -- Dynamic Panel Header ----------------------------------------------------

/// @brief Create a panel header whose icon, title, and subtitle can be updated
///        at runtime (e.g. when switching sub-tabs inside a composite panel).
[[nodiscard]] inline PanelHeaderWidgets createDynamicPanelHeader(QWidget* parent,
                                                                 const QString& iconPath,
                                                                 const QString& title,
                                                                 const QString& subtitle,
                                                                 QVBoxLayout* layout) {
    return detail::createPanelHeaderImpl(parent, iconPath, title, subtitle, layout);
}

/// @brief Update a dynamic panel header's icon, title, and subtitle in-place.
inline void updatePanelHeader(const PanelHeaderWidgets& hw,
                              const QString& iconPath,
                              const QString& title,
                              const QString& subtitle) {
    if (hw.iconLabel) {
        hw.iconLabel->setPixmap(
            QIcon(iconPath).pixmap(detail::kPanelIconSize, detail::kPanelIconSize));
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

// -- Action Card Factory -----------------------------------------------------

struct ActionCardWidgets {
    QFrame* card{nullptr};
    QLabel* iconLabel{nullptr};
    QLabel* titleLabel{nullptr};
    QLabel* descriptionLabel{nullptr};
    QPushButton* button{nullptr};
};

inline QString actionCardStyle() {
    return ui::cardFrameStyle(ui::kCssRadiusLargePx, ui::kMarginMedium);
}

inline QString actionCardTitleStyle() {
    return ui::cardTitleTextStyle();
}

inline QString actionCardDescriptionStyle() {
    return ui::cardDescriptionTextStyle();
}

[[nodiscard]] inline ActionCardWidgets createActionCard(QWidget* parent,
                                                        const QString& iconPath,
                                                        const QString& title,
                                                        const QString& description,
                                                        QPushButton* button) {
    Q_ASSERT(parent);
    Q_ASSERT(button);

    auto* card = new QFrame(parent);
    card->setProperty("sakCard", true);
    card->setStyleSheet(actionCardStyle());
    card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);

    auto* layout = new QVBoxLayout(card);
    layout->setSpacing(ui::kSpacingMedium);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* logo = new QLabel(card);
    logo->setPixmap(QIcon(iconPath).pixmap(detail::kPanelIconSize, detail::kPanelIconSize));
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(ui::kTransparentWidgetStyle);
    logo->setAccessibleName(title + QStringLiteral(" icon"));
    layout->addWidget(logo);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(actionCardTitleStyle());
    titleLabel->setAccessibleName(title);
    layout->addWidget(titleLabel);

    auto* descLabel = new QLabel(description, card);
    descLabel->setWordWrap(true);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setStyleSheet(actionCardDescriptionStyle());
    descLabel->setAccessibleName(description);
    layout->addWidget(descLabel);

    layout->addStretch();
    button->setParent(card);
    layout->addWidget(button);
    return {card, logo, titleLabel, descLabel, button};
}

// -- Accessibility Helpers ---------------------------------------------------

/// @brief Set accessible name and optional description on any QWidget.
///
/// Use this instead of calling setAccessibleName / setAccessibleDescription
/// individually to ensure widgets are never left without accessibility info.
///
/// @param widget The target widget (null-safe).
/// @param name   Short accessible name (read by screen readers).
/// @param description Optional longer description for context.
inline void setAccessible(QWidget* widget, const QString& name, const QString& description = {}) {
    if (!widget) {
        return;
    }
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
    struct StatusKeyword {
        const char* keyword;
        const char* color;
    };
    static constexpr StatusKeyword kSuccess[] = {
        {"success", nullptr},
        {"complete", nullptr},
        {"ready", nullptr},
    };
    if (std::any_of(std::begin(kSuccess), std::end(kSuccess), [&value](const StatusKeyword& e) {
            return value.contains(QLatin1String(e.keyword));
        })) {
        return QColor(ui::kStatusColorSuccess);
    }
    static constexpr const char* kError[] = {
        "fail",
        "error",
        "reject",
        "cancel",
    };
    if (std::any_of(std::begin(kError), std::end(kError), [&value](const char* kw) {
            return value.contains(QLatin1String(kw));
        })) {
        return QColor(ui::kStatusColorError);
    }
    static constexpr const char* kWarning[] = {
        "active",
        "transfer",
        "approved",
        "queued",
        "progress",
    };
    if (std::any_of(std::begin(kWarning), std::end(kWarning), [&value](const char* kw) {
            return value.contains(QLatin1String(kw));
        })) {
        return QColor(ui::kStatusColorWarning);
    }
    return QColor(ui::kStatusColorIdle);
}

/// @brief Map a progress percentage to an appropriate color
/// @param percent Progress value (0-100+)
/// @return Green for 100%+, Orange for in-progress, Gray for 0%
[[nodiscard]] inline QColor progressColor(int percent) {
    if (percent >= kPercentMax) {
        return QColor(ui::kStatusColorSuccess);
    }
    if (percent > 0) {
        return QColor(ui::kStatusColorWarning);
    }
    return QColor(ui::kStatusColorIdle);
}

/// @brief Apply background and foreground colors to a table widget item
/// @param item The table widget item to style (null-safe)
/// @param color The background color to apply (foreground set to white)
inline void applyStatusColors(QTableWidgetItem* item, const QColor& color) {
    if (!item) {
        return;
    }
    item->setBackground(color);
    item->setForeground(Qt::white);
}

}  // namespace sak
