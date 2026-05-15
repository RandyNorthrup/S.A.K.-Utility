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
    QCOMPARE(doc.object().value(QStringLiteral("run_id")).toString(), QStringLiteral("run_1"));
    QCOMPARE(doc.object().value(QStringLiteral("kind")).toString(), QStringLiteral("phase"));
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
    QCOMPARE(doc.object().value(QStringLiteral("session_id")).toString(),
             QStringLiteral("session_1"));
    QCOMPARE(doc.object().value(QStringLiteral("state")).toString(),
             QStringLiteral("running_tool"));
}

void AiTraceStoreTests::loadEvents_skipsInvalidLines() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.tracePath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("not json\n");
    file.write(R"({"run_id":"run_2","kind":"tool","name":"powershell","status":"completed"})");
    file.write("\n");
    file.close();

    QString error;
    const auto events = store.loadEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().run_id, QStringLiteral("run_2"));
    QCOMPARE(events.first().name, QStringLiteral("powershell"));
}

void AiTraceStoreTests::loadActivityEvents_skipsInvalidLines() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    sak::ai::TraceStore store(temp.path());
    QFile file(store.activityPath());
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("not json\n");
    file.write(
        R"({"session_id":"session_2","run_id":"run_2","kind":"tool","state":"complete","summary":"done"})");
    file.write("\n");
    file.close();

    QString error;
    const auto events = store.loadActivityEvents(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.first().session_id, QStringLiteral("session_2"));
    QCOMPARE(events.first().state, QStringLiteral("complete"));
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
    QCOMPARE(loaded.workflow_id, QStringLiteral("install_app_now"));
    QCOMPARE(loaded.phase_id, QStringLiteral("install"));
    QCOMPARE(loaded.agent_id, QStringLiteral("package_agent"));
    QCOMPARE(loaded.tool_name, QStringLiteral("sak_package_manager"));
    QCOMPARE(loaded.token_usage.total_tokens, 42);
    QCOMPARE(loaded.artifact_refs.size(), 1);
    QCOMPARE(loaded.evidence_refs.size(), 1);
}

QTEST_GUILESS_MAIN(AiTraceStoreTests)
#include "test_ai_trace_store.moc"
