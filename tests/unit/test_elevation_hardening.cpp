// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_hardening.cpp
/// @brief Phase 5 hardening tests -- legacy removal verification, gate behavior,
///        cancellation, and elevated-mode bypass

#include "sak/elevated_pipe_protocol.h"
#include "sak/elevated_pipe_server.h"
#include "sak/elevated_task_dispatcher.h"
#include "sak/elevation_broker.h"
#include "sak/elevation_gate.h"
#include "sak/elevation_manager.h"
#include "sak/error_codes.h"
#include "sak/program_integrity.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

#include <string>

class TestElevationHardening : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void helperIsNotStartedFromAFolderThatFailsTheCheck();
    void runAsAdminIsRefusedForAFolderThatFailsTheCheck();
    // ====================================================================
    // 5.1 -- Legacy Removal Verification
    // ====================================================================
    void legacyElevationManager_delegatesToIsElevated();

    // ====================================================================
    // Gate function classification
    // ====================================================================
    void gate_enumContainsAllThreeResults();
    void gate_preflightCoversBothElevationStates();

    // ====================================================================
    // Dispatcher -- allowlist hardening
    // ====================================================================
    void dispatcher_rejectsEmptyTaskId();
    void dispatcher_rejectsUnregisteredTaskId();
    void dispatcher_multipleRegistrationsAllAllowed();

    // ====================================================================
    // Error codes -- all elevation error codes exist
    // ====================================================================
    void errorCodes_elevationRequiredExists();
    void errorCodes_elevationFailedExists();
    void errorCodes_elevationDeniedExists();
    void errorCodes_elevationTimeoutExists();
    void errorCodes_helperConnectionFailedExists();
    void errorCodes_helperCrashedExists();
    void errorCodes_taskNotAllowedExists();

    // ====================================================================
    // IPC protocol -- cancellation round-trip
    // ====================================================================
    void ipc_cancelRequestRoundTrip();
    void ipc_taskRequestPreservesPayload();

    // ====================================================================
    // Identity binding -- fail-closed client/server verification seams
    // ====================================================================
    void identity_clientPidFailsClosedOnMissingParent();
    void identity_imageChecksCompareTheFileNotTheSlashes();
    void identity_clientImageFailsClosedOnEmpty();
    void identity_clientImageMatchIsCaseInsensitive();
    void identity_clientImageRejectsDifferentExecutable();
    void identity_serverPidFailsClosedOnInvalidHelper();
    void identity_serverImageFailsClosedOnEmpty();
    void session_oneBrokerForTheWholeSession();
    void session_helperReusedOnlyWellInsideItsIdleTimeout();
};

// ============================================================================
// 5.1 -- Legacy Removal Verification
// ============================================================================

void TestElevationHardening::legacyElevationManager_delegatesToIsElevated() {
    // Verify the consolidated isElevated() returns consistently
    bool first = sak::ElevationManager::isElevated();
    bool second = sak::ElevationManager::isElevated();
    QCOMPARE(first, second);
}

// ============================================================================
// Tier 2 -- Elevated Features
// ============================================================================

// ============================================================================
// Gate function classification
// ============================================================================

void TestElevationHardening::gate_enumContainsAllThreeResults() {
    // Distinctness is guaranteed by the language for sequential enumerators, so `!=` checks can
    // never fail. Pin the underlying values instead: inserting or reordering a result changes these
    // and turns this red.
    QCOMPARE(static_cast<uint8_t>(sak::ElevationGateResult::AlreadyElevated), uint8_t{0});
    QCOMPARE(static_cast<uint8_t>(sak::ElevationGateResult::RestartRequested), uint8_t{1});
    QCOMPARE(static_cast<uint8_t>(sak::ElevationGateResult::Declined), uint8_t{2});
}

void TestElevationHardening::gate_preflightCoversBothElevationStates() {
    const auto elevated = sak::elevationGatePreflight(true);
    QVERIFY(elevated.has_value());
    QCOMPARE(*elevated, sak::ElevationGateResult::AlreadyElevated);
    QVERIFY(!sak::elevationGatePreflight(false).has_value());
}

// ============================================================================
// Dispatcher -- allowlist hardening
// ============================================================================

void TestElevationHardening::dispatcher_rejectsEmptyTaskId() {
    sak::ElevatedTaskDispatcher dispatcher;
    QVERIFY(!dispatcher.isAllowed(""));
}

void TestElevationHardening::dispatcher_rejectsUnregisteredTaskId() {
    sak::ElevatedTaskDispatcher dispatcher;
    dispatcher.registerHandler(
        "KnownTask",
        [](const QJsonObject&, sak::ProgressCallback, sak::CancelCheck) -> sak::TaskHandlerResult {
            return sak::TaskHandlerResult{true, QJsonObject{}, {}};
        });
    QVERIFY(!dispatcher.isAllowed("UnknownMaliciousTask"));
    QVERIFY(!dispatcher.isAllowed("KNOWNTASK"));  // case-sensitive
}

void TestElevationHardening::dispatcher_multipleRegistrationsAllAllowed() {
    sak::ElevatedTaskDispatcher dispatcher;
    const QStringList task_ids = {"TakeOwnership",
                                  "StripPermissions",
                                  "SetStandardPermissions",
                                  "BackupFile",
                                  "ReadThermalData"};
    for (const auto& id : task_ids) {
        dispatcher.registerHandler(id,
                                   [](const QJsonObject&,
                                      sak::ProgressCallback,
                                      sak::CancelCheck) -> sak::TaskHandlerResult {
                                       return sak::TaskHandlerResult{true, QJsonObject{}, {}};
                                   });
    }
    for (const auto& id : task_ids) {
        QVERIFY2(dispatcher.isAllowed(id),
                 qPrintable(QString("Task '%1' should be allowed").arg(id)));
    }
    // The allowlist is EXACTLY these five: the loop proves none is missing, the count proves
    // no sixth task was registered alongside them.
    QCOMPARE(dispatcher.handlerCount(), 5);
    QVERIFY(!dispatcher.isAllowed("NotRegistered"));
}

// ============================================================================
// Error codes -- all elevation error codes exist
// ============================================================================

void TestElevationHardening::errorCodes_elevationRequiredExists() {
    auto code = sak::error_code::elevation_required;
    // `!empty()` is vacuous: to_string() returns the non-empty "Undefined error" fallback for an
    // unmapped code, so a deleted/wrong mapping stays green. Pin the exact message.
    QCOMPARE(std::string(sak::to_string(code)), std::string("Elevation required"));
}

void TestElevationHardening::errorCodes_elevationFailedExists() {
    auto code = sak::error_code::elevation_failed;
    QCOMPARE(std::string(sak::to_string(code)), std::string("Elevation failed"));
}

void TestElevationHardening::errorCodes_elevationDeniedExists() {
    auto code = sak::error_code::elevation_denied;
    QCOMPARE(std::string(sak::to_string(code)), std::string("User cancelled elevation request"));
}

void TestElevationHardening::errorCodes_elevationTimeoutExists() {
    auto code = sak::error_code::elevation_timeout;
    QCOMPARE(std::string(sak::to_string(code)),
             std::string("Elevated helper did not respond in time"));
}

void TestElevationHardening::errorCodes_helperConnectionFailedExists() {
    auto code = sak::error_code::helper_connection_failed;
    QCOMPARE(std::string(sak::to_string(code)),
             std::string("Could not connect to elevated helper"));
}

void TestElevationHardening::errorCodes_helperCrashedExists() {
    auto code = sak::error_code::helper_crashed;
    QCOMPARE(std::string(sak::to_string(code)), std::string("Elevated helper exited unexpectedly"));
}

void TestElevationHardening::errorCodes_taskNotAllowedExists() {
    auto code = sak::error_code::task_not_allowed;
    QCOMPARE(std::string(sak::to_string(code)),
             std::string("Task not in elevated helper allowlist"));
}

// ============================================================================
// IPC protocol -- cancellation round-trip
// ============================================================================

void TestElevationHardening::ipc_cancelRequestRoundTrip() {
    QByteArray frame = sak::buildCancelRequest("TestTask");
    // A 5-byte header plus the compact JSON {"task":"TestTask"} (19 bytes); the header's
    // declared length must agree with what was actually appended.
    QCOMPARE(frame.size(), sak::kPipeHeaderSize + 19);
    const auto decoded = sak::ElevatedPipeServer::decodeFrameHeader(frame.constData());
    QCOMPARE(decoded.payload_len, 19u);
    QCOMPARE(decoded.type, sak::PipeMessageType::CancelRequest);

    auto type =
        static_cast<sak::PipeMessageType>(static_cast<uint8_t>(frame.at(sak::kPipeHeaderSize - 1)));
    QCOMPARE(type, sak::PipeMessageType::CancelRequest);

    QByteArray payload_bytes = frame.mid(sak::kPipeHeaderSize);
    QJsonDocument doc = QJsonDocument::fromJson(payload_bytes);
    QVERIFY(doc.isObject());
    // The message carries the task name and NOTHING else.
    const QJsonObject expected{{QStringLiteral("task"), QStringLiteral("TestTask")}};
    QCOMPARE(doc.object(), expected);
}

void TestElevationHardening::ipc_taskRequestPreservesPayload() {
    QJsonObject payload;
    payload["path"] = QString::fromUtf8("C:\\Windows\\System32\\test.dll");
    payload["recursive"] = true;

    QByteArray frame = sak::buildTaskRequest("TakeOwnership", payload);
    const auto decoded = sak::ElevatedPipeServer::decodeFrameHeader(frame.constData());
    QCOMPARE(decoded.type, sak::PipeMessageType::TaskRequest);
    QCOMPARE(static_cast<int>(decoded.payload_len), frame.size() - sak::kPipeHeaderSize);

    QByteArray payload_bytes = frame.mid(sak::kPipeHeaderSize);
    QJsonDocument doc = QJsonDocument::fromJson(payload_bytes);
    QVERIFY(doc.isObject());

    // The request is exactly {task, payload} with the payload forwarded verbatim -- pin the
    // whole object so an added field, or a payload key silently dropped, cannot pass.
    QJsonObject expected;
    expected[QStringLiteral("task")] = QStringLiteral("TakeOwnership");
    expected[QStringLiteral("payload")] = payload;
    QCOMPARE(doc.object(), expected);
}

// ============================================================================
// Identity binding -- fail-closed client/server verification seams
// ============================================================================

void TestElevationHardening::identity_clientPidFailsClosedOnMissingParent() {
    // A missing/invalid expected parent PID must authorize NOBODY, even if the
    // client PID happens to be the same non-positive value.
    QVERIFY(!sak::ElevatedPipeServer::clientPidMatchesParent(0, 0));
    QVERIFY(!sak::ElevatedPipeServer::clientPidMatchesParent(-1, -1));
    QVERIFY(!sak::ElevatedPipeServer::clientPidMatchesParent(0, 1234));
    QVERIFY(sak::ElevatedPipeServer::clientPidMatchesParent(1234, 1234));
    QVERIFY(!sak::ElevatedPipeServer::clientPidMatchesParent(1234, 5678));
}

// One side of each check comes from Qt with forward slashes and the other from Windows with
// backslashes. Compared as they stood, they matched nothing, so from an unelevated app every
// administrator task failed with a helper connection error, and only a run in the VM showed it.
void TestElevationHardening::identity_imageChecksCompareTheFileNotTheSlashes() {
    const QString from_qt = QStringLiteral("C:/SAK/sak_elevated_helper.exe");
    const QString from_windows = QStringLiteral("C:\\SAK\\sak_elevated_helper.exe");
    QVERIFY(sak::ElevationBroker::imagePathsMatch(from_qt, from_windows));
    QVERIFY(sak::ElevatedPipeServer::clientImageMatchesExpected(from_qt, from_windows));
    QVERIFY(sak::ElevationBroker::imagePathsMatch(from_windows, from_qt));
    QVERIFY(sak::ElevatedPipeServer::clientImageMatchesExpected(from_windows, from_qt));
    // Still a different file in a different folder, however it is spelled.
    const QString elsewhere = QStringLiteral("C:\\Temp\\sak_elevated_helper.exe");
    QVERIFY(!sak::ElevationBroker::imagePathsMatch(from_qt, elsewhere));
    QVERIFY(!sak::ElevatedPipeServer::clientImageMatchesExpected(from_qt, elsewhere));
    // The real path Qt gives for this program, against the one Windows reports for it.
    wchar_t module[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    QVERIFY(length > 0 && length < MAX_PATH);
    const QString reported = QString::fromWCharArray(module, static_cast<int>(length));
    QVERIFY2(sak::ElevationBroker::imagePathsMatch(QCoreApplication::applicationFilePath(),
                                                   reported),
             qPrintable(reported));
}

void TestElevationHardening::identity_clientImageFailsClosedOnEmpty() {
    // An unresolvable expected OR client image path must match nothing.
    QVERIFY(!sak::ElevatedPipeServer::clientImageMatchesExpected(QString(), QString()));
    QVERIFY(!sak::ElevatedPipeServer::clientImageMatchesExpected(
        QStringLiteral("C:\\App\\sak_utility.exe"), QString()));
    QVERIFY(!sak::ElevatedPipeServer::clientImageMatchesExpected(
        QString(), QStringLiteral("C:\\App\\sak_utility.exe")));
}

void TestElevationHardening::identity_clientImageMatchIsCaseInsensitive() {
    QVERIFY(sak::ElevatedPipeServer::clientImageMatchesExpected(
        QStringLiteral("C:\\App\\sak_utility.exe"), QStringLiteral("c:\\app\\SAK_UTILITY.EXE")));
}

void TestElevationHardening::identity_clientImageRejectsDifferentExecutable() {
    QVERIFY(!sak::ElevatedPipeServer::clientImageMatchesExpected(
        QStringLiteral("C:\\App\\sak_utility.exe"), QStringLiteral("C:\\Temp\\evil.exe")));
}

void TestElevationHardening::identity_serverPidFailsClosedOnInvalidHelper() {
    // An unknown launched-helper PID (<=0) trusts nobody; a real match is required.
    QVERIFY(!sak::ElevationBroker::serverPidMatchesHelper(0, 0));
    QVERIFY(!sak::ElevationBroker::serverPidMatchesHelper(-1, -1));
    QVERIFY(!sak::ElevationBroker::serverPidMatchesHelper(0, 4321));
    QVERIFY(sak::ElevationBroker::serverPidMatchesHelper(4321, 4321));
    QVERIFY(!sak::ElevationBroker::serverPidMatchesHelper(4321, 9999));
}

void TestElevationHardening::identity_serverImageFailsClosedOnEmpty() {
    QVERIFY(!sak::ElevationBroker::imagePathsMatch(QString(), QString()));
    QVERIFY(!sak::ElevationBroker::imagePathsMatch(
        QStringLiteral("C:\\App\\sak_elevated_helper.exe"), QString()));
    QVERIFY(
        sak::ElevationBroker::imagePathsMatch(QStringLiteral("C:\\App\\sak_elevated_helper.exe"),
                                              QStringLiteral("c:\\app\\SAK_ELEVATED_HELPER.EXE")));
    QVERIFY(!sak::ElevationBroker::imagePathsMatch(
        QStringLiteral("C:\\App\\sak_elevated_helper.exe"), QStringLiteral("C:\\App\\other.exe")));
}

// Partition Manager's scans and operations each built their own broker, so every scan and every
// operation launched its own helper behind its own UAC prompt. They now share one per session,
// whichever thread asks first.
void TestElevationHardening::session_oneBrokerForTheWholeSession() {
    const auto first = sak::ElevationBroker::sessionBroker();
    QVERIFY(first);
    std::shared_ptr<sak::ElevationBroker> from_worker;
    QThread* worker =
        QThread::create([&from_worker]() { from_worker = sak::ElevationBroker::sessionBroker(); });
    worker->start();
    QVERIFY(worker->wait(sak::kPipeConnectTimeoutMs));
    delete worker;
    QCOMPARE(from_worker.get(), first.get());
    QCOMPARE(sak::ElevationBroker::sessionBroker().get(), first.get());
    // It belongs to the application thread, which tears it down at exit.
    QCOMPARE(first->thread(), QCoreApplication::instance()->thread());
}

// A shared helper is reused only while it is alive and well inside its idle timeout. One close
// to timing out could exit with the next (possibly destructive) request unread, so it is
// retired and a new helper started instead.
void TestElevationHardening::session_helperReusedOnlyWellInsideItsIdleTimeout() {
    QVERIFY(sak::ElevationBroker::helperReusable(true, 0));
    QVERIFY(sak::ElevationBroker::helperReusable(true, sak::kHelperTimeoutMs / 2));
    QVERIFY(!sak::ElevationBroker::helperReusable(false, 0));
    QVERIFY(!sak::ElevationBroker::helperReusable(true, sak::kHelperTimeoutMs));
    QVERIFY(!sak::ElevationBroker::helperReusable(true, sak::kHelperTimeoutMs - 1));
    QVERIFY(!sak::ElevationBroker::helperReusable(true, -1));
    // The boundary sits a fixed margin before the helper's own timeout.
    qint64 last_reusable = -1;
    for (qint64 idle = 0; idle <= sak::kHelperTimeoutMs; idle += 1000) {
        if (sak::ElevationBroker::helperReusable(true, idle)) {
            last_reusable = idle;
        }
    }
    QVERIFY(last_reusable > 0);
    QVERIFY(last_reusable < sak::kHelperTimeoutMs - 1000);
}

// Every program that can ask for elevation records its own file first, as the app does.
void TestElevationHardening::initTestCase() {
    sak::program_integrity::captureRunningImageIdentity();
}

// Security brief 4c/4d: the program files are checked before anything asks for elevation. A test
// runs from the build's output folder, which holds hundreds of test programs that are not part
// of S.A.K., so the check refuses there: no UAC request is made and the refusal is its own error.
void TestElevationHardening::helperIsNotStartedFromAFolderThatFailsTheCheck() {
    QString shown;
    sak::program_integrity::setRefusalObserver(
        [&shown](const QString& message) { shown = message; });
    sak::ElevationBroker broker;
    const auto result = broker.executeTask(QStringLiteral("ReadThermalData"),
                                           QStringLiteral("Integrity check test"),
                                           QJsonObject{});
    sak::program_integrity::setRefusalObserver({});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::untrusted_program_file);
    QVERIFY2(shown.contains(QStringLiteral("isn't part of S.A.K.")), qPrintable(shown));
}

void TestElevationHardening::runAsAdminIsRefusedForAFolderThatFailsTheCheck() {
    sak::program_integrity::setRefusalObserver([](const QString&) {});
    const auto result = sak::ElevationManager::restartElevated();
    sak::program_integrity::setRefusalObserver({});
    if (sak::ElevationManager::isElevated()) {
        QVERIFY(result.has_value());  // already elevated: nothing to restart, nothing checked
        return;
    }
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::untrusted_program_file);
    QVERIFY(!sak::ElevationManager::restartFailureNeedsNotice(result.error()));
}

QTEST_MAIN(TestElevationHardening)
#include "test_elevation_hardening.moc"
