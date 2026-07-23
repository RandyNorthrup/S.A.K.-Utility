// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_assistant_panel_tool_dispatch.cpp
/// @brief Certification test for P10-04: built-in AI tool handlers that block
/// (download/package/offline/provider-gateway) must run off the GUI thread so
/// the GUI stays responsive and Stop can cancel. Drives the real AiAssistantPanel
/// tool loop with an injected blocking handler and inspects the private async
/// state via a friend seam.

#include "sak/ai/ai_async_tool_runner.h"
#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/openai_response_types.h"
#include "sak/ai_assistant_panel.h"

#include <QComboBox>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <memory>

namespace sak {

// Friend of AiAssistantPanel: reaches the private tool dispatcher, run token, and
// async-in-flight state so the certification can drive and observe the real loop.
class AiAssistantPanelToolDispatchTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() {
        // Keep the panel's stores out of the real user data directory.
        QStandardPaths::setTestModeEnabled(true);
    }

    // A download_file call whose handler blocks must run off the GUI thread: the
    // handler enters on a non-GUI thread, the GUI event loop keeps running, and
    // the async-in-flight flag is set. Releasing the handler drives the completion
    // back onto the GUI thread and advances the tool turn.
    void blockingBuiltInRunsOffGuiThreadAndCompletes() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());

        // GUI thread was not blocked: the event loop still runs and the handler
        // enters on a different thread.
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(handler_thread.load() != QThread::currentThreadId());
        QVERIFY(panel.m_asyncToolInFlight);
        QVERIFY(panel.m_asyncToolRunner->isRunning());
        QVERIFY(panel.m_toolTurn.active());

        // Let the handler finish: completion marshals back to the GUI thread,
        // clears the in-flight flag and records the completed tool.
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(!panel.m_asyncToolInFlight, 5000);
        QCOMPARE(finished_spy.count(), 1);
        QCOMPARE(panel.m_runState.completed_tools, 1);
    }

    // Stop while a blocking handler is mid-flight must cancel: the run token is
    // cancelled and the runner is detached, so the late result is dropped -- the
    // tool turn does not advance and no completion is delivered.
    void stopCancelsInFlightBlockingTool() {
        AiAssistantPanel panel;
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};
        QSemaphore gate;

        installBlockingDownloadHandler(panel, &entered, &handler_thread, &gate);
        allowLocalToolsAndValidRun(panel);

        QSignalSpy finished_spy(panel.m_asyncToolRunner.get(), &ai::AiAsyncToolRunner::finished);
        panel.beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);

        panel.onStopClicked();
        QVERIFY(panel.m_activeToolRunToken.isValid());
        QVERIFY(panel.m_activeToolRunToken.isCancellationRequested());
        QVERIFY(!panel.m_asyncToolInFlight);
        QVERIFY(!panel.m_toolTurn.active());

        // Release the (now detached) handler and confirm the stale result is
        // dropped: finished() is never delivered and no tool is recorded.
        gate.release();
        QTest::qWait(300);
        QCOMPARE(finished_spy.count(), 0);
        QCOMPARE(panel.m_runState.completed_tools, 0);
    }

    // Destroying the panel while an async built-in tool is still running must not
    // dead-lock or use-after-free: the destructor drains the worker (which
    // captured `this`) before members are torn down.
    void destroyingPanelWhileToolRunsDrainsCleanly() {
        auto panel = std::make_unique<AiAssistantPanel>();
        std::atomic<bool> entered{false};
        std::atomic<Qt::HANDLE> handler_thread{QThread::currentThreadId()};

        // Handler ignores any gate and just sleeps briefly, so the worker is
        // genuinely in-flight when the panel is destroyed.
        panel->m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [&entered, &handler_thread](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                handler_thread = QThread::currentThreadId();
                entered = true;
                QThread::msleep(200);
                return QJsonObject{{QStringLiteral("success"), true}};
            });
        allowLocalToolsAndValidRun(*panel);
        panel->beginToolTurn(downloadResponse());
        QTRY_VERIFY_WITH_TIMEOUT(entered.load(), 5000);
        QVERIFY(panel->m_asyncToolInFlight);

        // Destroy mid-flight; must return without hanging or crashing.
        panel.reset();
        QVERIFY(true);
    }

private:
    static ai::OpenAIResponseResult downloadResponse() {
        ai::OpenAIFunctionCall call;
        call.call_id = QStringLiteral("call_1");
        call.name = QStringLiteral("download_file");
        call.arguments_json =
            QStringLiteral("{\"url\":\"https://example.com/f\",\"filename\":\"f\"}");
        ai::OpenAIResponseResult response;
        response.id = QStringLiteral("resp_1");
        response.function_calls.append(call);
        return response;
    }

    // Assisted Full Access (combo index 1) allows local tools with a lease; a
    // valid root run token makes Stop/cancel meaningful.
    static void allowLocalToolsAndValidRun(AiAssistantPanel& panel) {
        QVERIFY(panel.m_accessModeCombo != nullptr);
        panel.m_accessModeCombo->setCurrentIndex(1);
        panel.m_runToken = ai::CancellationToken::createRoot(QStringLiteral("test_run"));
    }

    // Replace the real download handler with one that blocks on the test's gate,
    // recording the thread it ran on.
    static void installBlockingDownloadHandler(AiAssistantPanel& panel,
                                               std::atomic<bool>* entered,
                                               std::atomic<Qt::HANDLE>* handler_thread,
                                               QSemaphore* gate) {
        panel.m_toolDispatcher->registerHandler(
            QStringLiteral("download_file"),
            [entered, handler_thread, gate](const QJsonObject&, const ai::AiToolPolicyDecision&) {
                *handler_thread = QThread::currentThreadId();
                *entered = true;
                gate->acquire();
                return QJsonObject{{QStringLiteral("success"), true},
                                   {QStringLiteral("path"), QStringLiteral("f")}};
            });
    }
};

}  // namespace sak

QTEST_MAIN(sak::AiAssistantPanelToolDispatchTest)
#include "test_ai_assistant_panel_tool_dispatch.moc"
