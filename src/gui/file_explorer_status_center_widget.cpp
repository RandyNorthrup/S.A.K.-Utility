// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_status_center_widget.h"

#include "sak/color_constants.h"
#include "sak/file_explorer_icon_registry.h"
#include "sak/style_constants.h"

#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace sak {

namespace {

// Files StatusCenter.xaml metrics.
constexpr int kCardPadding = 8;
constexpr int kCardIconCircle = 32;
constexpr int kCardGlyphSize = 16;
constexpr int kCardButtonSize = 32;
constexpr int kGraphHeight = 88;
constexpr int kProgressBarHeight = 6;  ///< Files card ProgressBar height
constexpr int kGraphInset = 4;         ///< Files graph-frame content margin
constexpr int kFlyoutWidth = 400;
constexpr int kFlyoutMinHeight = 120;
constexpr int kFlyoutMaxHeight = 500;
// Files IconBackgroundCircleBorderOpacity for in-progress/canceled circles.
constexpr double kMutedCircleOpacity = 0.1;
constexpr int kBadgeDiameter = 13;
constexpr double kRingPenWidth = 2.5;
// State-glyph (check/cross) stroke width, shared by both marks.
constexpr double kMarkPenWidth = 2.0;
// Files check-mark polyline vertices as fractions of the glyph rect.
constexpr double kCheckMarkStartX = 0.22;
constexpr double kCheckMarkStartY = 0.55;
constexpr double kCheckMarkMidX = 0.42;
constexpr double kCheckMarkMidY = 0.75;
constexpr double kCheckMarkEndX = 0.78;
constexpr double kCheckMarkEndY = 0.30;
// Files SpeedGraph: minimum points to draw a line, fill opacity, line stroke.
constexpr int kMinGraphPoints = 2;
constexpr double kGraphFillOpacity = 0.25;
constexpr double kGraphLinePenWidth = 1.5;
// Files ProgressBar percentage full-scale maximum.
constexpr int kProgressBarMax = 100;
// Files MedianOperationProgressRing geometry: centering divisor plus the arc
// start angle (12 o'clock) and Qt's 1/16-degree angle unit for drawArc.
constexpr double kRingCenterDivisor = 2.0;
constexpr int kRingStartAngleDeg = 90;
constexpr int kQtAngleSixteenths = 16;
// Files InfoBadgeState values beyond the allowed 0/1 sentinels: in progress
// with an error, and completed with an error.
constexpr int kBadgeStateInProgressWithError = 2;
constexpr int kBadgeStateCompletedWithError = 3;
// Files InfoBadge numeric-count font size.
constexpr int kBadgeFontPixelSize = 9;

/// Circle + glyph colors per card state (Files
/// StatusCenterStateToBrushConverter: attention/success/critical/secondary).
QColor stateColor(const FileExplorerStatusItemKind kind, const QPalette& palette) {
    switch (kind) {
    case FileExplorerStatusItemKind::Successful:
        return QColor(QString::fromLatin1(ui::kColorSuccess));
    case FileExplorerStatusItemKind::Error:
        return QColor(QString::fromLatin1(ui::kColorError));
    case FileExplorerStatusItemKind::Canceled:
        return palette.color(QPalette::Disabled, QPalette::Text);
    case FileExplorerStatusItemKind::InProgress:
    default:
        return palette.color(QPalette::Highlight);
    }
}

/// Icon-registry key per operation glyph (Files
/// StatusCenterStateToStateIconConverter geometries; icons8 swap pending).
QString iconKeyFor(const FileExplorerStatusIconKind kind) {
    switch (kind) {
    case FileExplorerStatusIconKind::Copy:
        return QStringLiteral("copy");
    case FileExplorerStatusIconKind::Move:
        return QStringLiteral("cut");
    case FileExplorerStatusIconKind::Extract:
    case FileExplorerStatusIconKind::Compress:
        return QStringLiteral("file");
    case FileExplorerStatusIconKind::Delete:
    case FileExplorerStatusIconKind::Recycle:
    default:
        return QStringLiteral("delete");
    }
}

void paintCheckMark(QPainter* painter, const QRectF& rect, const QColor& color) {
    QPen pen(color, kMarkPenWidth);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter->setPen(pen);
    QPainterPath path;
    path.moveTo(rect.left() + (rect.width() * kCheckMarkStartX),
                rect.top() + (rect.height() * kCheckMarkStartY));
    path.lineTo(rect.left() + (rect.width() * kCheckMarkMidX),
                rect.top() + (rect.height() * kCheckMarkMidY));
    path.lineTo(rect.left() + (rect.width() * kCheckMarkEndX),
                rect.top() + (rect.height() * kCheckMarkEndY));
    painter->drawPath(path);
}

void paintCrossMark(QPainter* painter, const QRectF& rect, const QColor& color) {
    QPen pen(color, kMarkPenWidth);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    const double inset_x = rect.width() * 0.30;
    const double inset_y = rect.height() * 0.30;
    const QRectF inner = rect.adjusted(inset_x, inset_y, -inset_x, -inset_y);
    painter->drawLine(inner.topLeft(), inner.bottomRight());
    painter->drawLine(inner.topRight(), inner.bottomLeft());
}

QToolButton* makeCardButton(QWidget* parent, const char* object_name, const QString& tool_tip) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QString::fromLatin1(object_name));
    button->setToolTip(tool_tip);
    button->setAutoRaise(true);
    button->setFixedSize(kCardButtonSize, kCardButtonSize);
    return button;
}

}  // namespace

FileExplorerSpeedGraph::FileExplorerSpeedGraph(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerStatusCardGraph"));
    setMinimumHeight(kGraphHeight);
}

void FileExplorerSpeedGraph::setPoints(const QList<QPointF>& points) {
    m_points = points;
    update();
}

void FileExplorerSpeedGraph::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_points.size() < kMinGraphPoints) {
        return;
    }
    // Files SpeedGraph: x maps progress percent over the width, y normalizes
    // against the running maximum speed.
    double highest = 0.0;
    for (const QPointF& point : m_points) {
        highest = std::max(highest, point.y());
    }
    if (highest <= 0.0) {
        highest = 1.0;
    }
    const double usable_height = height() - 4.0;
    QPolygonF line;
    for (const QPointF& point : m_points) {
        const double x = width() * point.x() / 100.0;
        const double y = height() - 2.0 - (usable_height * point.y() / highest);
        line.append(QPointF(x, y));
    }
    const QColor accent = palette().color(QPalette::Highlight);
    QPolygonF fill = line;
    fill.append(QPointF(line.last().x(), height()));
    fill.prepend(QPointF(line.first().x(), height()));
    QColor fill_color = accent;
    fill_color.setAlphaF(kGraphFillOpacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill_color);
    painter.drawPolygon(fill);
    painter.setPen(QPen(accent, kGraphLinePenWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(line);
}

FileExplorerStatusCardWidget::FileExplorerStatusCardWidget(FileExplorerStatusCenterItem* item,
                                                           QWidget* parent)
    : QFrame(parent), m_item(item) {
    setObjectName(QStringLiteral("fileExplorerStatusCard"));
    setFrameShape(QFrame::StyledPanel);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    column->setSpacing(ui::kSpacingTight);
    buildHeaderRow(column);
    buildProgressRows(column);
    connect(
        m_item, &FileExplorerStatusCenterItem::changed, this, &FileExplorerStatusCardWidget::sync);
    sync();
}

void FileExplorerStatusCardWidget::buildHeaderRow(QVBoxLayout* column) {
    auto* row = new QHBoxLayout();
    row->setSpacing(kCardPadding);
    m_icon_label = new QLabel(this);
    m_icon_label->setObjectName(QStringLiteral("fileExplorerStatusCardIcon"));
    m_icon_label->setFixedSize(kCardIconCircle, kCardIconCircle);
    row->addWidget(m_icon_label, 0, Qt::AlignTop);

    m_title_label = new QLabel(this);
    m_title_label->setObjectName(QStringLiteral("fileExplorerStatusCardTitle"));
    m_title_label->setWordWrap(true);
    row->addWidget(m_title_label, 1);

    // Files CloseItemButton: completed cards only.
    m_dismiss_button = makeCardButton(this, "fileExplorerStatusCardDismiss", tr("Clear"));
    m_dismiss_button->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("close")));
    connect(m_dismiss_button, &QToolButton::clicked, this, [this]() {
        Q_EMIT dismissRequested(m_item);
    });
    row->addWidget(m_dismiss_button, 0, Qt::AlignTop);

    // Files ExpandCollapseChevronItemButton: only when speed is available.
    m_expand_button = makeCardButton(this, "fileExplorerStatusCardExpand", tr("Expand"));
    m_expand_button->setCheckable(true);
    m_expand_button->setArrowType(Qt::DownArrow);
    connect(m_expand_button, &QToolButton::toggled, this, [this](const bool checked) {
        m_item->setExpanded(checked);
    });
    row->addWidget(m_expand_button, 0, Qt::AlignTop);
    column->addLayout(row);
}

void FileExplorerStatusCardWidget::buildProgressRows(QVBoxLayout* column) {
    auto* grid = new QHBoxLayout();
    grid->setSpacing(ui::kSpacingTight);
    auto* content = new QVBoxLayout();
    content->setSpacing(ui::kSpacingTight);

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setObjectName(QStringLiteral("fileExplorerStatusCardProgressBar"));
    m_progress_bar->setRange(0, kProgressBarMax);
    m_progress_bar->setTextVisible(false);
    m_progress_bar->setFixedHeight(kProgressBarHeight);
    content->addWidget(m_progress_bar);

    // Files MainGraphCartesianChartClipGrid: the expanded speed graph with the
    // "Speed:" overlay.
    m_graph_frame = new QFrame(this);
    m_graph_frame->setObjectName(QStringLiteral("fileExplorerStatusCardGraphFrame"));
    m_graph_frame->setFrameShape(QFrame::StyledPanel);
    auto* graph_column = new QVBoxLayout(m_graph_frame);
    graph_column->setContentsMargins(kGraphInset, kGraphInset, kGraphInset, kGraphInset);
    m_speed_label = new QLabel(m_graph_frame);
    m_speed_label->setObjectName(QStringLiteral("fileExplorerStatusCardSpeed"));
    graph_column->addWidget(m_speed_label);
    m_graph = new FileExplorerSpeedGraph(m_graph_frame);
    graph_column->addWidget(m_graph, 1);
    content->addWidget(m_graph_frame);

    m_name_label = new QLabel(this);
    m_name_label->setObjectName(QStringLiteral("fileExplorerStatusCardName"));
    content->addWidget(m_name_label);
    m_message_label = new QLabel(this);
    m_message_label->setObjectName(QStringLiteral("fileExplorerStatusCardMessage"));
    content->addWidget(m_message_label);
    grid->addLayout(content, 1);

    // Files MoreOptionsButton with the single Cancel flyout entry.
    m_more_button = makeCardButton(this, "fileExplorerStatusCardMore", tr("More options..."));
    m_more_button->setIcon(FileExplorerIconRegistry::iconForKey(QStringLiteral("more")));
    m_more_button->setPopupMode(QToolButton::InstantPopup);
    m_more_menu = new QMenu(m_more_button);
    QAction* cancel_action = m_more_menu->addAction(tr("Cancel"));
    cancel_action->setObjectName(QStringLiteral("fileExplorerStatusCardCancel"));
    connect(
        cancel_action, &QAction::triggered, m_item, &FileExplorerStatusCenterItem::requestCancel);
    m_more_button->setMenu(m_more_menu);
    grid->addWidget(m_more_button, 0, Qt::AlignTop);
    column->addLayout(grid);
}

void FileExplorerStatusCardWidget::sync() {
    m_icon_label->setPixmap(statePixmap());
    m_title_label->setText(m_item->header());
    m_title_label->setToolTip(m_item->subHeader().isEmpty() ? m_item->header()
                                                            : m_item->subHeader());
    m_dismiss_button->setVisible(!m_item->isInProgress());
    m_expand_button->setVisible(m_item->isSpeedAndProgressAvailable());
    m_expand_button->setChecked(m_item->isExpanded());
    m_expand_button->setArrowType(m_item->isExpanded() ? Qt::UpArrow : Qt::DownArrow);
    syncProgressRows();
}

void FileExplorerStatusCardWidget::syncProgressRows() {
    const bool in_progress = m_item->isInProgress();
    const bool expanded = m_item->isExpanded();
    const bool indeterminate = m_item->isIndeterminate();
    m_progress_bar->setVisible(in_progress && !expanded);
    // Qt busy bar for the Files indeterminate state.
    m_progress_bar->setRange(0, indeterminate ? 0 : kProgressBarMax);
    if (!indeterminate) {
        m_progress_bar->setValue(m_item->progressPercentage());
    }
    m_graph_frame->setVisible(in_progress && expanded);
    m_graph->setPoints(m_item->graphPoints());
    m_speed_label->setText(tr("Speed: %1").arg(m_item->speedText()));
    const bool footer = in_progress && !indeterminate;
    if (m_item->kind() == FileExplorerStatusItemKind::Error) {
        // Files surfaces a failed operation's detail only as the card tooltip, which
        // keyboard, touch, and assistive-technology users never see. Show the terminal
        // failure detail inline instead so the error is always visible.
        const QString detail = m_item->subHeader().isEmpty() ? m_item->message()
                                                             : m_item->subHeader();
        m_message_label->setText(detail);
        m_message_label->setVisible(!detail.isEmpty());
    } else {
        m_message_label->setVisible(footer);
        m_message_label->setText(m_item->message());
    }
    m_name_label->setVisible(footer && expanded);
    m_name_label->setText(tr("Name: %1").arg(m_item->currentItemName()));
    m_more_button->setVisible(in_progress && m_item->isCancelable());
}

QPixmap FileExplorerStatusCardWidget::statePixmap() const {
    const qreal ratio = devicePixelRatioF();
    QPixmap pixmap(QSize(kCardIconCircle, kCardIconCircle) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color = stateColor(m_item->kind(), palette());
    const bool solid = m_item->kind() == FileExplorerStatusItemKind::Successful ||
                       m_item->kind() == FileExplorerStatusItemKind::Error;
    QColor circle = color;
    // Files: full circles for terminal success/error, 0.1-opacity otherwise.
    circle.setAlphaF(solid ? 1.0 : kMutedCircleOpacity);
    painter.setPen(Qt::NoPen);
    painter.setBrush(circle);
    const QRectF bounds(0, 0, kCardIconCircle, kCardIconCircle);
    painter.drawEllipse(bounds);
    if (m_item->kind() == FileExplorerStatusItemKind::Successful) {
        paintCheckMark(&painter, bounds, Qt::white);
    } else if (m_item->kind() == FileExplorerStatusItemKind::Error) {
        paintCrossMark(&painter, bounds, Qt::white);
    } else {
        const QIcon icon = FileExplorerIconRegistry::iconForKey(iconKeyFor(m_item->iconKind()));
        const int offset = (kCardIconCircle - kCardGlyphSize) / 2;
        icon.paint(&painter, offset, offset, kCardGlyphSize, kCardGlyphSize);
    }
    return pixmap;
}

FileExplorerStatusCenterFlyout::FileExplorerStatusCenterFlyout(FileExplorerStatusCenterModel* model,
                                                               QWidget* parent)
    : QFrame(parent), m_model(model) {
    setObjectName(QStringLiteral("fileExplorerStatusCenterFlyout"));
    setFrameShape(QFrame::StyledPanel);
    setAutoFillBackground(true);
    setFixedWidth(kFlyoutWidth);
    setMinimumHeight(kFlyoutMinHeight);
    setMaximumHeight(kFlyoutMaxHeight);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(kCardPadding, kCardPadding, kCardPadding, kCardPadding);
    column->setSpacing(kCardPadding);
    buildHeader(column);
    buildCardList(column);
    connect(m_model,
            &FileExplorerStatusCenterModel::changed,
            this,
            &FileExplorerStatusCenterFlyout::syncCards);
    syncCards();
    hide();
}

void FileExplorerStatusCenterFlyout::buildHeader(QVBoxLayout* column) {
    auto* row = new QHBoxLayout();
    auto* title = new QLabel(tr("Status center"), this);
    title->setObjectName(QStringLiteral("fileExplorerStatusCenterTitle"));
    QFont title_font = title->font();
    title_font.setBold(true);
    title->setFont(title_font);
    row->addWidget(title);
    row->addStretch(1);
    m_clear_button = new QPushButton(tr("Clear completed"), this);
    m_clear_button->setObjectName(QStringLiteral("fileExplorerStatusClearCompleted"));
    m_clear_button->setFlat(true);
    connect(m_clear_button,
            &QPushButton::clicked,
            m_model,
            &FileExplorerStatusCenterModel::removeAllCompletedItems);
    row->addWidget(m_clear_button);
    column->addLayout(row);
}

void FileExplorerStatusCenterFlyout::buildCardList(QVBoxLayout* column) {
    m_scroll = new QScrollArea(this);
    m_scroll->setObjectName(QStringLiteral("fileExplorerStatusCardScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_card_container = new QWidget(m_scroll);
    m_card_layout = new QVBoxLayout(m_card_container);
    m_card_layout->setContentsMargins(
        ui::kMarginNone, ui::kMarginNone, ui::kMarginNone, ui::kMarginNone);
    // Files item margin 0,2.
    m_card_layout->setSpacing(ui::kSpacingTight);
    m_card_layout->addStretch(1);
    m_scroll->setWidget(m_card_container);
    column->addWidget(m_scroll, 1);

    m_empty_label = new QLabel(tr("No ongoing file operations"), this);
    m_empty_label->setObjectName(QStringLiteral("fileExplorerStatusEmptyLabel"));
    m_empty_label->setAlignment(Qt::AlignCenter);
    column->addWidget(m_empty_label);
}

QList<FileExplorerStatusCardWidget*> FileExplorerStatusCenterFlyout::cardWidgets() const {
    QList<FileExplorerStatusCardWidget*> cards;
    for (int index = 0; index < m_card_layout->count(); ++index) {
        if (auto* card = qobject_cast<FileExplorerStatusCardWidget*>(
                m_card_layout->itemAt(index)->widget())) {
            cards.append(card);
        }
    }
    return cards;
}

void FileExplorerStatusCenterFlyout::syncCards() {
    const QList<FileExplorerStatusCenterItem*>& items = m_model->items();
    QList<FileExplorerStatusCardWidget*> cards = cardWidgets();
    const bool membership_matches =
        cards.size() == items.size() &&
        std::equal(cards.begin(),
                   cards.end(),
                   items.begin(),
                   [](const FileExplorerStatusCardWidget* card,
                      const FileExplorerStatusCenterItem* item) { return card->item() == item; });
    if (!membership_matches) {
        for (FileExplorerStatusCardWidget* card : cards) {
            m_card_layout->removeWidget(card);
            // Detach immediately so stale cards leave the child tree before
            // the deferred delete runs.
            card->setParent(nullptr);
            card->deleteLater();
        }
        int index = 0;
        for (FileExplorerStatusCenterItem* item : items) {
            auto* card = new FileExplorerStatusCardWidget(item, m_card_container);
            connect(card,
                    &FileExplorerStatusCardWidget::dismissRequested,
                    m_model,
                    &FileExplorerStatusCenterModel::removeItem);
            m_card_layout->insertWidget(index, card);
            ++index;
        }
    }
    m_empty_label->setVisible(!m_model->hasAnyItem());
    m_clear_button->setEnabled(m_model->hasAnyItem());
}

void FileExplorerStatusCenterFlyout::openAt(const QPoint& top_right) {
    syncCards();
    move(top_right.x() - width(), top_right.y());
    show();
    raise();
    // Files OnStatusCenterFlyoutOpened resolves the progress-ring visibility.
    m_model->flyoutOpened();
}

FileExplorerStatusCenterButton::FileExplorerStatusCenterButton(QWidget* parent)
    : QPushButton(parent) {
    setObjectName(QStringLiteral("fileExplorerStatusCenterButton"));
    // Placeholder glyph until the icons8 asset swap.
    m_state_icon = FileExplorerIconRegistry::iconForKey(QStringLiteral("recent"));
    setIcon(m_state_icon);
    setAccessibleName(tr("Status center"));
    setToolTip(tr("Status center"));
}

void FileExplorerStatusCenterButton::setBadge(const int state,
                                              const int value,
                                              const int average_progress,
                                              const bool show_ring) {
    m_badge_state = state;
    m_badge_value = value;
    m_average_progress = average_progress;
    m_show_ring = show_ring;
    // Files hides the plain status icon whenever the ring shows.
    setIcon(show_ring ? QIcon() : m_state_icon);
    update();
}

void FileExplorerStatusCenterButton::paintEvent(QPaintEvent* event) {
    QPushButton::paintEvent(event);
    if (!m_show_ring) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double diameter = 18.0;
    const QRectF ring_rect((width() - diameter) / kRingCenterDivisor,
                           (height() - diameter) / kRingCenterDivisor,
                           diameter,
                           diameter);
    paintRing(&painter, ring_rect);
    paintBadge(&painter);
}

void FileExplorerStatusCenterButton::paintRing(QPainter* painter, const QRectF& rect) const {
    // Files MedianOperationProgressRing: green track when everything
    // succeeded, red after a completed failure, otherwise a subtle track with
    // the accent arc at the average progress.
    QColor track = palette().color(QPalette::Mid);
    if (m_badge_state == 0) {
        track = QColor(QString::fromLatin1(ui::kColorSuccess));
    } else if (m_badge_state == kBadgeStateCompletedWithError) {
        track = QColor(QString::fromLatin1(ui::kColorError));
    }
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(track, kRingPenWidth));
    painter->drawEllipse(rect);
    if (m_badge_state == 1 || m_badge_state == kBadgeStateInProgressWithError) {
        painter->setPen(QPen(palette().color(QPalette::Highlight), kRingPenWidth));
        const int span = -m_average_progress * 360 * 16 / 100;
        painter->drawArc(rect, kRingStartAngleDeg * kQtAngleSixteenths, span);
    }
}

void FileExplorerStatusCenterButton::paintBadge(QPainter* painter) const {
    const QRectF badge(width() - kBadgeDiameter - 2, 2, kBadgeDiameter, kBadgeDiameter);
    QColor background = palette().color(QPalette::Highlight);
    if (m_badge_state == 0) {
        background = QColor(QString::fromLatin1(ui::kColorSuccess));
    } else if (m_badge_state == kBadgeStateCompletedWithError) {
        background = QColor(QString::fromLatin1(ui::kColorError));
    }
    painter->setPen(Qt::NoPen);
    painter->setBrush(background);
    painter->drawEllipse(badge);
    if (m_badge_state == 0) {
        paintCheckMark(painter, badge, Qt::white);
    } else if (m_badge_state == kBadgeStateCompletedWithError) {
        paintCrossMark(painter, badge, Qt::white);
    } else if (m_badge_value > 0) {
        painter->setPen(Qt::white);
        QFont badge_font = painter->font();
        badge_font.setPixelSize(kBadgeFontPixelSize);
        painter->setFont(badge_font);
        painter->drawText(badge, Qt::AlignCenter, QString::number(m_badge_value));
    }
}

}  // namespace sak
