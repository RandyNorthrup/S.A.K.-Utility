// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_status_center_widget.h
/// @brief Files StatusCenter surfaces: the toolbar badge button, the flyout,
/// the per-operation cards, and the expanded speed graph
/// (Files UserControls/StatusCenter/StatusCenter.xaml, SpeedGraph.cs,
/// NavigationToolbar.xaml ShowStatusCenterButton).

#pragma once

#include "sak/file_explorer_status_center.h"

#include <QFrame>
#include <QPushButton>
#include <QWidget>

class QLabel;
class QMenu;
class QProgressBar;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace sak {

/// Files SpeedGraph: speed-over-progress polyline with a gradient fill,
/// x = progress percent (0-100), y = speed normalized to the running maximum.
class FileExplorerSpeedGraph : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerSpeedGraph(QWidget* parent = nullptr);

    void setPoints(const QList<QPointF>& points);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<QPointF> m_points;
};

/// One status-center card (Files StatusCenter.xaml item DataTemplate): state
/// icon circle, two-line title, dismiss (completed only), expand chevron
/// (only when speed/progress are available), collapsed progress bar or
/// expanded speed graph, footer message, and the cancel flyout.
class FileExplorerStatusCardWidget : public QFrame {
    Q_OBJECT

public:
    explicit FileExplorerStatusCardWidget(FileExplorerStatusCenterItem* item,
                                          QWidget* parent = nullptr);

    [[nodiscard]] FileExplorerStatusCenterItem* item() const { return m_item; }

Q_SIGNALS:
    void dismissRequested(sak::FileExplorerStatusCenterItem* item);

private:
    void buildHeaderRow(QVBoxLayout* column);
    void buildProgressRows(QVBoxLayout* column);
    void sync();
    void syncProgressRows();
    [[nodiscard]] QPixmap statePixmap() const;

    FileExplorerStatusCenterItem* m_item{nullptr};
    QLabel* m_icon_label{nullptr};
    QLabel* m_title_label{nullptr};
    QToolButton* m_dismiss_button{nullptr};
    QToolButton* m_expand_button{nullptr};
    QProgressBar* m_progress_bar{nullptr};
    QFrame* m_graph_frame{nullptr};
    FileExplorerSpeedGraph* m_graph{nullptr};
    QLabel* m_speed_label{nullptr};
    QLabel* m_name_label{nullptr};
    QLabel* m_message_label{nullptr};
    QToolButton* m_more_button{nullptr};
    QMenu* m_more_menu{nullptr};
};

/// The status-center flyout (Files StatusCenter.xaml): "Status center" header
/// with "Clear completed", the newest-first card list, and the empty state.
/// A plain child frame of the shell window, popup-free, so it behaves
/// identically offscreen (same pattern as the omnibar suggestion popup).
class FileExplorerStatusCenterFlyout : public QFrame {
    Q_OBJECT

public:
    explicit FileExplorerStatusCenterFlyout(FileExplorerStatusCenterModel* model,
                                            QWidget* parent = nullptr);

    /// Shows the flyout with its top-right corner at @p top_right (Files
    /// Placement=BottomEdgeAlignedRight) and notifies the model.
    void openAt(const QPoint& top_right);

    [[nodiscard]] QPushButton* clearCompletedButton() const { return m_clear_button; }
    [[nodiscard]] QList<FileExplorerStatusCardWidget*> cardWidgets() const;

private:
    void buildHeader(QVBoxLayout* column);
    void buildCardList(QVBoxLayout* column);
    void syncCards();

    FileExplorerStatusCenterModel* m_model{nullptr};
    QPushButton* m_clear_button{nullptr};
    QScrollArea* m_scroll{nullptr};
    QWidget* m_card_container{nullptr};
    QVBoxLayout* m_card_layout{nullptr};
    QLabel* m_empty_label{nullptr};
};

/// The toolbar status-center button (Files NavigationToolbar
/// ShowStatusCenterButton): plain icon at rest; while operations run it shows
/// a progress ring at the average percentage plus an InfoBadge (in-progress
/// count, green check when all succeeded, red cross after a failure).
class FileExplorerStatusCenterButton : public QPushButton {
    Q_OBJECT

public:
    explicit FileExplorerStatusCenterButton(QWidget* parent = nullptr);

    /// Mirrors the model aggregates (Files InfoBadgeState/InfoBadgeValue/
    /// AverageOperationProgressValue/ShowProgressRing).
    void setBadge(int state, int value, int average_progress, bool show_ring);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintRing(QPainter* painter, const QRectF& rect) const;
    void paintBadge(QPainter* painter) const;

    QIcon m_state_icon;
    int m_badge_state = 0;
    int m_badge_value = -1;
    int m_average_progress = 0;
    bool m_show_ring = false;
};

}  // namespace sak
