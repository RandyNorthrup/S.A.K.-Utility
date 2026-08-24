// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_trace_store.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AiTraceStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void appendEvent_writesTraceJsonl();
    void appendActivityEvent_writesActivityJsonl();
    void loadEvents_skipsInvalidLines();
    void loadActivityEvents_skipsInvalidLines();
    void traceEvent_roundTripsTokenUsageAndMetadata();
    void activityEvent_roundTripsContractFields();
    void appendReplayEvent_writesCompactReplayJsonl();
    void traceAndActivityMetadata_isRedacted();
    void appendEvent_rotatesInsteadOfDroppingAtCap();
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
                                             metadata});

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

void AiTraceStoreTests::loadEvents_skipsInvalidLines() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    // A valid line BEFORE the garbage: with the malformed line first, a loader that ABORTED on
    // the first parse failure and returned what it had would be indistinguishable from one that
    // skipped and continued -- both yield the single trailing event. Bracketing the garbage
    // proves the skip resumes, and pins the surviving ORDER.
    file.write(R"({"run_id":"run_1","kind":"phase","name":"diagnose","status":"started"})");
    file.write("\n");
    file.write("not json\n");
    file.write(R"({"run_id":"run_2","kind":"tool","name":"powershell","status":"completed"})");
    file.write("\n");
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.first().run_id, QStringLiteral("run_1"));
    QCOMPARE(events.first().name, QStringLiteral("diagnose"));
    QCOMPARE(events.last().run_id, QStringLiteral("run_2"));
    QCOMPARE(events.last().name, QStringLiteral("powershell"));
}

void AiTraceStoreTests::loadActivityEvents_skipsInvalidLines() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    // Same bracketing as the trace sibling: a valid line BEFORE the garbage distinguishes
    // "skipped and resumed" from "aborted on first parse failure".
    file.write(
        R"({"session_id":"session_2","run_id":"run_1","kind":"phase","state":"running","summary":"first"})");
    file.write("\n");
    file.write("not json\n");
    file.write(
        R"({"session_id":"session_2","run_id":"run_2","kind":"tool","state":"complete","summary":"done"})");
    file.write("\n");
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.first().run_id, QStringLiteral("run_1"));
    QCOMPARE(events.first().state, QStringLiteral("running"));
    QCOMPARE(events.last().session_id, QStringLiteral("session_2"));
    QCOMPARE(events.last().state, QStringLiteral("complete"));
}

void AiTraceStoreTests::traceEvent_roundTripsTokenUsageAndMetadata() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    sak::ai::AiTraceEvent event = sak::ai::TraceStore::event({QStringLiteral("run_3"),
                                                              QStringLiteral("model_call"),
                                                              QStringLiteral("responses.create"),
                                                              QStringLiteral("completed")});
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
                                             QStringLiteral("completed")});
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

void AiTraceStoreTests::appendEvent_rotatesInsteadOfDroppingAtCap() {
    // At the size cap the trail must roll, not stop: every append still succeeds
    // and a rolled file appears alongside the (fresh) live file.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    store.setMaxJsonlBytes(200);  // each event line exceeds this, so appends roll
    QCOMPARE(store.maxJsonlBytes(), static_cast<qint64>(200));

    QString error;
    for (int i = 0; i < 4; ++i) {
        auto event = sak::ai::TraceStore::event({QStringLiteral("run_%1").arg(i),
                                                 QStringLiteral("phase"),
                                                 QStringLiteral("diagnose"),
                                                 QStringLiteral("started")});
        QVERIFY2(store.appendEvent(event, &error), qPrintable(error));
    }

    // Rolling preserves the events: live=run_3, .1=run_2, .2=run_1, .3=run_0.
    const auto firstRunId = [](const QString& path) {
        QFile rolled(path);
        if (!rolled.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QJsonDocument::fromJson(rolled.readLine().trimmed())
            .object()
            .value(QStringLiteral("run_id"))
            .toString();
    };
    QCOMPARE(firstRunId(store.tracePath()), QStringLiteral("run_3"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".1")), QStringLiteral("run_2"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".2")), QStringLiteral("run_1"));
    QCOMPARE(firstRunId(store.tracePath() + QStringLiteral(".3")), QStringLiteral("run_0"));
}

void AiTraceStoreTests::rotation_keepsBoundedRingAndNewestLive() {
    // Many appends past the cap keep only a bounded ring of rolled files, and the
    // live file always holds the newest event.
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    store.setMaxJsonlBytes(200);

    QString error;
    constexpr int kEvents = 8;
    for (int i = 0; i < kEvents; ++i) {
        auto event = sak::ai::TraceStore::event({QStringLiteral("run_%1").arg(i),
                                                 QStringLiteral("phase"),
                                                 QStringLiteral("diagnose"),
                                                 QStringLiteral("started")});
        QVERIFY2(store.appendEvent(event, &error), qPrintable(error));
    }

    // Ring is bounded AND it actually rolls: the three rolled files hold the three
    // generations immediately before the live one, and .4 never exists.
    const auto firstRunId = [](const QString& path) {
        QFile rolled(path);
        if (!rolled.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QJsonDocument::fromJson(rolled.readLine().trimmed())
            .object()
            .value(QStringLiteral("run_id"))
            .toString();
    };
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
