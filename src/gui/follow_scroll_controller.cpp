// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/follow_scroll_controller.h"

#include <QEvent>
#include <QKeyEvent>
#include <QScrollBar>
#include <QSet>
#include <QTimer>

#include <algorithm>

namespace sak {

FollowScrollController::FollowScrollController(QAbstractScrollArea* scroll_area, QObject* parent)
    : QObject(parent), m_scrollArea(scroll_area) {
    if (!m_scrollArea) {
        return;
    }
    if (m_scrollArea->viewport()) {
        m_scrollArea->viewport()->installEventFilter(this);
    }
    if (auto* bar = m_scrollArea->verticalScrollBar()) {
        bar->installEventFilter(this);
        connect(bar, &QScrollBar::valueChanged, this, [this](int) { onScrollValueChanged(); });
        connect(bar, &QScrollBar::rangeChanged, this, [this](int, int) { onScrollRangeChanged(); });
    }
}

void FollowScrollController::setJumpToNewestButton(QPushButton* button) {
    m_jumpToNewestButton = button;
    updateJumpToNewestButton();
}

void FollowScrollController::setAutoScroll(bool enabled) {
    m_autoScroll = enabled;
    updateJumpToNewestButton();
}

bool FollowScrollController::isScrolledToBottom() const {
    if (!m_scrollArea || !m_scrollArea->verticalScrollBar()) {
        return true;
    }
    const auto* bar = m_scrollArea->verticalScrollBar();
    constexpr int kBottomTolerancePx = 4;
    return bar->maximum() - bar->value() <= kBottomTolerancePx;
}

bool FollowScrollController::shouldFollowNewestForAppend() const {
    return m_autoScroll || isScrolledToBottom();
}

int FollowScrollController::scrollValue() const {
    if (!m_scrollArea || !m_scrollArea->verticalScrollBar()) {
        return 0;
    }
    return m_scrollArea->verticalScrollBar()->value();
}

void FollowScrollController::scrollToBottomLater(bool force) {
    if (!canDeferredScroll(force)) {
        updateJumpToNewestButton();
        return;
    }

    QPointer<FollowScrollController> self = this;
    QTimer::singleShot(0, this, [self, force]() {
        if (self) {
            self->finishFirstDeferredScroll(force);
        }
    });
}

void FollowScrollController::restoreScrollPositionLater(int value) {
    QPointer<FollowScrollController> self = this;
    QTimer::singleShot(0, this, [self, value]() {
        if (!self || !self->m_scrollArea || !self->m_scrollArea->verticalScrollBar()) {
            return;
        }
        auto* bar = self->m_scrollArea->verticalScrollBar();
        self->setScrollValue(std::clamp(value, bar->minimum(), bar->maximum()));
        self->updateJumpToNewestButton();
    });
}

void FollowScrollController::jumpToNewest() {
    setAutoScroll(true);
    scrollToBottomLater(true);
}

void FollowScrollController::updateJumpToNewestButton() {
    if (!m_jumpToNewestButton) {
        return;
    }
    m_jumpToNewestButton->setVisible(!m_autoScroll && !isScrolledToBottom());
}

bool FollowScrollController::eventFilter(QObject* watched, QEvent* event) {
    bool scrollbar_event = false;
    if (!event || !ownsScrollEvent(watched, &scrollbar_event)) {
        return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Wheel:
        markUserScroll();
        break;
    case QEvent::MouseButtonPress:
        handleMouseButtonEvent(event->type(), scrollbar_event);
        break;
    case QEvent::MouseButtonRelease:
        handleMouseButtonEvent(event->type(), scrollbar_event);
        break;
    case QEvent::KeyPress:
        handleKeyPress(event);
        break;
    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

bool FollowScrollController::canDeferredScroll(bool force) const {
    return m_scrollArea && m_scrollArea->verticalScrollBar() && (force || m_autoScroll);
}

void FollowScrollController::finishFirstDeferredScroll(bool force) {
    if (!canDeferredScroll(force)) {
        updateJumpToNewestButton();
        return;
    }

    setScrollValue(m_scrollArea->verticalScrollBar()->maximum());
    QPointer<FollowScrollController> self = this;
    QTimer::singleShot(0, this, [self, force]() {
        if (self) {
            self->finishSecondDeferredScroll(force);
        }
    });
}

void FollowScrollController::finishSecondDeferredScroll(bool force) {
    if (!canDeferredScroll(force)) {
        updateJumpToNewestButton();
        return;
    }

    setScrollValue(m_scrollArea->verticalScrollBar()->maximum());
    setAutoScroll(true);
}

bool FollowScrollController::ownsScrollEvent(QObject* watched, bool* scrollbar_event) const {
    if (!m_scrollArea || !watched) {
        return false;
    }
    const bool viewport = watched == m_scrollArea->viewport();
    const bool scrollbar = watched == m_scrollArea->verticalScrollBar();
    if (scrollbar_event) {
        *scrollbar_event = scrollbar;
    }
    return viewport || scrollbar;
}

void FollowScrollController::handleMouseButtonEvent(QEvent::Type type, bool scrollbar_event) {
    if (!scrollbar_event) {
        return;
    }
    if (type == QEvent::MouseButtonPress) {
        m_userScrollActive = true;
        return;
    }
    m_userScrollActive = false;
    QTimer::singleShot(0, this, [this]() { setAutoScroll(isScrolledToBottom()); });
}

void FollowScrollController::handleKeyPress(QEvent* event) {
    const auto* key_event = static_cast<QKeyEvent*>(event);
    static const QSet<int> scroll_keys = {Qt::Key_Up,
                                          Qt::Key_Down,
                                          Qt::Key_PageUp,
                                          Qt::Key_PageDown,
                                          Qt::Key_Home,
                                          Qt::Key_End,
                                          Qt::Key_Space};
    if (scroll_keys.contains(key_event->key())) {
        markUserScroll();
    }
}

void FollowScrollController::markUserScroll() {
    m_userScrollActive = true;
    QTimer::singleShot(0, this, [this]() {
        m_userScrollActive = false;
        setAutoScroll(isScrolledToBottom());
    });
}

void FollowScrollController::onScrollValueChanged() {
    if (m_programmaticScroll) {
        updateJumpToNewestButton();
        return;
    }
    if (isScrolledToBottom()) {
        setAutoScroll(true);
        return;
    }
    if (m_userScrollActive) {
        setAutoScroll(false);
        return;
    }
    updateJumpToNewestButton();
}

void FollowScrollController::onScrollRangeChanged() {
    if (m_autoScroll) {
        scrollToBottomLater();
    } else {
        updateJumpToNewestButton();
    }
}

void FollowScrollController::setScrollValue(int value) {
    if (!m_scrollArea || !m_scrollArea->verticalScrollBar()) {
        return;
    }
    m_programmaticScroll = true;
    m_scrollArea->verticalScrollBar()->setValue(value);
    m_programmaticScroll = false;
}

}  // namespace sak
