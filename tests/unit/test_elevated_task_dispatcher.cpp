// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevated_task_dispatcher.cpp
/// @brief Unit tests for ElevatedTaskDispatcher (Phase 2)
///
///  - Handler registration and allowlist checks
///  - Task dispatch (success, failure, exception safety)
///  - Unregistered task rejection
///  - Progress and cancellation callbacks

#include "sak/elevated_task_dispatcher.h"

#include <QList>
#include <QPair>
#include <QTest>

class TestElevatedTaskDispatcher : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ======================================================================
    // Registration
    // ======================================================================

    void testInitiallyEmpty() {
        sak::ElevatedTaskDispatcher dispatcher;
        QCOMPARE(dispatcher.handlerCount(), 0);
    }

    void testRegisterSingle() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("TestTask",
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       return sak::TaskHandlerResult{true, {}, {}};
                                   });
        QCOMPARE(dispatcher.handlerCount(), 1);
        QVERIFY(dispatcher.isAllowed("TestTask"));
    }

    void testRegisterMultiple() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("Task1", [](auto&&, auto, auto) {
            return sak::TaskHandlerResult{true, {}, {}};
        });
        dispatcher.registerHandler("Task2", [](auto&&, auto, auto) {
            return sak::TaskHandlerResult{true, {}, {}};
        });
        dispatcher.registerHandler("Task3", [](auto&&, auto, auto) {
            return sak::TaskHandlerResult{true, {}, {}};
        });
        QCOMPARE(dispatcher.handlerCount(), 3);
    }

    // ======================================================================
    // Allowlist Checks
    // ======================================================================

    void testIsAllowedReturnsFalseForUnregistered() {
        sak::ElevatedTaskDispatcher dispatcher;
        QVERIFY(!dispatcher.isAllowed("SomethingNotRegistered"));
    }

    void testIsAllowedReturnsTrueForRegistered() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("AllowedTask", [](auto&&, auto, auto) {
            return sak::TaskHandlerResult{true, {}, {}};
        });
        QVERIFY(dispatcher.isAllowed("AllowedTask"));
        QVERIFY(!dispatcher.isAllowed("DisallowedTask"));
    }

    // ======================================================================
    // Dispatch -- Success
    // ======================================================================

    void testDispatchSuccess() {
        sak::ElevatedTaskDispatcher dispatcher;
        QJsonObject expected_data;
        expected_data["result"] = "fixed 3 items";

        dispatcher.registerHandler(
            "FixStuff", [&](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                sak::TaskHandlerResult result;
                result.success = true;
                result.data = expected_data;
                return result;
            });

        auto result =
            dispatcher.dispatch("FixStuff", {}, [](int, const QString&) {}, [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(result->success);
        // The handler's result is returned verbatim: pin the whole data object (an injected or
        // dropped key would survive a single-key check) and the empty error on a success.
        QCOMPARE(result->data, expected_data);
        QVERIFY(result->error_message.isEmpty());
    }

    void testDispatchFailure() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("FailTask",
                                   [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) {
                                       sak::TaskHandlerResult result;
                                       result.success = false;
                                       result.error_message = "disk not found";
                                       return result;
                                   });

        auto result =
            dispatcher.dispatch("FailTask", {}, [](int, const QString&) {}, [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(!result->success);
        QCOMPARE(result->error_message, "disk not found");
        // Fail closed: a failure hands back no data payload for the caller to act on.
        QCOMPARE(result->data, QJsonObject{});
    }

    // ======================================================================
    // Dispatch -- Unregistered Task
    // ======================================================================

    void testDispatchUnregisteredTask() {
        sak::ElevatedTaskDispatcher dispatcher;
        int progress_calls = 0;
        int cancel_checks = 0;
        auto result = dispatcher.dispatch(
            "NeverRegistered",
            {},
            [&](int, const QString&) { ++progress_calls; },
            [&] {
                ++cancel_checks;
                return false;
            });

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::task_not_allowed);
        // The allowlist check happens BEFORE anything runs: neither callback is ever reached,
        // so a rejected task cannot report progress or observe the cancel state.
        QCOMPARE(progress_calls, 0);
        QCOMPARE(cancel_checks, 0);
    }

    // ======================================================================
    // Dispatch -- Exception Safety
    // ======================================================================

    void testDispatchExceptionSafety() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("ThrowTask",
                                   [](const QJsonObject&,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       throw std::runtime_error("unexpected error");
                                   });

        auto result =
            dispatcher.dispatch("ThrowTask", {}, [](int, const QString&) {}, [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(!result->success);
        QCOMPARE(result->error_message, QStringLiteral("unexpected error"));
        QCOMPARE(result->data, QJsonObject{});
    }

    void testDispatchNonStdExceptionSafety() {
        // Sibling of the catch(const std::exception&) branch above: a NON-std throw must also be
        // converted into a structured failure rather than escaping and terminating the elevated
        // helper. This branch synthesizes its own message (the throw carries none), so the exact
        // text -- which names the task -- is what proves the right handler caught it.
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler("ThrowNonStdTask",
                                   [](const QJsonObject&,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult { throw 42; });

        auto result = dispatcher.dispatch(
            "ThrowNonStdTask", {}, [](int, const QString&) {}, [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(!result->success);
        QCOMPARE(result->error_message,
                 QStringLiteral("Task 'ThrowNonStdTask' failed with an unknown exception"));
        QCOMPARE(result->data, QJsonObject{});
    }

    // ======================================================================
    // Progress Callback
    // ======================================================================

    void testProgressCallback() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(
            "ProgressTask",
            [](const QJsonObject&, sak::ProgressCallback progress, sak::CancelCheck) {
                progress(25, "Step 1");
                progress(50, "Step 2");
                progress(100, "Done");
                return sak::TaskHandlerResult{true, {}, {}};
            });

        QList<QPair<int, QString>> progress_events;

        auto result = dispatcher.dispatch(
            "ProgressTask",
            {},
            [&](int pct, const QString& status) { progress_events.append({pct, status}); },
            [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(result->success);
        QVERIFY(result->error_message.isEmpty());
        QCOMPARE(result->data, QJsonObject{});
        // Recording only the LAST callback cannot catch a dropped, reordered or duplicated
        // intermediate report -- pin the whole ordered sequence the handler emitted.
        QCOMPARE(progress_events,
                 (QList<QPair<int, QString>>{{25, QStringLiteral("Step 1")},
                                             {50, QStringLiteral("Step 2")},
                                             {100, QStringLiteral("Done")}}));
    }

    // ======================================================================
    // Cancel Check
    // ======================================================================

    void testCancelCheck() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(
            "CancellableTask",
            [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck is_cancelled) {
                sak::TaskHandlerResult result;
                result.success = !is_cancelled();
                return result;
            });

        // Normal run -- not cancelled
        auto result1 = dispatcher.dispatch(
            "CancellableTask", {}, [](int, const QString&) {}, [] { return false; });
        QVERIFY(result1.has_value());
        QVERIFY(result1->success);
        QVERIFY(result1->error_message.isEmpty());
        QCOMPARE(result1->data, QJsonObject{});

        // Cancelled run: the handler reports failure through the SAME structured result, so the
        // two runs must differ in `success` alone -- a cancellation is not an error_message and
        // does not fabricate a data payload.
        auto result2 = dispatcher.dispatch(
            "CancellableTask", {}, [](int, const QString&) {}, [] { return true; });
        QVERIFY(result2.has_value());
        QVERIFY(!result2->success);
        QVERIFY(result2->error_message.isEmpty());
        QCOMPARE(result2->data, QJsonObject{});
    }

    // ======================================================================
    // Payload Forwarding
    // ======================================================================

    void testPayloadForwarding() {
        sak::ElevatedTaskDispatcher dispatcher;
        dispatcher.registerHandler(
            "PayloadTask", [](const QJsonObject& payload, sak::ProgressCallback, sak::CancelCheck) {
                sak::TaskHandlerResult result;
                result.success = true;
                result.data = payload;
                return result;
            });

        QJsonObject input;
        input["drive"] = "D:";
        input["deep_scan"] = true;

        auto result = dispatcher.dispatch(
            "PayloadTask", input, [](int, const QString&) {}, [] { return false; });

        QVERIFY(result.has_value());
        QVERIFY(result->success);
        QVERIFY(result->error_message.isEmpty());
        // The payload reaches the handler UNCHANGED -- pin the whole object so an added,
        // renamed or dropped key cannot pass a per-key check.
        QCOMPARE(result->data, input);
    }
};

QTEST_GUILESS_MAIN(TestElevatedTaskDispatcher)
#include "test_elevated_task_dispatcher.moc"
