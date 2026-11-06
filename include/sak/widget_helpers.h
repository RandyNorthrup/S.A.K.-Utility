// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file widget_helpers.h
/// @brief Shared UI helper functions for panel headers, accessibility,
///        status coloring, and table widget styling.

#pragma once

#include "sak/layout_constants.h"

#include "style_constants.h"

#include <QAbstractItemView>
#include <QDate>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QSizePolicy>
#include <QString>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QWidget>
#include <QWizard>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace sak {

/// @brief A table cell that sorts by the number it carries, not by its text. A plain item sorts
///        on the displayed string, so "9.0 KB" sorted above "10.0 MB".
class NumericSortTableItem : public QTableWidgetItem {
public:
    static constexpr int kSortRole = Qt::UserRole + 64;

    NumericSortTableItem(const QString& text, qlonglong value) : QTableWidgetItem(text) {
        setData(kSortRole, value);
    }

    [[nodiscard]] bool operator<(const QTableWidgetItem& other) const override {
        return data(kSortRole).toLongLong() < other.data(kSortRole).toLongLong();
    }
};

/// @brief Widens every column of @p table that is narrower than its header label to the label's
///        width: fixed widths cut "Response Time" and "Enabled" to fragments.
inline void fitHeaderLabels(QTableView* table) {
    table->ensurePolished();  // the theme's header font, not the default one, sets the width
    QHeaderView* header = table->horizontalHeader();
    for (int section = 0; section < header->count(); ++section) {
        if (header->isSectionHidden(section) ||
            header->sectionResizeMode(section) == QHeaderView::Stretch) {
            continue;
        }
        const int needed = header->sectionSizeHint(section);
        if (header->sectionSize(section) < needed) {
            header->resizeSection(section, needed);
        }
    }
}

/// @brief @p label as much of it as fits in @p width, or nothing when too little would show to
///        read. A disk bar's segment a few pixels wide drew ": ." from "*: 16.0 MB" (review 2,
///        F32); a segment that narrow says what it is in its tooltip and the table, not in glyphs.
inline QString labelThatFits(const QFontMetrics& metrics, const QString& label, int width) {
    if (metrics.horizontalAdvance(label) <= width) {
        return label;
    }
    // Two characters and an ellipsis ("E:...") read as noise, not as a name.
    constexpr qsizetype kFewestCharsShown = 3;
    const QString elided = metrics.elidedText(label, Qt::ElideRight, width);
    return elided.size() > kFewestCharsShown ? elided : QString();
}

/// @brief Where a bar's label of @p text_width goes so that no mark crosses it: centred on the
/// whole
///        @p track when it clears every mark in @p mark_xs by @p clearance, else the widest stretch
///        of the track between its ends and the marks. Create Partition's size handle sat in the
///        middle of "New 1.00 GB | Remaining 1.98 GB" and split it (review 2).
inline QRect labelRectClearOfMarks(const QRect& track,
                                   int text_width,
                                   QList<int> mark_xs,
                                   int clearance) {
    const int text_left = track.center().x() - (text_width / 2);
    const int text_right = text_left + text_width;
    const bool crossed = std::ranges::any_of(mark_xs, [&](int x) {
        return x + clearance >= text_left && x - clearance <= text_right;
    });
    if (!crossed) {
        return track;
    }
    std::ranges::sort(mark_xs);
    QRect widest;
    int from = track.left();
    mark_xs.append(track.right() + 1 + clearance);
    for (const int x : mark_xs) {
        const QRect gap(from, track.top(), (x - clearance) - from, track.height());
        if (gap.width() > widest.width()) {
            widest = gap;
        }
        from = std::max(from, x + clearance + 1);
    }
    return widest;
}

/// @brief Gives @p labels one width, the widest one's, so the fields after them in their rows
///        start at one left edge. For forms built row by row rather than with a QFormLayout.
inline void alignLabelColumn(std::initializer_list<QLabel*> labels) {
    int width = 0;
    for (QLabel* label : labels) {
        width = std::max(width, label->sizeHint().width());
    }
    for (QLabel* label : labels) {
        label->setMinimumWidth(width);
    }
}

/// @brief A one-colour glyph icon (the black icons8 glyphs) in the theme's body text colour: drawn
///        as shipped they were black on the dark surfaces, 1.2:1.
inline QIcon themedGlyphIcon(const QString& path) {
    const QIcon source(path);
    if (!ui::usingDarkUiTheme()) {
        return source;
    }
    constexpr int kGlyphSizes[] = {16, 20, 24, 32, 48};
    QIcon tinted;
    for (const int size : kGlyphSizes) {
        QPixmap pixmap = source.pixmap(size, size);
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), ui::themedColor(ui::kColorTextBody));
        painter.end();
        tinted.addPixmap(pixmap);
    }
    return tinted;
}

/// @brief @p path, or its dark-theme drawing ("panel_email-dark.svg" beside "panel_email.svg")
///        while the dark theme shows and the drawing exists. Artwork drawn for a light surface
///        loses its lines on a dark one: the Linux card's black penguin, the File Management tab's
///        grey lines at 1.41:1 (UI review DN4, DN5).
inline QString themedIconPath(const QString& path) {
    const QString suffix = QStringLiteral(".svg");
    if (!ui::usingDarkUiTheme() || !path.endsWith(suffix)) {
        return path;
    }
    QString dark = path;
    dark.insert(dark.size() - suffix.size(), QStringLiteral("-dark"));
    return QFileInfo::exists(dark) ? dark : path;
}

/// @brief A tab's icon when its tab is selected: the artwork in white on the blue gradient, where
///        its colours lost. A part drawn in white (Help's question mark, About's "i") is left out,
///        so it shows in the tab's blue; a plain white silhouette made those two blank discs.
inline QPixmap selectedTabPixmap(const QPixmap& drawn) {
    // A channel floor, not pure white: at 20 px a 1 px white stroke is blended with what is
    // around it, and About's "i" came out near (197, 219, 248) on its (139, 183, 240) bubble.
    constexpr int kNearWhite = 185;
    QImage image = drawn.toImage().convertToFormat(QImage::Format_ARGB32);
    const QRgb left_out = QColor(Qt::transparent).rgba();
    QColor on_tone(QString::fromLatin1(ui::kColorButtonTextOnTone));
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const bool white = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)}) >= kNearWhite;
            on_tone.setAlpha(qAlpha(pixel));
            line[x] = white ? left_out : on_tone.rgba();
        }
    }
    QPixmap selected = QPixmap::fromImage(image);
    selected.setDevicePixelRatio(drawn.devicePixelRatio());
    return selected;
}

/// @brief @p path as a tab icon: on unselected tabs the theme's drawing of it, and in white when
/// its
///        tab is selected (QIcon::On). The selected tab is the same blue in both themes, so its
///        white drawing comes from the light-theme artwork, whose light fills are its holes.
inline QIcon tabIcon(const QString& path, int size) {
    QIcon icon;
    const QIcon drawn(themedIconPath(path));
    const QIcon light(path);
    constexpr int kTabIconScales[] = {1, 2};  // normal and high-DPI pixmaps
    for (const int scale : kTabIconScales) {
        const int side = size * scale;
        icon.addPixmap(drawn.pixmap(side, side), QIcon::Normal, QIcon::Off);
        icon.addPixmap(selectedTabPixmap(light.pixmap(side, side)), QIcon::Normal, QIcon::On);
    }
    return icon;
}

/// @brief Keeps a label's icon pixmap in the theme's drawing when the theme changes while the
///        label is up.
class ThemedIconLabelRefresher final : public QObject {
public:
    ThemedIconLabelRefresher(QLabel* label, QString path, int size)
        : QObject(label), m_label(label), m_path(std::move(path)), m_size(size) {
        label->installEventFilter(this);
        refresh();
    }

    void refresh() const {
        m_label->setPixmap(QIcon(themedIconPath(m_path)).pixmap(m_size, m_size));
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_label && event->type() == QEvent::PaletteChange) {
            refresh();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QLabel* m_label;
    QString m_path;
    int m_size;
};

/// @brief Keeps the back arrow Qt makes for the Aero wizard frame hidden. Qt creates it with every
///        wizard; under ModernStyle it still came up, greyed, in the top-left corner of the page,
///        over the first letter of the title. A wizard here goes back with its own Back button.
class AeroBackArrowHider final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Show) {
            if (auto* arrow = qobject_cast<QWidget*>(watched)) {
                // Queued: hiding a widget inside its own Show event leaves Qt's state half set.
                QMetaObject::invokeMethod(arrow, &QWidget::hide, Qt::QueuedConnection);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

/// @brief One look for every wizard, in both themes. Left to the platform, a wizard took Windows'
///        Aero frame in light and fell back to the classic one, with "< Back" and "Next >", in
///        dark; the backup and restore wizards chose ModernStyle, so the Partition Manager wizards
///        looked different from them in light and from themselves in dark.
inline void useAppWizardStyle(QWizard* wizard) {
    wizard->setWizardStyle(QWizard::ModernStyle);
    for (QWidget* child : wizard->findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
        if (child->inherits("QVistaBackButton")) {
            child->hide();
            child->installEventFilter(new AeroBackArrowHider(child));
        }
    }
    // One tone per role, as in every dialog (UI review F27): Next and Finish go on with the work
    // and are primary; Back and Cancel only go back or leave.
    for (const QWizard::WizardButton which : {QWizard::BackButton, QWizard::CancelButton}) {
        wizard->button(which)->setStyleSheet(ui::kSecondaryButtonStyle);
    }
}

/// @brief An item delegate that puts a cell widget in its whole cell. Qt places a cell widget in
///        the item's text area, which the theme's 6 px item padding shrinks on every side, so an
///        editor lost 12 px of a row sized for it and its text was cut through the middle.
class WholeCellWidgetDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void updateEditorGeometry(QWidget* editor,
                              const QStyleOptionViewItem& option,
                              const QModelIndex& /*index*/) const override {
        editor->setGeometry(option.rect);
    }
};

/// @brief Gives @p view's cell widgets (setCellWidget, setItemWidget) their whole cells.
inline void placeCellWidgetsInWholeCells(QAbstractItemView* view) {
    view->setItemDelegate(new WholeCellWidgetDelegate(view));
}

/// @brief Sets @p item's text colour, or clears it back to the view's own when @p color is
///        invalid: an invalid QColor given to setForeground() paints black, 1.2:1 on dark.
inline void setItemForeground(QTableWidgetItem* item, const QColor& color) {
    if (color.isValid()) {
        item->setForeground(color);
    } else {
        item->setData(Qt::ForegroundRole, QVariant());
    }
}

/// @brief A registry InstallDate ("20261104") as "2026-11-04", which still sorts as text; any
///        other value as it is.
inline QString readableRegistryDate(const QString& value) {
    const QDate date = QDate::fromString(value.trimmed(), QStringLiteral("yyyyMMdd"));
    return date.isValid() ? date.toString(Qt::ISODate) : value;
}

/// @brief "1 hop", "2 hops": @p count with the noun that agrees with it.
inline QString countOf(qint64 count, const QString& singular, const QString& plural) {
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? singular : plural);
}

/// @brief A compiler __DATE__ ("Nov  4 2026") as "November 4, 2026", or simplified if it does
///        not parse.
inline QString readableBuildDate(const char* compiler_date) {
    const QString raw = QString::fromLatin1(compiler_date).simplified();
    const QDate date = QDate::fromString(raw, QStringLiteral("MMM d yyyy"));
    return date.isValid() ? date.toString(QStringLiteral("MMMM d, yyyy")) : raw;
}

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
    // Through the style sheet: the theme's "* { font-size: 10pt }" wins over a QFont size, so a
    // setFont() title drew at 10pt, smaller than the 13pt card titles under it.
    title_label->setStyleSheet(ui::fontSizeStyle(ui::kFontSizeSection) + ui::kFontWeightBoldStyle);
    return title_label;
}

inline QLabel* createHeaderSubtitleLabel(QWidget* parent, const QString& subtitle) {
    auto* subtitle_label = new QLabel(subtitle, parent);
    subtitle_label->setWordWrap(true);
    subtitle_label->setStyleSheet(ui::panelSubtitleStyle());
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
        iconLabel->setAccessibleDescription(QStringLiteral("The icon for ") + title);
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
        hw.iconLabel->setAccessibleDescription(QStringLiteral("The icon for ") + title);
    }
    if (hw.titleLabel) {
        hw.titleLabel->setText(title);
    }
    if (hw.subtitleLabel) {
        hw.subtitleLabel->setText(subtitle);
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
    return ui::cardFrameStyle(ui::kCssRadiusXLargePx, ui::kMarginMedium);
}

inline QString actionCardTitleStyle() {
    return ui::cardTitleTextStyle();
}

inline QString actionCardDescriptionStyle() {
    return ui::cardDescriptionTextStyle();
}

/// @brief The app's one action card: a 48 px logo, the title and description centred under it,
///        and @p button at the foot. Help, Backup and Restore and the Image Flasher build their
///        cards with it (UI review F41).
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
    new ThemedIconLabelRefresher(logo, iconPath, detail::kPanelIconSize);
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(ui::kTransparentWidgetStyle);
    logo->setAccessibleName(title + QStringLiteral(" icon"));
    logo->setAccessibleDescription(QStringLiteral("The icon for ") + title);
    layout->addWidget(logo);

    auto* titleLabel = new QLabel(title, card);
    titleLabel->setAlignment(Qt::AlignCenter);
    titleLabel->setStyleSheet(actionCardTitleStyle());
    layout->addWidget(titleLabel);

    auto* descLabel = new QLabel(description, card);
    descLabel->setWordWrap(true);
    descLabel->setAlignment(Qt::AlignCenter);
    descLabel->setStyleSheet(actionCardDescriptionStyle());
    layout->addWidget(descLabel);

    layout->addStretch();
    button->setParent(card);
    layout->addWidget(button);
    return {card, logo, titleLabel, descLabel, button};
}

// -- Untrusted Text Helpers --------------------------------------------------

/// @brief QLabel that shows @p text verbatim, never as markup.
///
/// QLabel defaults to Qt::AutoText, so a value carrying markup (a registry program name, a file
/// name read off a raw image, AI output) is silently rendered as rich text. Use this factory --
/// or setTextFormat(Qt::PlainText) on a label that is filled in later -- for any label whose
/// template contains no intended markup of its own. See sak/rich_text_safety.h.
///
/// @param text   The untrusted text to display.
/// @param parent Owning widget.
[[nodiscard]] inline QLabel* plainTextLabel(const QString& text, QWidget* parent = nullptr) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    return label;
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

/// @brief Apply the standard read-only table baseline shared across panels.
inline void configureStandardTable(QTableView* table, QAbstractItemView::SelectionMode mode) {
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(mode);
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->verticalHeader()->setVisible(false);
}

}  // namespace sak
