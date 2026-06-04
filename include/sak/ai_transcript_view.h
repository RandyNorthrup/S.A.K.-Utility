// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace sak {

class FollowScrollController;

/// @brief Self-contained transcript renderer for the AI Assistant chat surface.
class AiTranscriptView : public QWidget {
public:
    using TextRedactor = std::function<QString(const QString&)>;

    explicit AiTranscriptView(QWidget* parent = nullptr);
    ~AiTranscriptView() override = default;

    AiTranscriptView(const AiTranscriptView&) = delete;
    AiTranscriptView& operator=(const AiTranscriptView&) = delete;
    AiTranscriptView(AiTranscriptView&&) = delete;
    AiTranscriptView& operator=(AiTranscriptView&&) = delete;

    void setTextRedactor(TextRedactor redactor);
    [[nodiscard]] bool appendMessage(const QString& role,
                                     const QString& text,
                                     bool expanded = false);
    [[nodiscard]] bool appendLoadedLine(const QString& line);
    void clearMessages(bool render = true);
    [[nodiscard]] int messageCount() const noexcept { return m_messages.size(); }

    void setActivityText(const QString& text);
    void updateActivityFrame();

    void scrollToBottomLater(bool force = false);
    void restoreScrollPositionLater(int value);
    [[nodiscard]] bool isScrolledToBottom() const;
    void setAutoScroll(bool enabled);
    void updateJumpToNewestButton();
    void jumpToNewest();
    void renderMessages(bool scroll_to_bottom = true);

    [[nodiscard]] QScrollArea* scrollArea() const noexcept { return m_scroll; }

private:
    struct Message {
        QString id;
        QString role;
        QString text;
        bool expanded{false};
    };

    [[nodiscard]] QString redactedText(const QString& text) const;
    [[nodiscard]] QString messageBody(const Message& message, bool* long_text) const;
    [[nodiscard]] QWidget* createTranscriptRow(const Message& message, int bubble_max_width);
    [[nodiscard]] QWidget* createActivityRow(int bubble_max_width);
    [[nodiscard]] bool shouldFollowNewest(bool scroll_to_bottom) const;
    [[nodiscard]] int transcriptBubbleMaxWidth() const;
    void appendRenderedRows(int bubble_max_width);
    void restoreRenderScroll(bool follow_newest, int previous_scroll_value);
    void copyMessageToClipboard(const Message& message, QPushButton* button);
    void toggleMessageExpanded(const QString& message_id);
    void clearLayout();
    void applyActivityFrame();

    QWidget* m_content{nullptr};
    QVBoxLayout* m_layout{nullptr};
    QScrollArea* m_scroll{nullptr};
    FollowScrollController* m_scrollController{nullptr};
    QPushButton* m_jumpToNewestButton{nullptr};
    QLabel* m_activityLabel{nullptr};
    QVector<Message> m_messages;
    TextRedactor m_redactor;
    QString m_activityText;
    int m_messageSequence{0};
    int m_activityStep{0};
};

}  // namespace sak
