// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/ai/ai_mcp_stdio_session.h"
#include "sak/windows_process_job.h"

#include "../support/ai_mcp_native_fixture.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <concepts>
#include <future>
#include <limits>
#include <memory>
#include <thread>

#include <private/qobject_p.h>
#include <private/qprocess_p.h>
#include <private/qwindowspipereader_p.h>

namespace {

namespace native_mcp = sak::testfixtures::mcp;
using native_mcp::failureDiagnostic;

class WorkerMetaCallCounter final : public QObject {
public:
    WorkerMetaCallCounter(std::atomic_int& delivered, QObject* parent)
        : QObject(parent), m_delivered(delivered) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::MetaCall) {
            m_delivered.fetch_add(1, std::memory_order_relaxed);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::atomic_int& m_delivered;
};

class SlowLineBuffer final : public QBuffer {
public:
    explicit SlowLineBuffer(QByteArray* data) : QBuffer(data) {}

    int line_reads{0};

protected:
    qint64 readLineData(char* data, qint64 max_length) override {
        ++line_reads;
        QThread::msleep(80);
        return QIODevice::readLineData(data, max_length);
    }
};

constexpr auto kFakeMcpModeEnv = "SAK_TEST_MCP_SESSION_SERVER_MODE";
constexpr auto kFakeTranscriptEnv = "SAK_TEST_MCP_SESSION_TRANSCRIPT";
constexpr auto kFakeStartupMarkerEnv = "SAK_TEST_MCP_SESSION_STARTUP_MARKER";
constexpr auto kFakeFloodMappingEnv = "SAK_TEST_MCP_FLOOD_MAPPING";
constexpr auto kFakeFloodStartEnv = "SAK_TEST_MCP_FLOOD_START";
constexpr int kFakeTranscriptFailure = 3;
constexpr int kFakeStartupMarkerFailure = 4;
constexpr int kFakeFloodFailure = 5;
constexpr qint64 kMaxFakeTranscriptBytes = 32 * 1024;
constexpr int kFakeOverToolCeilingCount = 4097;
constexpr int kFakeRequestTimeoutMs = 5000;
constexpr int kFakeNoiseDelayMs = 25;
constexpr int kFakeLateReplyMs = 1500;
constexpr auto kCreatorDeadline = std::chrono::seconds(10);
constexpr int kFakeProcessLifetimeMs = 10'000;
constexpr UINT kFakeProcessDeadlineExit = 93;
constexpr int kShortCleanupDeadlineMs = 30;
constexpr int kFinishedCallbackHoldMs = 500;
constexpr int kLateQueryCloseDeadlineMs = 250;
constexpr int kLateQueryResultWaitMs = 375;
constexpr DWORD kFloodChunkBytes = 4096;
constexpr qint64 kStdoutFloodTargetBytes = 16 * 1024 * 1024;
constexpr qint64 kStderrFloodTargetBytes = 4 * 1024 * 1024;
constexpr qint64 kStdoutPipeCapBytes = 8 * 1024 * 1024;
constexpr qint64 kStderrPipeCapBytes = 64 * 1024;
constexpr qint64 kStdoutDrainBytes = 1024 * 1024;
constexpr qint64 kStderrDrainBytes = 16 * 1024;
constexpr int kFloodWaitMs = 4000;
constexpr int kFloodStallSampleMs = 100;
constexpr int kFloodStallSamples = 3;
constexpr int kFloodObservationWaitMs = 1000;

struct alignas(8) FloodCounters {
    volatile LONG64 completed_bytes;
    volatile LONG started;
    volatile LONG done;
    volatile LONG failure;
};

class OwnedFloodControl final {
public:
    OwnedFloodControl() {
        const QString suffix = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_mapping_name = QStringLiteral("Local\\SAK_MCP_FLOOD_MAP_%1").arg(suffix);
        m_start_name = QStringLiteral("Local\\SAK_MCP_FLOOD_START_%1").arg(suffix);
        m_mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE,
                                         nullptr,
                                         PAGE_READWRITE,
                                         0,
                                         static_cast<DWORD>(sizeof(FloodCounters)),
                                         m_mapping_name.toStdWString().c_str());
        if (m_mapping != nullptr) {
            m_counters = static_cast<FloodCounters*>(
                ::MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FloodCounters)));
        }
        if (m_counters != nullptr) {
            m_start = ::CreateEventW(nullptr, TRUE, FALSE, m_start_name.toStdWString().c_str());
        }
    }

    OwnedFloodControl(const OwnedFloodControl&) = delete;
    OwnedFloodControl& operator=(const OwnedFloodControl&) = delete;

    ~OwnedFloodControl() {
        if (m_counters != nullptr && ::UnmapViewOfFile(m_counters) == FALSE) {
            qFatal("Could not unmap owned MCP flood counters");
        }
        if (m_start != nullptr && ::CloseHandle(m_start) == FALSE) {
            qFatal("Could not close owned MCP flood start event");
        }
        if (m_mapping != nullptr && ::CloseHandle(m_mapping) == FALSE) {
            qFatal("Could not close owned MCP flood mapping");
        }
    }

    [[nodiscard]] bool isReady() const {
        return m_mapping != nullptr && m_counters != nullptr && m_start != nullptr;
    }
    [[nodiscard]] QString mappingName() const { return m_mapping_name; }
    [[nodiscard]] QString startName() const { return m_start_name; }
    [[nodiscard]] bool start() const { return ::SetEvent(m_start) != FALSE; }
    [[nodiscard]] qint64 completedBytes() const {
        return ::InterlockedCompareExchange64(&m_counters->completed_bytes, 0, 0);
    }
    [[nodiscard]] LONG started() const {
        return ::InterlockedCompareExchange(&m_counters->started, 0, 0);
    }
    [[nodiscard]] LONG done() const {
        return ::InterlockedCompareExchange(&m_counters->done, 0, 0);
    }
    [[nodiscard]] LONG failure() const {
        return ::InterlockedCompareExchange(&m_counters->failure, 0, 0);
    }

private:
    QString m_mapping_name;
    QString m_start_name;
    HANDLE m_mapping{nullptr};
    HANDLE m_start{nullptr};
    FloodCounters* m_counters{nullptr};
};

struct PipeSnapshot {
    qint64 pid{0};
    qint64 private_bytes{-1};
    qint64 cap_bytes{-1};
    qint64 drained_bytes{-1};
    bool running{false};
};

struct PipeObservation {
    QSemaphore completed;
    PipeSnapshot snapshot;
};

bool waitForProducerStall(const OwnedFloodControl& control, qint64 target_bytes) {
    QDeadlineTimer deadline(kFloodWaitMs, Qt::PreciseTimer);
    qint64 previous = control.completedBytes();
    int stable_samples = 0;
    while (!deadline.hasExpired()) {
        QThread::msleep(kFloodStallSampleMs);
        const qint64 current = control.completedBytes();
        if (current >= target_bytes) {
            return false;
        }
        stable_samples = current == previous ? stable_samples + 1 : 0;
        if (stable_samples >= kFloodStallSamples) {
            return true;
        }
        previous = current;
    }
    return false;
}

bool waitForProducerResume(const OwnedFloodControl& control, qint64 stalled_bytes) {
    QDeadlineTimer deadline(kFloodWaitMs, Qt::PreciseTimer);
    while (!deadline.hasExpired()) {
        if (control.completedBytes() > stalled_bytes) {
            return true;
        }
        QThread::msleep(kFloodStallSampleMs);
    }
    return false;
}

bool installWorkerMetaCallCounter(QObject* worker, std::atomic_int& delivered) {
    auto ready = std::make_shared<QSemaphore>();
    if (!QMetaObject::invokeMethod(
            worker,
            [worker, &delivered, ready]() {
                worker->installEventFilter(new WorkerMetaCallCounter(delivered, worker));
                ready->release();
            },
            Qt::QueuedConnection)) {
        return false;
    }
    return ready->tryAcquire(1, kFakeRequestTimeoutMs);
}

bool isJobEmpty(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    return ::QueryInformationJobObject(job,
                                       JobObjectBasicAccountingInformation,
                                       &accounting,
                                       sizeof(accounting),
                                       nullptr) != FALSE &&
           accounting.ActiveProcesses == 0;
}

void writeFakeMessage(QTextStream& output, const QJsonObject& message) {
    output << QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)) << '\n';
    output.flush();
}

void writeFakeResponse(QTextStream& output, int id, const QJsonObject& result) {
    const QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                               {QStringLiteral("id"), id},
                               {QStringLiteral("result"), result}};
    writeFakeMessage(output, response);
}

QJsonObject fakeToolsListResult(const QString& mode, const QString& cursor) {
    if (mode == QLatin1String("empty-tools")) {
        return QJsonObject{{QStringLiteral("tools"), QJsonArray{}}};
    }
    if (mode == QLatin1String("missing-tools")) {
        return {};
    }
    if (mode == QLatin1String("tool-ceiling")) {
        QJsonArray tools;
        for (int index = 0; index < kFakeOverToolCeilingCount; ++index) {
            tools.append(
                QJsonObject{{QStringLiteral("name"), QStringLiteral("tool_%1").arg(index)}});
        }
        return QJsonObject{{QStringLiteral("tools"), tools}};
    }

    const QJsonObject tool{
        {QStringLiteral("name"),
         cursor.isEmpty() ? QStringLiteral("first_tool") : QStringLiteral("second_tool")}};
    QJsonObject result{{QStringLiteral("tools"), QJsonArray{tool}}};
    if (mode == QLatin1String("cursor-cycle")) {
        result[QStringLiteral("nextCursor")] = QStringLiteral("cycle");
    } else if (mode == QLatin1String("page-ceiling")) {
        const int page = cursor.isEmpty() ? 1 : cursor.mid(5).toInt() + 1;
        result[QStringLiteral("nextCursor")] = QStringLiteral("page_%1").arg(page);
    } else if (cursor.isEmpty()) {
        result[QStringLiteral("nextCursor")] = QStringLiteral("page-2");
    }
    return result;
}

bool writeFakeFailure(QTextStream& output, int id, const QString& mode) {
    QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), id}};
    if (mode == QLatin1String("rpc-error")) {
        message[QStringLiteral("error")] =
            QJsonObject{{QStringLiteral("message"), QStringLiteral("fixture refused")}};
    } else if (mode == QLatin1String("scalar-result")) {
        message[QStringLiteral("result")] = 7;
    } else if (mode != QLatin1String("missing-result")) {
        return false;
    }
    writeFakeMessage(output, message);
    return true;
}

void writeFakeNoise(QTextStream& output) {
    output << "server diagnostic\n{malformed\n"
              "{\"jsonrpc\":\"2.0\",\"method\":\"notice\"}\n"
              "{\"jsonrpc\":\"2.0\",\"id\":-1,\"result\":{}}\n";
    output.flush();
    // Separate noise-only buffering from the eventual matching reply.
    QThread::msleep(kFakeNoiseDelayMs);
}

void writeFakeNoiseUntilLateReply(QTextStream& output) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < kFakeLateReplyMs) {
        output << "{\"jsonrpc\":\"2.0\",\"method\":\"notice\"}\n";
        output.flush();
        QThread::msleep(1);
    }
}

bool serveFakeToolRequest(QTextStream& output, const QJsonObject& request, const QString& mode) {
    const int id = request.value(QStringLiteral("id")).toInt();
    if (mode == QLatin1String("exit-before-response")) {
        return false;
    }
    if (writeFakeFailure(output, id, mode)) {
        return true;
    }
    if (mode == QLatin1String("noise")) {
        writeFakeNoise(output);
    }
    const QString method = request.value(QStringLiteral("method")).toString();
    if (mode == QLatin1String("late-noise") && method == QLatin1String("tools/list")) {
        writeFakeNoiseUntilLateReply(output);
    }
    if (method == QLatin1String("tools/list")) {
        const QString cursor = request.value(QStringLiteral("params"))
                                   .toObject()
                                   .value(QStringLiteral("cursor"))
                                   .toString();
        writeFakeResponse(output, id, fakeToolsListResult(mode, cursor));
    } else if (method == QLatin1String("tools/call")) {
        const QJsonObject result =
            mode == QLatin1String("missing-content")
                ? QJsonObject{}
                : QJsonObject{{QStringLiteral("content"),
                               QJsonArray{QJsonObject{
                                   {QStringLiteral("type"), QStringLiteral("text")},
                                   {QStringLiteral("text"), QStringLiteral("call-ok")}}}}};
        writeFakeResponse(output, id, result);
    }
    return true;
}

bool recordFakeRequest(const QString& path, const QJsonObject& request) {
    if (path.isEmpty() || !request.contains(QStringLiteral("id"))) {
        return true;
    }
    QFile transcript(path);
    if (!transcript.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return false;
    }
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > kMaxFakeTranscriptBytes - transcript.size()) {
        return false;
    }
    return transcript.write(line) == line.size() && transcript.flush();
}

bool recordFakeStartup(const QString& path) {
    if (path.isEmpty()) {
        return true;
    }
    if (!QDir::isAbsolutePath(path)) {
        return false;
    }
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }
    const QByteArray bytes = QByteArrayLiteral("fake-mcp-started\n");
    return marker.write(bytes) == bytes.size() && marker.flush();
}

int writeFloodChunks(HANDLE output, FloodCounters& counters, qint64 target) {
    QByteArray chunk(static_cast<qsizetype>(kFloodChunkBytes), 'x');
    while (::InterlockedCompareExchange64(&counters.completed_bytes, 0, 0) < target) {
        DWORD written = 0;
        if (::WriteFile(output, chunk.constData(), kFloodChunkBytes, &written, nullptr) == FALSE ||
            written == 0) {
            ::InterlockedExchange(&counters.failure, static_cast<LONG>(::GetLastError()));
            return kFakeFloodFailure;
        }
        ::InterlockedExchangeAdd64(&counters.completed_bytes, written);
    }
    ::InterlockedExchange(&counters.done, 1);
    return 0;
}

int writeFakeFlood(const QString& mode) {
    const QString mapping_name = qEnvironmentVariable(kFakeFloodMappingEnv);
    const QString start_name = qEnvironmentVariable(kFakeFloodStartEnv);
    if (mapping_name.isEmpty() || start_name.isEmpty()) {
        return kFakeFloodFailure;
    }
    HANDLE mapping =
        ::OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.toStdWString().c_str());
    if (mapping == nullptr) {
        return kFakeFloodFailure;
    }
    const auto close_mapping = qScopeGuard([&]() { ::CloseHandle(mapping); });
    auto* counters = static_cast<FloodCounters*>(
        ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FloodCounters)));
    if (counters == nullptr) {
        return kFakeFloodFailure;
    }
    const auto unmap = qScopeGuard([&]() { ::UnmapViewOfFile(counters); });
    HANDLE start = ::OpenEventW(SYNCHRONIZE, FALSE, start_name.toStdWString().c_str());
    if (start == nullptr) {
        return kFakeFloodFailure;
    }
    const auto close_start = qScopeGuard([&]() { ::CloseHandle(start); });
    if (::WaitForSingleObject(start, kFloodWaitMs) != WAIT_OBJECT_0) {
        return kFakeFloodFailure;
    }
    ::InterlockedExchange(&counters->started, 1);
    const bool stdout_channel = mode == QLatin1String("flood-stdout");
    const HANDLE output = ::GetStdHandle(stdout_channel ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    const qint64 target = stdout_channel ? kStdoutFloodTargetBytes : kStderrFloodTargetBytes;
    return writeFloodChunks(output, *counters, target);
}

int serveFakeRequest(QTextStream& output, const QJsonObject& request, const QString& mode) {
    const QString method = request.value(QStringLiteral("method")).toString();
    if (method == QLatin1String("initialize")) {
        writeFakeResponse(output,
                          request.value(QStringLiteral("id")).toInt(),
                          mode == QLatin1String("bad-initialize")
                              ? QJsonObject{}
                              : QJsonObject{{QStringLiteral("protocolVersion"),
                                             QStringLiteral("2024-11-05")}});
        return -1;
    }
    if (method == QLatin1String("notifications/initialized")) {
        if (mode == QLatin1String("exit-after-handshake")) {
            return 0;
        }
        if (mode == QLatin1String("flood-stdout") || mode == QLatin1String("flood-stderr")) {
            const int flood_result = writeFakeFlood(mode);
            return flood_result == 0 ? -1 : flood_result;
        }
        return -1;
    }
    return serveFakeToolRequest(output, request, mode) ? -1 : 0;
}

int runFakeMcpServer(const QString& mode) {
    if (!recordFakeStartup(qEnvironmentVariable(kFakeStartupMarkerEnv))) {
        return kFakeStartupMarkerFailure;
    }
    QTextStream input(stdin, QIODevice::ReadOnly);
    QTextStream output(stdout, QIODevice::WriteOnly);
    for (;;) {
        const QString line = input.readLine();
        if (line.isNull()) {
            return 0;
        }
        const QJsonObject request = QJsonDocument::fromJson(line.toUtf8()).object();
        if (!recordFakeRequest(qEnvironmentVariable(kFakeTranscriptEnv), request)) {
            return kFakeTranscriptFailure;
        }
        const int response = serveFakeRequest(output, request, mode);
        if (response != -1) {
            return response;
        }
    }
}

template <typename Session>
concept HasExpectedResults = requires(Session& session,
                                      const sak::ai::AiMcpSessionConfig& config,
                                      const QJsonObject& arguments) {
    { session.open(config) } -> std::same_as<std::expected<void, QString>>;
    {
        session.listTools()
    } -> std::same_as<std::expected<QVector<sak::ai::AiMcpToolDescriptor>, QString>>;
    {
        session.callTool(QStringLiteral("tool"), arguments)
    } -> std::same_as<std::expected<QJsonObject, QString>>;
};

template <typename Session, typename ErrorSink>
concept CanOpenWithErrorSink = requires(Session& session,
                                        const sak::ai::AiMcpSessionConfig& config,
                                        ErrorSink error) { session.open(config, error); };

template <typename Session, typename ErrorSink>
concept CanListWithErrorSink =
    requires(Session& session, ErrorSink error) { session.listTools(error); };

template <typename Session, typename ErrorSink>
concept CanCallWithErrorSink =
    requires(Session& session, const QJsonObject& arguments, ErrorSink error) {
        session.callTool(QStringLiteral("tool"), arguments, error);
    };

sak::ai::AiMcpSessionConfig fakeSessionConfig(const QString& mode) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kFakeMcpModeEnv), mode);
    // Only the case owning a temporary marker may opt this child into startup observation.
    environment.remove(QString::fromLatin1(kFakeStartupMarkerEnv));
    return {.command = QCoreApplication::applicationFilePath(),
            .environment = environment,
            .request_timeout_ms = kFakeRequestTimeoutMs};
}

std::expected<void, QString> openFakeSession(sak::ai::AiMcpStdioSession* session,
                                             const QString& mode) {
    return session->open(fakeSessionConfig(mode));
}

QJsonObject requestObservation(int id, const QString& method, const QString& cursor = {}) {
    return {{QStringLiteral("id"), id},
            {QStringLiteral("method"), method},
            {QStringLiteral("cursor"), cursor}};
}

void verifyFakeRequests(const QString& path, const QJsonArray& expected) {
    QFile transcript(path);
    QVERIFY(transcript.open(QIODevice::ReadOnly));
    QVERIFY(transcript.size() <= kMaxFakeTranscriptBytes);
    QJsonArray observed;
    while (!transcript.atEnd()) {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(transcript.readLine(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        const auto request = document.object();
        // Keep the exact received ID value: coercion could hide a changed wire type or fraction.
        observed.append(
            QJsonObject{{QStringLiteral("id"), request.value(QStringLiteral("id"))},
                        {QStringLiteral("method"), request.value(QStringLiteral("method"))},
                        {QStringLiteral("cursor"),
                         request.value(QStringLiteral("params"))
                             .toObject()
                             .value(QStringLiteral("cursor"))
                             .toString()}});
    }
    QCOMPARE(transcript.error(), QFileDevice::NoError);
    QCOMPARE(observed, expected);
}

void verifyExhaustedRequests(sak::ai::AiMcpStdioSession& session) {
    const auto called = session.callTool(QStringLiteral("fixture"), {});
    QVERIFY(!called.has_value());
    QCOMPARE(called.error(), QStringLiteral("MCP request ID space exhausted"));
    const auto listed = session.listTools();
    QVERIFY(!listed.has_value());
    QCOMPARE(listed.error(), QStringLiteral("MCP request ID space exhausted"));
}

struct ControllerObservation {
    std::atomic_int allocated{0};
    std::atomic_int detached{0};
    std::atomic_int finished{0};
    std::atomic_int destroyed{0};
    std::atomic_int matching_affinity{0};
    std::atomic_int finished_at_destruction{0};
};

void observeController(QThread& controller, ControllerObservation& observed, bool start_worker) {
    observed.allocated.fetch_add(1);
    if (controller.thread() == nullptr) {
        observed.detached.fetch_add(1);
    }
    QObject::connect(
        &controller,
        &QThread::finished,
        &controller,
        [&observed]() { observed.finished.fetch_add(1); },
        Qt::DirectConnection);
    QObject::connect(
        &controller,
        &QObject::destroyed,
        &controller,
        [&observed](QObject* object) {
            // The QThread-derived lifetime has ended: inspect only QObject affinity.
            if (object->thread() == QThread::currentThread()) {
                observed.matching_affinity.fetch_add(1);
            }
            observed.finished_at_destruction.store(observed.finished.load());
            observed.destroyed.fetch_add(1);
        },
        Qt::DirectConnection);
    if (start_worker) {
        controller.start();
    }
}

struct OpenedSession {
    std::unique_ptr<sak::ai::AiMcpStdioSession> session;
    std::expected<void, QString> opened;
};

OpenedSession openObservedSession(ControllerObservation& observed,
                                  bool start_worker,
                                  bool missing_command) {
    auto session = std::make_unique<sak::ai::AiMcpStdioSession>();
    session->setThreadStarterForTesting([&observed, start_worker](QThread& controller) {
        observeController(controller, observed, start_worker);
    });
    auto config = fakeSessionConfig(QStringLiteral("paginate"));
    if (missing_command) {
        config.command = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral("no_such_mcp_server_binary.exe"));
    }
    auto opened = session->open(config);
    return {std::move(session), std::move(opened)};
}

class SessionCreator {
public:
    SessionCreator(ControllerObservation& observed, bool start_worker, bool missing_command)
        : m_opened(m_result.get_future())
        , m_release_future(m_release.get_future())
        , m_creator([this, &observed, start_worker, missing_command]() {
            m_result.set_value(openObservedSession(observed, start_worker, missing_command));
            m_release_future.wait();  // Deliberately no Qt event loop on the live creator.
        }) {}

    ~SessionCreator() {
        m_release.set_value();
        m_creator.join();
    }

    std::future<OpenedSession>& opened() { return m_opened; }

private:
    std::promise<OpenedSession> m_result;
    std::future<OpenedSession> m_opened;
    std::promise<void> m_release;
    std::future<void> m_release_future;
    std::jthread m_creator;
};

void verifyTwoDiscoveredPages(sak::ai::AiMcpStdioSession& session) {
    const auto tools = session.listTools();
    QVERIFY2(tools.has_value(), qPrintable(failureDiagnostic(tools)));
    QCOMPARE(tools->size(), 2);
    QCOMPARE(tools->at(0).name, QStringLiteral("first_tool"));
    QCOMPARE(tools->at(1).name, QStringLiteral("second_tool"));
}

}  // namespace

class AiMcpStdioSessionTests : public QObject {
    Q_OBJECT

private:
    static std::expected<PipeSnapshot, QString> observePipe(sak::ai::AiMcpStdioSession& session,
                                                            bool stdout_channel,
                                                            qint64 drain_bytes = 0);
    static std::expected<PipeSnapshot, QString> waitForPipeFill(sak::ai::AiMcpStdioSession& session,
                                                                const OwnedFloodControl& control,
                                                                bool stdout_channel,
                                                                qint64 cap_bytes,
                                                                qint64 target_bytes);
    static std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString> jobWithTerminationFault(
        std::atomic_bool& refuse_termination);
    static std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString> jobWithPreparationFault(
        bool fail_create);
    static std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString> jobWithHeldQuery(
        std::atomic_bool& hold_next_query,
        std::atomic_bool& hold_expired,
        std::atomic<HANDLE>& observed_job,
        QSemaphore& entered,
        QSemaphore& release_query);

private Q_SLOTS:
    void init();
    void buildsToolsListPayload();
    void validatesInitializeProtocolVersion();
    void emptyJsonRpcErrorGetsDefaultMessage();
    void parsesToolsListPage();
    void rejectsMalformedToolsListPage();
    void resultsAreExpectedWithoutLegacyErrorSinks();
    void notOpenSessionRejectsCalls();
    void openFailsCleanlyForMissingCommand();
    void listToolsFollowsPagination();
    void listToolsRejectsCursorCycle();
    void listToolsRejectsPageCeiling();
    void listToolsRejectsToolCeiling();
    void validEmptyCatalogIsSuccess();
    void skipsNoiseAndUnmatchedReplies();
    void pipeFloodBackpressure_data();
    void pipeFloodBackpressure();
    void finiteChildNoiseLateReplyCannotExtendRequest();
    void bufferedNoiseCannotOutrunDeadline();
    void expiredDeadlineLeavesBufferedInputUnread();
    void matchingLineCompletedAfterDeadlineIsRejected();
    void fullNewlineFreeBufferIsRejected();
    void oversizedBufferedLineIsRejected();
    void responseFailures_data();
    void responseFailures();
    void callToolReturnsExpectedContent();
    void requestIdBoundary_data();
    void requestIdBoundary();
    void fakeTranscriptRespectsBound();
    void jobPreparationFailurePreventsChildStartup_data();
    void jobPreparationFailurePreventsChildStartup();
    void failedCleanupRetainsWorkerUntilRecovery();
    void openPreservesPrimaryAndCleanupFailures();
    void cleanupDeadlineRetainsOwner();
    void stoppedWorkerCannotBeReportedClosed();
    void cleanupDeadlineIncludesJoinAndRetry();
    void cleanupDeadlineIncludesWorkerDispatch();
    void cleanupDroppedAfterWorkerStops();
    void cleanupStartedJobQueryFinishesAfterCallerDeadline();
    void controllerLifetime_data();
    void controllerLifetime();
    void resettingStarterRestoresRealStartup();
    void isOpenClearsAfterServerExit();
    void builtInMcpPersistentSession();
};

std::expected<PipeSnapshot, QString> AiMcpStdioSessionTests::observePipe(
    sak::ai::AiMcpStdioSession& session, bool stdout_channel, qint64 drain_bytes) {
    QObject* const worker = session.workerObjectForTesting();
    if (worker == nullptr) {
        return std::unexpected(QStringLiteral("MCP flood Worker is missing"));
    }
    auto observation = std::make_shared<PipeObservation>();
    const bool queued = QMetaObject::invokeMethod(
        worker,
        [worker, observation, stdout_channel, drain_bytes]() {
            const auto completed =
                qScopeGuard([observation]() { observation->completed.release(); });
            auto* process = worker->findChild<QProcess*>(QString(), Qt::FindDirectChildrenOnly);
            if (process == nullptr) {
                return;
            }
            observation->snapshot.pid = process->processId();
            observation->snapshot.running = process->state() == QProcess::Running;
            auto* private_process = static_cast<QProcessPrivate*>(QObjectPrivate::get(process));
            auto* reader = stdout_channel ? private_process->stdoutChannel.reader
                                          : private_process->stderrChannel.reader;
            if (reader == nullptr) {
                return;
            }
            observation->snapshot.cap_bytes = reader->maxReadBufferSize();
            if (drain_bytes > 0) {
                QByteArray drained(static_cast<qsizetype>(drain_bytes), '\0');
                observation->snapshot.drained_bytes = reader->read(drained.data(), drain_bytes);
            }
            observation->snapshot.private_bytes = reader->bytesAvailable();
        },
        Qt::QueuedConnection);
    if (!queued || !observation->completed.tryAcquire(1, kFloodObservationWaitMs)) {
        return std::unexpected(QStringLiteral("MCP flood Worker observation timed out"));
    }
    return observation->snapshot;
}

std::expected<PipeSnapshot, QString> AiMcpStdioSessionTests::waitForPipeFill(
    sak::ai::AiMcpStdioSession& session,
    const OwnedFloodControl& control,
    bool stdout_channel,
    qint64 cap_bytes,
    qint64 target_bytes) {
    QDeadlineTimer deadline(kFloodWaitMs, Qt::PreciseTimer);
    while (!deadline.hasExpired()) {
        const auto observed = observePipe(session, stdout_channel);
        if (!observed) {
            return std::unexpected(observed.error());
        }
        if (observed->private_bytes >= cap_bytes - kFloodChunkBytes ||
            control.completedBytes() >= target_bytes) {
            return *observed;
        }
        QThread::msleep(kFloodStallSampleMs);
    }
    return std::unexpected(QStringLiteral("MCP flood did not fill its private pipe in time"));
}

std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString>
AiMcpStdioSessionTests::jobWithPreparationFault(bool fail_create) {
    auto api = sak::WindowsProcessJob::nativeApi();
    if (fail_create) {
        api.create_job = [](LPSECURITY_ATTRIBUTES, LPCWSTR) -> HANDLE {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return nullptr;
        };
    } else {
        api.update_attribute =
            [](LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T)
            -> BOOL {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        };
    }
    return sak::WindowsProcessJob::createWithApi(std::move(api));
}

std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString>
AiMcpStdioSessionTests::jobWithTerminationFault(std::atomic_bool& refuse_termination) {
    auto api = sak::WindowsProcessJob::nativeApi();
    const auto terminate = api.terminate_job;
    api.terminate_job = [&, terminate](HANDLE job, UINT code) {
        if (refuse_termination.load()) {
            ::SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        return terminate(job, code);
    };
    return sak::WindowsProcessJob::createWithApi(std::move(api));
}

std::expected<std::unique_ptr<sak::WindowsProcessJob>, QString>
AiMcpStdioSessionTests::jobWithHeldQuery(std::atomic_bool& hold_next_query,
                                         std::atomic_bool& hold_expired,
                                         std::atomic<HANDLE>& observed_job,
                                         QSemaphore& entered,
                                         QSemaphore& release_query) {
    auto api = sak::WindowsProcessJob::nativeApi();
    const auto real_query = api.query_information;
    api.query_information = [&, real_query](HANDLE job,
                                            JOBOBJECTINFOCLASS info_class,
                                            LPVOID info,
                                            DWORD size,
                                            LPDWORD returned) -> BOOL {
        observed_job.store(job);
        if (hold_next_query.exchange(false)) {
            entered.release();
            hold_expired.store(!release_query.tryAcquire(1, kFinishedCallbackHoldMs));
        }
        return real_query(job, info_class, info, size, returned);
    };
    return sak::WindowsProcessJob::createWithApi(std::move(api));
}

void AiMcpStdioSessionTests::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("QProcess: Destroyed while process.*")));
}

void AiMcpStdioSessionTests::buildsToolsListPayload() {
    const QJsonObject payload = sak::ai::mcp::toolsListPayload(7);
    QCOMPARE(payload.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QCOMPARE(payload.value(QStringLiteral("id")).toInt(), 7);
    QCOMPARE(payload.value(QStringLiteral("method")).toString(), QStringLiteral("tools/list"));
    QCOMPARE(payload.value(QStringLiteral("params")).toObject(), QJsonObject{});

    const QJsonObject next = sak::ai::mcp::toolsListPayload(8, QStringLiteral("page-2"));
    QCOMPARE(
        next.value(QStringLiteral("params")).toObject().value(QStringLiteral("cursor")).toString(),
        QStringLiteral("page-2"));
}

void AiMcpStdioSessionTests::validatesInitializeProtocolVersion() {
    const QJsonObject valid{
        {QStringLiteral("result"),
         QJsonObject{{QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")}}}};
    QVERIFY(sak::ai::mcp::initializeResponseError(valid).isEmpty());

    QCOMPARE(sak::ai::mcp::initializeResponseError(QJsonObject{}),
             QStringLiteral("MCP initialize response is missing a result object"));
    const QJsonObject missing_version{{QStringLiteral("result"), QJsonObject{}}};
    QCOMPARE(sak::ai::mcp::initializeResponseError(missing_version),
             QStringLiteral("MCP initialize result missing protocolVersion"));
}

void AiMcpStdioSessionTests::emptyJsonRpcErrorGetsDefaultMessage() {
    const QJsonObject empty_message{
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("message"), QString()}}}};
    QCOMPARE(sak::ai::mcp::jsonRpcErrorMessage(empty_message),
             QStringLiteral("MCP JSON-RPC error"));

    const QJsonObject detail{
        {QStringLiteral("error"),
         QJsonObject{{QStringLiteral("message"), QStringLiteral("tool refused")}}}};
    QCOMPARE(sak::ai::mcp::jsonRpcErrorMessage(detail), QStringLiteral("tool refused"));
}

void AiMcpStdioSessionTests::parsesToolsListPage() {
    const QJsonObject result{
        {QStringLiteral("tools"),
         QJsonArray{
             QJsonObject{{QStringLiteral("name"), QStringLiteral("list_windows")},
                         {QStringLiteral("description"), QStringLiteral("List top-level windows")},
                         {QStringLiteral("inputSchema"),
                          QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}}}},
             QJsonObject{{QStringLiteral("name"), QStringLiteral("focus_window")}},
         }}};

    const auto page = sak::ai::AiMcpStdioSession::parseToolsListPage(result);
    QVERIFY(page.has_value());
    QCOMPARE(page->tools.size(), 2);
    QCOMPARE(page->tools[0].name, QStringLiteral("list_windows"));
    QCOMPARE(page->tools[0].description, QStringLiteral("List top-level windows"));
    QCOMPARE(page->tools[0].input_schema.value(QStringLiteral("type")).toString(),
             QStringLiteral("object"));
    QCOMPARE(page->tools[1].name, QStringLiteral("focus_window"));
}

void AiMcpStdioSessionTests::rejectsMalformedToolsListPage() {
    const auto missing_tools = sak::ai::AiMcpStdioSession::parseToolsListPage(QJsonObject{});
    QVERIFY(!missing_tools.has_value());
    QVERIFY(missing_tools.error().contains(QStringLiteral("tools array")));

    const QJsonObject nameless{
        {QStringLiteral("tools"),
         QJsonArray{QJsonObject{{QStringLiteral("description"), QStringLiteral("no name")}}}}};
    const auto missing_name = sak::ai::AiMcpStdioSession::parseToolsListPage(nameless);
    QVERIFY(!missing_name.has_value());
    QVERIFY(missing_name.error().contains(QStringLiteral("no name")));

    const QJsonObject bad_cursor{{QStringLiteral("tools"), QJsonArray{}},
                                 {QStringLiteral("nextCursor"), 7}};
    const auto cursor = sak::ai::AiMcpStdioSession::parseToolsListPage(bad_cursor);
    QVERIFY(!cursor.has_value());
    QVERIFY(cursor.error().contains(QStringLiteral("nextCursor")));

    const QJsonObject null_cursor{{QStringLiteral("tools"), QJsonArray{}},
                                  {QStringLiteral("nextCursor"), QJsonValue::Null}};
    const auto null_result = sak::ai::AiMcpStdioSession::parseToolsListPage(null_cursor);
    QVERIFY(!null_result.has_value());
    QVERIFY(null_result.error().contains(QStringLiteral("nextCursor")));
}

void AiMcpStdioSessionTests::resultsAreExpectedWithoutLegacyErrorSinks() {
    using Session = sak::ai::AiMcpStdioSession;
    QVERIFY(HasExpectedResults<Session>);
    QVERIFY((!CanOpenWithErrorSink<Session, QString&>));
    QVERIFY((!CanListWithErrorSink<Session, QString&>));
    QVERIFY((!CanCallWithErrorSink<Session, QString&>));
    QVERIFY((!CanOpenWithErrorSink<Session, QString*>));
    QVERIFY((!CanListWithErrorSink<Session, QString*>));
    QVERIFY((!CanCallWithErrorSink<Session, QString*>));
}

void AiMcpStdioSessionTests::notOpenSessionRejectsCalls() {
    sak::ai::AiMcpStdioSession session;
    QVERIFY(!session.isOpen());

    const auto tools = session.listTools();
    QVERIFY(!tools.has_value());
    QCOMPARE(tools.error(), QStringLiteral("MCP session is not open"));
    const auto called = session.callTool(QStringLiteral("list_windows"), QJsonObject{});
    QVERIFY(!called.has_value());
    QCOMPARE(called.error(), QStringLiteral("MCP session is not open"));
}

void AiMcpStdioSessionTests::openFailsCleanlyForMissingCommand() {
    // A missing command must fail open() -- and must spin the worker thread up and
    // back down without hanging or leaking (the failure path calls close()).
    sak::ai::AiMcpStdioSession session;
    sak::ai::AiMcpSessionConfig config;
    config.command = QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("no_such_mcp_server_binary.exe"));
    config.request_timeout_ms = kFakeRequestTimeoutMs;
    const auto opened = session.open(config);
    QVERIFY(!opened.has_value());
    // Fail closed with the exact missing-command message (the command is a known, absent path).
    QCOMPARE(opened.error(), QStringLiteral("MCP stdio command missing: %1").arg(config.command));
    QVERIFY(!session.isOpen());
    // A second open attempt (which internally closes the first) must also be clean.
    const auto reopened = session.open(config);
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error(), opened.error());
    QVERIFY(!session.isOpen());
}

void AiMcpStdioSessionTests::listToolsFollowsPagination() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    verifyTwoDiscoveredPages(session);
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::listToolsRejectsCursorCycle() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("cursor-cycle"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto tools = session.listTools();
    QVERIFY(!tools.has_value());
    QCOMPARE(tools.error(), QStringLiteral("MCP tools/list returned a cursor cycle"));
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::listToolsRejectsPageCeiling() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("page-ceiling"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto tools = session.listTools();
    QVERIFY(!tools.has_value());
    QCOMPARE(tools.error(), QStringLiteral("MCP tools/list exceeded the 128-page ceiling"));
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::listToolsRejectsToolCeiling() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("tool-ceiling"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto tools = session.listTools();
    QVERIFY(!tools.has_value());
    QCOMPARE(tools.error(), QStringLiteral("MCP tools/list exceeded the 4096-tool ceiling"));
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::validEmptyCatalogIsSuccess() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("empty-tools"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto tools = session.listTools();
    QVERIFY2(tools.has_value(), qPrintable(failureDiagnostic(tools)));
    QVERIFY(tools->isEmpty());
}

void AiMcpStdioSessionTests::skipsNoiseAndUnmatchedReplies() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("noise"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    verifyTwoDiscoveredPages(session);
    const auto called = session.callTool(QStringLiteral("fixture"), {});
    QVERIFY2(called.has_value(), qPrintable(failureDiagnostic(called)));
    QCOMPARE(called->value(QStringLiteral("content")).toArray().size(), 1);
}

void AiMcpStdioSessionTests::pipeFloodBackpressure_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("stdout_channel");
    QTest::addColumn<qint64>("cap_bytes");
    QTest::addColumn<qint64>("target_bytes");
    QTest::addColumn<qint64>("drain_bytes");
    QTest::newRow("stdout") << QStringLiteral("flood-stdout") << true << kStdoutPipeCapBytes
                            << kStdoutFloodTargetBytes << kStdoutDrainBytes;
    QTest::newRow("stderr") << QStringLiteral("flood-stderr") << false << kStderrPipeCapBytes
                            << kStderrFloodTargetBytes << kStderrDrainBytes;
}

void AiMcpStdioSessionTests::pipeFloodBackpressure() {
    QFETCH(QString, mode);
    QFETCH(bool, stdout_channel);
    QFETCH(qint64, cap_bytes);
    QFETCH(qint64, target_bytes);
    QFETCH(qint64, drain_bytes);
    OwnedFloodControl control;
    QVERIFY2(control.isReady(), "Could not create owned MCP flood event and mapping");
    auto config = fakeSessionConfig(mode);
    config.environment.insert(QString::fromLatin1(kFakeFloodMappingEnv), control.mappingName());
    config.environment.insert(QString::fromLatin1(kFakeFloodStartEnv), control.startName());
    sak::ai::AiMcpStdioSession session;
    const auto cleanup = qScopeGuard([&]() {
        if (!session.isClosed()) {
            const auto closed = session.close();
            if (!closed) {
                qFatal("MCP flood Job cleanup failed: %s", qPrintable(closed.error()));
            }
        }
    });
    const auto opened = session.open(config);
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto initial = observePipe(session, stdout_channel);
    QVERIFY2(initial.has_value(), qPrintable(failureDiagnostic(initial)));
    QVERIFY(initial->running);
    auto generation = native_mcp::ProcessGeneration::observe(initial->pid);
    QVERIFY2(generation.has_value(), qPrintable(failureDiagnostic(generation)));
    QVERIFY(control.start());
    const auto filled = waitForPipeFill(session, control, stdout_channel, cap_bytes, target_bytes);
    QVERIFY2(filled.has_value(), qPrintable(failureDiagnostic(filled)));
    const QString channel_name = stdout_channel ? QStringLiteral("stdout")
                                                : QStringLiteral("stderr");
    const QString no_backpressure =
        QStringLiteral("MCP %1 producer reached target without backpressure").arg(channel_name);
    QVERIFY2(waitForProducerStall(control, target_bytes), qPrintable(no_backpressure));
    const qint64 stalled_bytes = control.completedBytes();
    QVERIFY2(stalled_bytes < target_bytes, qPrintable(no_backpressure));
    QCOMPARE(control.started(), LONG{1});
    QCOMPARE(control.done(), LONG{0});
    QCOMPARE(control.failure(), LONG{0});
    QCOMPARE(filled->cap_bytes, cap_bytes);
    QVERIFY(filled->private_bytes >= cap_bytes - kFloodChunkBytes);
    QVERIFY(filled->private_bytes <= cap_bytes);
    const auto alive = generation->isAlive();
    QVERIFY2(alive.has_value(), qPrintable(failureDiagnostic(alive)));
    QVERIFY(*alive);
    const auto drained = observePipe(session, stdout_channel, drain_bytes);
    QVERIFY2(drained.has_value(), qPrintable(failureDiagnostic(drained)));
    QVERIFY(drained->drained_bytes > 0);
    QVERIFY(drained->drained_bytes <= drain_bytes);
    QVERIFY2(waitForProducerResume(control, stalled_bytes),
             "MCP flood producer did not resume after bounded Worker drain");
    const auto resumed = observePipe(session, stdout_channel);
    QVERIFY2(resumed.has_value(), qPrintable(failureDiagnostic(resumed)));
    QVERIFY(resumed->private_bytes <= cap_bytes);
    const auto closed = session.close();
    QVERIFY2(closed.has_value(), qPrintable(failureDiagnostic(closed)));
    QVERIFY(session.isClosed());
    const auto exited = generation->waitForExit();
    QVERIFY2(exited.has_value(), qPrintable(failureDiagnostic(exited)));
    QVERIFY(*exited != kFakeProcessDeadlineExit);
}

void AiMcpStdioSessionTests::finiteChildNoiseLateReplyCannotExtendRequest() {
    sak::ai::AiMcpStdioSession session;
    auto config = fakeSessionConfig(QStringLiteral("late-noise"));
    config.request_timeout_ms = 1000;
    const auto opened = session.open(config);
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));

    QElapsedTimer elapsed;
    elapsed.start();
    const auto listed = session.listTools();
    QVERIFY(!listed.has_value());
    QCOMPARE(listed.error(), QStringLiteral("MCP stdio request timed out"));
    QVERIFY2(elapsed.elapsed() < 2250,
             "A stream of complete noise lines extended request deadline");

    // Let the child send its late tools/list response while this Worker is idle.
    // A later request must skip that stale id and accept only its own response.
    QThread::msleep(kFakeLateReplyMs - 1000 + 300);
    const auto called = session.callTool(QStringLiteral("fixture"), {});
    QVERIFY2(called.has_value(), qPrintable(failureDiagnostic(called)));
    QCOMPARE(called->value(QStringLiteral("content")).toArray().size(), 1);
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::bufferedNoiseCannotOutrunDeadline() {
    const QByteArray notice = QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"method\":\"notice\"}\n");
    const QByteArray response =
        QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"content\":[]}}\n");
    QByteArray input_bytes = notice + response;
    SlowLineBuffer slow_input(&input_bytes);
    QVERIFY(slow_input.open(QIODevice::ReadOnly));
    const auto timed_out = sak::ai::AiMcpStdioSession::drainMatchingLines(
        slow_input, 42, QDeadlineTimer(30, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(!timed_out.has_value());
    QCOMPARE(timed_out.error(), QStringLiteral("MCP stdio request timed out"));
    QCOMPARE(slow_input.line_reads, 1);

    QBuffer timely_input(&input_bytes);
    QVERIFY(timely_input.open(QIODevice::ReadOnly));
    const auto matched = sak::ai::AiMcpStdioSession::drainMatchingLines(
        timely_input, 42, QDeadlineTimer(1000, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(matched.has_value());
    QVERIFY(matched->has_value());
    QCOMPARE(matched->value().value(QStringLiteral("id")).toInt(), 42);
}

void AiMcpStdioSessionTests::expiredDeadlineLeavesBufferedInputUnread() {
    QByteArray input_bytes =
        QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"content\":[]}}\n");
    QBuffer input(&input_bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto timed_out = sak::ai::AiMcpStdioSession::drainMatchingLines(
        input, 42, QDeadlineTimer(0, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(!timed_out.has_value());
    QCOMPARE(timed_out.error(), QStringLiteral("MCP stdio request timed out"));
    QCOMPARE(input.pos(), 0);
}

void AiMcpStdioSessionTests::matchingLineCompletedAfterDeadlineIsRejected() {
    QByteArray input_bytes =
        QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"content\":[]}}\n");
    SlowLineBuffer slow_input(&input_bytes);
    QVERIFY(slow_input.open(QIODevice::ReadOnly));
    const auto timed_out = sak::ai::AiMcpStdioSession::drainMatchingLines(
        slow_input, 42, QDeadlineTimer(30, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(!timed_out.has_value());
    QCOMPARE(timed_out.error(), QStringLiteral("MCP stdio request timed out"));
    QCOMPARE(slow_input.line_reads, 1);
}

void AiMcpStdioSessionTests::fullNewlineFreeBufferIsRejected() {
    QByteArray input_bytes(8 * 1024 * 1024, 'x');
    QBuffer input(&input_bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto rejected = sak::ai::AiMcpStdioSession::drainMatchingLines(
        input, 42, QDeadlineTimer(1000, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error(),
             QStringLiteral("MCP stdio response exceeded 8388608 bytes without a newline"));
    QCOMPARE(input.pos(), 0);
}

void AiMcpStdioSessionTests::oversizedBufferedLineIsRejected() {
    QByteArray input_bytes(8 * 1024 * 1024, 'x');
    input_bytes.append('\n');
    QBuffer input(&input_bytes);
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto rejected = sak::ai::AiMcpStdioSession::drainMatchingLines(
        input, 42, QDeadlineTimer(1000, Qt::PreciseTimer), []() {
            return QStringLiteral("MCP stdio request timed out");
        });
    QVERIFY(!rejected.has_value());
    QCOMPARE(rejected.error(),
             QStringLiteral("MCP stdio response exceeded 8388608 bytes without a newline"));
    QCOMPARE(input.pos(), 0);
}

void AiMcpStdioSessionTests::responseFailures_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("list_tools");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("list-rpc-error")
        << QStringLiteral("rpc-error") << true << QStringLiteral("fixture refused");
    QTest::newRow("call-rpc-error")
        << QStringLiteral("rpc-error") << false << QStringLiteral("fixture refused");
    QTest::newRow("list-missing-result")
        << QStringLiteral("missing-result") << true
        << QStringLiteral("MCP stdio response missing a result object");
    QTest::newRow("call-scalar-result")
        << QStringLiteral("scalar-result") << false
        << QStringLiteral("MCP stdio response missing a result object");
    QTest::newRow("list-missing-tools")
        << QStringLiteral("missing-tools") << true
        << QStringLiteral("MCP tools/list result is missing a tools array");
    QTest::newRow("call-missing-content")
        << QStringLiteral("missing-content") << false
        << QStringLiteral("MCP tools/call result missing a content array");
    QTest::newRow("list-server-exit") << QStringLiteral("exit-before-response") << true
                                      << QStringLiteral("MCP stdio server exited before response");
}

void AiMcpStdioSessionTests::responseFailures() {
    QFETCH(QString, mode);
    QFETCH(bool, list_tools);
    QFETCH(QString, diagnostic);
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, mode);
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    if (list_tools) {
        const auto tools = session.listTools();
        QVERIFY2(!tools.has_value(), "Failed tools/list returned a success-shaped value");
        QCOMPARE(tools.error(), diagnostic);
    } else {
        const auto called = session.callTool(QStringLiteral("fixture"), {});
        QVERIFY2(!called.has_value(), "Failed tools/call returned a success-shaped value");
        QCOMPARE(called.error(), diagnostic);
    }
}

void AiMcpStdioSessionTests::callToolReturnsExpectedContent() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto nameless = session.callTool(QStringLiteral("  "), {});
    QVERIFY(!nameless.has_value());
    QCOMPARE(nameless.error(), QStringLiteral("MCP tool name is empty"));
    for (int index = 0; index < 2; ++index) {
        const auto called = session.callTool(QStringLiteral("fixture"), {});
        QVERIFY2(called.has_value(), qPrintable(failureDiagnostic(called)));
        const auto content = called->value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 1);
        QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
                 QStringLiteral("call-ok"));
    }
}

void AiMcpStdioSessionTests::requestIdBoundary_data() {
    QTest::addColumn<int>("initial_id");
    QTest::addColumn<bool>("list_tools");
    QTest::addColumn<bool>("open_succeeds");
    QTest::addColumn<bool>("operation_succeeds");
    QTest::addColumn<bool>("exhausted_after");
    QTest::addColumn<QJsonArray>("expected_requests");
    constexpr int maximum = std::numeric_limits<int>::max();
    const QString initialize = QStringLiteral("initialize");
    const QString call = QStringLiteral("tools/call");
    const QString list = QStringLiteral("tools/list");
    QTest::newRow("last-initialize") << maximum - 1 << false << true << false << true
                                     << QJsonArray{requestObservation(maximum, initialize)};
    QTest::newRow("exhausted-initialize")
        << maximum << false << false << false << true << QJsonArray{};
    QTest::newRow("last-call") << maximum - 2 << false << true << true << true
                               << QJsonArray{requestObservation(maximum - 1, initialize),
                                             requestObservation(maximum, call)};
    QTest::newRow("last-pagination")
        << maximum - 3 << true << true << true << true
        << QJsonArray{requestObservation(maximum - 2, initialize),
                      requestObservation(maximum - 1, list),
                      requestObservation(maximum, list, QStringLiteral("page-2"))};
    QTest::newRow("partial-pagination-refused")
        << maximum - 2 << true << true << false << true
        << QJsonArray{requestObservation(maximum - 1, initialize),
                      requestObservation(maximum, list)};
    QTest::newRow("first-page-refused") << maximum - 1 << true << true << false << true
                                        << QJsonArray{requestObservation(maximum, initialize)};
    QTest::newRow("ordinary-new-session")
        << 0 << true << true << true << false
        << QJsonArray{requestObservation(1, initialize),
                      requestObservation(2, list),
                      requestObservation(3, list, QStringLiteral("page-2"))};
}

void AiMcpStdioSessionTests::requestIdBoundary() {
    QFETCH(int, initial_id);
    QFETCH(bool, list_tools);
    QFETCH(bool, open_succeeds);
    QFETCH(bool, operation_succeeds);
    QFETCH(bool, exhausted_after);
    QFETCH(QJsonArray, expected_requests);
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const QString transcript_path = temporary_dir.filePath(QStringLiteral("requests.jsonl"));
    auto config = fakeSessionConfig(QStringLiteral("paginate"));
    config.environment.insert(QString::fromLatin1(kFakeTranscriptEnv), transcript_path);
    sak::ai::AiMcpStdioSession session;
    session.m_initial_request_id_for_testing = initial_id;
    const auto opened = session.open(config);
    QCOMPARE(opened.has_value(), open_succeeds);
    if (!opened) {
        QCOMPARE(opened.error(), QStringLiteral("MCP request ID space exhausted"));
        QVERIFY(!session.isOpen());
        QVERIFY(!QFileInfo::exists(transcript_path));
        return;
    }
    if (list_tools) {
        const auto listed = session.listTools();
        QCOMPARE(listed.has_value(), operation_succeeds);
        if (listed) {
            QCOMPARE(listed->size(), 2);
            QCOMPARE(listed->at(0).name, QStringLiteral("first_tool"));
            QCOMPARE(listed->at(1).name, QStringLiteral("second_tool"));
        } else {
            QCOMPARE(listed.error(), QStringLiteral("MCP request ID space exhausted"));
        }
    } else {
        const auto called = session.callTool(QStringLiteral("fixture"), {});
        QCOMPARE(called.has_value(), operation_succeeds);
        if (called) {
            const auto content = called->value(QStringLiteral("content")).toArray();
            QCOMPARE(content.size(), 1);
            QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
                     QStringLiteral("call-ok"));
        } else {
            QCOMPARE(called.error(), QStringLiteral("MCP request ID space exhausted"));
        }
    }
    if (exhausted_after) {
        verifyExhaustedRequests(session);
        verifyExhaustedRequests(session);
    }
    QVERIFY(session.close().has_value());
    verifyFakeRequests(transcript_path, expected_requests);
}

void AiMcpStdioSessionTests::fakeTranscriptRespectsBound() {
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const QString path = temporary_dir.filePath(QStringLiteral("bounded.jsonl"));
    const QJsonObject request{{QStringLiteral("id"), 1},
                              {QStringLiteral("method"), QStringLiteral("initialize")}};
    const QByteArray line = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    const QByteArray prefix(kMaxFakeTranscriptBytes - line.size(), 'x');
    QFile transcript(path);
    QVERIFY(transcript.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    QCOMPARE(transcript.write(prefix), prefix.size());
    transcript.close();
    QVERIFY(recordFakeRequest(path, request));
    QCOMPARE(QFileInfo(path).size(), kMaxFakeTranscriptBytes);
    QVERIFY(!recordFakeRequest(path, request));
    QVERIFY(transcript.open(QIODevice::ReadOnly));
    QCOMPARE(transcript.readAll(), prefix + line);
    QCOMPARE(transcript.error(), QFileDevice::NoError);
}

void AiMcpStdioSessionTests::jobPreparationFailurePreventsChildStartup_data() {
    QTest::addColumn<bool>("fail_create");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("create-job") << true
                                << QStringLiteral(
                                       "Create unnamed Windows process Job failed (err=5)");
    QTest::newRow("job-list-attribute")
        << false << QStringLiteral("Attach Windows process Job launch attribute failed (err=5)");
}

void AiMcpStdioSessionTests::jobPreparationFailurePreventsChildStartup() {
    QFETCH(bool, fail_create);
    QFETCH(QString, diagnostic);
    constexpr int kExpectedControllerGenerations = 2;
    QTemporaryDir temporary_dir;
    QVERIFY(temporary_dir.isValid());
    const QString marker_path = temporary_dir.filePath(QStringLiteral("startup.marker"));
    QVERIFY(QDir::isAbsolutePath(marker_path));
    QVERIFY(!QFileInfo::exists(marker_path));
    auto config = fakeSessionConfig(QStringLiteral("paginate"));
    config.environment.insert(QString::fromLatin1(kFakeStartupMarkerEnv), marker_path);
    ControllerObservation observed;
    std::atomic_int factory_calls{0};
    sak::ai::AiMcpStdioSession session;
    session.setThreadStarterForTesting(
        [&observed](QThread& thread) { observeController(thread, observed, true); });
    session.m_job_factory = [&factory_calls, fail_create]() {
        ++factory_calls;
        return jobWithPreparationFault(fail_create);
    };
    const auto refused = session.open(config);
    QVERIFY(!refused.has_value());
    QCOMPARE(refused.error(), diagnostic);
    QCOMPARE(factory_calls.load(), 1);
    QVERIFY(session.isClosed());
    QVERIFY(!session.isOpen());
    QCOMPARE(observed.allocated.load(), 1);
    QCOMPARE(observed.destroyed.load(), 1);
    QCOMPARE(observed.finished_at_destruction.load(), 1);
    QCOMPARE(observed.matching_affinity.load(), 1);
    QVERIFY(!QFileInfo::exists(marker_path));

    // Same executable, environment and marker path: remove only the injected API failure.
    session.m_job_factory = sak::WindowsProcessJob::create;
    const auto opened = session.open(config);
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    QFile marker(marker_path);
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(marker.readAll(), QByteArrayLiteral("fake-mcp-started\n"));
    QCOMPARE(marker.error(), QFileDevice::NoError);
    const auto called = session.callTool(QStringLiteral("fixture"), {});
    QVERIFY2(called.has_value(), qPrintable(failureDiagnostic(called)));
    const auto content = called->value(QStringLiteral("content")).toArray();
    QCOMPARE(content.size(), 1);
    QCOMPARE(content.at(0).toObject().value(QStringLiteral("text")).toString(),
             QStringLiteral("call-ok"));
    QVERIFY(session.close().has_value());
    QVERIFY(session.isClosed());
    QCOMPARE(observed.allocated.load(), kExpectedControllerGenerations);
    QCOMPARE(observed.destroyed.load(), kExpectedControllerGenerations);
}

void AiMcpStdioSessionTests::failedCleanupRetainsWorkerUntilRecovery() {
    ControllerObservation observed;
    std::atomic_bool refuse_termination{true};
    sak::ai::AiMcpStdioSession session;
    const auto disarm = qScopeGuard([&]() { refuse_termination.store(false); });
    session.setThreadStarterForTesting(
        [&](QThread& thread) { observeController(thread, observed, true); });
    session.m_job_factory = [&]() {
        return jobWithTerminationFault(refuse_termination);
    };
    const auto opened = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    const auto closed = session.close();
    QVERIFY(!closed.has_value());
    QCOMPARE(closed.error(), QStringLiteral("Terminate Windows process Job failed (err=5)"));
    QVERIFY(!session.isOpen());
    QVERIFY(!session.isClosed());
    QCOMPARE(observed.destroyed.load(), 0);
    const auto reopened = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY(!reopened.has_value());
    QCOMPARE(reopened.error(), closed.error());
    QCOMPARE(observed.allocated.load(), 1);
    refuse_termination.store(false);
    QVERIFY(session.close().has_value());
    QVERIFY(session.isClosed());
    QCOMPARE(observed.destroyed.load(), 1);
}

void AiMcpStdioSessionTests::openPreservesPrimaryAndCleanupFailures() {
    std::atomic_bool refuse_termination{true};
    sak::ai::AiMcpStdioSession session;
    const auto disarm = qScopeGuard([&]() { refuse_termination.store(false); });
    session.m_job_factory = [&]() {
        return jobWithTerminationFault(refuse_termination);
    };
    const auto opened = openFakeSession(&session, QStringLiteral("bad-initialize"));
    QVERIFY(!opened.has_value());
    QCOMPARE(opened.error(),
             QStringLiteral("MCP initialize result missing protocolVersion; MCP cleanup failed: "
                            "Terminate Windows process Job failed (err=5)"));
    QVERIFY(!session.isClosed());
    QVERIFY(!session.isOpen());
    refuse_termination.store(false);
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::cleanupDeadlineRetainsOwner() {
    std::atomic_bool report_active{false};
    std::atomic_int active_observations{0};
    sak::ai::AiMcpStdioSession session;
    const auto disarm = qScopeGuard([&]() {
        report_active.store(false);
        for (int attempt = 0; attempt < 3 && !session.isClosed(); ++attempt) {
            const auto recovered = session.close();
            if (recovered) {
                break;
            }
        }
        if (!session.isClosed()) {
            qFatal("Descendant-deadline fixture could not close its retained Job");
        }
    });
    session.m_job_factory = [&]() {
        auto api = sak::WindowsProcessJob::nativeApi();
        const auto query = api.query_information;
        api.query_information = [&, query](HANDLE job,
                                           JOBOBJECTINFOCLASS info_class,
                                           LPVOID info,
                                           DWORD size,
                                           LPDWORD returned) {
            const BOOL result = query(job, info_class, info, size, returned);
            if (result != FALSE && report_active.load()) {
                active_observations.fetch_add(1);
                static_cast<JOBOBJECT_BASIC_ACCOUNTING_INFORMATION*>(info)->ActiveProcesses = 1;
            }
            return result;
        };
        return sak::WindowsProcessJob::createWithApi(std::move(api));
    };
    const auto opened = openFakeSession(&session, QStringLiteral("exit-after-handshake"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    QTRY_VERIFY_WITH_TIMEOUT(!session.isOpen(), kFakeRequestTimeoutMs);
    report_active.store(true);
    QElapsedTimer elapsed;
    elapsed.start();
    const QDeadlineTimer deadline(kShortCleanupDeadlineMs, Qt::PreciseTimer);
    const auto closed = session.closeWithDeadline(deadline);
    QVERIFY(!closed.has_value());
    const QString descendant_error = QStringLiteral("MCP descendants exceeded cleanup deadline");
    const QString dispatch_error = QStringLiteral("MCP worker cleanup dispatch exceeded deadline");
    QVERIFY2(closed.error() == descendant_error || closed.error() == dispatch_error,
             qPrintable(closed.error()));
    QVERIFY2(deadline.hasExpired(),
             qPrintable(
                 QStringLiteral("Cleanup returned before deadline: elapsed_ns=%1 remaining_ns=%2")
                     .arg(elapsed.nsecsElapsed())
                     .arg(deadline.remainingTimeNSecs())));
    QVERIFY(active_observations.load() > 0);
    QVERIFY(elapsed.elapsed() < kFakeRequestTimeoutMs);
    QVERIFY(!session.isClosed());
    if (closed.error() == dispatch_error) {
        const auto pending = session.close();
        QVERIFY(!pending.has_value());
        QCOMPARE(pending.error(), descendant_error);
        QVERIFY(!session.isClosed());
    }
    report_active.store(false);
    QVERIFY(session.close().has_value());
    QVERIFY(session.isClosed());
}

void AiMcpStdioSessionTests::stoppedWorkerCannotBeReportedClosed() {
    sak::ai::AiMcpStdioSession session;
    QVERIFY(session.startWorker().has_value());
    QThread* const controller = session.m_thread;
    QPointer<QObject> worker(session.workerObjectForTesting());
    QVERIFY(!worker.isNull());
    // Keep the actual, as-yet-unopened Worker alive while deliberately stopping its loop.
    // Restore that connection and loop on every exit, before session destruction.
    const auto restore = qScopeGuard([&]() {
        if (session.isClosed() && worker.isNull()) {
            return;
        }
        if (worker.isNull() || session.m_thread != controller) {
            qFatal("Stopped-worker fixture lost its retained ownership");
        }
        QObject::connect(controller,
                         &QThread::finished,
                         worker.data(),
                         &QObject::deleteLater,
                         Qt::UniqueConnection);
        if (!controller->isRunning()) {
            controller->start();
        }
        const auto closed = session.close();
        if (!closed || !session.isClosed() || !worker.isNull()) {
            qFatal("Stopped-worker fixture could not restore and close its owner");
        }
    });
    QVERIFY(
        QObject::disconnect(controller, &QThread::finished, worker.data(), &QObject::deleteLater));
    controller->quit();
    QVERIFY(controller->wait(kFakeRequestTimeoutMs));
    const auto closed = session.close();
    QVERIFY(!closed.has_value());
    QCOMPARE(closed.error(),
             QStringLiteral("MCP cleanup unavailable: worker thread is not running"));
    QCOMPARE(session.m_thread, controller);
    QCOMPARE(session.workerObjectForTesting(), worker.data());
    QVERIFY(!session.isClosed());
}

void AiMcpStdioSessionTests::cleanupDeadlineIncludesJoinAndRetry() {
    QSemaphore release_finished;
    std::atomic_bool finished_entered{false};
    std::atomic_bool finished_waiting{false};
    std::atomic_bool finish_hold_expired{false};
    sak::ai::AiMcpStdioSession session;
    QPointer<QObject> worker;
    const auto recover = qScopeGuard([&]() {
        release_finished.release();
        if (!session.isClosed() && finished_entered.load()) {
            // Cleanup for a guard reversal: wait only on this known-owned, independently bounded
            // callback, then repair only state whose real QObject destruction was observed.
            if (!session.m_thread->wait(kFakeRequestTimeoutMs) || !worker.isNull()) {
                qFatal("Join fixture did not complete its bounded finish callback");
            }
            session.m_worker = nullptr;
            session.m_join_requested = true;
        }
    });
    session.setThreadStarterForTesting([&](QThread& thread) {
        QObject::connect(
            &thread,
            &QThread::finished,
            &thread,
            [&]() {
                finished_entered.store(true);
                finished_waiting.store(true);
                finish_hold_expired.store(!release_finished.tryAcquire(1, kFinishedCallbackHoldMs));
                finished_waiting.store(false);
            },
            Qt::DirectConnection);
        thread.start();
    });
    QVERIFY(session.startWorker().has_value());
    worker = session.workerObjectForTesting();
    QThread* const controller = session.m_thread;
    const auto closed =
        session.closeWithDeadline(QDeadlineTimer(kShortCleanupDeadlineMs, Qt::PreciseTimer));
    QVERIFY(!closed.has_value());
    QCOMPARE(closed.error(), QStringLiteral("MCP worker thread exceeded cleanup deadline"));
    QVERIFY(!session.isClosed());
    QVERIFY(!session.isOpen());
    QVERIFY(session.m_join_requested);
    QCOMPARE(session.m_thread, controller);
    QVERIFY(finished_waiting.load());
    QVERIFY(!worker.isNull());
    release_finished.release();
    const auto joined = session.close();
    QVERIFY2(joined.has_value(), qPrintable(failureDiagnostic(joined)));
    QVERIFY(session.isClosed());
    QVERIFY(!session.m_join_requested);
    QVERIFY(worker.isNull());
    QVERIFY(!finish_hold_expired.load());
}

void AiMcpStdioSessionTests::cleanupDeadlineIncludesWorkerDispatch() {
    QSemaphore entered;
    QSemaphore release_worker;
    std::atomic_int delivered_calls{0};
    std::atomic_bool waiting{false};
    std::atomic_bool hold_expired{false};
    sak::ai::AiMcpStdioSession session;
    const auto recover = qScopeGuard([&]() {
        release_worker.release();
        if (!session.isClosed()) {
            const auto first = session.close();
            if (!first && !session.isClosed() && !session.close()) {
                qFatal("Dispatch fixture could not recover its retained Worker");
            }
        }
    });
    QVERIFY(session.startWorker().has_value());
    QThread* const controller = session.m_thread;
    QPointer<QObject> worker(session.workerObjectForTesting());
    QObject* const worker_object = worker.data();
    QVERIFY(installWorkerMetaCallCounter(worker_object, delivered_calls));
    QVERIFY(QMetaObject::invokeMethod(
        worker_object,
        [&]() {
            waiting.store(true);
            entered.release();
            hold_expired.store(!release_worker.tryAcquire(1, kFinishedCallbackHoldMs));
            waiting.store(false);
        },
        Qt::QueuedConnection));
    QVERIFY(entered.tryAcquire(1, kFakeRequestTimeoutMs));
    QCOMPARE(delivered_calls.load(), 1);
    const auto closed =
        session.closeWithDeadline(QDeadlineTimer(kShortCleanupDeadlineMs, Qt::PreciseTimer));
    QVERIFY(!closed.has_value());
    QCOMPARE(closed.error(), QStringLiteral("MCP worker cleanup dispatch exceeded deadline"));
    QVERIFY(waiting.load());
    QVERIFY(!session.isClosed());
    QVERIFY(!session.isOpen());
    QCOMPARE(session.m_thread, controller);
    QCOMPARE(session.workerObjectForTesting(), worker.data());
    QVERIFY(!session.m_join_requested);
    const auto pending = session.m_pending_cleanup;
    QVERIFY(pending);
    const auto still_waiting =
        session.closeWithDeadline(QDeadlineTimer(kShortCleanupDeadlineMs, Qt::PreciseTimer));
    QVERIFY(!still_waiting.has_value());
    QCOMPARE(still_waiting.error(), closed.error());
    QCOMPARE(session.m_pending_cleanup, pending);
    QCOMPARE(delivered_calls.load(), 1);
    release_worker.release();
    const auto late = session.close();
    QVERIFY(!late.has_value());
    QCOMPARE(late.error(), closed.error());
    QCOMPARE(session.m_thread, controller);
    QVERIFY(!session.m_pending_cleanup);
    QCOMPARE(delivered_calls.load(), 2);
    const auto recovered = session.close();
    QVERIFY2(recovered.has_value(), qPrintable(failureDiagnostic(recovered)));
    QVERIFY(session.isClosed());
    QVERIFY(worker.isNull());
    QVERIFY(!hold_expired.load());
    QCOMPARE(delivered_calls.load(), 3);
}

void AiMcpStdioSessionTests::cleanupDroppedAfterWorkerStops() {
    QSemaphore entered;
    QSemaphore release_worker;
    std::atomic_bool hold_expired{false};
    sak::ai::AiMcpStdioSession session;
    QPointer<QObject> worker;
    const auto recover = qScopeGuard([&]() {
        release_worker.release();
        if (session.isClosed()) {
            return;
        }
        if (!session.m_thread->isRunning()) {
            if (!session.m_thread->wait(kFakeRequestTimeoutMs) || !worker.isNull()) {
                qFatal("Dropped-dispatch fixture cannot prove Worker destruction");
            }
            // Only the actual Worker destructor proves this startWorker-only fixture clean.
            session.m_pending_cleanup.reset();
            session.m_worker = nullptr;
            session.m_join_requested = true;
        }
        const auto first = session.close();
        if (!first && !session.isClosed() && !session.close()) {
            qFatal("Dropped-dispatch fixture could not release its controller");
        }
    });
    QVERIFY(session.startWorker().has_value());
    worker = session.workerObjectForTesting();
    QVERIFY(QMetaObject::invokeMethod(
        worker.data(),
        [&]() {
            entered.release();
            hold_expired.store(!release_worker.tryAcquire(1, kFinishedCallbackHoldMs));
        },
        Qt::QueuedConnection));
    QVERIFY(entered.tryAcquire(1, kFakeRequestTimeoutMs));
    const auto closed =
        session.closeWithDeadline(QDeadlineTimer(kShortCleanupDeadlineMs, Qt::PreciseTimer));
    QVERIFY(!closed.has_value());
    QCOMPARE(closed.error(), QStringLiteral("MCP worker cleanup dispatch exceeded deadline"));
    QVERIFY(session.m_pending_cleanup);
    session.m_thread->quit();
    release_worker.release();
    QVERIFY(session.m_thread->wait(kFakeRequestTimeoutMs));
    QVERIFY(worker.isNull());
    QElapsedTimer elapsed;
    elapsed.start();
    const auto recovered = session.close();
    QVERIFY2(recovered.has_value(), qPrintable(failureDiagnostic(recovered)));
    QVERIFY(elapsed.elapsed() < kFinishedCallbackHoldMs);
    QVERIFY(session.isClosed());
    QVERIFY(!session.m_pending_cleanup);
    QVERIFY(!hold_expired.load());
}

void AiMcpStdioSessionTests::cleanupStartedJobQueryFinishesAfterCallerDeadline() {
    QSemaphore entered;
    QSemaphore release_query;
    QSemaphore after_cleanup;
    std::atomic_bool hold_next_query{false};
    std::atomic_bool hold_expired{false};
    std::atomic_int delivered_calls{0};
    std::atomic<HANDLE> observed_job{nullptr};
    std::promise<std::expected<void, QString>> first_result;
    auto first_future = first_result.get_future();
    sak::ai::AiMcpStdioSession session;
    std::jthread closer;
    const auto recover = qScopeGuard([&]() {
        release_query.release();
        if (closer.joinable()) {
            closer.join();
        }
        for (int attempt = 0; attempt < 3 && !session.isClosed(); ++attempt) {
            const auto closed = session.close();
            if (closed) {
                break;
            }
        }
        if (!session.isClosed()) {
            qFatal("Late Job query fixture could not close its owner");
        }
    });
    session.m_job_factory = [&]() {
        return jobWithHeldQuery(
            hold_next_query, hold_expired, observed_job, entered, release_query);
    };
    const auto opened = openFakeSession(&session, QStringLiteral("exit-after-handshake"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    QTRY_VERIFY_WITH_TIMEOUT(!session.isOpen(), kFakeRequestTimeoutMs);
    const HANDLE job = observed_job.load();
    QVERIFY(job != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(isJobEmpty(job), kFakeRequestTimeoutMs);
    QObject* const worker = session.workerObjectForTesting();
    QVERIFY(installWorkerMetaCallCounter(worker, delivered_calls));
    hold_next_query.store(true);
    closer = std::jthread([&]() {
        first_result.set_value(
            session.closeWithDeadline(QDeadlineTimer(kLateQueryCloseDeadlineMs, Qt::PreciseTimer)));
    });
    QVERIFY(entered.tryAcquire(1, kFakeRequestTimeoutMs));
    QVERIFY(first_future.wait_for(std::chrono::milliseconds(kLateQueryResultWaitMs)) ==
            std::future_status::ready);
    const auto first = first_future.get();
    QVERIFY(!first.has_value());
    QCOMPARE(first.error(), QStringLiteral("MCP worker cleanup dispatch exceeded deadline"));
    QVERIFY(session.m_pending_cleanup);
    const auto pending = session.m_pending_cleanup;
    QVERIFY(!session.isClosed());
    closer.join();
    QVERIFY(QMetaObject::invokeMethod(
        worker, [&]() { after_cleanup.release(); }, Qt::QueuedConnection));
    release_query.release();
    QVERIFY(after_cleanup.tryAcquire(1, kFakeRequestTimeoutMs));
    QCOMPARE(session.m_pending_cleanup, pending);
    QCOMPARE(delivered_calls.load(), 2);
    const auto closed = session.close();
    QVERIFY2(closed.has_value(), qPrintable(failureDiagnostic(closed)));
    QVERIFY(session.isClosed());
    QVERIFY(!session.m_pending_cleanup);
    QVERIFY(!hold_expired.load());
    QCOMPARE(delivered_calls.load(), 2);
}

void AiMcpStdioSessionTests::controllerLifetime_data() {
    QTest::addColumn<bool>("cross_thread");
    QTest::addColumn<bool>("by_destructor");
    QTest::addColumn<bool>("start_worker");
    QTest::addColumn<bool>("missing_command");
    QTest::newRow("cross-thread-close") << true << false << true << false;
    QTest::newRow("cross-thread-destructor") << true << true << true << false;
    QTest::newRow("same-thread-close") << false << false << true << false;
    QTest::newRow("same-thread-destructor") << false << true << true << false;
    QTest::newRow("missing-command") << true << false << true << true;
    QTest::newRow("no-op-starter") << true << false << false << false;
}

void AiMcpStdioSessionTests::controllerLifetime() {
    QFETCH(bool, cross_thread);
    QFETCH(bool, by_destructor);
    QFETCH(bool, start_worker);
    QFETCH(bool, missing_command);
    ControllerObservation observed;
    // Declare creator first: any early test return destroys the session while its creator
    // remains alive, then releases and joins the creator without depending on a Qt event loop.
    std::unique_ptr<SessionCreator> creator;
    OpenedSession created;
    if (cross_thread) {
        creator = std::make_unique<SessionCreator>(observed, start_worker, missing_command);
        QVERIFY(creator->opened().wait_for(kCreatorDeadline) == std::future_status::ready);
        created = creator->opened().get();
    } else {
        created = openObservedSession(observed, start_worker, missing_command);
    }
    if (!start_worker || missing_command) {
        QVERIFY(!created.opened.has_value());
        const QString expected =
            start_worker ? QStringLiteral("MCP stdio command missing: %1")
                               .arg(QDir(QCoreApplication::applicationDirPath())
                                        .filePath(QStringLiteral("no_such_mcp_server_binary.exe")))
                         : QStringLiteral("Could not start MCP session worker thread");
        QCOMPARE(created.opened.error(), expected);
    } else {
        QVERIFY2(created.opened.has_value(), qPrintable(failureDiagnostic(created.opened)));
        verifyTwoDiscoveredPages(*created.session);
    }
    if (!by_destructor) {
        QVERIFY(created.session->close().has_value());
        QVERIFY(!created.session->isOpen());
        QVERIFY(created.session->close().has_value());
    }
    created.session.reset();
    QCOMPARE(observed.allocated.load(), 1);
    QCOMPARE(observed.destroyed.load(), 1);
    QVERIFY2(observed.matching_affinity.load() == 1,
             "Actual QThread controller destroyed outside its QObject affinity thread");
    QCOMPARE(observed.detached.load(), 1);
    QCOMPARE(observed.finished.load(), start_worker ? 1 : 0);
    QCOMPARE(observed.finished_at_destruction.load(), start_worker ? 1 : 0);
}

void AiMcpStdioSessionTests::resettingStarterRestoresRealStartup() {
    sak::ai::AiMcpStdioSession session;
    session.setThreadStarterForTesting([](QThread&) {});
    const auto refused = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY(!refused.has_value());
    QCOMPARE(refused.error(), QStringLiteral("Could not start MCP session worker thread"));
    session.setThreadStarterForTesting({});
    const auto opened = openFakeSession(&session, QStringLiteral("paginate"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    verifyTwoDiscoveredPages(session);
}

void AiMcpStdioSessionTests::isOpenClearsAfterServerExit() {
    sak::ai::AiMcpStdioSession session;
    const auto opened = openFakeSession(&session, QStringLiteral("exit-after-handshake"));
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    QTRY_VERIFY_WITH_TIMEOUT(!session.isOpen(), kFakeRequestTimeoutMs);
    QVERIFY(session.close().has_value());
}

void AiMcpStdioSessionTests::builtInMcpPersistentSession() {
    QTemporaryDir local_app_data;
    QVERIFY(local_app_data.isValid());
    const QString parent_local_app_data =
        qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment);
    const QString command = native_mcp::builtInCommand();
    const QProcessEnvironment environment =
        native_mcp::environment(local_app_data.path(), native_mcp::SecurityProfile::ReadOnly);
    // This preflight precedes open(): a broken mode route cannot launch the GUI or touch the real
    // browser-authority path.
    const auto preflight = native_mcp::validateBeforeLaunch(command,
                                                            environment,
                                                            local_app_data.path(),
                                                            native_mcp::SecurityProfile::ReadOnly,
                                                            QStringLiteral("health_check"));
    QVERIFY2(preflight.has_value(), qPrintable(failureDiagnostic(preflight)));
    QCOMPARE(qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment), parent_local_app_data);

    sak::ai::AiMcpStdioSession session;
    const auto opened = session.open({.command = command,
                                      .environment = environment,
                                      .request_timeout_ms = native_mcp::kRequestTimeoutMs});
    QVERIFY2(opened.has_value(), qPrintable(failureDiagnostic(opened)));
    QVERIFY(session.isOpen());
    const QString rendezvous_path = native_mcp::rendezvousPath(local_app_data.path());
    QVERIFY2(QFileInfo(rendezvous_path).isFile(), qPrintable(rendezvous_path));

    // Catalog inspection is safe: no desktop, clipboard, browser or registry tool is executed.
    const auto tools = session.listTools();
    QVERIFY2(tools.has_value(), qPrintable(failureDiagnostic(tools)));
    QVERIFY(native_mcp::containsTool(*tools, QStringLiteral("health_check")));
    QVERIFY(native_mcp::containsTool(*tools, QStringLiteral("list_windows")));
    QVERIFY(!native_mcp::containsTool(*tools, QStringLiteral("clipboard_write")));

    const auto first_result = session.callTool(QStringLiteral("health_check"), QJsonObject{});
    QVERIFY2(first_result.has_value(), qPrintable(failureDiagnostic(first_result)));
    const auto first_pid = native_mcp::healthPid(*first_result);
    QVERIFY2(first_pid.has_value(), qPrintable(failureDiagnostic(first_pid)));
    auto primary_generation = native_mcp::ProcessGeneration::observe(*first_pid);
    QVERIFY2(primary_generation.has_value(), qPrintable(failureDiagnostic(primary_generation)));
    const auto initially_alive = primary_generation->isAlive();
    QVERIFY2(initially_alive.has_value(), qPrintable(failureDiagnostic(initially_alive)));
    QVERIFY(*initially_alive);

    const auto repeated_result = session.callTool(QStringLiteral("health_check"), QJsonObject{});
    QVERIFY2(repeated_result.has_value(), qPrintable(failureDiagnostic(repeated_result)));
    const auto repeated_pid = native_mcp::healthPid(*repeated_result);
    QVERIFY2(repeated_pid.has_value(), qPrintable(failureDiagnostic(repeated_pid)));
    QCOMPARE(*repeated_pid, *first_pid);
    const auto alive_after_repeat = primary_generation->isAlive();
    QVERIFY2(alive_after_repeat.has_value(), qPrintable(failureDiagnostic(alive_after_repeat)));
    QVERIFY(*alive_after_repeat);

    QVERIFY(session.close().has_value());
    QVERIFY(!session.isOpen());
    const auto primary_exit = primary_generation->waitForExit();
    QVERIFY2(primary_exit.has_value(), qPrintable(failureDiagnostic(primary_exit)));
    QCOMPARE(qEnvironmentVariable(native_mcp::kLocalAppDataEnvironment), parent_local_app_data);
}

int main(int argc, char* argv[]) {
    const QString fake_mode = qEnvironmentVariable(kFakeMcpModeEnv).trimmed();
    if (!fake_mode.isEmpty()) {
        const native_mcp::OwnedChildDeadline deadline(kFakeProcessLifetimeMs,
                                                      kFakeProcessDeadlineExit);
        return runFakeMcpServer(fake_mode);
    }
    QCoreApplication application(argc, argv);
    AiMcpStdioSessionTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_ai_mcp_stdio_session.moc"
