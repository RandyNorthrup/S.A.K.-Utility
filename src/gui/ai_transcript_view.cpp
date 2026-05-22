// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai_transcript_view.h"

#include "sak/follow_scroll_controller.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSize>
#include <QSizePolicy>

#include <utility>
#include <vector>

namespace sak {

namespace {

QScrollArea* createTranscriptScrollArea(QWidget* parent, QWidget* content) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

void configureTranscriptButton(QPushButton* button, const QString& icon_path) {
    if (!button) {
        return;
    }
    button->setMinimumHeight(34);
    button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    if (!icon_path.isEmpty()) {
        button->setIcon(QIcon(icon_path));
        button->setIconSize(QSize(16, 16));
    }
}

QString chatRoleHeading(const QString& role) {
    const QString normalized = role.trimmed().toLower();
    if (normalized == QLatin1String("you") || normalized == QLatin1String("user")) {
        return QStringLiteral("PROMPT:");
    }
    if (normalized == QLatin1String("assistant")) {
        return QStringLiteral("ASSISTANT RESULT:");
    }
    const std::vector<std::pair<const char*, const char*>> contains_rules{
        {"workflow", "WORKFLOW RESULT:"},
        {"tool", "TOOL RESULT:"},
        {"error", "ERROR:"},
        {"system", "SYSTEM:"},
        {"queued", "QUEUED REQUEST:"},
        {"steering", "RUN STEERING:"},
    };
    for (const auto& [needle, heading] : contains_rules) {
        if (normalized.contains(QString::fromLatin1(needle))) {
            return QString::fromLatin1(heading);
        }
    }
    return role.trimmed().isEmpty() ? QStringLiteral("MESSAGE") : role.trimmed().toUpper();
}

bool isUserChatRole(const QString& role) {
    const QString normalized = role.trimmed().toLower();
    return normalized == QLatin1String("you") || normalized == QLatin1String("user") ||
           normalized == QLatin1String("user request");
}

}  // namespace

AiTranscriptView::AiTranscriptView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("aiTranscriptContent"));
    m_content->setStyleSheet(QStringLiteral("QWidget#aiTranscriptContent { background: %1; }")
                                 .arg(sak::ui::kColorBgSurface));
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium);
    m_layout->setSpacing(sak::ui::kSpacingMedium);
    m_layout->addStretch();

    m_scroll = createTranscriptScrollArea(this, m_content);
    m_scroll->setObjectName(QStringLiteral("aiTranscriptScroll"));
    m_scroll->setMinimumHeight(320);
    m_scroll->setStyleSheet(
        QStringLiteral("QScrollArea#aiTranscriptScroll { background: %1; border: 0; }")
            .arg(sak::ui::kColorBgSurface));
    setAccessible(m_scroll, QObject::tr("AI conversation transcript"));
    root->addWidget(m_scroll, 1);

    m_scrollController = new FollowScrollController(m_scroll, this);

    auto* nav_row = new QHBoxLayout();
    nav_row->setContentsMargins(sak::ui::kMarginMedium,
                                sak::ui::kSpacingTight,
                                sak::ui::kMarginMedium,
                                sak::ui::kSpacingTight);
    nav_row->addStretch();

    m_jumpToNewestButton = new QPushButton(QObject::tr("Jump to newest"), this);
    configureTranscriptButton(m_jumpToNewestButton,
                              QStringLiteral(":/icons/icons/icons8-sent.svg"));
    m_jumpToNewestButton->setToolTip(QObject::tr("Resume following the latest AI chat activity"));
    setAccessible(m_jumpToNewestButton,
                  QObject::tr("Jump to newest AI message"),
                  QObject::tr("Scrolls the AI transcript to the bottom and resumes auto-scroll"));
    connect(m_jumpToNewestButton,
            &QPushButton::clicked,
            m_scrollController,
            &FollowScrollController::jumpToNewest);
    m_jumpToNewestButton->hide();
    m_scrollController->setJumpToNewestButton(m_jumpToNewestButton);
    nav_row->addWidget(m_jumpToNewestButton);
    root->addLayout(nav_row);
}

void AiTranscriptView::setTextRedactor(TextRedactor redactor) {
    m_redactor = std::move(redactor);
}

bool AiTranscriptView::appendMessage(const QString& role, const QString& text, bool expanded) {
    const QString body = redactedText(text).trimmed();
    if (body.isEmpty()) {
        return false;
    }
    Message message;
    message.id = QStringLiteral("msg_%1").arg(++m_messageSequence);
    message.role = role.trimmed().isEmpty() ? QStringLiteral("Message") : role.trimmed();
    message.text = body;
    message.expanded = expanded;
    m_messages.append(message);
    renderMessages(true);
    return true;
}

bool AiTranscriptView::appendLoadedLine(const QString& line) {
    QString trimmed = line.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }

    QString role;
    QString text;
    if (trimmed.startsWith(QLatin1Char('['))) {
        const int end = trimmed.indexOf(QStringLiteral("]\n"));
        if (end > 1) {
            role = trimmed.mid(1, end - 1).trimmed();
            text = trimmed.mid(end + 2).trimmed();
        }
    }
    if (role.isEmpty()) {
        const int colon = trimmed.indexOf(QStringLiteral(": "));
        if (colon > 0 && colon < 32) {
            role = trimmed.left(colon).trimmed();
            text = trimmed.mid(colon + 2).trimmed();
        }
    }
    if (role.isEmpty()) {
        role = QStringLiteral("Message");
        text = trimmed;
    }

    const QString body = redactedText(text).trimmed();
    if (body.isEmpty()) {
        return false;
    }
    Message message;
    message.id = QStringLiteral("msg_%1").arg(++m_messageSequence);
    message.role = role;
    message.text = body;
    message.expanded = false;
    m_messages.append(message);
    return true;
}

void AiTranscriptView::clearMessages(bool render) {
    m_messages.clear();
    m_messageSequence = 0;
    if (render) {
        renderMessages(false);
    }
}

void AiTranscriptView::setActivityText(const QString& text) {
    const QString next_text = text.trimmed();
    const bool text_changed = m_activityText != next_text;
    m_activityText = next_text;
    m_activityStep = 0;
    if (text_changed || (m_activityLabel == nullptr && !m_activityText.isEmpty())) {
        renderMessages(true);
    }
    applyActivityFrame();
}

void AiTranscriptView::updateActivityFrame() {
    if (m_activityText.isEmpty()) {
        return;
    }
    if (!m_activityLabel) {
        renderMessages(true);
    }
    if (!m_activityLabel) {
        return;
    }
    applyActivityFrame();
    scrollToBottomLater();
}

void AiTranscriptView::scrollToBottomLater(bool force) {
    if (m_scrollController) {
        m_scrollController->scrollToBottomLater(force);
    }
}

void AiTranscriptView::restoreScrollPositionLater(int value) {
    if (m_scrollController) {
        m_scrollController->restoreScrollPositionLater(value);
    }
}

bool AiTranscriptView::isScrolledToBottom() const {
    return !m_scrollController || m_scrollController->isScrolledToBottom();
}

void AiTranscriptView::setAutoScroll(bool enabled) {
    if (m_scrollController) {
        m_scrollController->setAutoScroll(enabled);
    }
}

void AiTranscriptView::updateJumpToNewestButton() {
    if (m_scrollController) {
        m_scrollController->updateJumpToNewestButton();
    }
}

void AiTranscriptView::jumpToNewest() {
    if (m_scrollController) {
        m_scrollController->jumpToNewest();
    }
}

void AiTranscriptView::renderMessages(bool scroll_to_bottom) {
    if (!m_layout || !m_content || !m_scroll) {
        return;
    }

    const bool follow_newest = shouldFollowNewest(scroll_to_bottom);
    const int previous_scroll_value = m_scrollController ? m_scrollController->scrollValue() : 0;

    clearLayout();
    appendRenderedRows(transcriptBubbleMaxWidth());
    restoreRenderScroll(follow_newest, previous_scroll_value);
    updateJumpToNewestButton();
}

bool AiTranscriptView::shouldFollowNewest(bool scroll_to_bottom) const {
    return scroll_to_bottom && (!m_scrollController || m_scrollController->autoScroll());
}

int AiTranscriptView::transcriptBubbleMaxWidth() const {
    const int viewport_width = m_scroll && m_scroll->viewport() ? m_scroll->viewport()->width()
                                                                : 900;
    return qMax(360, (viewport_width * 76) / 100);
}

void AiTranscriptView::appendRenderedRows(int bubble_max_width) {
    for (const auto& message : m_messages) {
        m_layout->addWidget(createTranscriptRow(message, bubble_max_width));
    }
    if (!m_activityText.isEmpty()) {
        m_layout->addWidget(createActivityRow(bubble_max_width));
        applyActivityFrame();
    }
    m_layout->addStretch();
}

void AiTranscriptView::restoreRenderScroll(bool follow_newest, int previous_scroll_value) {
    if (follow_newest) {
        scrollToBottomLater();
    } else {
        restoreScrollPositionLater(previous_scroll_value);
    }
}

QString AiTranscriptView::redactedText(const QString& text) const {
    return m_redactor ? m_redactor(text) : text;
}

QString AiTranscriptView::messageBody(const Message& message, bool* long_text) const {
    constexpr int kCollapsedChars = 1800;
    constexpr int kCollapsedLines = 28;
    const QStringList all_lines = message.text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const bool is_long = message.text.size() > kCollapsedChars ||
                         all_lines.size() > kCollapsedLines;
    if (long_text) {
        *long_text = is_long;
    }
    if (!is_long || message.expanded) {
        return message.text;
    }
    QString body = message.text.left(kCollapsedChars).trimmed();
    const auto collapsed_lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (collapsed_lines.size() > kCollapsedLines) {
        body = collapsed_lines.mid(0, kCollapsedLines).join(QLatin1Char('\n')).trimmed();
    }
    return body + QStringLiteral("\n...[truncated]");
}

QWidget* AiTranscriptView::createTranscriptRow(const Message& message, int bubble_max_width) {
    bool long_text = false;
    const bool user = isUserChatRole(message.role);
    const QString body = messageBody(message, &long_text);
    auto* row = new QWidget(m_content);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(0);
    if (user) {
        row_layout->addStretch();
    }

    auto* bubble = new QFrame(row);
    bubble->setObjectName(user ? QStringLiteral("aiTranscriptBubbleUser")
                               : QStringLiteral("aiTranscriptBubbleResult"));
    bubble->setMaximumWidth(bubble_max_width);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    bubble->setStyleSheet(
        user ? QStringLiteral(
                   "QFrame#aiTranscriptBubbleUser { background: %1; border: 2px solid #ffffff; "
                   "border-radius: 12px; }")
                   .arg(sak::ui::kColorPrimaryDark)
             : QStringLiteral(
                   "QFrame#aiTranscriptBubbleResult { background: %1; border: 1px solid %2; "
                   "border-radius: 12px; }")
                   .arg(sak::ui::kColorBgWhite, sak::ui::kColorBorderDefault));
    auto* bubble_layout = new QVBoxLayout(bubble);
    bubble_layout->setContentsMargins(12, 10, 12, 10);
    bubble_layout->setSpacing(sak::ui::kSpacingSmall);

    auto* role_label = new QLabel(chatRoleHeading(message.role), bubble);
    role_label->setStyleSheet(
        QStringLiteral("background: transparent; border: 0; "
                       "font-size: 8pt; font-weight: 700; color: %1;")
            .arg(user ? QStringLiteral("#dbeafe") : QString::fromLatin1(sak::ui::kColorTextMuted)));
    bubble_layout->addWidget(role_label);

    auto* body_label = new QLabel(body, bubble);
    body_label->setWordWrap(true);
    body_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    body_label->setStyleSheet(
        QStringLiteral("background: transparent; border: 0; "
                       "font-size: 9.5pt; line-height: 142%; color: %1;")
            .arg(user ? QStringLiteral("#ffffff") : QString::fromLatin1(sak::ui::kColorTextBody)));
    bubble_layout->addWidget(body_label);

    if (long_text) {
        auto* toggle = new QPushButton(
            message.expanded ? QObject::tr("Collapse") : QObject::tr("Expand full result"), bubble);
        toggle->setFlat(true);
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setStyleSheet(
            QStringLiteral("QPushButton { border: 0; padding: 2px 0; text-align: left; "
                           "font-weight: 700; color: %1; background: transparent; }")
                .arg(user ? QStringLiteral("#ffffff")
                          : QString::fromLatin1(sak::ui::kColorPrimaryDark)));
        const QString message_id = message.id;
        connect(toggle, &QPushButton::clicked, this, [this, message_id]() {
            toggleMessageExpanded(message_id);
        });
        bubble_layout->addWidget(toggle, 0, Qt::AlignLeft);
    }

    row_layout->addWidget(bubble);
    if (!user) {
        row_layout->addStretch();
    }
    return row;
}

QWidget* AiTranscriptView::createActivityRow(int bubble_max_width) {
    auto* row = new QWidget(m_content);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(0, 0, 0, 0);
    row_layout->setSpacing(0);

    auto* bubble = new QFrame(row);
    bubble->setObjectName(QStringLiteral("aiTranscriptActivityBubble"));
    bubble->setMaximumWidth(qMin(bubble_max_width, 520));
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    bubble->setStyleSheet(QStringLiteral("QFrame#aiTranscriptActivityBubble { background: %1; "
                                         "border: 1px solid %2; border-radius: 12px; }")
                              .arg(sak::ui::kColorBgWhite, sak::ui::kStatusColorRunning));

    auto* bubble_layout = new QVBoxLayout(bubble);
    bubble_layout->setContentsMargins(12, 8, 12, 8);
    bubble_layout->setSpacing(sak::ui::kSpacingTight);

    auto* role_label = new QLabel(QObject::tr("Status"), bubble);
    role_label->setStyleSheet(QStringLiteral("background: transparent; border: 0; "
                                             "font-size: 8pt; font-weight: 700; color: %1;")
                                  .arg(sak::ui::kColorTextMuted));
    bubble_layout->addWidget(role_label);

    m_activityLabel = new QLabel(bubble);
    m_activityLabel->setWordWrap(true);
    m_activityLabel->setToolTip(QObject::tr("Current AI activity"));
    m_activityLabel->setStyleSheet(QStringLiteral("background: transparent; border: 0; "
                                                  "font-size: 9.5pt; font-weight: 700; color: %1;")
                                       .arg(sak::ui::kStatusColorRunning));
    setAccessible(m_activityLabel, QObject::tr("AI activity indicator"));
    bubble_layout->addWidget(m_activityLabel);

    row_layout->addWidget(bubble);
    row_layout->addStretch();
    return row;
}

void AiTranscriptView::toggleMessageExpanded(const QString& message_id) {
    for (auto& item : m_messages) {
        if (item.id == message_id) {
            item.expanded = !item.expanded;
            renderMessages(false);
            return;
        }
    }
}

void AiTranscriptView::clearLayout() {
    if (!m_layout) {
        return;
    }
    m_activityLabel = nullptr;
    while (QLayoutItem* item = m_layout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        if (QLayout* child_layout = item->layout()) {
            while (QLayoutItem* child = child_layout->takeAt(0)) {
                if (QWidget* child_widget = child->widget()) {
                    child_widget->deleteLater();
                }
                delete child;
            }
            delete child_layout;
        }
        delete item;
    }
}

void AiTranscriptView::applyActivityFrame() {
    if (!m_activityLabel || m_activityText.isEmpty()) {
        return;
    }
    const QString dots = QString(m_activityStep % 4, QLatin1Char('.'));
    m_activityLabel->setText(QStringLiteral("%1%2").arg(m_activityText, dots));
    m_activityStep = (m_activityStep + 1) % 4;
}

}  // namespace sak
