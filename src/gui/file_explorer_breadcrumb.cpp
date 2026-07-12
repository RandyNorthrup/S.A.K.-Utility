// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_breadcrumb.h"

#include "sak/file_explorer_icon_registry.h"
#include "sak/layout_constants.h"
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
    while (QLayoutItem* item = layout->takeAt(0)) {
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
    m_layout->setContentsMargins(ui::kMarginSmall, 2, ui::kMarginSmall, 2);
    m_layout->setSpacing(0);
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
    if (event && event->button() == Qt::LeftButton) {
        Q_EMIT editRequested();
    }
}

QVector<FileExplorerBreadcrumb::Segment> FileExplorerBreadcrumb::splitPathSegments() const {
    QVector<Segment> segments;
    const QString normalized = QString(m_path).replace(QLatin1Char('\\'), QLatin1Char('/'));
    const bool rooted = normalized.startsWith(QLatin1Char('/'));
    QString accumulated = rooted ? QStringLiteral("/") : QString();
    if (rooted) {
        segments.append(Segment{QStringLiteral("/"), accumulated});
    }
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        if (!accumulated.isEmpty() && !accumulated.endsWith(QLatin1Char('/'))) {
            accumulated += QLatin1Char('/');
        }
        accumulated += part;
        segments.append(Segment{part, accumulated});
    }
    return segments;
}

void FileExplorerBreadcrumb::rebuildSegments() {
    clearLayout(m_layout);
    const QVector<Segment> segments = splitPathSegments();

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
    button->setToolTip(segment.target_path);
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
