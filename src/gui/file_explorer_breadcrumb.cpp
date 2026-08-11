// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_breadcrumb.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
#include "sak/rich_text_safety.h"
#include "sak/style_constants.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>

namespace sak {

namespace {

// Deep paths collapse their leading segments into one overflow menu so the
// tail (where the user is working) always stays visible.
constexpr int kMaxVisibleSegments = 5;
constexpr int kChevronIconPx = 10;

void clearLayout(QHBoxLayout* layout) {
    while (const QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

}  // namespace

FileExplorerBreadcrumb::FileExplorerBreadcrumb(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerBreadcrumb"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAccessibleName(tr("Explorer breadcrumb path"));
    setToolTip(tr("Click a segment to navigate; click the empty area to type a path"));
    setCursor(Qt::IBeamCursor);
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginTiny, ui::kMarginSmall, ui::kMarginTiny);
    m_layout->setSpacing(ui::kSpacingNone);
    m_layout->addStretch(1);
}

void FileExplorerBreadcrumb::setPath(const QString& path) {
    if (m_path == path) {
        return;
    }
    m_path = path;
    rebuildSegments();
}

QString FileExplorerBreadcrumb::path() const {
    return m_path;
}

void FileExplorerBreadcrumb::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    if ((event != nullptr) && event->button() == Qt::LeftButton) {
        Q_EMIT editRequested();
    }
}

namespace {

// UNC (\\server\share\...): host and share are one navigable root, and the
// reconstructed targets must keep the \\ prefix and backslashes -- otherwise a
// segment activates \server\... on the current drive, not the network path.
QVector<FileExplorerBreadcrumb::Segment> splitUncSegments(const QString& path) {
    using Segment = FileExplorerBreadcrumb::Segment;
    constexpr int kFirstUncPathSegmentIndex = 2;
    QVector<Segment> segments;
    const QString normalized = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QStringList parts = normalized.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return segments;
    }
    QString accumulated = QStringLiteral("\\\\") + parts.at(0);
    if (parts.size() == 1) {
        // Only the host is known; the share is still required to navigate.
        segments.append(Segment{.label = accumulated, .target_path = accumulated});
        return segments;
    }
    accumulated += QLatin1Char('\\') + parts.at(1);
    segments.append(Segment{.label = QStringLiteral("\\\\%1\\%2").arg(parts.at(0), parts.at(1)),
                            .target_path = accumulated});
    for (int i = kFirstUncPathSegmentIndex; i < parts.size(); ++i) {
        accumulated += QLatin1Char('\\') + parts.at(i);
        segments.append(Segment{.label = parts.at(i), .target_path = accumulated});
    }
    return segments;
}

}  // namespace

QVector<FileExplorerBreadcrumb::Segment> FileExplorerBreadcrumb::splitPathSegments(
    const QString& path) {
    // Defensive ceiling: the breadcrumb builds one clickable widget per path
    // component and one accumulated prefix string per segment, so a pathological
    // path (e.g. a crafted foreign-filesystem image with absurd nesting) could
    // otherwise freeze the GUI or exhaust memory. No real navigable path
    // approaches this, so above the ceiling fall back to a single whole-path
    // segment rather than splitting. This also keeps the segment count far below
    // INT_MAX, so the int component indexes below cannot narrow or wrap.
    constexpr int kMaxBreadcrumbPathChars = 32'768;
    if (path.size() > kMaxBreadcrumbPathChars) {
        return {Segment{.label = path, .target_path = path}};
    }

    if (path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"))) {
        return splitUncSegments(path);
    }

    QVector<Segment> segments;
    const QString normalized = QString(path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool rooted = normalized.startsWith(QLatin1Char('/'));
    QString accumulated = rooted ? QStringLiteral("/") : QString();
    if (rooted) {
        segments.append(Segment{.label = QStringLiteral("/"), .target_path = accumulated});
    }
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!accumulated.isEmpty() && !accumulated.endsWith(QLatin1Char('/'))) {
            accumulated += QLatin1Char('/');
        }
        accumulated += part;
        segments.append(Segment{.label = part, .target_path = accumulated});
    }
    return segments;
}

void FileExplorerBreadcrumb::rebuildSegments() {
    clearLayout(m_layout);
    const QVector<Segment> segments = splitPathSegments(m_path);

    QVector<Segment> collapsed;
    int first_visible = 0;
    if (segments.size() > kMaxVisibleSegments) {
        first_visible = segments.size() - kMaxVisibleSegments;
        collapsed = segments.mid(0, first_visible);
    }
    if (!collapsed.isEmpty()) {
        addOverflowButton(collapsed);
        addChevron();
    }
    for (int i = first_visible; i < segments.size(); ++i) {
        addSegmentButton(segments.at(i), i == segments.size() - 1);
        if (i != segments.size() - 1) {
            addChevron();
        }
    }
    m_layout->addStretch(1);
}

void FileExplorerBreadcrumb::addSegmentButton(const Segment& segment, const bool is_last) {
    auto* button = new QPushButton(segment.label, this);
    button->setObjectName(QStringLiteral("fileExplorerBreadcrumbSegment"));
    button->setAccessibleName(tr("Go to %1").arg(segment.target_path));
    // A directory path can contain markup on a raw (non-Windows) medium, and a tooltip has no
    // plain-text mode, so wrap it to be shown literally.
    button->setToolTip(ui::asLiteralRichText(segment.target_path));
    button->setCursor(Qt::PointingHandCursor);
    if (is_last) {
        button->setProperty("breadcrumbCurrent", true);
    }
    const QString target = segment.target_path;
    connect(button, &QPushButton::clicked, this, [this, target]() {
        Q_EMIT segmentActivated(target);
    });
    m_layout->addWidget(button, 0);
}

void FileExplorerBreadcrumb::addChevron() {
    auto* chevron = new QLabel(this);
    chevron->setObjectName(QStringLiteral("fileExplorerBreadcrumbChevron"));
    chevron->setPixmap(FileExplorerIconRegistry::iconForKey(QStringLiteral("chevron-right"))
                           .pixmap(kChevronIconPx, kChevronIconPx));
    m_layout->addWidget(chevron, 0);
}

void FileExplorerBreadcrumb::addOverflowButton(const QVector<Segment>& collapsed) {
    auto* button = new QPushButton(QStringLiteral("..."), this);
    button->setObjectName(QStringLiteral("fileExplorerBreadcrumbOverflow"));
    button->setAccessibleName(tr("Show collapsed path segments"));
    button->setToolTip(tr("Ancestors of the current folder"));
    button->setCursor(Qt::PointingHandCursor);
    auto* menu = new QMenu(button);
    for (const Segment& segment : collapsed) {
        const QString target = segment.target_path;
        menu->addAction(segment.label, this, [this, target]() { Q_EMIT segmentActivated(target); });
    }
    button->setMenu(menu);
    m_layout->addWidget(button, 0);
}

}  // namespace sak
