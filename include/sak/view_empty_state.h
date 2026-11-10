// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file view_empty_state.h
/// @brief Reusable empty/loading-state overlay for item views.

#pragma once

#include <QObject>
#include <QString>

class QAbstractItemView;
class QEvent;
class QLabel;

namespace sak::ui {

/// @brief A shared empty/loading-state overlay for any item view.
///
/// Qt item views (QTableView, QTableWidget, QListWidget, QTreeWidget) render as
/// a blank body when their model has no rows, which reads to a technician as
/// "nothing happened" and is indistinguishable from a still-loading view. This
/// helper installs a centered, muted, click-through QLabel over the view's
/// viewport and shows it whenever the view's top-level model is empty, or a
/// loading message is active. It closes R5-G20-7 (designed empty/loading states
/// for every panel) with ONE implementation instead of a bespoke overlay per
/// panel.
///
/// The overlay covers only the viewport, so column headers stay visible. It is
/// transparent to mouse events, so it never intercepts clicks on the view.
/// Construct one per view, parented to the view so its lifetime is tied to it.
class ViewEmptyState : public QObject {
    Q_OBJECT

public:
    /// @param view The item view to decorate. Must be non-null and should
    ///        outlive this object; passing it as the QObject parent (the
    ///        intended usage) guarantees that.
    /// @param empty_text The message shown when the model has zero top-level
    ///        rows and no loading message is active.
    ViewEmptyState(QAbstractItemView* view, QString empty_text);

    /// Replace the empty-state message; refreshes visibility immediately.
    void setEmptyText(QString empty_text);

    /// Show @p loading_text over the view regardless of row count (e.g. during a
    /// scan). An empty string is equivalent to clearLoading().
    void setLoading(QString loading_text);

    /// Return to the row-count-driven empty state (hide while rows exist).
    void clearLoading();

    /// @name Headless test accessors
    /// isHidden() (not isVisible()) is used so these report the intended state
    /// even when the host window is never shown, as in offscreen unit tests.
    /// @{
    bool isOverlayVisible() const;
    QString overlayText() const;
    /// @}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void reposition();
    void refresh();

    QAbstractItemView* m_view{nullptr};
    QLabel* m_label{nullptr};
    QString m_empty_text;
    QString m_loading_text;
    bool m_loading{false};
};

}  // namespace sak::ui
