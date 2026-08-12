// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/view_empty_state.h"

#include "sak/style_constants.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QEvent>
#include <QLabel>

#include <utility>

namespace sak::ui {

ViewEmptyState::ViewEmptyState(QAbstractItemView* view, QString empty_text)
    : QObject(view), m_view(view), m_empty_text(std::move(empty_text)) {
    // The view owns the overlay's lifetime (parented above); a null view is a
    // programming error, and a fail-closed guard keeps the rest safe.
    if (m_view == nullptr) {
        return;
    }

    m_label = new QLabel(m_view->viewport());
    m_label->setAlignment(Qt::AlignCenter);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::NoTextInteraction);
    // Never intercept clicks meant for the view underneath.
    m_label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_label->setStyleSheet(textColorAndFontSizeStyle(kColorTextMuted, kFontSizeNote));

    m_view->viewport()->installEventFilter(this);

    // Re-evaluate visibility whenever the model's row set changes. Views in this
    // codebase set their model once at construction, so binding to the current
    // model is sufficient.
    if (auto* model = m_view->model()) {
        connect(model, &QAbstractItemModel::rowsInserted, this, [this] { refresh(); });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { refresh(); });
        connect(model, &QAbstractItemModel::modelReset, this, [this] { refresh(); });
        connect(model, &QAbstractItemModel::layoutChanged, this, [this] { refresh(); });
    }

    refresh();
}

void ViewEmptyState::setEmptyText(QString empty_text) {
    m_empty_text = std::move(empty_text);
    refresh();
}

void ViewEmptyState::setLoading(QString loading_text) {
    m_loading_text = std::move(loading_text);
    m_loading = !m_loading_text.isEmpty();
    refresh();
}

void ViewEmptyState::clearLoading() {
    m_loading = false;
    m_loading_text.clear();
    refresh();
}

bool ViewEmptyState::isOverlayVisible() const {
    // isHidden() (not isVisible()) reports the intended state even when the host
    // window is never shown, as in offscreen unit tests.
    return m_label != nullptr && !m_label->isHidden();
}

QString ViewEmptyState::overlayText() const {
    return m_label != nullptr ? m_label->text() : QString();
}

bool ViewEmptyState::eventFilter(QObject* watched, QEvent* event) {
    if (m_view != nullptr && watched == m_view->viewport() && event->type() == QEvent::Resize) {
        reposition();
    }
    return QObject::eventFilter(watched, event);
}

void ViewEmptyState::reposition() {
    if (m_label != nullptr && m_view != nullptr) {
        m_label->setGeometry(m_view->viewport()->rect());
    }
}

void ViewEmptyState::refresh() {
    if (m_label == nullptr || m_view == nullptr) {
        return;
    }
    const QAbstractItemModel* model = m_view->model();
    const int rows = model != nullptr ? model->rowCount(m_view->rootIndex()) : 0;
    const bool show = m_loading || rows == 0;

    m_label->setText(m_loading ? m_loading_text : m_empty_text);
    // Announce the empty/loading message to assistive technology.
    m_label->setAccessibleName(m_label->text());
    m_label->setVisible(show);
    if (show) {
        reposition();
    }
}

}  // namespace sak::ui
