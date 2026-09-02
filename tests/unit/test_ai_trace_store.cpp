// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_trace_store.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>

namespace {

constexpr qint64 kJsonlLineCapBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kJsonlLoadCapBytes = 128LL * 1024LL * 1024LL;
constexpr qsizetype kRotationPaddingChars = 900;
constexpr qint64 kCapSlackDivisor = 2;

QByteArray traceLine(const QString& run_id) {
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                     {QStringLiteral("run_id"), run_id},
                                     {QStringLiteral("kind"), QStringLiteral("phase")},
                                     {QStringLiteral("name"), QStringLiteral("diagnose")},
                                     {QStringLiteral("status"), QStringLiteral("started")}})
               .toJson(QJsonDocument::Compact) +
           '\n';
}

QByteArray activityLine(const QString& run_id) {
    return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                     {QStringLiteral("session_id"), QStringLiteral("session_2")},
                                     {QStringLiteral("run_id"), run_id},
                                     {QStringLiteral("kind"), QStringLiteral("phase")},
                                     {QStringLiteral("state"), QStringLiteral("running")},
                                     {QStringLiteral("summary"), QStringLiteral("diagnose")}})
               .toJson(QJsonDocument::Compact) +
           '\n';
}

sak::ai::AiTraceEvent paddedTraceEvent(int index) {
    auto event = sak::ai::TraceStore::event({QStringLiteral("run_%1").arg(index),
                                             QStringLiteral("phase"),
                                             QStringLiteral("diagnose"),
                                             QStringLiteral("started"),
                                             {},
                                             {}});
    event.timestamp_utc = QDateTime::fromString(QStringLiteral("2026-09-01T12:00:00.000Z"),
                                                Qt::ISODateWithMs);
    event.span_id = QStringLiteral("span_fixed");
    event.metadata.insert(QStringLiteral("padding"),
                          QString(kRotationPaddingChars, QLatin1Char('x')));
    return event;
}

qint64 serializedLineSize(const sak::ai::AiTraceEvent& event) {
    return QJsonDocument(event.toJson()).toJson(QJsonDocument::Compact).size() + 1;
}

QString firstRunId(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readLine().trimmed())
        .object()
        .value(QStringLiteral("run_id"))
        .toString();
}

}  // namespace

class AiTraceStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendEvent_writesTraceJsonl();
    void appendActivityEvent_writesActivityJsonl();
    void loadEvents_rejectsMalformedLine();
    void loadActivityEvents_rejectsMalformedLine();
    void loadEvents_rejectsUnsupportedSchema();
    void loadActivityEvents_rejectsUnsupportedSchema();
    void loadEvents_rejectsTruncatedLine();
    void loadActivityEvents_rejectsTruncatedLine();
    void loadEvents_rejectsOversizedLine();
    void loadActivityEvents_rejectsOversizedLine();
    void loadEvents_rejectsAggregateCapWithoutPartialData();
    void loadActivityEvents_rejectsAggregateCapWithoutPartialData();
    void loadEvents_reportsReadFailureWithoutPartialData();
    void loadActivityEvents_reportsReadFailureWithoutPartialData();
    void traceEvent_roundTripsTokenUsageAndMetadata();
    void activityEvent_roundTripsContractFields();
    void appendReplayEvent_writesCompactReplayJsonl();
    void traceAndActivityMetadata_isRedacted();
    void appendEvent_rotatesBeforeWriteWouldExceedCap();
    void appendEvent_rejectsRecordLargerThanFileCap();
    void appendActivityEvent_rejectsRecordLargerThanLineCap();
    void rotation_keepsBoundedRingAndNewestLive();
};

void AiTraceStoreTests::appendEvent_writesTraceJsonl() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QJsonObject metadata;
    metadata[QStringLiteral("phase_id")] = QStringLiteral("diagnose");
    auto event = sak::ai::TraceStore::event({QStringLiteral("run_1"),
                                             QStringLiteral("phase"),
                                             QStringLiteral("diagnose"),
                                             QStringLiteral("started"),
                                             metadata,
                                             {}});

    QString error;
    QVERIFY2(store.appendEvent(event, &error), qPrintable(error));

    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto doc = QJsonDocument::fromJson(file.readLine().trimmed());
    QVERIFY(doc.isObject());
    const auto obj = doc.object();
    QCOMPARE(obj.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_1"));
    QCOMPARE(obj.value(QStringLiteral("kind")).toString(), QStringLiteral("phase"));
    QCOMPARE(obj.value(QStringLiteral("name")).toString(), QStringLiteral("diagnose"));
    QCOMPARE(obj.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
    QCOMPARE(obj.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(obj.value(QStringLiteral("duration_ms")).toInteger(), static_cast<qint64>(0));
    QCOMPARE(obj.value(QStringLiteral("parent_span_id")).toString(), QString());
    const auto written_metadata = obj.value(QStringLiteral("metadata")).toObject();
    QCOMPARE(written_metadata.value(QStringLiteral("phase_id")).toString(),
             QStringLiteral("diagnose"));
    // appendEvent stamps a generated span id when the seed carries none: "span_" + 12 uuid chars.
    const QString span_id = obj.value(QStringLiteral("span_id")).toString();
    QVERIFY(span_id.startsWith(QStringLiteral("span_")));
    QCOMPARE(span_id.size(), static_cast<qsizetype>(17));
}

void AiTraceStoreTests::appendActivityEvent_writesActivityJsonl() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QJsonObject metadata;
    metadata[QStringLiteral("workflow_id")] = QStringLiteral("download_offline_installer");
    auto event = sak::ai::TraceStore::activityEvent({QStringLiteral("session_1"),
                                                     QStringLiteral("run_1"),
                                                     QStringLiteral("tool_call"),
                                                     QStringLiteral("running_tool"),
                                                     QStringLiteral("Downloading installer"),
                                                     metadata});

    QString error;
    QVERIFY2(store.appendActivityEvent(event, &error), qPrintable(error));

    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto doc = QJsonDocument::fromJson(file.readLine().trimmed());
    QVERIFY(doc.isObject());
    const auto obj = doc.object();
    QCOMPARE(obj.value(QStringLiteral("session_id")).toString(), QStringLiteral("session_1"));
    QCOMPARE(obj.value(QStringLiteral("state")).toString(), QStringLiteral("running_tool"));
    QCOMPARE(obj.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_1"));
    QCOMPARE(obj.value(QStringLiteral("kind")).toString(), QStringLiteral("tool_call"));
    QCOMPARE(obj.value(QStringLiteral("summary")).toString(),
             QStringLiteral("Downloading installer"));
    QCOMPARE(obj.value(QStringLiteral("workflow_id")).toString(),
             QStringLiteral("download_offline_installer"));
    QCOMPARE(obj.value(QStringLiteral("schema_version")).toInt(), 1);
    // appendActivityEvent stamps a generated activity id: "act_" + 12 uuid chars.
    const QString activity_id = obj.value(QStringLiteral("activity_id")).toString();
    QVERIFY(activity_id.startsWith(QStringLiteral("act_")));
    QCOMPARE(activity_id.size(), static_cast<qsizetype>(16));
}

void AiTraceStoreTests::loadEvents_rejectsMalformedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(traceLine(QStringLiteral("run_1")));
    file.write("not json\n");
    file.write(traceLine(QStringLiteral("run_2")));
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Trace audit line 2 does not contain a valid JSON object"));
}

void AiTraceStoreTests::loadActivityEvents_rejectsMalformedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(activityLine(QStringLiteral("run_1")));
    file.write("not json\n");
    file.write(activityLine(QStringLiteral("run_2")));
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Activity audit line 2 does not contain a valid JSON object"));
}

void AiTraceStoreTests::loadEvents_rejectsUnsupportedSchema() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"schema_version\":99}\n");
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Trace audit line 1 has unsupported schema_version"));
}

void AiTraceStoreTests::loadActivityEvents_rejectsUnsupportedSchema() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"schema_version\":99}\n");
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Activity audit line 1 has unsupported schema_version"));
}

void AiTraceStoreTests::loadEvents_rejectsTruncatedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(traceLine(QStringLiteral("run_1")));
    QByteArray truncated = traceLine(QStringLiteral("run_2"));
    truncated.chop(1);
    file.write(truncated);
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Trace audit line 2 is truncated (missing newline)"));
}

void AiTraceStoreTests::loadActivityEvents_rejectsTruncatedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(activityLine(QStringLiteral("run_1")));
    QByteArray truncated = activityLine(QStringLiteral("run_2"));
    truncated.chop(1);
    file.write(truncated);
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Activity audit line 2 is truncated (missing newline)"));
}

void AiTraceStoreTests::loadEvents_rejectsOversizedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(traceLine(QStringLiteral("run_1")));
    file.write("{");
    QVERIFY(file.resize(file.pos() + kJsonlLineCapBytes));
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Trace audit line 2 exceeds the 4194304-byte line cap"));
}

void AiTraceStoreTests::loadActivityEvents_rejectsOversizedLine() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(activityLine(QStringLiteral("run_1")));
    file.write("{");
    QVERIFY(file.resize(file.pos() + kJsonlLineCapBytes));
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Activity audit line 2 exceeds the 4194304-byte line cap"));
}

void AiTraceStoreTests::loadEvents_rejectsAggregateCapWithoutPartialData() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(traceLine(QStringLiteral("run_1")));
    QVERIFY(file.resize(kJsonlLoadCapBytes + 1));
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Trace audit file exceeds the 134217728-byte load cap"));
}

void AiTraceStoreTests::loadActivityEvents_rejectsAggregateCapWithoutPartialData() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(activityLine(QStringLiteral("run_1")));
    QVERIFY(file.resize(kJsonlLoadCapBytes + 1));
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QCOMPARE(error, QStringLiteral("Activity audit file exceeds the 134217728-byte load cap"));
}

void AiTraceStoreTests::loadEvents_reportsReadFailureWithoutPartialData() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(traceLine(QStringLiteral("run_1")));
    file.close();

    QFile locked_file(store.tracePath());
    QVERIFY(locked_file.open(QIODevice::ReadWrite));
    OVERLAPPED overlap{};
    QVERIFY(LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(locked_file.handle())),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0,
                       MAXDWORD,
                       MAXDWORD,
                       &overlap));

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY(events.isEmpty());
    QVERIFY2(error.startsWith(QStringLiteral("Could not read trace audit line 1: ")),
             qPrintable(error));
}

void AiTraceStoreTests::loadActivityEvents_reportsReadFailureWithoutPartialData() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(activityLine(QStringLiteral("run_1")));
    file.close();

    QFile locked_file(store.activityPath());
    QVERIFY(locked_file.open(QIODevice::ReadWrite));
    OVERLAPPED overlap{};
    QVERIFY(LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(locked_file.handle())),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0,
                       MAXDWORD,
                       MAXDWORD,
                       &overlap));

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY(events.isEmpty());
    QVERIFY2(error.startsWith(QStringLiteral("Could not read activity audit line 1: ")),
             qPrintable(error));
}

void AiTraceStoreTests::traceEvent_roundTripsTokenUsageAndMetadata() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    sak::ai::AiTraceEvent event = sak::ai::TraceStore::event({QStringLiteral("run_3"),
                                                              QStringLiteral("model_call"),
                                                              QStringLiteral("responses.create"),
                                                              QStringLiteral("completed"),
                                                              {},
                                                              {}});
    event.token_usage.input_tokens = 10;
    event.token_usage.output_tokens = 5;
    event.token_usage.total_tokens = 15;
    event.metadata[QStringLiteral("model")] = QStringLiteral("gpt-5.4");

    QString error;
    QVERIFY2(store.appendEvent(event, &error), qPrintable(error));

    const auto events = store.loadEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    const auto& loaded = events.first();
    QCOMPARE(loaded.token_usage.input_tokens, 10);
    QCOMPARE(loaded.token_usage.output_tokens, 5);
    QCOMPARE(loaded.token_usage.total_tokens, 15);
    QCOMPARE(loaded.token_usage.cached_input_tokens, 0);
    QCOMPARE(loaded.token_usage.reasoning_tokens, 0);
    QCOMPARE(loaded.kind, QStringLiteral("model_call"));
    QCOMPARE(loaded.name, QStringLiteral("responses.create"));
    QCOMPARE(loaded.status, QStringLiteral("completed"));
    QCOMPARE(loaded.metadata.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-5.4"));

    // Pin the on-disk shape too: a symmetric key rename or a swapped field in the
    // writer/reader pair would otherwise survive the round trip unnoticed.
    QFile raw(store.tracePath());
    QVERIFY(raw.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto stored = QJsonDocument::fromJson(raw.readLine().trimmed()).object();
    const auto usage = stored.value(QStringLiteral("token_usage")).toObject();
    QCOMPARE(usage.size(), static_cast<qsizetype>(5));
    QCOMPARE(usage.value(QStringLiteral("input_tokens")).toInteger(), static_cast<qint64>(10));
    QCOMPARE(usage.value(QStringLiteral("output_tokens")).toInteger(), static_cast<qint64>(5));
    QCOMPARE(usage.value(QStringLiteral("total_tokens")).toInteger(), static_cast<qint64>(15));
    QCOMPARE(usage.value(QStringLiteral("cached_input_tokens")).toInteger(),
             static_cast<qint64>(0));
    QCOMPARE(usage.value(QStringLiteral("reasoning_tokens")).toInteger(), static_cast<qint64>(0));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().token_usage.total_tokens, 15);
    QCOMPARE(events.first().metadata.value(QStringLiteral("model")).toString(),
             QStringLiteral("gpt-5.4"));
}

void AiTraceStoreTests::activityEvent_roundTripsContractFields() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QJsonObject metadata;
    metadata[QStringLiteral("workflow_id")] = QStringLiteral("install_app_now");
    metadata[QStringLiteral("phase_id")] = QStringLiteral("install");
    metadata[QStringLiteral("agent_id")] = QStringLiteral("package_agent");
    metadata[QStringLiteral("tool_name")] = QStringLiteral("sak_package_manager");
    metadata[QStringLiteral("artifact_refs")] =
        QJsonArray{QStringLiteral("artifacts/My Chat/logs/install.txt")};
    metadata[QStringLiteral("evidence_refs")] = QJsonArray{QStringLiteral("trace:span_123")};
    metadata[QStringLiteral("error_message")] = QStringLiteral("none");

    auto event = sak::ai::TraceStore::activityEvent({QStringLiteral("session_3"),
                                                     QStringLiteral("run_3"),
                                                     QStringLiteral("tool_call"),
                                                     QStringLiteral("complete"),
                                                     QStringLiteral("Installed app"),
                                                     metadata});
    event.token_usage.total_tokens = 42;

    QString error;
    QVERIFY2(store.appendActivityEvent(event, &error), qPrintable(error));

    const auto events = store.loadActivityEvents(&error);
    QCOMPARE(events.size(), 1);
    const auto& loaded = events.first();
    QCOMPARE(loaded.session_id, QStringLiteral("session_3"));
    QCOMPARE(loaded.run_id, QStringLiteral("run_3"));
    QCOMPARE(loaded.kind, QStringLiteral("tool_call"));
    QCOMPARE(loaded.state, QStringLiteral("complete"));
    QCOMPARE(loaded.summary, QStringLiteral("Installed app"));
    QCOMPARE(loaded.workflow_id, QStringLiteral("install_app_now"));
    QCOMPARE(loaded.phase_id, QStringLiteral("install"));
    QCOMPARE(loaded.agent_id, QStringLiteral("package_agent"));
    QCOMPARE(loaded.tool_name, QStringLiteral("sak_package_manager"));
    QCOMPARE(loaded.token_usage.total_tokens, 42);
    // activityEvent() maps metadata "error_message" onto the event's error field.
    QCOMPARE(loaded.error, QStringLiteral("none"));
    QCOMPARE(loaded.artifact_refs,
             (QStringList{QStringLiteral("artifacts/My Chat/logs/install.txt")}));
    QCOMPARE(loaded.evidence_refs, (QStringList{QStringLiteral("trace:span_123")}));

    // Second arm of the artifact_refs guard: legacy metadata carries the list under
    // "artifacts" instead of "artifact_refs".
    QJsonObject legacy_metadata;
    legacy_metadata[QStringLiteral("artifacts")] =
        QJsonArray{QStringLiteral("artifacts/My Chat/logs/legacy.txt")};
    const auto legacy = sak::ai::TraceStore::activityEvent({QStringLiteral("session_3"),
                                                            QStringLiteral("run_4"),
                                                            QStringLiteral("tool_call"),
                                                            QStringLiteral("complete"),
                                                            QStringLiteral("Installed app"),
                                                            legacy_metadata});
    QCOMPARE(legacy.artifact_refs,
             (QStringList{QStringLiteral("artifacts/My Chat/logs/legacy.txt")}));
    QCOMPARE(legacy.evidence_refs, QStringList());
    QCOMPARE(loaded.artifact_refs,
             (QStringList{QStringLiteral("artifacts/My Chat/logs/install.txt")}));
    QCOMPARE(loaded.evidence_refs, (QStringList{QStringLiteral("trace:span_123")}));
}

void AiTraceStoreTests::appendReplayEvent_writesCompactReplayJsonl() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QJsonObject metadata;
    metadata[QStringLiteral("prompt_sha256")] = QStringLiteral("abc123");
    metadata[QStringLiteral("model")] = QStringLiteral("gpt-5.5");
    metadata[QStringLiteral("tool_name")] = QStringLiteral("sak_provider_gateway");
    metadata[QStringLiteral("failure_class")] = QStringLiteral("health_backoff");
    metadata[QStringLiteral("api_key")] = QStringLiteral("ctx7") + QStringLiteral("s") +
                                          QStringLiteral("k-fc513191-580d-40c0-b244-17ea71f182b9");
    metadata[QStringLiteral("authorization")] = QStringLiteral("Bearer ") +
                                                QStringLiteral("abcdef") +
                                                QStringLiteral("ghijklmnopqrstuvwxyz");

    QString error;
    QVERIFY2(store.appendReplayEvent(QStringLiteral("run_4"),
                                     QStringLiteral("tool_call:sak_provider_gateway"),
                                     QStringLiteral("health_suppressed"),
                                     metadata,
                                     &error),
             qPrintable(error));

    QFile file(store.replayPath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto doc = QJsonDocument::fromJson(file.readLine().trimmed());
    QVERIFY(doc.isObject());
    const auto obj = doc.object();
    QCOMPARE(obj.value(QStringLiteral("run_id")).toString(), QStringLiteral("run_4"));
    QCOMPARE(obj.value(QStringLiteral("prompt_sha256")).toString(), QStringLiteral("abc123"));
    QCOMPARE(obj.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-5.5"));
    QCOMPARE(obj.value(QStringLiteral("tool_name")).toString(),
             QStringLiteral("sak_provider_gateway"));
    QCOMPARE(obj.value(QStringLiteral("status")).toString(), QStringLiteral("health_suppressed"));
    QCOMPARE(obj.value(QStringLiteral("event_type")).toString(),
             QStringLiteral("tool_call:sak_provider_gateway"));
    QCOMPARE(obj.value(QStringLiteral("schema_version")).toInt(), 1);
    const QJsonObject stored_metadata = obj.value(QStringLiteral("metadata")).toObject();
    QCOMPARE(stored_metadata.value(QStringLiteral("api_key")).toString(),
             QStringLiteral("[redacted]"));
    QCOMPARE(stored_metadata.value(QStringLiteral("authorization")).toString(),
             QStringLiteral("[redacted]"));
    // prompt_sha256 is MOVED out of metadata; model / tool_name are promoted AND kept,
    // and unpromoted keys ride along untouched.
    QVERIFY(!stored_metadata.contains(QStringLiteral("prompt_sha256")));
    QCOMPARE(stored_metadata.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-5.5"));
    QCOMPARE(stored_metadata.value(QStringLiteral("tool_name")).toString(),
             QStringLiteral("sak_provider_gateway"));
    QCOMPARE(stored_metadata.value(QStringLiteral("failure_class")).toString(),
             QStringLiteral("health_backoff"));
    QCOMPARE(obj.value(QStringLiteral("schema_version")).toInt(), 1);
    const QString line = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    QVERIFY(!line.contains(QStringLiteral("ctx7") + QStringLiteral("sk-")));
    QVERIFY(!line.contains(QStringLiteral("abcdefghijklmnopqrstuvwxyz")));
}

void AiTraceStoreTests::traceAndActivityMetadata_isRedacted() {
    // P08-16: trace.jsonl and activity.jsonl must redact secrets like the replay
    // artifact does, not persist workflow input values / previews in cleartext.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());

    const QString secret = QStringLiteral("ctx7") + QStringLiteral("s") +
                           QStringLiteral("k-fc513191-580d-40c0-b244-17ea71f182b9");
    QJsonObject metadata;
    metadata[QStringLiteral("model")] = QStringLiteral("gpt-5.5");
    metadata[QStringLiteral("api_key")] = secret;

    QString error;
    auto trace = sak::ai::TraceStore::event({QStringLiteral("run_5"),
                                             QStringLiteral("model_call"),
                                             QStringLiteral("responses.create"),
                                             QStringLiteral("completed"),
                                             {},
                                             {}});
    trace.metadata = metadata;
    QVERIFY2(store.appendEvent(trace, &error), qPrintable(error));

    auto activity = sak::ai::TraceStore::activityEvent({QStringLiteral("session_5"),
                                                        QStringLiteral("run_5"),
                                                        QStringLiteral("tool_call"),
                                                        QStringLiteral("complete"),
                                                        QStringLiteral("done ") + secret,
                                                        metadata});
    QVERIFY2(store.appendActivityEvent(activity, &error), qPrintable(error));

    const auto trace_events = store.loadEvents(&error);
    QCOMPARE(trace_events.size(), 1);
    QCOMPARE(trace_events.first().metadata.value(QStringLiteral("api_key")).toString(),
             QStringLiteral("[redacted]"));
    QCOMPARE(trace_events.first().metadata.value(QStringLiteral("model")).toString(),
             QStringLiteral("gpt-5.5"));

    const auto activity_events = store.loadActivityEvents(&error);
    QCOMPARE(activity_events.size(), 1);
    QCOMPARE(activity_events.first().metadata.value(QStringLiteral("api_key")).toString(),
             QStringLiteral("[redacted]"));
    // Free-text summary runs through the same redactor: the key is rewritten IN PLACE and the
    // surrounding text is kept, so a redactor that blanked the whole field is caught too.
    QCOMPARE(activity_events.first().summary, QStringLiteral("done [redacted-context7-key]"));

    // The raw secret must not survive anywhere in EITHER audit file -- the activity trail was
    // never checked, so a redactor applied only on the trace path shipped green.
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(!QString::fromUtf8(file.readAll()).contains(secret));
    QFile activity_file(store.activityPath());
    QVERIFY(activity_file.open(QIODevice::ReadOnly | QIODevice::Text));
    QVERIFY(!QString::fromUtf8(activity_file.readAll()).contains(secret));
}

void AiTraceStoreTests::appendEvent_rotatesBeforeWriteWouldExceedCap() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    const qint64 line_size = serializedLineSize(paddedTraceEvent(0));
    const qint64 cap = line_size + (line_size / kCapSlackDivisor);
    store.setMaxJsonlBytes(cap);
    QCOMPARE(store.maxJsonlBytes(), cap);

    QString error;
    for (int i = 0; i < 4; ++i) {
        QVERIFY2(store.appendEvent(paddedTraceEvent(i), &error), qPrintable(error));
    }

    QCOMPARE(firstRunId(store.tracePath()), QStringLiteral("run_3"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".1")), QStringLiteral("run_2"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".2")), QStringLiteral("run_1"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".3")), QStringLiteral("run_0"));
    for (int i = 0; i <= 3; ++i) {
        const QString path = i == 0 ? store.tracePath()
                                    : store.tracePath() + QStringLiteral(".%1").arg(i);
        QVERIFY(QFileInfo(path).size() <= cap);
    }
}

void AiTraceStoreTests::appendEvent_rejectsRecordLargerThanFileCap() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    const auto event = paddedTraceEvent(0);
    const qint64 cap = serializedLineSize(event) - 1;
    store.setMaxJsonlBytes(cap);

    QString error;
    QVERIFY(!store.appendEvent(event, &error));
    QCOMPARE(error,
             QStringLiteral("Could not append trace: audit record exceeds the %1-byte file cap")
                 .arg(cap));
    QVERIFY(!QFile::exists(store.tracePath()));
    QVERIFY(!QFile::exists(store.tracePath() + QStringLiteral(".1")));
}

void AiTraceStoreTests::appendActivityEvent_rejectsRecordLargerThanLineCap() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    sak::ai::TraceStore store(temp.path());
    auto event = sak::ai::TraceStore::activityEvent({QStringLiteral("session_1"),
                                                     QStringLiteral("run_1"),
                                                     QStringLiteral("tool_call"),
                                                     QStringLiteral("running"),
                                                     QStringLiteral("diagnose"),
                                                     {}});
    event.artifact_refs.append(QString(kJsonlLineCapBytes, QLatin1Char('x')));

    QString error;
    QVERIFY(!store.appendActivityEvent(event, &error));
    QCOMPARE(error,
             QStringLiteral(
                 "Could not append activity: audit record exceeds the 4194304-byte line cap"));
    QVERIFY(!QFile::exists(store.activityPath()));
}

void AiTraceStoreTests::rotation_keepsBoundedRingAndNewestLive() {
    // Many appends past the cap keep only a bounded ring of rolled files, and the
    // live file always holds the newest event.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    const qint64 line_size = serializedLineSize(paddedTraceEvent(0));
    const qint64 cap = line_size + (line_size / kCapSlackDivisor);
    store.setMaxJsonlBytes(cap);

    QString error;
    constexpr int kEvents = 8;
    for (int i = 0; i < kEvents; ++i) {
        QVERIFY2(store.appendEvent(paddedTraceEvent(i), &error), qPrintable(error));
    }

    // Ring is bounded AND it actually rolls: the three rolled files hold the three
    // generations immediately before the live one, and .4 never exists.
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".1")), QStringLiteral("run_6"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".2")), QStringLiteral("run_5"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".3")), QStringLiteral("run_4"));
    QVERIFY(!QFile::exists(store.tracePath() + QStringLiteral(".4")));

    // The live file holds the newest event.
    QFile live(store.tracePath());
    QVERIFY(live.open(QIODevice::ReadOnly | QIODevice::Text));
    const auto doc = QJsonDocument::fromJson(live.readLine().trimmed());
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value(QStringLiteral("run_id")).toString(),
             QStringLiteral("run_%1").arg(kEvents - 1));
}

QTEST_GUILESS_MAIN(AiTraceStoreTests)
#include "test_ai_trace_store.moc"
