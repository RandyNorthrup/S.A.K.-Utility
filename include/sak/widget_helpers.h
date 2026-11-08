// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file widget_helpers.h
/// @brief Shared UI helper functions for panel headers, accessibility,
///        status coloring, and table widget styling.

#pragma once

#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/rich_text_safety.h"

#include "style_constants.h"

#include <QAbstractButton>
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
#include <QIconEngine>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QResizeEvent>
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

/// A selected tab's white drawing leaves out the light fills of the artwork, its holes: at 20 px
/// a 1 px white stroke is blended with what is around it, and About's "i" came out near
/// (197, 219, 248) on its (139, 183, 240) bubble.
inline constexpr int kTabGlyphNearWhite = 185;
/// A button's glyph leaves out only what is drawn white (Run Details' "i"): a pale fill is part of
/// the drawing, and leaving out Rename Chat's pale blue field (#c2e8ff) left only its text
/// cursor, an "!". The palest fill a button icon uses is Send's #e1ebf2.
inline constexpr int kButtonGlyphNearWhite = 240;

/// @brief @p drawn as a one-colour glyph in @p color: every part of it in that colour, except the
///        parts at least @p near_white in every channel, which are left out, so a white page or
///        question mark shows as a hole. A plain silhouette made Help's and About's tab icons
///        blank discs.
inline QPixmap toneGlyphPixmap(const QPixmap& drawn, QColor color, int near_white) {
    QImage image = drawn.toImage().convertToFormat(QImage::Format_ARGB32);
    const QRgb left_out = QColor(Qt::transparent).rgba();
    for (int y = 0; y < image.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = line[x];
            const bool white = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)}) >= near_white;
            color.setAlpha(qAlpha(pixel));
            line[x] = white ? left_out : color.rgba();
        }
    }
    QPixmap glyph = QPixmap::fromImage(image);
    glyph.setDevicePixelRatio(drawn.devicePixelRatio());
    return glyph;
}

/// @brief A button icon drawn as its button's label is: a one-colour glyph (toneGlyphPixmap) in
///        the label's colour, white on the toned buttons that carry these icons, and in the
///        colour a disabled button's label takes (the muted text colour) while disabled. Qt
///        greyed each drawing its own way, so on the AI page a disabled black glyph vanished on
///        dark (1.06:1) while a white and blue drawing stayed bright there (review 3, X2); and
///        enabled, three secondary buttons showed a blue paperclip, a white book and a black bin
///        beside their white labels (review 4, Y4). The colours are read as the icon is drawn, so
///        a change of theme takes effect at once.
class ButtonIconEngine final : public QIconEngine {
public:
    explicit ButtonIconEngine(QString path) : m_path(std::move(path)), m_source(m_path) {}

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        // What is white is decided on the drawing, not on its anti-aliased button-size pixels:
        // at 20 px Run Details' white "i" is nowhere a whole pixel wide, so read at that size it
        // is no whiter than a pale fill. Toned at four times the size, then scaled down.
        constexpr int kOversample = 4;
        const QPixmap drawn = m_source.pixmap(size * kOversample, QIcon::Normal, state);
        const QColor color = mode == QIcon::Disabled
                                 ? ui::themedColor(ui::kColorTextMuted)
                                 : QColor(QString::fromLatin1(ui::kColorButtonTextOnTone));
        // As many device pixels as the drawing would have had at the size asked for.
        QPixmap glyph =
            toneGlyphPixmap(drawn, color, kButtonGlyphNearWhite)
                .scaled(drawn.size() / kOversample, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        glyph.setDevicePixelRatio(drawn.devicePixelRatio());
        return glyph;
    }

    void paint(QPainter* painter,
               const QRect& rect,
               QIcon::Mode mode,
               QIcon::State state) override {
        const qreal ratio = painter->device() != nullptr ? painter->device()->devicePixelRatioF()
                                                         : 1.0;
        const QSize device_size(qRound(rect.width() * ratio), qRound(rect.height() * ratio));
        painter->drawPixmap(rect, pixmap(device_size, mode, state));
    }

    [[nodiscard]] QIconEngine* clone() const override { return new ButtonIconEngine(m_path); }

private:
    QString m_path;
    QIcon m_source;
};

/// @brief The icon a button shows, through ButtonIconEngine.
inline QIcon buttonIcon(const QString& path) {
    return QIcon(new ButtonIconEngine(path));
}

/// @brief A label that shortens its text with an ellipsis when it is given less room than the text
///        needs, and keeps the whole text in its tooltip. A plain QLabel squeezed by the status bar
///        cut the AI status mid-word ("Context: pe", review 3, S22).
class ElidingLabel final : public QLabel {
public:
    using QLabel::QLabel;

    void setFullText(const QString& text) {
        m_full = text;
        setToolTip(ui::asLiteralRichText(text));
        updateGeometry();
        elide();
    }

    [[nodiscard]] QString fullText() const { return m_full; }

    [[nodiscard]] QSize sizeHint() const override {
        const QSize hint = QLabel::sizeHint();
        const QMargins margins = contentsMargins();
        return {fontMetrics().horizontalAdvance(m_full) + margins.left() + margins.right(),
                hint.height()};
    }

    [[nodiscard]] QSize minimumSizeHint() const override {
        // Room for a few characters and the ellipsis; the status bar gives it what is left.
        constexpr int kFewestCharacters = 8;
        return {fontMetrics().averageCharWidth() * kFewestCharacters,
                QLabel::minimumSizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        elide();
    }

private:
    void elide() {
        const int room = contentsRect().width();
        setText(fontMetrics().horizontalAdvance(m_full) <= room
                    ? m_full
                    : fontMetrics().elidedText(m_full, Qt::ElideRight, room));
    }

    QString m_full;
};

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
    return toneGlyphPixmap(drawn,
                           QColor(QString::fromLatin1(ui::kColorButtonTextOnTone)),
                           kTabGlyphNearWhite);
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

    /// Shows @p path from now on, in the theme's drawing.
    void setPath(QString path) {
        m_path = std::move(path);
        refresh();
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

/// @brief The Aero frame's back arrow (Qt's private QVistaBackButton), or null.
///        That class has no Q_OBJECT of its own, so its meta-object is QAbstractButton's and asking
///        whether it inherits "QVistaBackButton" is always false: the first hider matched nothing.
///        It is the one button QWizard parents to itself; every wizard button and a side widget
///        live in the wizard's inner frame.
inline QAbstractButton* aeroBackArrowOf(QWizard* wizard) {
    for (QObject* child : wizard->children()) {
        auto* button = qobject_cast<QAbstractButton*>(child);
        if (button != nullptr && button->metaObject() == &QAbstractButton::staticMetaObject) {
            return button;
        }
    }
    return nullptr;
}

/// @brief One look for every wizard, in both themes. Left to the platform, a wizard took Windows'
///        Aero frame in light and fell back to the classic one, with "< Back" and "Next >", in
///        dark; the backup and restore wizards chose ModernStyle, so the Partition Manager wizards
///        looked different from them in light and from themselves in dark.
///        In light, Qt starts a wizard in AeroStyle and marks the Aero back arrow to be shown
///        before this runs; changing the style before the window exists does not take that back,
///        so the arrow came up at the page's top-left over the title (testpc1, 2026-11-06).
inline void useAppWizardStyle(QWizard* wizard) {
    wizard->setWizardStyle(QWizard::ModernStyle);
    if (QAbstractButton* arrow = aeroBackArrowOf(wizard)) {
        arrow->hide();
        arrow->installEventFilter(new AeroBackArrowHider(arrow));
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

/// @brief Gives @p view's cell widgets (setCellWidget, setItemWidget) their whole cells, and a
///        table's rows the height its cell widgets ask for.
inline void placeCellWidgetsInWholeCells(QAbstractItemView* view) {
    view->setItemDelegate(new WholeCellWidgetDelegate(view));
    if (auto* table = qobject_cast<QTableView*>(view)) {
        // A row of the header's default height was a pixel or two shorter than a themed combo box
        // in Segoe UI, and the combo cut its descenders ("Morgan", "Merge": review 3, N2).
        table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    }
}

/// @brief Every column of @p header as wide as its contents but @p stretch_column, which takes
///        the room left. Columns of equal shares cut names and paths while a neighbour had slack:
///        the restore's "Microsoft.Windows.SecureAssessmen..." beside a half-empty package column,
///        "VS Code Extensi..." beside the size (review 4, Y8); and a check-box column was 177 px
///        wide with its box at the left under a centred header (Y16).
inline void fitColumnsToContents(QHeaderView* header, int stretch_column) {
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(stretch_column, QHeaderView::Stretch);
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
        // The theme's drawing, as a tab and a card show it: the light drawings of the tab icons
        // are drawn dark for the light tab, and lost their lines on a dark page (review 4, Y3).
        new ThemedIconLabelRefresher(iconLabel, iconPath, kPanelIconSize);
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
        // The refresher has no meta-object of its own, so it is found by its C++ type.
        ThemedIconLabelRefresher* refresher = nullptr;
        for (QObject* child : hw.iconLabel->children()) {
            refresher = refresher != nullptr ? refresher
                                             : dynamic_cast<ThemedIconLabelRefresher*>(child);
        }
        if (refresher != nullptr) {
            refresher->setPath(iconPath);
        } else {
            hw.iconLabel->setPixmap(QIcon(themedIconPath(iconPath))
                                        .pixmap(detail::kPanelIconSize, detail::kPanelIconSize));
        }
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
