// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai_transcript_view.h"

#include "sak/follow_scroll_controller.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QClipboard>
#include <QFrame>
#include <QGuiApplication>
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

constexpr int kBracketHeaderDelimiterLength = 2;
constexpr int kRoleDelimiterLength = 2;
constexpr int kMaximumRolePrefixChars = 32;
constexpr int kFallbackViewportWidth = 900;
constexpr int kBubbleWidthPercent = 76;
constexpr double kTranscriptBodyFontPointSize = 9.5;
constexpr int kActivityAnimationFrameCount = 4;
constexpr int kCopyButtonSize = 26;

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
    button->setMinimumHeight(sak::ui::kUiButtonHeightDialog);
    button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    if (!icon_path.isEmpty()) {
        button->setIcon(QIcon(icon_path));
        button->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
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
    root->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    root->setSpacing(sak::ui::kSpacingNone);

    m_content = new QWidget(this);
    m_content->setObjectName(QStringLiteral("aiTranscriptContent"));
    m_content->setStyleSheet(
        sak::ui::backgroundStyle("QWidget#aiTranscriptContent", sak::ui::kColorBgSurface));
    m_layout = new QVBoxLayout(m_content);
    m_layout->setContentsMargins(sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium,
                                 sak::ui::kMarginMedium);
    m_layout->setSpacing(sak::ui::kSpacingMedium);
    m_layout->addStretch();

    m_scroll = createTranscriptScrollArea(this, m_content);
    m_scroll->setObjectName(QStringLiteral("aiTranscriptScroll"));
    m_scroll->setMinimumHeight(sak::kDialogHeightSmall + sak::ui::kMarginXLarge);
    m_scroll->setStyleSheet(
        sak::ui::backgroundStyle("QScrollArea#aiTranscriptScroll", sak::ui::kColorBgSurface));
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
            text = trimmed.mid(end + kBracketHeaderDelimiterLength).trimmed();
        }
    }
    if (role.isEmpty()) {
        const int colon = trimmed.indexOf(QStringLiteral(": "));
        if (colon > 0 && colon < kMaximumRolePrefixChars) {
            role = trimmed.left(colon).trimmed();
            text = trimmed.mid(colon + kRoleDelimiterLength).trimmed();
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
                                                                : kFallbackViewportWidth;
    return qMax(sak::kDetachLogMinW, (viewport_width * kBubbleWidthPercent) / sak::kPercentMax);
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
    row_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    row_layout->setSpacing(sak::ui::kSpacingNone);
    if (user) {
        row_layout->addStretch();
    }

    auto* bubble = createTranscriptBubble(message, body, long_text, bubble_max_width);
    row_layout->addWidget(bubble);
    if (!user) {
        row_layout->addStretch();
    }
    return row;
}

QFrame* AiTranscriptView::createTranscriptBubble(const Message& message,
                                                 const QString& body,
                                                 bool long_text,
                                                 int bubble_max_width) {
    const bool user = isUserChatRole(message.role);
    auto* bubble = new QFrame(m_content);
    bubble->setObjectName(user ? QStringLiteral("aiTranscriptBubbleUser")
                               : QStringLiteral("aiTranscriptBubbleResult"));
    bubble->setMaximumWidth(bubble_max_width);
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    bubble->setStyleSheet(sak::ui::transcriptBubbleStyle(user));
    auto* bubble_layout = new QVBoxLayout(bubble);
    bubble_layout->setContentsMargins(sak::ui::kMarginLarge,
                                      sak::ui::kSpacingDefault,
                                      sak::ui::kMarginLarge,
                                      sak::ui::kSpacingDefault);
    bubble_layout->setSpacing(sak::ui::kSpacingSmall);
    addTranscriptHeading(bubble_layout, bubble, message);
    addTranscriptBody(bubble_layout, bubble, body, user);
    addTranscriptToggle(bubble_layout, bubble, message, user, long_text);
    return bubble;
}

void AiTranscriptView::addTranscriptHeading(QVBoxLayout* bubble_layout,
                                            QFrame* bubble,
                                            const Message& message) {
    const bool user = isUserChatRole(message.role);
    auto* heading_row = new QHBoxLayout();
    heading_row->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    heading_row->setSpacing(sak::ui::kSpacingSmall);

    auto* role_label = new QLabel(chatRoleHeading(message.role), bubble);
    role_label->setStyleSheet(sak::ui::transparentTextStyle(sak::ui::kFontSizeSmall,
                                                            sak::ui::kFontWeightBold,
                                                            user ? sak::ui::kColorBgUserBubbleText
                                                                 : sak::ui::kColorTextMuted));
    heading_row->addWidget(role_label, 1);
    if (!user) {
        auto* copy = new QPushButton(bubble);
        copy->setObjectName(QStringLiteral("aiTranscriptCopyButton"));
        copy->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-pm-copy.svg")));
        copy->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
        copy->setFixedSize(kCopyButtonSize, kCopyButtonSize);
        copy->setFlat(true);
        copy->setCursor(Qt::PointingHandCursor);
        copy->setToolTip(QObject::tr("Copy this chat result"));
        copy->setAccessibleName(QObject::tr("Copy chat result"));
        copy->setAccessibleDescription(
            QObject::tr("Copies only this chat result bubble to the clipboard"));
        copy->setStyleSheet(sak::ui::transparentHoverButtonStyle(sak::ui::kColorBgPageHover));
        connect(copy, &QPushButton::clicked, this, [this, message, copy]() {
            copyMessageToClipboard(message, copy);
        });
        heading_row->addWidget(copy, 0, Qt::AlignRight | Qt::AlignTop);
    }
    bubble_layout->addLayout(heading_row);
}

void AiTranscriptView::addTranscriptBody(QVBoxLayout* bubble_layout,
                                         QFrame* bubble,
                                         const QString& body,
                                         bool user) {
    auto* body_label = new QLabel(body, bubble);
    body_label->setWordWrap(true);
    body_label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    body_label->setStyleSheet(sak::ui::transparentBodyTextStyle(
        kTranscriptBodyFontPointSize, user ? sak::ui::kColorBgWhite : sak::ui::kColorTextBody));
    bubble_layout->addWidget(body_label);
}

void AiTranscriptView::addTranscriptToggle(
    QVBoxLayout* bubble_layout, QFrame* bubble, const Message& message, bool user, bool long_text) {
    if (!long_text) {
        return;
    }
    auto* toggle = new QPushButton(
        message.expanded ? QObject::tr("Collapse") : QObject::tr("Expand full result"), bubble);
    toggle->setFlat(true);
    toggle->setCursor(Qt::PointingHandCursor);
    toggle->setStyleSheet(
        sak::ui::transcriptToggleStyle(user ? QString::fromLatin1(sak::ui::kColorBgWhite)
                                            : QString::fromLatin1(sak::ui::kColorPrimaryDark)));
    const QString message_id = message.id;
    connect(toggle, &QPushButton::clicked, this, [this, message_id]() {
        toggleMessageExpanded(message_id);
    });
    bubble_layout->addWidget(toggle, 0, Qt::AlignLeft);
}

QWidget* AiTranscriptView::createActivityRow(int bubble_max_width) {
    auto* row = new QWidget(m_content);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    row_layout->setSpacing(sak::ui::kSpacingNone);

    auto* bubble = new QFrame(row);
    bubble->setObjectName(QStringLiteral("aiTranscriptActivityBubble"));
    bubble->setMaximumWidth(qMin(bubble_max_width, sak::kDialogWidthLarge));
    bubble->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    bubble->setStyleSheet(sak::ui::transcriptActivityBubbleStyle());

    auto* bubble_layout = new QVBoxLayout(bubble);
    bubble_layout->setContentsMargins(
        sak::ui::kMarginLarge, sak::ui::kMarginSmall, sak::ui::kMarginLarge, sak::ui::kMarginSmall);
    bubble_layout->setSpacing(sak::ui::kSpacingTight);

    auto* role_label = new QLabel(QObject::tr("Status"), bubble);
    role_label->setStyleSheet(sak::ui::transparentTextStyle(
        sak::ui::kFontSizeSmall, sak::ui::kFontWeightBold, sak::ui::kColorTextMuted));
    bubble_layout->addWidget(role_label);

    m_activityLabel = new QLabel(bubble);
    m_activityLabel->setWordWrap(true);
    m_activityLabel->setToolTip(QObject::tr("Current AI activity"));
    m_activityLabel->setStyleSheet(sak::ui::transparentTextStyle(
        kTranscriptBodyFontPointSize, sak::ui::kFontWeightBold, sak::ui::kStatusColorRunning));
    setAccessible(m_activityLabel, QObject::tr("AI activity indicator"));
    bubble_layout->addWidget(m_activityLabel);

    row_layout->addWidget(bubble);
    row_layout->addStretch();
    return row;
}

void AiTranscriptView::copyMessageToClipboard(const Message& message, QPushButton* button) {
    if (auto* clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(message.text);
    }
    if (button) {
        button->setToolTip(QObject::tr("Copied"));
    }
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
    const QString dots = QString(m_activityStep % kActivityAnimationFrameCount, QLatin1Char('.'));
    m_activityLabel->setText(QStringLiteral("%1%2").arg(m_activityText, dots));
    m_activityStep = (m_activityStep + 1) % kActivityAnimationFrameCount;
}

}  // namespace sak
