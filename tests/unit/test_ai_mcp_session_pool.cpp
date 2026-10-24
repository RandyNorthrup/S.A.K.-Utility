// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_session_pool.h"
#include "sak/contained_process_quarantine.h"
#include "sak/layout_constants.h"
#include "sak/windows_process_job.h"

#include "../support/ai_mcp_native_fixture.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <concepts>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

namespace native_mcp = sak::testfixtures::mcp;
using native_mcp::failureDiagnostic;
using native_mcp::fakeProcessId;

constexpr auto kFakeMcpModeEnv = "SAK_TEST_MCP_POOL_SERVER_MODE";
constexpr auto kFakeMcpMarkerEnv = "SAK_TEST_MCP_POOL_SERVER_MARKER";
constexpr int kFakeTransportErrorCode = -32'000;
constexpr int kFakeMissingMarkerExitCode = 2;
constexpr int kFakeMarkerCreateExitCode = 3;
constexpr int kFakeProcessLifetimeMs = 10'000;
constexpr UINT kFakeProcessDeadlineExit = 93;
constexpr int kContendedCleanupDeadlineMs = 30;
constexpr int kContendedLockHoldMs = 500;
constexpr int kLockAcquisitionTimeoutMs = 1000;
constexpr auto kLockHolderReadyTimeout = std::chrono::seconds(2);

struct FakeServerBehavior {
    bool reject_tool_calls{false};
    bool reject_list_tools{false};
};

struct PoolCleanupObservation {
    std::atomic_bool fail_termination{false};
    // Hung child: TerminateJobObject reports success but nothing dies, so the primary wait
    // runs out its deadline exactly as a child that outlives cleanup under machine load does.
    std::atomic_bool ignore_termination{false};
    std::atomic_int termination_attempts{0};
    std::atomic_int job_handle_closes{0};
    std::atomic<HANDLE> job{nullptr};  // The most recently created Job, for membership checks.
    std::atomic_int controllers_created{0};
    std::atomic_int controllers_finished{0};
    std::atomic_int controllers_destroyed{0};
};

void writeFakeResponse(QTextStream& output, int id, const QJsonObject& result) {
    const QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("result"), result}};
    output << QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)) << '\n';
    output.flush();
}

void writeFakeError(QTextStream& output, int id, const QString& message) {
    const QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("error"),
                                QJsonObject{{QStringLiteral("code"), kFakeTransportErrorCode},
                                            {QStringLiteral("message"), message}}}};
    output << QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact)) << '\n';
    output.flush();
}

std::expected<FakeServerBehavior, int> fakeServerBehavior(const QString& mode) {
    if (mode == QLatin1String("error-on-first-process") ||
        mode == QLatin1String("list-error-on-first-process")) {
        const QString marker_path = qEnvironmentVariable(kFakeMcpMarkerEnv);
        if (marker_path.isEmpty()) {
            return std::unexpected(kFakeMissingMarkerExitCode);
        }
        if (!QFileInfo::exists(marker_path)) {
            QFile marker(marker_path);
            if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                return std::unexpected(kFakeMarkerCreateExitCode);
            }
            return FakeServerBehavior{
                .reject_tool_calls = mode == QLatin1String("error-on-first-process"),
                .reject_list_tools = mode == QLatin1String("list-error-on-first-process")};
        }
    }
    return FakeServerBehavior{};
}

void handleFakeRequest(QTextStream& output,
                       const QJsonObject& request,
                       const FakeServerBehavior& behavior) {
    const int id = request.value(QStringLiteral("id")).toInt();
    const QString method = request.value(QStringLiteral("method")).toString();
    if (method == QLatin1String("initialize")) {
        writeFakeResponse(output,
                          id,
                          QJsonObject{
                              {QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")}});
        return;
    }
    if (method == QLatin1String("tools/call")) {
        if (request.value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("name"))
                .toString() == QLatin1String("process-id")) {
            const QJsonObject block{{QStringLiteral("type"), QStringLiteral("text")},
                                    {QStringLiteral("text"),
                                     QString::number(QCoreApplication::applicationPid())}};
            writeFakeResponse(output,
                              id,
                              QJsonObject{{QStringLiteral("content"), QJsonArray{block}},
                                          {QStringLiteral("isError"), false}});
            return;
        }
        if (behavior.reject_tool_calls) {
            writeFakeError(output, id, QStringLiteral("forced transport error"));
            return;
        }
        writeFakeResponse(output, id, QJsonObject{{QStringLiteral("content"), QJsonArray{}}});
        return;
    }
    if (method != QLatin1String("tools/list")) {
        return;
    }
    if (behavior.reject_list_tools) {
        writeFakeError(output, id, QStringLiteral("forced list transport error"));
        return;
    }
    writeFakeResponse(output, id, QJsonObject{{QStringLiteral("tools"), QJsonArray{}}});
}

int runFakeMcpServer(const QString& mode) {
    const auto behavior = fakeServerBehavior(mode);
    if (!behavior) {
        return behavior.error();
    }
    QTextStream input(stdin, QIODevice::ReadOnly);
    QTextStream output(stdout, QIODevice::WriteOnly);
    for (;;) {
        const QString line = input.readLine();
        if (line.isNull()) {
            return 0;
        }
        handleFakeRequest(output, QJsonDocument::fromJson(line.toUtf8()).object(), *behavior);
    }
}

template <typename Pool>
concept UsesTypedResults = requires(Pool& pool, const sak::ai::AiMcpStdioCallRequest& request) {
    { pool.callTool(request) } -> std::same_as<std::expected<QJsonObject, QString>>;
    {
        pool.listTools(request)
    } -> std::same_as<std::expected<QVector<sak::ai::AiMcpToolDescriptor>, QString>>;
};

template <typename Pool>
concept CanCallToolWithLegacyReferenceSink =
    requires(Pool& pool, const sak::ai::AiMcpStdioCallRequest& request, QString& error_message) {
        pool.callTool(request, error_message);
    };

template <typename Pool>
concept CanCallToolWithLegacyPointerSink =
    requires(Pool& pool, const sak::ai::AiMcpStdioCallRequest& request, QString* error_message) {
        pool.callTool(request, error_message);
    };

template <typename Pool>
concept CanListToolsWithLegacyReferenceSink =
    requires(Pool& pool, const sak::ai::AiMcpStdioCallRequest& request, QString& error_message) {
        pool.listTools(request, error_message);
    };

template <typename Pool>
concept CanListToolsWithLegacyPointerSink =
    requires(Pool& pool, const sak::ai::AiMcpStdioCallRequest& request, QString* error_message) {
        pool.listTools(request, error_message);
    };

std::expected<qint64, QString> callHealth(sak::ai::AiMcpSessionPool* pool,
                                          const sak::ai::AiMcpStdioCallRequest& request) {
    const auto message = pool->callTool(request);
    if (!message) {
        return std::unexpected(message.error());
    }
    return native_mcp::healthPid(message->value(QStringLiteral("result")).toObject());
}

sak::ai::AiMcpStdioCallRequest builtInPoolRequest(const QString& command,
                                                  const QString& local_app_data,
                                                  native_mcp::SecurityProfile profile) {
    return {.command = command,
            .tool_name = QStringLiteral("health_check"),
            .arguments = QJsonObject{},
            .environment = native_mcp::environment(local_app_data, profile),
            .timeout_ms = native_mcp::kRequestTimeoutMs};
}

std::expected<void, QString> validatePoolRequestsBeforeLaunch(
    const sak::ai::AiMcpStdioCallRequest& read_only_request,
    const QString& read_only_app_data,
    const sak::ai::AiMcpStdioCallRequest& full_request,
    const QString& full_app_data) {
    const auto read_only = native_mcp::validateBeforeLaunch(read_only_request.command,
                                                            read_only_request.environment,
                                                            read_only_app_data,
                                                            native_mcp::SecurityProfile::ReadOnly,
                                                            read_only_request.tool_name);
    if (!read_only) {
        return std::unexpected(read_only.error());
    }
    return native_mcp::validateBeforeLaunch(full_request.command,
                                            full_request.environment,
                                            full_app_data,
                                            native_mcp::SecurityProfile::Full,
                                            full_request.tool_name);
}

struct HealthProcessObservation {
    qint64 pid;
    native_mcp::ProcessGeneration generation;
};

std::expected<HealthProcessObservation, QString> observeHealthProcess(
    sak::ai::AiMcpSessionPool* pool, const sak::ai::AiMcpStdioCallRequest& request) {
    const auto pid = callHealth(pool, request);
    if (!pid) {
        return std::unexpected(pid.error());
    }
    auto generation = native_mcp::ProcessGeneration::observe(*pid);
    if (!generation) {
        return std::unexpected(generation.error());
    }
    const auto alive = generation->isAlive();
    if (!alive) {
        return std::unexpected(alive.error());
    }
    if (!*alive) {
        return std::unexpected(QStringLiteral("Observed health process already exited"));
    }
    return HealthProcessObservation{*pid, std::move(*generation)};
}

sak::ai::AiMcpStdioCallRequest fakePoolRequest(const QString& mode, const QString& marker_path) {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kFakeMcpModeEnv), mode);
    environment.insert(QString::fromLatin1(kFakeMcpMarkerEnv), marker_path);
    return {.command = QCoreApplication::applicationFilePath(),
            .tool_name = QStringLiteral("health_check"),
            .arguments = {},
            .environment = environment,
            .timeout_ms = native_mcp::kRequestTimeoutMs};
}

std::expected<qint64, QString> fakeProcessPid(sak::ai::AiMcpSessionPool& pool,
                                              sak::ai::AiMcpStdioCallRequest request) {
    request.tool_name = QStringLiteral("process-id");
    const auto result = pool.callTool(request);
    if (!result) {
        return std::unexpected(result.error());
    }
    const auto payload = result->value(QStringLiteral("result"));
    if (!payload.isObject()) {
        return std::unexpected(QStringLiteral("Fake MCP process-id payload is not an object"));
    }
    return fakeProcessId(payload.toObject());
}

std::expected<native_mcp::ProcessGeneration, QString> observeFakeProcess(
    sak::ai::AiMcpSessionPool& pool, sak::ai::AiMcpStdioCallRequest request) {
    const auto pid = fakeProcessPid(pool, std::move(request));
    if (!pid) {
        return std::unexpected(pid.error());
    }
    return native_mcp::ProcessGeneration::observe(*pid);
}

}  // namespace

class AiMcpSessionPoolTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void keyIsolatesCommandAndEnvironment();
    void keyIsolatesTimeout();
    void keyNormalizesSubminimumTimeout();
    void resultsUseExpectedWithoutLegacyErrorSink();
    void emptyCommandRejected();
    void emptyToolRejectedBeforePoolUse();
    void inheritedEnvironmentRejectedBeforePoolUse();
    void emptySuccessIsDistinctFromFailure();
    void transportFailureDropsSessionBeforeRetry();
    void listFailureDropsSessionBeforeRetry();
    void missingCommandFailsCleanlyAndCloses();
    void builtInMcpReuseAndPolicyIsolation();
    void cleanupFailurePreservesSession_data();
    void cleanupFailurePreservesSession();
    void partialCloseAllRetainsFailedEntry();
    void closeAllHonorsContendedLockDeadline_data();
    void closeAllHonorsContendedLockDeadline();
    void destructionQuarantinesUnprovenCleanup_data();
    void destructionQuarantinesUnprovenCleanup();

private:
    static std::unique_ptr<sak::ai::AiMcpStdioSession> observedSession(
        const std::shared_ptr<PoolCleanupObservation>& observed);
    static void verifyUnprovenCloseAll(sak::ai::AiMcpSessionPool& pool,
                                       PoolCleanupObservation& observed,
                                       bool hung_child);
    static void verifyBlockedAdmissions(sak::ai::AiMcpSessionPool& pool,
                                        const sak::ai::AiMcpStdioCallRequest& request);
    static sak::ai::AiMcpStdioCallRequest requestWith(const QString& command,
                                                      const QString& security_profile) {
        QProcessEnvironment env;
        env.insert(QStringLiteral("WIN32_MCP_SECURITY_PROFILE"), security_profile);
        return {.command = command,
                .tool_name = QStringLiteral("health_check"),
                .arguments = QJsonObject{},
                .environment = env,
                .timeout_ms = 4000};
    }
};

std::unique_ptr<sak::ai::AiMcpStdioSession> AiMcpSessionPoolTests::observedSession(
    const std::shared_ptr<PoolCleanupObservation>& observed) {
    auto session = std::make_unique<sak::ai::AiMcpStdioSession>();
    session->m_job_factory = [observed]() {
        auto api = sak::WindowsProcessJob::nativeApi();
        const auto native_terminate = api.terminate_job;
        api.terminate_job = [observed, native_terminate](HANDLE job, UINT exit_code) {
            observed->termination_attempts.fetch_add(1);
            if (observed->fail_termination.load()) {
                SetLastError(ERROR_ACCESS_DENIED);
                return FALSE;
            }
            if (observed->ignore_termination.load()) {
                return TRUE;
            }
            return native_terminate(job, exit_code);
        };
        const auto native_create = api.create_job;
        api.create_job = [observed, native_create](LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) {
            const HANDLE job = native_create(attributes, name);
            observed->job.store(job);
            return job;
        };
        const auto native_close = api.close_handle;
        api.close_handle = [observed, native_close](HANDLE handle) {
            observed->job_handle_closes.fetch_add(1);
            return native_close(handle);
        };
        return sak::WindowsProcessJob::createWithApi(std::move(api));
    };
    session->setThreadStarterForTesting([observed](QThread& controller) {
        observed->controllers_created.fetch_add(1);
        QObject::connect(
            &controller,
            &QThread::finished,
            &controller,
            [observed]() { observed->controllers_finished.fetch_add(1); },
            Qt::DirectConnection);
        QObject::connect(
            &controller,
            &QObject::destroyed,
            &controller,
            [observed]() { observed->controllers_destroyed.fetch_add(1); },
            Qt::DirectConnection);
        controller.start();
    });
    return session;
}

void AiMcpSessionPoolTests::verifyBlockedAdmissions(sak::ai::AiMcpSessionPool& pool,
                                                    const sak::ai::AiMcpStdioCallRequest& request) {
    auto different_key = request;
    different_key.environment.insert(QStringLiteral("SAK_TEST_POOL_DISTINCT_KEY"),
                                     QStringLiteral("different"));
    const auto repeated_call = pool.callTool(request);
    const auto repeated_list = pool.listTools(request);
    const auto different_call = pool.callTool(different_key);
    const auto different_list = pool.listTools(different_key);
    QVERIFY(!repeated_call);
    QVERIFY(!repeated_list);
    QVERIFY(!different_call);
    QVERIFY(!different_list);
    const QString refusal = QStringLiteral("MCP pool cleanup is unresolved; refusing reuse");
    QCOMPARE(repeated_call.error(), refusal);
    QCOMPARE(repeated_list.error(), refusal);
    QCOMPARE(different_call.error(), refusal);
    QCOMPARE(different_list.error(), refusal);
}

void AiMcpSessionPoolTests::resultsUseExpectedWithoutLegacyErrorSink() {
    using Pool = sak::ai::AiMcpSessionPool;
    QVERIFY(UsesTypedResults<Pool>);
    QVERIFY(!CanCallToolWithLegacyReferenceSink<Pool>);
    QVERIFY(!CanCallToolWithLegacyPointerSink<Pool>);
    QVERIFY(!CanListToolsWithLegacyReferenceSink<Pool>);
    QVERIFY(!CanListToolsWithLegacyPointerSink<Pool>);
}

void AiMcpSessionPoolTests::keyIsolatesCommandAndEnvironment() {
    using Pool = sak::ai::AiMcpSessionPool;
    const auto read_only = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    const auto read_only2 = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    auto full = read_only;
    full.environment.remove(QString::fromLatin1(native_mcp::kSecurityProfileEnvironment));
    const auto other_cmd = requestWith(QStringLiteral("other.exe"), QStringLiteral("read_only"));

    // Same command + same environment -> same session.
    QCOMPARE(Pool::sessionKeyForTesting(read_only), Pool::sessionKeyForTesting(read_only2));
    // Full policy is the ABSENCE of WIN32_MCP_SECURITY_PROFILE. This pure key check changes only
    // that profile field, so different integration-test LOCALAPPDATA roots cannot mask a profile
    // hashing regression.
    QVERIFY(
        !full.environment.contains(QString::fromLatin1(native_mcp::kSecurityProfileEnvironment)));
    QVERIFY(Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(full));
    // A different command -> different session.
    QVERIFY(Pool::sessionKeyForTesting(read_only) != Pool::sessionKeyForTesting(other_cmd));

    // INHERIT-FROM-PARENT IS NOT THE SAME AS EMPTY, and the key could not tell them apart:
    // QProcessEnvironment::toStringList() reports an EMPTY list for an inheriting environment,
    // exactly like one that was deliberately emptied. One hands the server every variable this
    // process holds -- including whatever secrets are in it -- and the other hands it none, so a
    // session opened with the full user environment could have been reused for a call that asked
    // for a bare one. Nothing constructs an inheriting environment today, which is why this never
    // fired; the key is the isolation guarantee, so it must hold before something does.
    auto inheriting = read_only;
    inheriting.environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);
    auto emptied = read_only;
    emptied.environment = QProcessEnvironment();

    QVERIFY(inheriting.environment.toStringList().isEmpty());
    QVERIFY(emptied.environment.toStringList().isEmpty());
    QVERIFY(inheriting.environment.inheritsFromParent());
    QVERIFY(!emptied.environment.inheritsFromParent());
    // Same command, same timeout, both env lists empty -- and still distinct keys.
    QVERIFY(Pool::sessionKeyForTesting(inheriting) != Pool::sessionKeyForTesting(emptied));
    // Each still keys stably to itself, so this is not just "always different".
    QCOMPARE(Pool::sessionKeyForTesting(inheriting), Pool::sessionKeyForTesting(inheriting));
    QCOMPARE(Pool::sessionKeyForTesting(emptied), Pool::sessionKeyForTesting(emptied));
}

void AiMcpSessionPoolTests::keyIsolatesTimeout() {
    using Pool = sak::ai::AiMcpSessionPool;
    const auto base = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    auto longer = base;
    longer.timeout_ms = base.timeout_ms + 1000;

    // Same command + env but a different per-call timeout MUST key to a distinct
    // session, so a later call never inherits the first call's stale timeout.
    QVERIFY(Pool::sessionKeyForTesting(base) != Pool::sessionKeyForTesting(longer));
    // Identical timeout -> same session (reuse still works).
    const auto same = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    QCOMPARE(Pool::sessionKeyForTesting(base), Pool::sessionKeyForTesting(same));
}

void AiMcpSessionPoolTests::keyNormalizesSubminimumTimeout() {
    using Pool = sak::ai::AiMcpSessionPool;
    auto zero = requestWith(QStringLiteral("srv.exe"), QStringLiteral("read_only"));
    zero.timeout_ms = 0;
    auto below_minimum = zero;
    below_minimum.timeout_ms = sak::kMillisecondsPerSecond - 1;
    auto at_minimum = zero;
    at_minimum.timeout_ms = sak::kMillisecondsPerSecond;
    auto above_minimum = zero;
    above_minimum.timeout_ms = sak::kMillisecondsPerSecond + 1;

    QCOMPARE(sak::ai::AiMcpStdioSession::effectiveRequestTimeoutMs(zero.timeout_ms),
             sak::kMillisecondsPerSecond);
    QCOMPARE(Pool::sessionKeyForTesting(zero), Pool::sessionKeyForTesting(below_minimum));
    QCOMPARE(Pool::sessionKeyForTesting(zero), Pool::sessionKeyForTesting(at_minimum));
    QVERIFY(Pool::sessionKeyForTesting(zero) != Pool::sessionKeyForTesting(above_minimum));
}

void AiMcpSessionPoolTests::emptyCommandRejected() {
    sak::ai::AiMcpSessionPool pool;
    sak::ai::AiMcpStdioCallRequest request;
    request.tool_name = QStringLiteral("list_windows");
    const auto result = pool.callTool(request);
    QVERIFY(!result);
    // Fail closed with the exact empty-command message.
    QCOMPARE(result.error(), QStringLiteral("MCP stdio command is empty"));
    QCOMPARE(pool.pooledSlotCount(), 0);  // nothing was cached
}

void AiMcpSessionPoolTests::emptyToolRejectedBeforePoolUse() {
    sak::ai::AiMcpSessionPool pool;
    auto request = requestWith(QStringLiteral("missing-server.exe"), QStringLiteral("read_only"));
    request.tool_name = QStringLiteral("   ");

    const auto result = pool.callTool(request);
    QVERIFY(!result);
    QCOMPARE(result.error(), QStringLiteral("MCP tool name is empty"));
    QCOMPARE(pool.pooledSlotCount(), 0);
}

void AiMcpSessionPoolTests::inheritedEnvironmentRejectedBeforePoolUse() {
    sak::ai::AiMcpSessionPool pool;
    auto request = requestWith(QStringLiteral("missing-server.exe"), QStringLiteral("read_only"));
    request.environment = QProcessEnvironment(QProcessEnvironment::InheritFromParent);

    const auto result = pool.callTool(request);
    QVERIFY(!result);
    QCOMPARE(result.error(),
             QStringLiteral("MCP session pool requires an explicit process environment"));
    QCOMPARE(pool.pooledSlotCount(), 0);
}

void AiMcpSessionPoolTests::emptySuccessIsDistinctFromFailure() {
    sak::ai::AiMcpSessionPool pool;
    auto request = requestWith(QCoreApplication::applicationFilePath(),
                               QStringLiteral("read_only"));
    request.environment = QProcessEnvironment::systemEnvironment();
    request.environment.insert(QString::fromLatin1(kFakeMcpModeEnv),
                               QStringLiteral("empty-success"));

    const auto call_result = pool.callTool(request);
    const QString call_error = call_result ? QString{} : call_result.error();
    QVERIFY2(call_result.has_value(), qPrintable(call_error));
    QVERIFY(call_result->contains(QStringLiteral("result")));
    QVERIFY(call_result->value(QStringLiteral("result")).isObject());
    const QJsonObject result = call_result->value(QStringLiteral("result")).toObject();
    QVERIFY(result.value(QStringLiteral("content")).isArray());
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    QVERIFY(content.isEmpty());

    const auto tools_result = pool.listTools(request);
    const QString tools_error = tools_result ? QString{} : tools_result.error();
    QVERIFY2(tools_result.has_value(), qPrintable(tools_error));
    QVERIFY(tools_result->isEmpty());
    QCOMPARE(pool.pooledSlotCount(), 1);
}

void AiMcpSessionPoolTests::transportFailureDropsSessionBeforeRetry() {
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const QString marker_path = temporary_dir.filePath(QStringLiteral("first-process.marker"));

    sak::ai::AiMcpSessionPool pool;
    auto request = requestWith(QCoreApplication::applicationFilePath(),
                               QStringLiteral("read_only"));
    request.environment = QProcessEnvironment::systemEnvironment();
    request.environment.insert(QString::fromLatin1(kFakeMcpModeEnv),
                               QStringLiteral("error-on-first-process"));
    request.environment.insert(QString::fromLatin1(kFakeMcpMarkerEnv), marker_path);

    const auto first_result = pool.callTool(request);
    QVERIFY(!first_result);
    QCOMPARE(first_result.error(), QStringLiteral("forced transport error"));
    QVERIFY(QFileInfo::exists(marker_path));
    QCOMPARE(pool.pooledSlotCount(), 1);

    // Same live process would return the forced error again. Success proves the failed session was
    // dropped and this retry opened a fresh process, which observes the marker left by the first.
    const auto retry_result = pool.callTool(request);
    const QString retry_error = retry_result ? QString{} : retry_result.error();
    QVERIFY2(retry_result.has_value(), qPrintable(retry_error));
    const QJsonObject result = retry_result->value(QStringLiteral("result")).toObject();
    QVERIFY(result.value(QStringLiteral("content")).isArray());
    QVERIFY(result.value(QStringLiteral("content")).toArray().isEmpty());
    QCOMPARE(pool.pooledSlotCount(), 1);
    QVERIFY(pool.closeAll().has_value());
}

void AiMcpSessionPoolTests::listFailureDropsSessionBeforeRetry() {
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const QString marker_path = temporary_dir.filePath(QStringLiteral("first-process.marker"));

    sak::ai::AiMcpSessionPool pool;
    auto request = requestWith(QCoreApplication::applicationFilePath(),
                               QStringLiteral("read_only"));
    request.environment = QProcessEnvironment::systemEnvironment();
    request.environment.insert(QString::fromLatin1(kFakeMcpModeEnv),
                               QStringLiteral("list-error-on-first-process"));
    request.environment.insert(QString::fromLatin1(kFakeMcpMarkerEnv), marker_path);

    const auto first_result = pool.listTools(request);
    QVERIFY(!first_result);
    QCOMPARE(first_result.error(), QStringLiteral("forced list transport error"));
    QVERIFY(QFileInfo::exists(marker_path));
    QCOMPARE(pool.pooledSlotCount(), 1);

    const auto retry_result = pool.listTools(request);
    const QString retry_error = retry_result ? QString{} : retry_result.error();
    QVERIFY2(retry_result.has_value(), qPrintable(retry_error));
    QVERIFY(retry_result->isEmpty());
    QCOMPARE(pool.pooledSlotCount(), 1);
    QVERIFY(pool.closeAll().has_value());
}

void AiMcpSessionPoolTests::cleanupFailurePreservesSession_data() {
    QTest::addColumn<bool>("list_failure");
    QTest::newRow("call-primary-and-cleanup-error") << false;
    QTest::newRow("list-primary-and-cleanup-error") << true;
}

void AiMcpSessionPoolTests::cleanupFailurePreservesSession() {
    QFETCH(bool, list_failure);
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const auto observed = std::make_shared<PoolCleanupObservation>();
    sak::ai::AiMcpSessionPool pool;
    pool.m_session_factory = [observed]() {
        return observedSession(observed);
    };
    // Declared after pool: every early Qt assertion return disarms before pool destruction.
    const auto disarm = qScopeGuard([observed]() { observed->fail_termination.store(false); });
    const QString mode = list_failure ? QStringLiteral("list-error-on-first-process")
                                      : QStringLiteral("error-on-first-process");
    const auto request =
        fakePoolRequest(mode, temporary_dir.filePath(QStringLiteral("first-process.marker")));
    auto generation = observeFakeProcess(pool, request);
    QVERIFY2(generation.has_value(), qPrintable(failureDiagnostic(generation)));
    const QString key = pool.sessionKeyForTesting(request);
    auto* original_session = pool.m_sessions.at(key)->session.get();
    QThread* original_controller = original_session->m_thread;
    QVERIFY(original_controller != nullptr);
    observed->fail_termination.store(true);
    QString diagnostic;
    if (list_failure) {
        const auto failed = pool.listTools(request);
        QVERIFY(!failed);
        diagnostic = failed.error();
    } else {
        const auto failed = pool.callTool(request);
        QVERIFY(!failed);
        diagnostic = failed.error();
    }
    const QString primary = list_failure ? QStringLiteral("forced list transport error")
                                         : QStringLiteral("forced transport error");
    QVERIFY2(diagnostic.startsWith(primary + QStringLiteral("; MCP cleanup failed: ")),
             qPrintable(diagnostic));
    QVERIFY2(diagnostic.contains(QStringLiteral("Terminate Windows process Job")),
             qPrintable(diagnostic));
    QCOMPARE(pool.pooledSlotCount(), 1);
    QVERIFY(pool.m_sessions.contains(key));
    QCOMPARE(pool.m_sessions.at(key)->session.get(), original_session);
    QCOMPARE(original_session->m_thread, original_controller);
    QVERIFY(!original_session->isOpen());
    verifyBlockedAdmissions(pool, request);
    QCOMPARE(pool.pooledSlotCount(), 1);
    QCOMPARE(observed->controllers_created.load(), 1);
    QCOMPARE(observed->controllers_finished.load(), 0);
    QCOMPARE(observed->controllers_destroyed.load(), 0);
    QCOMPARE(observed->termination_attempts.load(), 1);
    const auto alive = generation->isAlive();
    QVERIFY2(alive.has_value(), qPrintable(failureDiagnostic(alive)));
    QVERIFY(*alive);
    observed->fail_termination.store(false);
    const auto recovered = pool.closeAll();
    QVERIFY2(recovered.has_value(), qPrintable(failureDiagnostic(recovered)));
    QCOMPARE(pool.pooledSlotCount(), 0);
    QCOMPARE(observed->controllers_finished.load(), 1);
    QCOMPARE(observed->controllers_destroyed.load(), 1);
    const auto exited = generation->waitForExit();
    QVERIFY2(exited.has_value(), qPrintable(failureDiagnostic(exited)));
    QVERIFY(pool.callTool(request).has_value());
    QCOMPARE(observed->controllers_created.load(), 2);
    QVERIFY(pool.closeAll().has_value());
}

void AiMcpSessionPoolTests::partialCloseAllRetainsFailedEntry() {
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const auto failed_owner = std::make_shared<PoolCleanupObservation>();
    const auto clean_owner = std::make_shared<PoolCleanupObservation>();
    sak::ai::AiMcpSessionPool pool;
    int created = 0;
    pool.m_session_factory = [&]() {
        return observedSession(created++ == 0 ? failed_owner : clean_owner);
    };
    const auto disarm = qScopeGuard([&]() {
        failed_owner->fail_termination.store(false);
        clean_owner->fail_termination.store(false);
    });
    auto failed_request = fakePoolRequest(QStringLiteral("empty-success"),
                                          temporary_dir.filePath(QStringLiteral("failed.marker")));
    auto clean_request = fakePoolRequest(QStringLiteral("empty-success"),
                                         temporary_dir.filePath(QStringLiteral("clean.marker")));
    // A failed first map entry must not prevent cleanup of the following healthy entry.
    if (pool.sessionKeyForTesting(clean_request) < pool.sessionKeyForTesting(failed_request)) {
        std::swap(failed_request, clean_request);
    }
    auto failed_generation = observeFakeProcess(pool, failed_request);
    QVERIFY2(failed_generation.has_value(), qPrintable(failureDiagnostic(failed_generation)));
    auto clean_generation = observeFakeProcess(pool, clean_request);
    QVERIFY2(clean_generation.has_value(), qPrintable(failureDiagnostic(clean_generation)));
    QCOMPARE(pool.pooledSlotCount(), 2);
    const QString failed_key = pool.sessionKeyForTesting(failed_request);
    const QString clean_key = pool.sessionKeyForTesting(clean_request);
    auto* original_session = pool.m_sessions.at(failed_key)->session.get();
    failed_owner->fail_termination.store(true);
    const auto partial = pool.closeAll();
    QVERIFY(!partial);
    QVERIFY2(partial.error().contains(QStringLiteral("Terminate Windows process Job")),
             qPrintable(partial.error()));
    QCOMPARE(pool.pooledSlotCount(), 1);
    QVERIFY(pool.m_sessions.contains(failed_key));
    QVERIFY(!pool.m_sessions.contains(clean_key));
    QCOMPARE(pool.m_sessions.at(failed_key)->session.get(), original_session);
    QCOMPARE(failed_owner->controllers_finished.load(), 0);
    QCOMPARE(failed_owner->controllers_destroyed.load(), 0);
    QCOMPARE(clean_owner->controllers_finished.load(), 1);
    QCOMPARE(clean_owner->controllers_destroyed.load(), 1);
    const auto clean_exit = clean_generation->waitForExit();
    QVERIFY2(clean_exit.has_value(), qPrintable(failureDiagnostic(clean_exit)));
    const auto failed_alive = failed_generation->isAlive();
    QVERIFY2(failed_alive.has_value(), qPrintable(failureDiagnostic(failed_alive)));
    QVERIFY(*failed_alive);
    verifyBlockedAdmissions(pool, clean_request);
    QCOMPARE(created, 2);
    failed_owner->fail_termination.store(false);
    const auto recovered = pool.closeAll();
    QVERIFY2(recovered.has_value(), qPrintable(failureDiagnostic(recovered)));
    QCOMPARE(pool.pooledSlotCount(), 0);
    QCOMPARE(failed_owner->controllers_finished.load(), 1);
    QCOMPARE(failed_owner->controllers_destroyed.load(), 1);
    const auto failed_exit = failed_generation->waitForExit();
    QVERIFY2(failed_exit.has_value(), qPrintable(failureDiagnostic(failed_exit)));
    QVERIFY(pool.callTool(clean_request).has_value());
    QCOMPARE(created, 3);
    QVERIFY(pool.closeAll().has_value());
}

void AiMcpSessionPoolTests::closeAllHonorsContendedLockDeadline_data() {
    QTest::addColumn<bool>("map_lock");
    QTest::newRow("map-lock-deadline") << true;
    QTest::newRow("entry-lock-deadline") << false;
}

void AiMcpSessionPoolTests::closeAllHonorsContendedLockDeadline() {
    QFETCH(bool, map_lock);
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const auto observed = std::make_shared<PoolCleanupObservation>();
    sak::ai::AiMcpSessionPool pool;
    pool.m_session_factory = [observed]() {
        return observedSession(observed);
    };
    const auto request = fakePoolRequest(QStringLiteral("empty-success"),
                                         temporary_dir.filePath(QStringLiteral("lock.marker")));
    auto generation = observeFakeProcess(pool, request);
    QVERIFY2(generation.has_value(), qPrintable(failureDiagnostic(generation)));
    const QString key = pool.sessionKeyForTesting(request);
    const auto original_entry = pool.m_sessions.at(key);
    auto* original_session = original_entry->session.get();
    QMutex& contended_mutex = map_lock ? pool.m_mutex : original_entry->mutex;
    std::promise<bool> acquired;
    auto ready = acquired.get_future();
    std::atomic_bool holding{false};
    std::jthread holder([&]() {
        if (!contended_mutex.tryLock(kLockAcquisitionTimeoutMs)) {
            acquired.set_value(false);
            return;
        }
        const std::unique_lock lock(contended_mutex, std::adopt_lock);
        holding.store(true);
        acquired.set_value(true);
        // Independent release lets the old blocking path complete and fail normal assertions.
        std::this_thread::sleep_for(std::chrono::milliseconds(kContendedLockHoldMs));
        holding.store(false);
    });
    QVERIFY(ready.wait_for(kLockHolderReadyTimeout) == std::future_status::ready);
    QVERIFY(ready.get());
    const auto closed = pool.closeAllWithDeadline(QDeadlineTimer(kContendedCleanupDeadlineMs));
    const bool returned_while_contended = holding.load();
    holder.join();
    QVERIFY(returned_while_contended);
    QVERIFY(!closed);
    const QString expected =
        map_lock ? QStringLiteral("MCP pool remained busy through cleanup deadline")
                 : QStringLiteral("MCP pool entry remained busy through cleanup deadline");
    QCOMPARE(closed.error(), expected);
    QCOMPARE(pool.pooledSlotCount(), 1);
    QVERIFY(pool.m_sessions.contains(key));
    QCOMPARE(pool.m_sessions.at(key), original_entry);
    QCOMPARE(original_entry->session.get(), original_session);
    QCOMPARE(observed->controllers_finished.load(), 0);
    QCOMPARE(observed->controllers_destroyed.load(), 0);
    QCOMPARE(observed->termination_attempts.load(), 0);
    const auto alive = generation->isAlive();
    QVERIFY2(alive.has_value(), qPrintable(failureDiagnostic(alive)));
    QVERIFY(*alive);
    verifyBlockedAdmissions(pool, request);
    QCOMPARE(pool.pooledSlotCount(), 1);
    QCOMPARE(observed->controllers_created.load(), 1);
    const auto recovered = pool.closeAll();
    QVERIFY2(recovered.has_value(), qPrintable(failureDiagnostic(recovered)));
    QCOMPARE(pool.pooledSlotCount(), 0);
    QCOMPARE(observed->controllers_finished.load(), 1);
    QCOMPARE(observed->controllers_destroyed.load(), 1);
    const auto exited = generation->waitForExit();
    QVERIFY2(exited.has_value(), qPrintable(failureDiagnostic(exited)));
}

void AiMcpSessionPoolTests::verifyUnprovenCloseAll(sak::ai::AiMcpSessionPool& pool,
                                                   PoolCleanupObservation& observed,
                                                   bool hung_child) {
    if (hung_child) {
        observed.ignore_termination.store(true);
        const auto timed_out = pool.closeAllWithDeadline(
            QDeadlineTimer(native_mcp::kHungChildCleanupDeadlineMs, Qt::PreciseTimer));
        QVERIFY(!timed_out);
        // Caller and Worker share one deadline, so either bound may be the one observed first.
        const QString primary =
            QStringLiteral("MCP primary cleanup failed: Process operation timed out");
        const QString dispatch = QStringLiteral("MCP worker cleanup dispatch exceeded deadline");
        QVERIFY2(timed_out.error() == primary || timed_out.error() == dispatch,
                 qPrintable(timed_out.error()));
    } else {
        observed.fail_termination.store(true);
        const auto refused = pool.closeAll();
        QVERIFY(!refused);
        QCOMPARE(refused.error(), QStringLiteral("Terminate Windows process Job failed (err=5)"));
    }
    QCOMPARE(pool.pooledSlotCount(), 1);
}

void AiMcpSessionPoolTests::destructionQuarantinesUnprovenCleanup_data() {
    QTest::addColumn<bool>("hung_child");
    QTest::newRow("termination-refused") << false;
    QTest::newRow("hung-child-cleanup-timeout") << true;
}

void AiMcpSessionPoolTests::destructionQuarantinesUnprovenCleanup() {
    QFETCH(bool, hung_child);
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const auto observed = std::make_shared<PoolCleanupObservation>();
    const int custody_before = sak::ContainedProcessQuarantine::retainedCount();
    auto pool = std::make_unique<sak::ai::AiMcpSessionPool>();
    // Every early Qt assertion return disarms, destroys the pool, then releases custody.
    const auto recover = qScopeGuard([&]() {
        observed->fail_termination.store(false);
        observed->ignore_termination.store(false);
        pool.reset();
        if (!native_mcp::releaseCustodyTo(custody_before)) {
            qFatal("Custody fixture could not release its retained MCP session");
        }
    });
    pool->m_session_factory = [observed]() {
        return observedSession(observed);
    };
    const auto request = fakePoolRequest(QStringLiteral("empty-success"),
                                         temporary_dir.filePath(QStringLiteral("custody.marker")));
    const auto pid = fakeProcessPid(*pool, request);
    QVERIFY2(pid.has_value(), qPrintable(failureDiagnostic(pid)));
    auto generation = native_mcp::ProcessGeneration::observe(*pid);
    QVERIFY2(generation.has_value(), qPrintable(failureDiagnostic(generation)));
    verifyUnprovenCloseAll(*pool, *observed, hung_child);
    verifyBlockedAdmissions(*pool, request);
    // Destruction reruns closeAll(); a refusal keeps that attempt immediate for the hung row.
    observed->ignore_termination.store(false);
    observed->fail_termination.store(true);
    pool.reset();  // Used to qFatal: "MCP pool destruction refused: ..."
    QCOMPARE(sak::ContainedProcessQuarantine::retainedCount(), custody_before + 1);
    const QString reason = sak::ContainedProcessQuarantine::retainedReasons().constLast();
    QVERIFY2(reason.startsWith(QStringLiteral("MCP pool destruction could not prove cleanup: ")),
             qPrintable(reason));
    // Contained, not torn down: the child lives inside the still-open Job, its controller runs.
    const auto alive = generation->isAlive();
    QVERIFY2(alive.has_value(), qPrintable(failureDiagnostic(alive)));
    QVERIFY(*alive);
    QCOMPARE(observed->job_handle_closes.load(), 0);
    const auto contained = native_mcp::isProcessInJob(*pid, observed->job.load());
    QVERIFY2(contained.has_value(), qPrintable(failureDiagnostic(contained)));
    QVERIFY(*contained);
    QCOMPARE(observed->controllers_destroyed.load(), 0);
    observed->fail_termination.store(false);
    const auto released = native_mcp::releaseCustodyTo(custody_before);
    QVERIFY2(released.has_value(), qPrintable(failureDiagnostic(released)));
    QCOMPARE(observed->controllers_destroyed.load(), 1);
    QCOMPARE(observed->job_handle_closes.load(), 1);
    // The retained Job, not the child's own deadline, ended the child.
    const auto exited = generation->waitForExit();
    QVERIFY2(exited.has_value(), qPrintable(failureDiagnostic(exited)));
    QCOMPARE(*exited, DWORD{sak::kWindowsProcessJobFailureExitCode});
}

void AiMcpSessionPoolTests::missingCommandFailsCleanlyAndCloses() {
    // A missing server binary must fail the call (spinning the session worker
    // thread up and back down without hanging), and closeAll must tear everything
    // down cleanly afterwards.
    sak::ai::AiMcpSessionPool pool;
    const auto request = requestWith(QDir(QCoreApplication::applicationDirPath())
                                         .filePath(QStringLiteral("no_such_mcp_server.exe")),
                                     QStringLiteral("read_only"));

    const QString expected_error =
        QStringLiteral("MCP stdio command missing: %1").arg(request.command);
    const auto call_result = pool.callTool(request);
    QVERIFY(!call_result);
    QCOMPARE(call_result.error(), expected_error);

    const auto tools_result = pool.listTools(request);
    QVERIFY(!tools_result);
    QCOMPARE(tools_result.error(), expected_error);

    QVERIFY(pool.closeAll().has_value());
    QCOMPARE(pool.pooledSlotCount(), 0);
    QVERIFY(pool.closeAll().has_value());  // idempotent
}

void AiMcpSessionPoolTests::builtInMcpReuseAndPolicyIsolation() {
    QTemporaryDir read_only_app_data;
    QTemporaryDir full_app_data;
    QVERIFY(read_only_app_data.isValid());
    QVERIFY(full_app_data.isValid());
    const QString parent_local_app_data =
        qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment);
    const QString command = native_mcp::builtInCommand();

    const auto read_only_request = builtInPoolRequest(command,
                                                      read_only_app_data.path(),
                                                      native_mcp::SecurityProfile::ReadOnly);
    const auto full_request =
        builtInPoolRequest(command, full_app_data.path(), native_mcp::SecurityProfile::Full);

    const auto preflight = validatePoolRequestsBeforeLaunch(
        read_only_request, read_only_app_data.path(), full_request, full_app_data.path());
    QVERIFY2(preflight.has_value(), qPrintable(failureDiagnostic(preflight)));
    QCOMPARE(qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment), parent_local_app_data);

    sak::ai::AiMcpSessionPool pool;
    auto primary = observeHealthProcess(&pool, read_only_request);
    QVERIFY2(primary.has_value(), qPrintable(failureDiagnostic(primary)));
    const auto repeated_pid = callHealth(&pool, read_only_request);
    QVERIFY2(repeated_pid.has_value(), qPrintable(failureDiagnostic(repeated_pid)));
    QCOMPARE(*repeated_pid, primary->pid);
    const auto primary_alive_after_repeat = primary->generation.isAlive();
    QVERIFY2(primary_alive_after_repeat.has_value(),
             qPrintable(failureDiagnostic(primary_alive_after_repeat)));
    QVERIFY(*primary_alive_after_repeat);
    QCOMPARE(pool.pooledSlotCount(), 1);
    const QString read_only_rendezvous = native_mcp::rendezvousPath(read_only_app_data.path());
    QVERIFY2(QFileInfo(read_only_rendezvous).isFile(), qPrintable(read_only_rendezvous));

    const auto read_only_tools = pool.listTools(read_only_request);
    QVERIFY2(read_only_tools.has_value(), qPrintable(failureDiagnostic(read_only_tools)));
    QVERIFY(native_mcp::containsTool(*read_only_tools, QStringLiteral("health_check")));
    QVERIFY(native_mcp::containsTool(*read_only_tools, QStringLiteral("list_windows")));
    QVERIFY(!native_mcp::containsTool(*read_only_tools, QStringLiteral("clipboard_write")));

    auto full = observeHealthProcess(&pool, full_request);
    QVERIFY2(full.has_value(), qPrintable(failureDiagnostic(full)));
    QVERIFY(full->pid != primary->pid);
    const auto primary_alive_after_full = primary->generation.isAlive();
    QVERIFY2(primary_alive_after_full.has_value(),
             qPrintable(failureDiagnostic(primary_alive_after_full)));
    QVERIFY(*primary_alive_after_full);
    QCOMPARE(pool.pooledSlotCount(), 2);
    const QString full_rendezvous = native_mcp::rendezvousPath(full_app_data.path());
    QVERIFY2(QFileInfo(full_rendezvous).isFile(), qPrintable(full_rendezvous));
    const auto full_tools = pool.listTools(full_request);
    QVERIFY2(full_tools.has_value(), qPrintable(failureDiagnostic(full_tools)));
    QVERIFY(native_mcp::containsTool(*full_tools, QStringLiteral("clipboard_write")));

    QVERIFY(pool.closeAll().has_value());
    QCOMPARE(pool.pooledSlotCount(), 0);
    const auto primary_exit = primary->generation.waitForExit();
    QVERIFY2(primary_exit.has_value(), qPrintable(failureDiagnostic(primary_exit)));
    const auto full_exit = full->generation.waitForExit();
    QVERIFY2(full_exit.has_value(), qPrintable(failureDiagnostic(full_exit)));
    QCOMPARE(qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment), parent_local_app_data);
}

int main(int argc, char* argv[]) {
    const QString fake_mode = qEnvironmentVariable(kFakeMcpModeEnv).trimmed();
    if (fake_mode == QLatin1String("empty-success") ||
        fake_mode == QLatin1String("error-on-first-process") ||
        fake_mode == QLatin1String("list-error-on-first-process")) {
        const native_mcp::OwnedChildDeadline deadline(kFakeProcessLifetimeMs,
                                                      kFakeProcessDeadlineExit);
        return runFakeMcpServer(fake_mode);
    }
    QCoreApplication application(argc, argv);
    AiMcpSessionPoolTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_ai_mcp_session_pool.moc"
