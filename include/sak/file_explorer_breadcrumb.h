// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_breadcrumb.h
/// @brief Clickable breadcrumb address control for the File Explorer omnibar.

#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QHBoxLayout;

namespace sak {

/// Files-style breadcrumb: one button per path segment separated by chevrons.
/// Clicking a segment navigates to that ancestor; clicking the empty area
/// requests inline path editing. Leading segments collapse into an overflow
/// menu when the path is deep.
class FileExplorerBreadcrumb : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerBreadcrumb(QWidget* parent = nullptr);

    void setPath(const QString& path);
    [[nodiscard]] QString path() const;

Q_SIGNALS:
    /// Emitted with the ancestor path when a segment is clicked.
    void segmentActivated(const QString& path);
    /// Emitted when the user clicks the empty area to type a path.
    void editRequested();

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    struct Segment {
        QString label;
        QString target_path;
    };

    void rebuildSegments();
    void addSegmentButton(const Segment& segment, bool is_last);
    void addChevron();
    void addOverflowButton(const QVector<Segment>& collapsed);
    [[nodiscard]] QVector<Segment> splitPathSegments() const;

    QHBoxLayout* m_layout{nullptr};
    QString m_path;
};

}  // namespace sak
