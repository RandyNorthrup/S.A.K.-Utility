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
    // Dispatch — Success
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
        QCOMPARE(result->data["result"].toString(), "fixed 3 items");
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
    }

    // ======================================================================
    // Dispatch — Unregistered Task
    // ======================================================================

    void testDispatchUnregisteredTask() {
        sak::ElevatedTaskDispatcher dispatcher;
        auto result = dispatcher.dispatch(
            "NeverRegistered", {}, [](int, const QString&) {}, [] { return false; });

        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::task_not_allowed);
    }

    // ======================================================================
    // Dispatch — Exception Safety
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
        QVERIFY(result->error_message.contains("unexpected error"));
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

        int last_percent = 0;
        QString last_status;
        int callback_count = 0;

        auto result = dispatcher.dispatch(
            "ProgressTask",
            {},
            [&](int pct, const QString& status) {
                last_percent = pct;
                last_status = status;
                ++callback_count;
            },
            [] { return false; });

        QVERIFY(result.has_value());
        QCOMPARE(callback_count, 3);
        QCOMPARE(last_percent, 100);
        QCOMPARE(last_status, "Done");
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

        // Normal run — not cancelled
        auto result1 = dispatcher.dispatch(
            "CancellableTask", {}, [](int, const QString&) {}, [] { return false; });
        QVERIFY(result1->success);

        // Cancelled run
        auto result2 = dispatcher.dispatch(
            "CancellableTask", {}, [](int, const QString&) {}, [] { return true; });
        QVERIFY(!result2->success);
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
        QCOMPARE(result->data["drive"].toString(), "D:");
        QCOMPARE(result->data["deep_scan"].toBool(), true);
    }
};

QTEST_GUILESS_MAIN(TestElevatedTaskDispatcher)
#include "test_elevated_task_dispatcher.moc"
