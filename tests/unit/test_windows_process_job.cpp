// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_process_job.h"

#include "../support/ai_mcp_native_fixture.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QtTest/QtTest>

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <io.h>
#include <private/qobject_p.h>
#include <private/qprocess_p.h>
#include <private/qwindowspipereader_p.h>

namespace {

using Job = sak::WindowsProcessJob;
using Generation = sak::testfixtures::mcp::ProcessGeneration;
using sak::testfixtures::mcp::failureDiagnostic;
using sak::testfixtures::mcp::OwnedChildDeadline;
constexpr int kChildLifetimeMs = 10'000;
constexpr int kProcessWaitMs = 5000;
constexpr int kPollMs = 10;
constexpr int kWatchdogExit = 93;
constexpr int kWatchdogProbeLifetimeMs = 500;
constexpr int kFixtureFailureExit = 94;
constexpr int kUnexpectedDestructorReturn = 95;
constexpr DWORD kInjectedError = ERROR_ACCESS_DENIED;
constexpr UINT kExpectedTerminationExit = 197;
constexpr WORD kStartupReservedBytes = 7;
constexpr DWORD kControlledTreeProcesses = 2;
constexpr qint64 kExpectedMaximumPipeBytes = 64 * 1024 * 1024;
constexpr qint64 kTestStdoutPipeBytes = 4096;
constexpr qint64 kTestStderrPipeBytes = 2048;
constexpr auto kChildSwitch = "--job-child";

class FixtureEvent {
public:
    FixtureEvent() : m_handle(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    FixtureEvent(const FixtureEvent&) = delete;
    FixtureEvent& operator=(const FixtureEvent&) = delete;
    ~FixtureEvent() {
        if (m_handle != nullptr && ::CloseHandle(m_handle) == FALSE) {
            qFatal("Could not close owned test event");
        }
    }
    HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle{nullptr};
};

struct StartupFixture {
    enum Coordinate : DWORD {
        X = 11,
        Y = 13,
        Width = 17,
        Height = 19,
        Columns = 23,
        Rows = 29
    };
    std::wstring application{L"owned.exe"};
    std::wstring command{L"owned.exe argument"};
    std::wstring title{L"owned title"};
    std::wstring desktop{L"owned desktop"};
    std::wstring directory{L"C:\\owned"};
    std::array<BYTE, kStartupReservedBytes> reserved{};
    SECURITY_ATTRIBUTES process_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    SECURITY_ATTRIBUTES thread_attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    PROCESS_INFORMATION information{};
    STARTUPINFOW startup{};

    StartupFixture(HANDLE input, HANDLE output, HANDLE error) {
        startup.cb = sizeof(startup);
        startup.lpReserved = command.data();
        startup.lpDesktop = desktop.data();
        startup.lpTitle = title.data();
        startup.dwX = X;
        startup.dwY = Y;
        startup.dwXSize = Width;
        startup.dwYSize = Height;
        startup.dwXCountChars = Columns;
        startup.dwYCountChars = Rows;
        startup.dwFillAttribute = FOREGROUND_GREEN;
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.cbReserved2 = kStartupReservedBytes;
        startup.lpReserved2 = reserved.data();
        startup.hStdInput = input;
        startup.hStdOutput = output;
        startup.hStdError = error;
    }

    QProcess::CreateProcessArguments arguments() {
        return {application.c_str(),
                command.data(),
                &process_attributes,
                &thread_attributes,
                true,
                CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
                reserved.data(),
                directory.c_str(),
                &startup,
                &information};
    }
};

enum class ApiSlot {
    Create,
    Limits,
    Initialize,
    Update,
    Delete,
    Query,
    Terminate,
    Close,
    Error
};

enum class PrepFault {
    None,
    Create,
    Limits,
    SizeFailure,
    SizeSuccess,
    SizeZero,
    Initialize,
    Update
};

struct ApiObservation {
    PrepFault preparation{PrepFault::None};
    bool fail_query{false};
    bool fail_terminate{false};
    bool fail_close{false};
    int creations{0};
    int closures{0};
    int attribute_deletions{0};
    DWORD limit_flags{0};
    DWORD_PTR attribute_key{0};
    SIZE_T attribute_bytes{0};
    HANDLE created_handle{nullptr};
    HANDLE attribute_job{nullptr};
    UINT termination_code{0};
};

BOOL injectFailure() {
    ::SetLastError(kInjectedError);
    return FALSE;
}

bool prepareBinaryStdio() {
    return _setmode(_fileno(stdin), _O_BINARY) != -1 &&
           _setmode(_fileno(stdout), _O_BINARY) != -1 && _setmode(_fileno(stderr), _O_BINARY) != -1;
}

bool usesControlInput(const QString& mode) {
    return mode == QLatin1String("echo") || mode == QLatin1String("tree") ||
           mode == QLatin1String("watchdog");
}

int runOrdinaryChild(const QString& mode) {
    const int lifetime = mode == QLatin1String("watchdog") ? kWatchdogProbeLifetimeMs
                                                           : kChildLifetimeMs;
    OwnedChildDeadline deadline(lifetime, kWatchdogExit);
    if (mode == QLatin1String("leaf")) {
        QThread::msleep(kChildLifetimeMs);
        return 0;
    }
    if (!usesControlInput(mode)) {
        return kFixtureFailureExit;
    }
    if (!prepareBinaryStdio()) {
        return kFixtureFailureExit;
    }
    qint64 descendant = 0;
    if (mode == QLatin1String("tree") &&
        !QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                 {QString::fromLatin1(kChildSwitch), QStringLiteral("leaf")},
                                 QString(),
                                 &descendant)) {
        return kFixtureFailureExit;
    }
    QTextStream output(stdout, QIODevice::WriteOnly);
    QTextStream error(stderr, QIODevice::WriteOnly);
    output.setEncoding(QStringConverter::Utf8);
    error.setEncoding(QStringConverter::Utf8);
    const QJsonObject ready{{QStringLiteral("pid"), QCoreApplication::applicationPid()},
                            {QStringLiteral("descendant"), descendant}};
    output << QString::fromUtf8(QJsonDocument(ready).toJson(QJsonDocument::Compact)) << Qt::endl;
    error << "owned-stderr-ready" << Qt::endl;
    QTextStream input(stdin, QIODevice::ReadOnly);
    input.setEncoding(QStringConverter::Utf8);
    const QString line = input.readLine();
    if (mode == QLatin1String("watchdog")) {
        return kUnexpectedDestructorReturn;
    }
    if (line == QLatin1String("exit-parent")) {
        return 0;
    }
    output << "echo:" << line << Qt::endl;
    error << "owned-stderr-finished" << Qt::endl;
    return 0;
}

std::expected<QJsonObject, QString> readReady(QProcess& process) {
    QElapsedTimer clock;
    clock.start();
    while (!process.canReadLine() && clock.elapsed() < kProcessWaitMs) {
        const int remaining = kProcessWaitMs - static_cast<int>(clock.elapsed());
        if (remaining <= 0 || !process.waitForReadyRead(remaining)) {
            break;
        }
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(process.readLine(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("Owned child did not emit its readiness object"));
    }
    return document.object();
}

std::expected<void, QString> writeOwnedLine(QProcess& process, const QByteArray& line) {
    if (process.write(line) != line.size()) {
        return std::unexpected(QStringLiteral("Owned stdin write was incomplete"));
    }
    if (process.bytesToWrite() > 0 && !process.waitForBytesWritten(kProcessWaitMs)) {
        return std::unexpected(QStringLiteral("Owned stdin write did not complete"));
    }
    return {};
}

std::expected<void, QString> waitForEmpty(const Job& job) {
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < kProcessWaitMs) {
        const auto count = job.accounting();
        if (!count) {
            return std::unexpected(count.error());
        }
        if (count->active_processes == 0) {
            return {};
        }
        QThread::msleep(kPollMs);
    }
    return std::unexpected(QStringLiteral("Owned Job did not reach zero active members"));
}

std::expected<bool, QString> belongsToJob(qint64 pid, HANDLE job) {
    if (pid <= 0 || static_cast<quint64>(pid) > std::numeric_limits<DWORD>::max()) {
        return std::unexpected(QStringLiteral("Invalid owned process id"));
    }
    HANDLE handle =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return std::unexpected(QStringLiteral("Could not inspect owned process membership"));
    }
    BOOL member = FALSE;
    const BOOL queried = ::IsProcessInJob(handle, job, &member);
    const BOOL closed = ::CloseHandle(handle);
    if (queried == FALSE || closed == FALSE) {
        return std::unexpected(QStringLiteral("Owned membership query/handle close failed"));
    }
    return member != FALSE;
}

void verifyStartupFields(const STARTUPINFOW& actual, const STARTUPINFOW& original) {
    QCOMPARE(actual.cb, static_cast<DWORD>(sizeof(STARTUPINFOEXW)));
    QCOMPARE(actual.lpReserved, original.lpReserved);
    QCOMPARE(actual.lpDesktop, original.lpDesktop);
    QCOMPARE(actual.lpTitle, original.lpTitle);
    QCOMPARE(actual.dwX, original.dwX);
    QCOMPARE(actual.dwY, original.dwY);
    QCOMPARE(actual.dwXSize, original.dwXSize);
    QCOMPARE(actual.dwYSize, original.dwYSize);
    QCOMPARE(actual.dwXCountChars, original.dwXCountChars);
    QCOMPARE(actual.dwYCountChars, original.dwYCountChars);
    QCOMPARE(actual.dwFillAttribute, original.dwFillAttribute);
    QCOMPARE(actual.dwFlags, original.dwFlags);
    QCOMPARE(actual.wShowWindow, original.wShowWindow);
    QCOMPARE(actual.cbReserved2, original.cbReserved2);
    QCOMPARE(actual.lpReserved2, original.lpReserved2);
    QCOMPARE(actual.hStdInput, original.hStdInput);
    QCOMPARE(actual.hStdOutput, original.hStdOutput);
    QCOMPARE(actual.hStdError, original.hStdError);
}

struct PipeCapObservation {
    int modifier_calls{0};
    qint64 pid_before_create{-1};
    qint64 stdout_cap{-1};
    qint64 stderr_cap{-1};
    QWindowsPipeReader* stdout_reader{nullptr};
    QWindowsPipeReader* stderr_reader{nullptr};
};

bool installPipeCapProbe(QProcess& process, PipeCapObservation& observed) {
    const auto job_modifier = process.createProcessArgumentsModifier();
    if (!job_modifier) {
        return false;
    }
    process.setCreateProcessArgumentsModifier(
        [&process, &observed, job_modifier](QProcess::CreateProcessArguments* arguments) {
            job_modifier(arguments);
            ++observed.modifier_calls;
            observed.pid_before_create = process.processId();
            const auto* state = static_cast<QProcessPrivate*>(QObjectPrivate::get(&process));
            if (state->stdoutChannel.type != QProcessPrivate::Channel::Normal ||
                state->stderrChannel.type != QProcessPrivate::Channel::Normal) {
                return;
            }
            observed.stdout_reader = state->stdoutChannel.reader;
            observed.stderr_reader = state->stderrChannel.reader;
            if (observed.stdout_reader != nullptr && observed.stderr_reader != nullptr) {
                observed.stdout_cap = observed.stdout_reader->maxReadBufferSize();
                observed.stderr_cap = observed.stderr_reader->maxReadBufferSize();
            }
        });
    return true;
}

}  // namespace

Q_DECLARE_METATYPE(ApiSlot)
Q_DECLARE_METATYPE(PrepFault)

class WindowsProcessJobTests : public QObject {
    Q_OBJECT

public:
    static int runDestructorChild(const QString& mode);

private Q_SLOTS:
    void incompleteApiRefused_data();
    void incompleteApiRefused();
    void preparationFailures_data();
    void preparationFailures();
    void preparesActualJobAndAttribute();
    void modifierPreservesArguments();
    void invalidPipeLimitsRefusedBeforeLaunch_data();
    void invalidPipeLimitsRefusedBeforeLaunch();
    void nativeLaunchInstallsBothPipeCapsBeforeChildCreation();
    void hookLifecycleRefusals();
    void destroyedLaunchProcessRefusedSafely();
    void completionRequiresInvocationAndMembership();
    void failedNativeLaunchClearsHook();
    void nativeStdioAndEmptyCompletion();
    void watchdogTerminatesBlockedStdin();
    void startupDescendantSurvivesPrimaryUntilTermination();
    void queryFailureRetainsHandle();
    void terminateFailureRetainsHandle();
    void closeFailureRetainsHandle();
    void destructorRefusesUnprovedCleanup_data();
    void destructorRefusesUnprovedCleanup();

private:
    using Api = Job::Api;
    static Api observedApi(const std::shared_ptr<ApiObservation>& state);
    static void observePreparation(Api& api, const std::shared_ptr<ApiObservation>& state);
    static void observeAttributePreparation(Api& api, const std::shared_ptr<ApiObservation>& state);
    static void clearApiSlot(Api& api, ApiSlot slot);
    static std::expected<void, QString> launch(Job& job, QProcess& process, const QString& mode);
    static void cleanOwned(Job& job, QProcess* process = nullptr);
    static void clearOwnedLaunch(Job& job);
};

void WindowsProcessJobTests::observePreparation(Api& api,
                                                const std::shared_ptr<ApiObservation>& state) {
    const auto create = api.create_job;
    api.create_job = [state, create](LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) -> HANDLE {
        ++state->creations;
        if (state->preparation == PrepFault::Create) {
            injectFailure();
            return nullptr;
        }
        state->created_handle = create(attributes, name);
        return state->created_handle;
    };
    const auto limits = api.set_information;
    api.set_information = [state,
                           limits](HANDLE job, JOBOBJECTINFOCLASS kind, LPVOID info, DWORD size) {
        if (state->preparation == PrepFault::Limits) {
            return injectFailure();
        }
        state->limit_flags = static_cast<JOBOBJECT_EXTENDED_LIMIT_INFORMATION*>(info)
                                 ->BasicLimitInformation.LimitFlags;
        return limits(job, kind, info, size);
    };
    observeAttributePreparation(api, state);
}

void WindowsProcessJobTests::observeAttributePreparation(
    Api& api, const std::shared_ptr<ApiObservation>& state) {
    const auto initialize = api.initialize_attributes;
    api.initialize_attributes = [state, initialize](LPPROC_THREAD_ATTRIBUTE_LIST list,
                                                    DWORD count,
                                                    DWORD flags,
                                                    PSIZE_T size) -> BOOL {
        if (list == nullptr && state->preparation == PrepFault::SizeSuccess) {
            return TRUE;
        }
        if (list == nullptr && state->preparation == PrepFault::SizeZero) {
            *size = 0;
            ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        if ((list == nullptr && state->preparation == PrepFault::SizeFailure) ||
            (list != nullptr && state->preparation == PrepFault::Initialize)) {
            return injectFailure();
        }
        return initialize(list, count, flags, size);
    };
    const auto update = api.update_attribute;
    api.update_attribute = [state, update](LPPROC_THREAD_ATTRIBUTE_LIST list,
                                           DWORD flags,
                                           DWORD_PTR key,
                                           PVOID value,
                                           SIZE_T size,
                                           PVOID previous,
                                           PSIZE_T returned) {
        state->attribute_key = key;
        state->attribute_bytes = size;
        state->attribute_job = *static_cast<HANDLE*>(value);
        if (state->preparation == PrepFault::Update) {
            return injectFailure();
        }
        return update(list, flags, key, value, size, previous, returned);
    };
}

WindowsProcessJobTests::Api WindowsProcessJobTests::observedApi(
    const std::shared_ptr<ApiObservation>& state) {
    Api api = Job::nativeApi();
    observePreparation(api, state);
    const auto erase = api.delete_attributes;
    api.delete_attributes = [state, erase](LPPROC_THREAD_ATTRIBUTE_LIST list) {
        ++state->attribute_deletions;
        erase(list);
    };
    const auto query = api.query_information;
    api.query_information =
        [state,
         query](HANDLE job, JOBOBJECTINFOCLASS kind, LPVOID info, DWORD size, LPDWORD returned) {
            return state->fail_query ? injectFailure() : query(job, kind, info, size, returned);
        };
    const auto terminate = api.terminate_job;
    api.terminate_job = [state, terminate](HANDLE job, UINT code) {
        state->termination_code = code;
        return state->fail_terminate ? injectFailure() : terminate(job, code);
    };
    const auto close = api.close_handle;
    api.close_handle = [state, close](HANDLE handle) {
        if (state->fail_close) {
            return injectFailure();
        }
        const BOOL result = close(handle);
        if (result != FALSE) {
            ++state->closures;
        }
        return result;
    };
    return api;
}

void WindowsProcessJobTests::clearApiSlot(Api& api, ApiSlot slot) {
    switch (slot) {
    case ApiSlot::Create:
        api.create_job = {};
        return;
    case ApiSlot::Limits:
        api.set_information = {};
        return;
    case ApiSlot::Initialize:
        api.initialize_attributes = {};
        return;
    case ApiSlot::Update:
        api.update_attribute = {};
        return;
    case ApiSlot::Delete:
        api.delete_attributes = {};
        return;
    case ApiSlot::Query:
        api.query_information = {};
        return;
    case ApiSlot::Terminate:
        api.terminate_job = {};
        return;
    case ApiSlot::Close:
        api.close_handle = {};
        return;
    case ApiSlot::Error:
        api.last_error = {};
        return;
    }
}

std::expected<void, QString> WindowsProcessJobTests::launch(Job& job,
                                                            QProcess& process,
                                                            const QString& mode) {
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QString::fromLatin1(kChildSwitch), mode});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    const auto installed = job.installOn(process);
    if (!installed) {
        return std::unexpected(installed.error());
    }
    process.start();
    const bool started = process.waitForStarted(kProcessWaitMs);
    const auto completed = job.completeLaunch(process, started);
    if (!completed) {
        return std::unexpected(completed.error());
    }
    if (!started) {
        return std::unexpected(process.errorString());
    }
    return {};
}

void WindowsProcessJobTests::clearOwnedLaunch(Job& job) {
    if (job.m_installed_process != nullptr) {
        const auto completed = job.completeLaunch(*job.m_installed_process, false);
        if (!completed) {
            qFatal("Owned fixture launch cleanup failed: %s", qPrintable(completed.error()));
        }
    }
}

void WindowsProcessJobTests::cleanOwned(Job& job, QProcess* process) {
    clearOwnedLaunch(job);
    if (job.m_job != nullptr) {
        const auto terminated = job.terminate();
        if (!terminated) {
            qFatal("Owned fixture termination failed: %s", qPrintable(terminated.error()));
        }
    }
    if (process != nullptr && process->state() != QProcess::NotRunning) {
        process->kill();  // Only this QProcess-owned probe, including failed Job association.
        if (!process->waitForFinished(kProcessWaitMs)) {
            qFatal("Owned fixture primary did not exit");
        }
    }
    if (job.m_job != nullptr) {
        const auto empty = waitForEmpty(job);
        if (!empty) {
            qFatal("Owned fixture Job did not empty: %s", qPrintable(empty.error()));
        }
        const auto closed = job.closeEmpty();
        if (!closed) {
            qFatal("Owned fixture close failed: %s", qPrintable(closed.error()));
        }
    }
}

void WindowsProcessJobTests::incompleteApiRefused_data() {
    QTest::addColumn<ApiSlot>("slot");
    QTest::newRow("create") << ApiSlot::Create;
    QTest::newRow("limits") << ApiSlot::Limits;
    QTest::newRow("initialize") << ApiSlot::Initialize;
    QTest::newRow("update") << ApiSlot::Update;
    QTest::newRow("delete") << ApiSlot::Delete;
    QTest::newRow("query") << ApiSlot::Query;
    QTest::newRow("terminate") << ApiSlot::Terminate;
    QTest::newRow("close") << ApiSlot::Close;
    QTest::newRow("last-error") << ApiSlot::Error;
}

void WindowsProcessJobTests::incompleteApiRefused() {
    QFETCH(ApiSlot, slot);
    auto state = std::make_shared<ApiObservation>();
    auto api = observedApi(state);
    clearApiSlot(api, slot);
    const auto created = Job::createWithApi(std::move(api));
    QVERIFY(!created);
    QCOMPARE(created.error(), QStringLiteral("Windows process Job API table is incomplete"));
    QCOMPARE(state->creations, 0);
}

void WindowsProcessJobTests::preparationFailures_data() {
    QTest::addColumn<PrepFault>("fault");
    QTest::addColumn<QString>("operation");
    QTest::addColumn<DWORD>("error_code");
    QTest::newRow("create") << PrepFault::Create
                            << QStringLiteral("Create unnamed Windows process Job")
                            << kInjectedError;
    QTest::newRow("limits") << PrepFault::Limits
                            << QStringLiteral("Set Windows process Job kill-on-close limit")
                            << kInjectedError;
    QTest::newRow("sizing-refused")
        << PrepFault::SizeFailure << QStringLiteral("Size Windows process Job attribute list")
        << kInjectedError;
    QTest::newRow("sizing-unexpected-success")
        << PrepFault::SizeSuccess << QStringLiteral("Size Windows process Job attribute list")
        << DWORD{ERROR_SUCCESS};
    QTest::newRow("sizing-zero") << PrepFault::SizeZero
                                 << QStringLiteral("Size Windows process Job attribute list")
                                 << DWORD{ERROR_INSUFFICIENT_BUFFER};
    QTest::newRow("initialize") << PrepFault::Initialize
                                << QStringLiteral("Initialize Windows process Job attribute list")
                                << kInjectedError;
    QTest::newRow("attribute") << PrepFault::Update
                               << QStringLiteral("Attach Windows process Job launch attribute")
                               << kInjectedError;
}

void WindowsProcessJobTests::preparationFailures() {
    QFETCH(PrepFault, fault);
    QFETCH(QString, operation);
    QFETCH(DWORD, error_code);
    auto state = std::make_shared<ApiObservation>();
    state->preparation = fault;
    const auto created = Job::createWithApi(observedApi(state));
    QVERIFY(!created);
    QCOMPARE(created.error(), QStringLiteral("%1 failed (err=%2)").arg(operation).arg(error_code));
    QCOMPARE(state->creations, 1);
    QCOMPARE(state->closures, fault == PrepFault::Create ? 0 : 1);
    QCOMPARE(state->attribute_deletions, fault == PrepFault::Update ? 1 : 0);
}

void WindowsProcessJobTests::preparesActualJobAndAttribute() {
    auto state = std::make_shared<ApiObservation>();
    auto created = Job::createWithApi(observedApi(state));
    QVERIFY2(created.has_value(), qPrintable(failureDiagnostic(created)));
    auto& job = **created;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job); });
    QCOMPARE(state->limit_flags, DWORD{JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE});
    QCOMPARE(state->attribute_key, DWORD_PTR{PROC_THREAD_ATTRIBUTE_JOB_LIST});
    QCOMPARE(state->attribute_bytes, sizeof(HANDLE));
    QCOMPARE(state->attribute_job, state->created_handle);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    QVERIFY(::QueryInformationJobObject(
                job.m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) !=
            FALSE);
    QCOMPARE(limits.BasicLimitInformation.LimitFlags, DWORD{JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE});
    const auto count = job.accounting();
    QVERIFY2(count.has_value(), qPrintable(failureDiagnostic(count)));
    QCOMPARE(count->total_processes, DWORD{0});
    QCOMPARE(count->active_processes, DWORD{0});
    QVERIFY(job.closeEmpty().has_value());
    QVERIFY(job.closeEmpty().has_value());
    QCOMPARE(state->closures, 1);
    QCOMPARE(state->attribute_deletions, 1);
    QVERIFY(!job.accounting());
    QVERIFY(!job.terminate());
    QProcess process;
    QVERIFY(!job.installOn(process));
}

void WindowsProcessJobTests::modifierPreservesArguments() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    QVERIFY(job.installOn(process).has_value());
    FixtureEvent input;
    FixtureEvent output;
    FixtureEvent error;
    QVERIFY(input.get() != nullptr && output.get() != nullptr && error.get() != nullptr);
    StartupFixture fixture(input.get(), output.get(), error.get());
    const STARTUPINFOW before = fixture.startup;
    auto arguments = fixture.arguments();
    const auto modifier = process.createProcessArgumentsModifier();
    QVERIFY(static_cast<bool>(modifier));
    modifier(&arguments);
    verifyStartupFields(*arguments.startupInfo, before);
    QCOMPARE(arguments.flags,
             DWORD{CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT});
    QCOMPARE(arguments.processInformation, &fixture.information);
    QCOMPARE(arguments.applicationName, fixture.application.c_str());
    QCOMPARE(arguments.arguments, fixture.command.data());
    QCOMPARE(arguments.environment, static_cast<void*>(fixture.reserved.data()));
    QCOMPARE(arguments.currentDirectory, fixture.directory.c_str());
    QCOMPARE(arguments.processAttributes, &fixture.process_attributes);
    QCOMPARE(arguments.threadAttributes, &fixture.thread_attributes);
    QVERIFY(arguments.inheritHandles);
    const auto* extended = reinterpret_cast<const STARTUPINFOEXW*>(arguments.startupInfo);
    QCOMPARE(extended->lpAttributeList, job.m_attribute_list);
    QVERIFY(job.completeLaunch(process, false).has_value());
    QVERIFY(!process.createProcessArgumentsModifier());
    QVERIFY(job.m_attribute_list == nullptr);
}

void WindowsProcessJobTests::invalidPipeLimitsRefusedBeforeLaunch_data() {
    QTest::addColumn<qint64>("stdout_bytes");
    QTest::addColumn<qint64>("stderr_bytes");
    QTest::addColumn<bool>("merged_channels");
    QTest::newRow("zero-stdout") << qint64{0} << kTestStderrPipeBytes << false;
    QTest::newRow("zero-stderr") << kTestStdoutPipeBytes << qint64{0} << false;
    QTest::newRow("negative-stdout") << qint64{-1} << kTestStderrPipeBytes << false;
    QTest::newRow("negative-stderr") << kTestStdoutPipeBytes << qint64{-1} << false;
    QTest::newRow("oversize-stdout")
        << kExpectedMaximumPipeBytes + 1 << kTestStderrPipeBytes << false;
    QTest::newRow("oversize-stderr")
        << kTestStdoutPipeBytes << kExpectedMaximumPipeBytes + 1 << false;
    QTest::newRow("merged-channels") << kTestStdoutPipeBytes << kTestStderrPipeBytes << true;
}

void WindowsProcessJobTests::invalidPipeLimitsRefusedBeforeLaunch() {
    QFETCH(qint64, stdout_bytes);
    QFETCH(qint64, stderr_bytes);
    QFETCH(bool, merged_channels);
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QString::fromLatin1(kChildSwitch), QStringLiteral("echo")});
    process.setProcessChannelMode(merged_channels ? QProcess::MergedChannels
                                                  : QProcess::SeparateChannels);

    const auto refused = job.installOn(process,
                                       sak::WindowsJobPipeLimits{stdout_bytes, stderr_bytes});
    QVERIFY(!refused);
    QCOMPARE(refused.error(),
             merged_channels ? QStringLiteral("Windows process Job pipe limits require separate "
                                              "channels")
                             : QStringLiteral("Windows process Job pipe limits are invalid"));
    QCOMPARE(process.state(), QProcess::NotRunning);
    QVERIFY(!process.createProcessArgumentsModifier());
    QVERIFY(job.m_installed_process == nullptr);
    QVERIFY(!job.m_pipe_limits.has_value());
    const auto count = job.accounting();
    QVERIFY2(count.has_value(), qPrintable(failureDiagnostic(count)));
    QCOMPARE(count->total_processes, DWORD{0});
    QCOMPARE(count->active_processes, DWORD{0});
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::nativeLaunchInstallsBothPipeCapsBeforeChildCreation() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QString::fromLatin1(kChildSwitch), QStringLiteral("echo")});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    const auto installed = job.installOn(
        process, sak::WindowsJobPipeLimits{kTestStdoutPipeBytes, kTestStderrPipeBytes});
    QVERIFY2(installed.has_value(), qPrintable(failureDiagnostic(installed)));

    PipeCapObservation observed;
    QVERIFY(installPipeCapProbe(process, observed));

    process.start();
    const bool started = process.waitForStarted(kProcessWaitMs);
    const auto completed = job.completeLaunch(process, started);
    QVERIFY2(completed.has_value(), qPrintable(failureDiagnostic(completed)));
    QVERIFY2(started, qPrintable(process.errorString()));
    QCOMPARE(observed.modifier_calls, 1);
    QCOMPARE(observed.pid_before_create, qint64{0});
    QVERIFY(observed.stdout_reader != nullptr && observed.stderr_reader != nullptr);
    QVERIFY(observed.stdout_reader != observed.stderr_reader);
    QCOMPARE(observed.stdout_cap, kTestStdoutPipeBytes);
    QCOMPARE(observed.stderr_cap, kTestStderrPipeBytes);
    const auto* state = static_cast<QProcessPrivate*>(QObjectPrivate::get(&process));
    QCOMPARE(state->stdoutChannel.reader, observed.stdout_reader);
    QCOMPARE(state->stderrChannel.reader, observed.stderr_reader);
    QCOMPARE(state->stdoutChannel.reader->maxReadBufferSize(), kTestStdoutPipeBytes);
    QCOMPARE(state->stderrChannel.reader->maxReadBufferSize(), kTestStderrPipeBytes);

    const qint64 child_pid = process.processId();
    auto generation = Generation::observe(child_pid);
    QVERIFY(generation.has_value());
    const auto member = belongsToJob(process.processId(), job.m_job);
    QVERIFY2(member.has_value() && *member, qPrintable(failureDiagnostic(member)));
    const QByteArray payload = QByteArrayLiteral("pipe-limits\n");
    QVERIFY(writeOwnedLine(process, payload).has_value());
    process.closeWriteChannel();
    QVERIFY(process.waitForFinished(kProcessWaitMs));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    const QByteArray output = process.readAllStandardOutput();
    const qsizetype first_line_end = output.indexOf('\n');
    QVERIFY(first_line_end > 0);
    const QJsonObject ready = QJsonDocument::fromJson(output.first(first_line_end)).object();
    QCOMPARE(ready.value(QStringLiteral("pid")).toInteger(), child_pid);
    QCOMPARE(output.mid(first_line_end + 1), QByteArrayLiteral("echo:") + payload);
    QCOMPARE(process.readAllStandardError(),
             QByteArrayLiteral("owned-stderr-ready\nowned-stderr-finished\n"));
    QVERIFY(generation->waitForExit().has_value());
    QVERIFY(waitForEmpty(job).has_value());
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::hookLifecycleRefusals() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    QProcess other;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    int original_calls = 0;
    process.setCreateProcessArgumentsModifier(
        [&](QProcess::CreateProcessArguments*) { ++original_calls; });
    const auto occupied = job.installOn(process);
    QVERIFY(!occupied);
    QCOMPARE(occupied.error(),
             QStringLiteral("QProcess already has a CreateProcess arguments modifier"));
    process.createProcessArgumentsModifier()(nullptr);
    QCOMPARE(original_calls, 1);
    process.setCreateProcessArgumentsModifier({});
    QVERIFY(job.installOn(process).has_value());
    QVERIFY(!job.installOn(other));
    const auto hooked_close = job.closeEmpty();
    QVERIFY(!hooked_close);
    QVERIFY(hooked_close.error().contains(QStringLiteral("hook is still installed")));
    const auto mismatch = job.completeLaunch(other, false);
    QVERIFY(!mismatch);
    QVERIFY(mismatch.error().contains(QStringLiteral("does not match")));
    QVERIFY(!process.createProcessArgumentsModifier());
    QVERIFY(!other.createProcessArgumentsModifier());
    QVERIFY(!job.installOn(process));
    QVERIFY(!job.completeLaunch(process, false));
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::completionRequiresInvocationAndMembership() {
    for (const bool invoke : {false, true}) {
        auto created = Job::create();
        QVERIFY(created.has_value());
        auto& job = **created;
        QProcess process;
        const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
        QVERIFY(job.installOn(process).has_value());
        if (invoke) {
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            QProcess::CreateProcessArguments arguments{};
            arguments.startupInfo = &startup;
            process.createProcessArgumentsModifier()(&arguments);
        }
        const auto completed = job.completeLaunch(process, true);
        QVERIFY(!completed);
        QCOMPARE(completed.error(),
                 invoke ? QStringLiteral("QProcess JOB_LIST launch recorded no associated process")
                        : QStringLiteral("QProcess JOB_LIST launch modifier was not invoked"));
        QVERIFY(!process.createProcessArgumentsModifier());
        QVERIFY(job.m_attribute_list == nullptr);
        QVERIFY(job.closeEmpty().has_value());
    }
}

void WindowsProcessJobTests::destroyedLaunchProcessRefusedSafely() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job); });
    auto process = std::make_unique<QProcess>();
    QVERIFY(job.installOn(*process).has_value());
    process.reset();
    QProcess different;
    const auto completed = job.completeLaunch(different, false);
    QVERIFY(!completed);
    QCOMPARE(completed.error(), QStringLiteral("Windows process Job launch hook is not installed"));
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::failedNativeLaunchClearsHook() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    process.setProgram(QCoreApplication::applicationFilePath() + QStringLiteral(".absent"));
    QVERIFY(job.installOn(process).has_value());
    process.start();
    const bool started = process.waitForStarted(kProcessWaitMs);
    const auto completed = job.completeLaunch(process, started);
    QVERIFY(!started);
    QVERIFY(completed.has_value());
    QVERIFY(!process.createProcessArgumentsModifier());
    QVERIFY(job.m_attribute_list == nullptr);
    const auto count = job.accounting();
    QVERIFY(count.has_value());
    QCOMPARE(count->total_processes, DWORD{0});
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::nativeStdioAndEmptyCompletion() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    const auto launched = launch(job, process, QStringLiteral("echo"));
    QVERIFY2(launched.has_value(), qPrintable(failureDiagnostic(launched)));
    const auto ready = readReady(process);
    QVERIFY2(ready.has_value(), qPrintable(failureDiagnostic(ready)));
    QCOMPARE(ready->value(QStringLiteral("pid")).toInteger(), process.processId());
    auto generation = Generation::observe(process.processId());
    QVERIFY(generation.has_value());
    const auto member = belongsToJob(process.processId(), job.m_job);
    QVERIFY2(member.has_value() && *member, qPrintable(failureDiagnostic(member)));
    const QByteArray payload = QByteArrayLiteral("owned-input-\xc3\xa9\n");
    QVERIFY(writeOwnedLine(process, payload).has_value());
    process.closeWriteChannel();
    QVERIFY(process.waitForFinished(kProcessWaitMs));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    QCOMPARE(process.readAllStandardOutput(), QByteArrayLiteral("echo:") + payload);
    QCOMPARE(process.readAllStandardError(),
             QByteArrayLiteral("owned-stderr-ready\nowned-stderr-finished\n"));
    const auto stopped = generation->isAlive();
    QVERIFY(stopped.has_value() && !*stopped);
    QVERIFY(waitForEmpty(job).has_value());
    const auto count = job.accounting();
    QVERIFY(count.has_value());
    // The identity probe observed Windows conhost members too. Those may join/exit
    // asynchronously; require our named generation plus exact empty completion, not a snapshot
    // count of Windows support processes.
    QVERIFY(count->total_processes >= 1);
    QCOMPARE(count->active_processes, DWORD{0});
    QVERIFY(generation->waitForExit().has_value());
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::watchdogTerminatesBlockedStdin() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    const auto launched = launch(job, process, QStringLiteral("watchdog"));
    QVERIFY2(launched.has_value(), qPrintable(failureDiagnostic(launched)));
    const auto ready = readReady(process);
    QVERIFY2(ready.has_value(), qPrintable(failureDiagnostic(ready)));
    auto generation = Generation::observe(process.processId());
    QVERIFY(generation.has_value());
    // Stdin stays open and unfed: only the child's independent watchdog can finish this read.
    const auto exited = generation->waitForExit();
    QVERIFY2(exited.has_value(), qPrintable(failureDiagnostic(exited)));
    QCOMPARE(*exited, DWORD{kWatchdogExit});
    QVERIFY(process.waitForFinished(kProcessWaitMs));
    QCOMPARE(process.exitCode(), kWatchdogExit);
    QVERIFY(process.readAllStandardOutput().isEmpty());
    QCOMPARE(process.readAllStandardError(), QByteArrayLiteral("owned-stderr-ready\n"));
    QVERIFY(waitForEmpty(job).has_value());
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::startupDescendantSurvivesPrimaryUntilTermination() {
    auto created = Job::create();
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    const auto launched = launch(job, process, QStringLiteral("tree"));
    QVERIFY2(launched.has_value(), qPrintable(failureDiagnostic(launched)));
    const auto ready = readReady(process);
    QVERIFY(ready.has_value());
    const qint64 child_pid = ready->value(QStringLiteral("descendant")).toInteger();
    QVERIFY(child_pid > 0 && child_pid != process.processId());
    auto primary = Generation::observe(process.processId());
    auto descendant = Generation::observe(child_pid);
    QVERIFY(primary.has_value() && descendant.has_value());
    const auto member = belongsToJob(child_pid, job.m_job);
    QVERIFY2(member.has_value() && *member, qPrintable(failureDiagnostic(member)));
    const auto primary_member = belongsToJob(process.processId(), job.m_job);
    QVERIFY2(primary_member.has_value() && *primary_member,
             qPrintable(failureDiagnostic(primary_member)));
    const auto initial = job.accounting();
    QVERIFY(initial.has_value());
    QVERIFY(initial->total_processes >= kControlledTreeProcesses);
    QVERIFY(initial->active_processes >= kControlledTreeProcesses);
    const auto refused = job.closeEmpty();
    QVERIFY(!refused);
    QVERIFY(refused.error().contains(QStringLiteral("active processes")));
    QVERIFY(writeOwnedLine(process, QByteArrayLiteral("exit-parent\n")).has_value());
    QVERIFY(process.waitForFinished(kProcessWaitMs));
    QVERIFY(primary->waitForExit().has_value());
    const auto alive = descendant->isAlive();
    QVERIFY(alive.has_value() && *alive);
    const auto count = job.accounting();
    QVERIFY(count.has_value());
    // Total is cumulative; additional console hosts are not fixed-lifetime test participants.
    QVERIFY(count->total_processes >= initial->total_processes);
    QVERIFY(count->active_processes >= 1);
    QVERIFY(!job.closeEmpty());
    QVERIFY(job.terminate().has_value());
    // Keep the independent generation handle open while Job accounting reaches zero.
    QVERIFY(waitForEmpty(job).has_value());
    const auto descendant_exit = descendant->waitForExit();
    QVERIFY2(descendant_exit.has_value(), qPrintable(failureDiagnostic(descendant_exit)));
    QCOMPARE(*descendant_exit, DWORD{kExpectedTerminationExit});
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::queryFailureRetainsHandle() {
    auto state = std::make_shared<ApiObservation>();
    auto created = Job::createWithApi(observedApi(state));
    QVERIFY(created.has_value());
    auto& job = **created;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job); });
    const HANDLE original = job.m_job;
    state->fail_query = true;
    const auto query = job.accounting();
    const auto close = job.closeEmpty();
    state->fail_query = false;
    QVERIFY(!query && !close);
    QCOMPARE(
        query.error(),
        QStringLiteral("Query Windows process Job accounting failed (err=%1)").arg(kInjectedError));
    QCOMPARE(close.error(), query.error());
    QCOMPARE(job.m_job, original);
    QCOMPARE(state->closures, 0);
    QVERIFY(job.accounting().has_value());
    QVERIFY(job.closeEmpty().has_value());
    QCOMPARE(state->closures, 1);
}

void WindowsProcessJobTests::terminateFailureRetainsHandle() {
    auto state = std::make_shared<ApiObservation>();
    auto created = Job::createWithApi(observedApi(state));
    QVERIFY(created.has_value());
    auto& job = **created;
    QProcess process;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job, &process); });
    QVERIFY(launch(job, process, QStringLiteral("echo")).has_value());
    QVERIFY(readReady(process).has_value());
    auto generation = Generation::observe(process.processId());
    QVERIFY(generation.has_value());
    const HANDLE original = job.m_job;
    state->fail_terminate = true;
    const auto refused = job.terminate();
    state->fail_terminate = false;
    QVERIFY(!refused);
    QCOMPARE(refused.error(),
             QStringLiteral("Terminate Windows process Job failed (err=%1)").arg(kInjectedError));
    QCOMPARE(job.m_job, original);
    QCOMPARE(state->closures, 0);
    const auto alive = generation->isAlive();
    QVERIFY(alive.has_value() && *alive);
    QVERIFY(job.terminate().has_value());
    QCOMPARE(state->termination_code, kExpectedTerminationExit);
    QVERIFY(process.waitForFinished(kProcessWaitMs));
    QVERIFY(waitForEmpty(job).has_value());
    const auto exited = generation->waitForExit();
    QVERIFY(exited.has_value());
    QCOMPARE(*exited, DWORD{kExpectedTerminationExit});
    QVERIFY(job.closeEmpty().has_value());
}

void WindowsProcessJobTests::closeFailureRetainsHandle() {
    auto state = std::make_shared<ApiObservation>();
    auto created = Job::createWithApi(observedApi(state));
    QVERIFY(created.has_value());
    auto& job = **created;
    const auto cleanup = qScopeGuard([&] { cleanOwned(job); });
    const HANDLE original = job.m_job;
    state->fail_close = true;
    const auto refused = job.closeEmpty();
    state->fail_close = false;
    QVERIFY(!refused);
    QCOMPARE(refused.error(),
             QStringLiteral("Close empty Windows process Job failed (err=%1)").arg(kInjectedError));
    QCOMPARE(job.m_job, original);
    QCOMPARE(state->closures, 0);
    QVERIFY(job.accounting().has_value());
    QVERIFY(job.closeEmpty().has_value());
    QCOMPARE(state->closures, 1);
    QVERIFY(job.m_job == nullptr);
}

int WindowsProcessJobTests::runDestructorChild(const QString& mode) {
    OwnedChildDeadline deadline(kChildLifetimeMs, kWatchdogExit);
    // This subprocess deliberately exercises qFatal. Prevent Windows fault UI,
    // while retaining qFatal stderr diagnostics and the real abnormal exit.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto state = std::make_shared<ApiObservation>();
    auto created = Job::createWithApi(observedApi(state));
    if (!created) {
        return kFixtureFailureExit;
    }
    QProcess process;
    if (mode == QLatin1String("destructor-active")) {
        const auto launched = launch(**created, process, QStringLiteral("leaf"));
        if (!launched) {
            cleanOwned(**created, &process);
            return kFixtureFailureExit;
        }
    } else {
        state->fail_query = true;
    }
    created->reset();  // Must fail-fast before an unproved owner is destroyed.
    return kUnexpectedDestructorReturn;
}

void WindowsProcessJobTests::destructorRefusesUnprovedCleanup_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("query-failure") << QStringLiteral("destructor-query")
                                   << QStringLiteral("Query Windows process Job accounting failed");
    QTest::newRow("active-member")
        << QStringLiteral("destructor-active")
        << QStringLiteral("Windows process Job destroyed with 1 active processes");
}

void WindowsProcessJobTests::destructorRefusesUnprovedCleanup() {
    QFETCH(QString, mode);
    QFETCH(QString, diagnostic);
    QProcess process;
    QProcessEnvironment child_environment = QProcessEnvironment::systemEnvironment();
    child_environment.insert(QStringLiteral("QT_FORCE_STDERR_LOGGING"), QStringLiteral("1"));
    process.setProcessEnvironment(child_environment);
    process.start(QCoreApplication::applicationFilePath(),
                  {QString::fromLatin1(kChildSwitch), mode});
    QVERIFY(process.waitForStarted(kProcessWaitMs));
    const bool finished = process.waitForFinished(kChildLifetimeMs + kProcessWaitMs);
    if (!finished) {
        process.kill();
        QVERIFY(process.waitForFinished(kProcessWaitMs));
    }
    QVERIFY2(finished, "Owned destructor probe exceeded independent lifetime");
    QCOMPARE(process.exitStatus(), QProcess::CrashExit);
    QVERIFY(process.exitCode() != kFixtureFailureExit &&
            process.exitCode() != kUnexpectedDestructorReturn);
    const QString error = QString::fromUtf8(process.readAllStandardError());
    QVERIFY2(error.contains(diagnostic), qPrintable(error));
}

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() > 1 && arguments.at(1) == QLatin1String(kChildSwitch)) {
        constexpr int kChildArgumentCount = 3;
        constexpr int kChildModeIndex = 2;
        if (arguments.size() != kChildArgumentCount) {
            return kFixtureFailureExit;
        }
        const QString mode = arguments.at(kChildModeIndex);
        if (mode.startsWith(QLatin1String("destructor-"))) {
            return WindowsProcessJobTests::runDestructorChild(mode);
        }
        return runOrdinaryChild(mode);
    }
    WindowsProcessJobTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "test_windows_process_job.moc"
