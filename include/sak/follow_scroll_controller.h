// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QAbstractScrollArea>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QPushButton>

namespace sak {

class FollowScrollController : public QObject {
public:
    explicit FollowScrollController(QAbstractScrollArea* scroll_area, QObject* parent = nullptr);
    ~FollowScrollController() override = default;

    void setJumpToNewestButton(QPushButton* button);

    [[nodiscard]] bool autoScroll() const { return m_autoScroll; }
    void setAutoScroll(bool enabled);

    [[nodiscard]] bool isScrolledToBottom() const;
    [[nodiscard]] bool shouldFollowNewestForAppend() const;
    [[nodiscard]] int scrollValue() const;

    void scrollToBottomLater(bool force = false);
    void restoreScrollPositionLater(int value);
    void jumpToNewest();
    void updateJumpToNewestButton();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] bool canDeferredScroll(bool force) const;
    [[nodiscard]] bool ownsScrollEvent(QObject* watched, bool* scrollbar_event) const;
    void finishFirstDeferredScroll(bool force);
    void finishSecondDeferredScroll(bool force);
    void handleMouseButtonEvent(QEvent::Type type, bool scrollbar_event);
    void handleKeyPress(QEvent* event);
    void markUserScroll();
    void onScrollValueChanged();
    void onScrollRangeChanged();
    void setScrollValue(int value);

    QPointer<QAbstractScrollArea> m_scrollArea;
    QPointer<QPushButton> m_jumpToNewestButton;
    bool m_autoScroll{true};
    bool m_programmaticScroll{false};
    bool m_userScrollActive{false};
};

}  // namespace sak
