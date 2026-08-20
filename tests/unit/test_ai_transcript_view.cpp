// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai_transcript_view.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QLabel>
#include <QPushButton>
#include <QtTest/QtTest>

#include <algorithm>

namespace {

QString redactSecret(const QString& text) {
    QString redacted = text;
    redacted.replace(QStringLiteral("SECRET"), QStringLiteral("[REDACTED]"));
    return redacted;
}

bool hasLabelContaining(const QWidget& widget, const QString& needle) {
    const auto labels = widget.findChildren<QLabel*>();
    return std::any_of(labels.begin(), labels.end(), [&needle](const QLabel* label) {
        return label && label->text().contains(needle);
    });
}

QPushButton* buttonWithText(QWidget& widget, const QString& text) {
    const auto buttons = widget.findChildren<QPushButton*>();
    const auto it = std::find_if(buttons.begin(), buttons.end(), [&text](const QPushButton* btn) {
        return btn && btn->text() == text;
    });
    return it == buttons.end() ? nullptr : *it;
}

}  // namespace

class AiTranscriptViewTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendMessageRendersRedactedText();
    void appendLoadedLineParsesRoleAndBody();
    void longMessageCanExpand();
    void resultBubbleCopyCopiesOnlyThatMessage();
};

void AiTranscriptViewTests::appendMessageRendersRedactedText() {
    sak::AiTranscriptView view;
    view.resize(800, 600);
    view.setTextRedactor(redactSecret);

    QVERIFY(view.appendMessage(QStringLiteral("You"), QStringLiteral("Run SECRET scan"), true));

    QCOMPARE(view.messageCount(), 1);
    QVERIFY(hasLabelContaining(view, QStringLiteral("PROMPT:")));
    QVERIFY(hasLabelContaining(view, QStringLiteral("Run [REDACTED] scan")));
    QVERIFY(!hasLabelContaining(view, QStringLiteral("SECRET")));
}

void AiTranscriptViewTests::appendLoadedLineParsesRoleAndBody() {
    sak::AiTranscriptView view;
    view.resize(800, 600);

    QVERIFY(view.appendLoadedLine(QStringLiteral("[Assistant]\nFinished scan")));
    view.renderMessages(false);

    QCOMPARE(view.messageCount(), 1);
    QVERIFY(hasLabelContaining(view, QStringLiteral("ASSISTANT RESULT:")));
    QVERIFY(hasLabelContaining(view, QStringLiteral("Finished scan")));
}

void AiTranscriptViewTests::longMessageCanExpand() {
    sak::AiTranscriptView view;
    view.resize(900, 700);
    const QString long_text = QString(2200, QLatin1Char('x'));

    QVERIFY(view.appendMessage(QStringLiteral("Tool Result"), long_text));
    // Pin the exact collapsed body, not just presence of the marker: the boundary is
    // kCollapsedChars=1800 (ai_transcript_view.cpp:319), so the body label is the first
    // 1800 'x' plus the truncation marker. A regressed boundary would still emit the marker.
    const auto labels = view.findChildren<QLabel*>();
    const auto body_it = std::find_if(labels.begin(), labels.end(), [](const QLabel* l) {
        return l != nullptr && l->text().endsWith(QStringLiteral("...[truncated]"));
    });
    QVERIFY(body_it != labels.end());
    QCOMPARE((*body_it)->text(),
             QString(1800, QLatin1Char('x')) + QStringLiteral("\n...[truncated]"));

    auto* expand = buttonWithText(view, QStringLiteral("Expand full result"));
    QVERIFY(expand != nullptr);
    expand->click();
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    QVERIFY(buttonWithText(view, QStringLiteral("Collapse")) != nullptr);
    QVERIFY(!hasLabelContaining(view, QStringLiteral("...[truncated]")));
}

void AiTranscriptViewTests::resultBubbleCopyCopiesOnlyThatMessage() {
    sak::AiTranscriptView view;
    view.resize(900, 700);
    view.setTextRedactor(redactSecret);

    QVERIFY(view.appendMessage(QStringLiteral("You"), QStringLiteral("User prompt"), true));
    QVERIFY(view.appendMessage(
        QStringLiteral("Assistant"), QStringLiteral("First SECRET result"), true));
    QVERIFY(view.appendMessage(QStringLiteral("Assistant"), QStringLiteral("Second result"), true));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // Locate the copy affordances by their AT-observable accessible name -- the contract a
    // screen-reader user actually relies on, set unconditionally on every result bubble's copy
    // button -- rather than the internal test-handle objectName. The Expand/Collapse toggle
    // carries no accessible name, so this matches exactly the two result-bubble copy buttons and
    // the user bubble still exposes none. findChildren preserves child order, so the first match
    // is still the first ("First ...") result's copy button.
    QList<QPushButton*> copyButtons;
    for (QPushButton* button : view.findChildren<QPushButton*>()) {
        if (button != nullptr && button->accessibleName() == QObject::tr("Copy chat result")) {
            copyButtons.append(button);
        }
    }
    QCOMPARE(copyButtons.size(), 2);
    copyButtons.first()->click();

    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("First [REDACTED] result"));
}

QTEST_MAIN(AiTranscriptViewTests)
#include "test_ai_transcript_view.moc"
